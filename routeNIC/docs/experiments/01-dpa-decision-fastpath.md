# Experiment 1: DPA decision fast path

**Device:** BlueField-3 DPA (compile-verified for `nv-dpa-cx7` too, but only
the BF3 has an actual DPA to execute on).

## The question

Can a request whose routing decision depends only on keyword-type signals
be resolved entirely on the DPA — never waking the GPU, never touching the
Spark's Grace CPU past the one-time kernel launch — and if the exact
software matching algorithms (BM25, character n-grams, Levenshtein fuzzy
matching) don't fit a constrained data-path core, how much does swapping
them for something that does fit actually cost in accuracy?

This BF3 unit has no hardware RegEx accelerator block, which ruled out the
original brainstorm's "just offload keyword matching to the RegEx engine"
plan. The DPA-suited replacement — a compiled multi-pattern exact-substring
matcher — turned out to be the more interesting question anyway: it's not
a hardware-availability workaround, it's an algorithm-hardware co-design
question with a real, measured answer.

## What's implemented, and what's actually been run

- `control-plane/accuracy_study/` — pure-software accuracy/tolerance study.
  No hardware required, run it with `python3 run_study.py`. Full
  methodology and real results (not projected) are in that directory's own
  README: **85.3% decision-level accuracy, perfect precision, in the
  case-handling mode that mirrors the software engines exactly; 76.5% and a
  new class of false positive once the DPA-realistic case-folding
  simplification is applied.**
- `data-plane/dpa_decision_fastpath/dpa_kernel/fastpath_kernel_dev.c` — the
  actual DPA kernel, compiled via `dpacc` for `nv-dpa-bf3`
  (`./build_dpa_kernel.sh`) both on this host and on the real BF3. Implements
  the same exact-substring + OR/AND/NOR semantics the accuracy study
  measured, as a nested-loop scan (see the file's own comment for why that's
  the right starting point instead of a compiled Aho-Corasick table).
- `data-plane/dpa_decision_fastpath/host/fastpath_launcher.c` +
  `fastpath_main.c` — **actually run against live DPA hardware on the lab's
  BlueField-3** (not just compiled): linked against the real
  `doca_dpa`/`doca_rdma`/`doca_common`/`flexio` libraries, executed with
  `sudo` against `mlx5_0`, and validated with 6 fixed test requests (one per
  rule, plus one that should hit nothing) against hand-computed expected
  results. **6/6 passed**, byte-for-byte, against real silicon. Raw output:
  `real_run_output.txt` in this experiment's directory.

### Two real bugs this run caught that compiling never would have

Getting to a real run — not just a clean `dpacc`/`gcc -fsyntax-only` — surfaced
two genuine defects:

1. **Wrong `dpacc --app-name`.** `dpa_common.c` (the shared DPA bootstrap
   every sample in this DOCA install links against, reused here rather than
   re-derived) hardcodes `extern struct doca_dpa_app *dpa_sample_app;` and
   passes that literal symbol to `doca_dpa_set_app()`. Every real DOCA DPA
   sample's build script passes `--app-name=dpa_sample_app` for exactly this
   reason. This experiment's build script originally used
   `routenic_fastpath_app` instead — `dpacc` compiled it fine (it doesn't
   know or care about downstream host-side linkage), but the host binary
   would have failed to link the first time anything actually called
   `allocate_dpa_resources()`. Fixed by matching the fixed convention.
2. **Dead rule-table build.** `routenic_fastpath_launch()` built a local
   `struct fastpath_keyword_rule[]` via `routenic_build_rule_table()`,
   used it only to read off `num_rules`, and never copied it anywhere — the
   function silently discarded the very data its own comment claimed the
   caller had already DMA-copied to DPA memory. In other words: nothing
   ever actually populated the DPA-side rule table. Fixed by moving that
   responsibility explicitly to the caller (`fastpath_main.c`, which now
   builds the table, `doca_dpa_h2d_memcpy`s it to DPA memory itself, and
   passes `num_rules` directly), and stripping the dead build out of the
   launch function.
3. (Caught live, not from reading source) `open_dpa_device()` in
   `dpa_common.c` hard-errors if `pf_device_name` and `rdma_device_name` are
   both explicitly set to the same real device name — this kernel doesn't
   use RDMA, so `rdma_device_name` needed to stay `DEVICE_DEFAULT_NAME`
   ("NOT_SET") rather than also being set to `mlx5_0`. Only surfaced by
   actually running the binary and reading its real error message.

## What's not implemented yet (needs more BF3 time)

- The rule table and request text are written to DPA memory directly by the
  test driver (single-request, blocking, correctness-first shape). Wiring
  this to an actual RDMA-triggered arrival path (BF3 receives a real request
  over the wire, writes it to DPA-local memory, triggers the kernel) is the
  natural next step now that this base mechanism is validated end-to-end on
  real hardware.
- `out_matched` results currently just sit in memory for the host to read;
  routing them onward (either "resolved, skip the GPU" or "unresolved, hand
  off to Experiment 2/4") isn't wired up yet.
- The DPA-side rule table is hand-duplicated from
  `control-plane/accuracy_study/workload.py` rather than generated from a
  shared schema — fine for a first correctness pass, a real gap for
  anything beyond it (see the launcher's own comment).
- Only 6 fixed single-word/phrase test requests have been run — real
  latency/throughput numbers (as opposed to functional correctness) need a
  proper timed loop, which `control-plane/bench/` is built to consume once
  this driver emits a trace in that format.

## Metrics this experiment feeds

- Fast-path hit rate: fraction of traffic resolved without reaching the GPU
  at all (the accuracy study's coverage finding — 2 of 23 decisions in this
  repo's own reference config are keyword-only — sets the ceiling on this
  until the DPA-tractable signal set grows beyond keyword matching).
- CPU-free fraction: this experiment's whole point is that, past kernel
  load, the Spark's CPU is never involved for a fast-path-resolved request.
- The accuracy/tolerance numbers above, which are the actual deliverable
  the project asked this experiment to produce: not "is DPA offload fast"
  (assumed) but "what does it cost, precisely, and is that cost
  acceptable."

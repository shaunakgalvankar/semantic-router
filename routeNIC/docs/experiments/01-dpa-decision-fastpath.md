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

## Real throughput benchmark vs. the real software baseline

`host/fastpath_bench_main.c` — a timed loop against the exact same 4 real
`config/config.yaml` rules `control-plane/software_baseline_bench` benchmarks
on the router's actual `KeywordClassifier.Classify()`, now driven by either
corpus: the real 23,448-query LMSYS Chatbot Arena corpus (`bench_large_corpus_output.txt`,
the primary result — see `routeNIC/docs/BENCHMARK_COMPARISON.md`) or the
original 34-query hand-curated set (`bench_real_run_output.txt`). Same
binary, `<corpus-file> [batch_size] [max_queries]` args — see the file's own
header. Two real optimizations, found by profiling a naive first version
against real hardware rather than assumed in advance:

1. **The single biggest lever: reuse the completion sync event.** The first
   version of `routenic_fastpath_launch()` created a fresh
   `doca_sync_event` on every call and destroyed it at the end — a hardware
   registration handshake, not free bookkeeping. Measured cost: **19.7ms
   mean per launch, 50.8 launches/sec** — about 470x *slower* than the
   42µs/op software baseline, i.e. the naive DPA path was dramatically
   worse before this fix, not better. `routenic_fastpath_launch_reuse_event()`
   creates the event once and reuses it via monotonically increasing target
   values (`doca_dpa_kernel_launch_update_set`'s `comp_event_val` parameter
   exists for exactly this). Result: **1.25ms mean, 801.8 launches/sec** —
   a 15.8x speedup from this one change, though still far behind software.
2. **Batch requests into one launch.** Even with the event reused, every
   request still pays DPA kernel dispatch overhead individually.
   `routenic_fastpath_evaluate_batch()` (the kernel) uses
   `doca_dpa_dev_thread_rank()` to give each DPA thread in one launch its
   own request — the whole 34-query corpus evaluated in a single launch,
   one thread per query, instead of 34 separate launches. Result:
   **13.7–18.7µs/request amortized mean, ~6µs/request at the launch p50,
   72,747 requests/sec** — this **beats the 42µs/op real software
   baseline**, and beats even software's cheapest per-query case (~3.7µs)
   at the batched median, while dominating its expensive fuzzy-matching
   tail (up to 225µs) by 1–2 orders of magnitude.

Honest gap in this result: batched mean (467–637µs/launch on the small
corpus) sits well above batched median (~202µs/launch) across repeated
runs — a real, reproducible right skew, not a one-off fluke, but not yet
root-caused. At the 184-batch scale of the large-corpus run this widened
further (p99 = 100.9ms/launch, two orders of magnitude above the mean).
Worth investigating with per-launch tracing before citing the mean figure
as a stable SLA number; median and wall-clock throughput are more robust.

**Real hardware limit found while scaling up:** batch sizes above 128 fail
outright with `Exceeded valid max number of threads per kernel` (probed
128/192/256/512/1024, then binary-searched between 128 and 192 to confirm
128 is the exact ceiling). This BlueField-3's DPA cannot launch more than
128 threads in a single kernel invocation — the large-corpus benchmark
chunks its 23,448 queries into 184 launches of 128 for exactly this reason.

## What's not implemented yet (needs more BF3 time)

- The rule table and request text are written to DPA memory directly by the
  test driver (batched-but-still-host-triggered shape). Wiring this to an
  actual RDMA-triggered arrival path (BF3 receives a real request over the
  wire, writes it to DPA-local memory, triggers the kernel) is the natural
  next step now that both correctness and a real throughput number are
  validated on real hardware.
- `out_matched` results currently just sit in memory for the host to read;
  routing them onward (either "resolved, skip the GPU" or "unresolved, hand
  off to Experiment 2/4") isn't wired up yet.
- The DPA-side rule table is hand-duplicated from
  `control-plane/accuracy_study/workload.py` rather than generated from a
  shared schema — fine for a first correctness pass, a real gap for
  anything beyond it (see the launcher's own comment).
- The batched-launch mean/median gap (and its large-corpus p99 tail) above
  isn't root-caused yet.
- Batch size is capped at the confirmed real hardware limit (128 threads),
  but nothing here explores *below* that — whether 128 is actually optimal
  vs. some smaller size with a better tail-latency profile is unmeasured.

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

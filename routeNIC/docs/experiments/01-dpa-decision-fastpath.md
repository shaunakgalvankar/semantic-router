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

## What's implemented

- `control-plane/accuracy_study/` — pure-software accuracy/tolerance study.
  No hardware required, run it with `python3 run_study.py`. Full
  methodology and real results (not projected) are in that directory's own
  README: **85.3% decision-level accuracy, perfect precision, in the
  case-handling mode that mirrors the software engines exactly; 76.5% and a
  new class of false positive once the DPA-realistic case-folding
  simplification is applied.**
- `data-plane/dpa_decision_fastpath/dpa_kernel/fastpath_kernel_dev.c` — the
  actual DPA kernel, compiled and verified via `dpacc` for `nv-dpa-bf3` on
  this host (`./build_dpa_kernel.sh`). Implements the same exact-substring +
  OR/AND/NOR semantics the accuracy study measured, as a nested-loop scan
  (see the file's own comment for why that's the right starting point
  instead of a compiled Aho-Corasick table).
- `data-plane/dpa_decision_fastpath/host/fastpath_launcher.c` — host-side
  launch code, syntax-verified against the real DOCA DPA headers on this
  host. Reuses the DPA bootstrap helpers every DOCA DPA sample on this
  machine already shares (`dpa_common.c`/`.h`) rather than re-deriving
  device/context setup from scratch.

## What's not implemented yet (needs the live BF3)

- The rule table and request text are currently written to DPA memory by
  the launcher directly (single-request, blocking, correctness-first
  shape). Wiring this to an actual RDMA-triggered arrival path (BF3
  receives a real request over the wire, writes it to DPA-local memory,
  triggers the kernel) is the natural next step once this base mechanism
  is validated end-to-end on hardware.
- `out_matched` results currently just sit in memory for the host to read;
  routing them onward (either "resolved, skip the GPU" or "unresolved, hand
  off to Experiment 2/4") isn't wired up yet.
- The DPA-side rule table is hand-duplicated from
  `control-plane/accuracy_study/workload.py` rather than generated from a
  shared schema — fine for a first correctness pass, a real gap for
  anything beyond it (see the launcher's own comment).

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

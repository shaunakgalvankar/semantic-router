# Software vs. DPA: a real, apples-to-apples benchmark comparison

This is the comparison the project's benchmarking phase asked for: the real
vLLM Semantic Router's actual keyword classifier vs. the DPA fast path, on
identical inputs, with real numbers from real hardware on both sides — not
projections, and not two different workloads dressed up as comparable.

**Bottom line: with two real optimizations applied, the DPA path is faster
than the real software path on this workload — 13.7–18.7µs/request amortized
vs. 42µs/op — while trading some accuracy (76.5–85.3% agreement with the
software engines, not identical behavior). Before those optimizations, the
naive DPA implementation was ~470x *slower* than software, not faster. The
speedup is real, but it took real, hardware-informed engineering to get
there — it wasn't the hardware being inherently faster by default.**

## What was actually compared

Both sides evaluate the exact same 4 keyword rules from this repo's own
`config/config.yaml` (`code_keywords`, `machine_learning`, `urgent_keywords`,
`fuzzy_sensitive_keywords`) against the exact same 34-query corpus
(`control-plane/accuracy_study/workload.py`, mirrored verbatim in both the Go
benchmark and the C benchmark driver). Neither corpus nor ruleset was tuned
to favor either side.

| Side | What's actually running | Where |
|---|---|---|
| Software | The real, unmodified `KeywordClassifier.Classify()` — `src/semantic-router/pkg/classification`, imported live via `go.mod replace`, not reimplemented | `control-plane/software_baseline_bench/` |
| Hardware | The DPA kernel's case-folded exact-substring matcher, on the lab's real BlueField-3 DPA | `data-plane/dpa_decision_fastpath/` |

## Results

| Configuration | Mean latency | Throughput | Notes |
|---|---|---|---|
| **Software** (real router code, single-core) | 42,025 ns/op | ~23,800 ops/sec (1/mean) | 980 B/op, 22 allocs/op. Per-query range: 3.7µs (bm25 hit) to 225µs (fuzzy-fallback miss) — a real ~60x spread depending on which rule matches. |
| **DPA, naive** (fresh sync event per launch) | 19,697,070 ns | 50.8 launches/sec | First working version. ~470x slower than software. |
| **DPA, event reused** | 1,247,063 ns | 801.8 launches/sec | 15.8x faster than naive, from one fix. Still ~30x slower than software. |
| **DPA, batched** (34 requests/launch, event reused) | 13,745–18,745 ns/request (amortized) | 72,747–53,343 requests/sec | **Faster than software's 42,025 ns/op mean.** Median per-launch ÷34 ≈ 5.9µs/request, competitive with software's *cheapest* case and far ahead of its expensive fuzzy-fallback tail. |

Raw output: `control-plane/software_baseline_bench/real_run_output.txt`,
`data-plane/dpa_decision_fastpath/bench_real_run_output.txt`.

## Why the DPA needed two real fixes to win at all

The instinct "hardware path should just be faster" was wrong until these were
found and fixed — worth stating plainly, since it's the actual lesson here:

1. **Per-call hardware registration is not free.** A `doca_sync_event`
   created and destroyed on every launch dominated everything else by
   orders of magnitude. This has no software analogue — a Go function call
   has no comparable "device registration" step — so naively porting a
   "one call per request" shape from software to a DPA launch imports a
   fixed cost software never had to pay.
2. **Per-launch dispatch overhead needs amortizing.** Even with the event
   reused, one DPA kernel launch per request still pays real dispatch cost
   per request. Batching multiple requests into one launch (one DPA thread
   per request, via `doca_dpa_dev_thread_rank()`) amortizes that dispatch
   cost across the batch — this is the single lever that took the DPA path
   from "still 30x slower" to "faster than software."

Software has no equivalent second lever available to it here: there's no
hardware dispatch cost to amortize away by batching Go function calls. So
this isn't "software could just batch too and win back the lead" — the
batching win is closing a cost that is specific to invoking hardware from a
host, not a generic trick either side could apply equally.

## The honest limits of this comparison

- **Not integrated into the real router.** Nothing here has ever received a
  request from an actual running semantic-router deployment. This is a
  microbenchmark of the same decision computation on both sides, run in
  isolation, not an end-to-end system comparison.
- **Not the same algorithm.** The DPA kernel does case-folded exact-substring
  matching; the software path does real bm25/ngram scoring and Levenshtein
  fuzzy matching. `control-plane/accuracy_study/` measured the DPA-realistic
  algorithm's agreement with the real software engines at **85.3%** (case
  mode matching software exactly) to **76.5%** (DPA-realistic case-folded
  mode) — the DPA path is not a drop-in behavioral replacement. The speed
  numbers above have to be read together with this, not instead of it.
- **Coverage ceiling.** Only 2 of 23 decisions in this repo's own reference
  config are keyword-only at all — the DPA fast path (in its current,
  keyword-only scope) is only ever eligible for a minority of real traffic
  regardless of how fast it is.
- **Unexplained latency variance.** The batched DPA mean (467–637µs/launch)
  sits well above its own median (~202µs/launch) reproducibly — a real skew
  that isn't root-caused yet (see Experiment 1's doc). The median/throughput
  figures above are more trustworthy than the mean until that's explained.
- **Single-node, single-request-stream measurement.** Neither side was
  tested under concurrent multi-client load, connection/queueing overhead,
  or realistic production traffic shape.
- **GPU path (Experiment 2)** has no numbers in this comparison at all yet —
  see its own experiment doc for why (blocked on a host-level GPUDirect RDMA
  registration issue, not an application bug).

## What would make this a complete, production-credible comparison

1. Root-cause the batched-launch mean/median gap.
2. Wire the DPA path to a real RDMA-triggered arrival path (BF3 receiving
   actual request bytes over the wire) instead of a host-driven benchmark
   loop, and re-measure — the numbers above are DPA-launch cost only, not
   full network-to-result latency.
3. Unblock and benchmark the GPU path the same way, for the (much larger)
   share of decisions that need BERT-classifier signals, not just keywords.
4. Report a single blended number weighted by the real fast-path hit rate
   (currently ceilinged at 2/23 decisions), not just the keyword-only-path
   number in isolation — that's the number that actually reflects
   end-to-end impact on real traffic.

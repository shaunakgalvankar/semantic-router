# Software vs. DPA: a real, apples-to-apples benchmark comparison

This is the comparison the project's benchmarking phase asked for: the real
vLLM Semantic Router's actual keyword classifier vs. the DPA fast path, on
identical inputs, with real numbers from real hardware on both sides — not
projections, and not two different workloads dressed up as comparable.

**Bottom line: with two real optimizations applied, the DPA path is faster
than the real software path — on a real, large, well-known dataset
(23,448 real Chatbot Arena user prompts, not a small hand-picked set), the
DPA's amortized cost (42.6µs/request mean, ~15.5µs/request at median) beats
software's real mean (145.7µs/op) by 3.4–9x. That gap is *wider* than on a
small curated query set, because real traffic is dominated by non-matches,
which are cheap for the DPA's fixed-cost scan but expensive for software's
fallback-chasing dispatch. The DPA does trade real accuracy for this speed —
87.5% raw agreement with the real classifier, but only 38.7% recall,
because much of software's real matching behavior turns out to be loose
statistical similarity (bm25/ngram), not literal substring presence. Before
optimization, the naive DPA implementation was ~470x *slower* than software,
not faster — the speedup took real, hardware-informed engineering, not just
"hardware is inherently faster."**

## What was actually compared

Both sides evaluate the exact same 4 keyword rules from this repo's own
`config/config.yaml` (`code_keywords`, `machine_learning`, `urgent_keywords`,
`fuzzy_sensitive_keywords`).

**Corpus: 23,448 real, unique, English, non-flagged first-turn user prompts
from the [LMSYS Chatbot Arena Conversations dataset](https://huggingface.co/datasets/lmsys/chatbot_arena_conversations)**
(Zheng et al., NeurIPS 2023) — a real, large-scale, widely-cited dataset of
actual user traffic to real chatbots, not queries designed to exercise any
particular engine's behavior. See
`control-plane/software_baseline_bench/data/download_chatbot_arena.py` to
reproduce the extraction (the dataset is gated; this repo doesn't
redistribute a copy — see that script's header for why).

A smaller, 34-query hand-curated corpus (`control-plane/accuracy_study/workload.py`)
is also still benchmarked and reported below — it's useful for isolating
*why* each engine behaves the way it does (deliberately probes bm25
stemming, ngram typo tolerance, fuzzy edit-distance, case sensitivity one
mechanism at a time), which the large real corpus can't cleanly isolate. The
large corpus is the number that matters for "does this generalize"; the
small one is the number that explains the mechanism behind it.

| Side | What's actually running | Where |
|---|---|---|
| Software | The real, unmodified `KeywordClassifier.Classify()` — `src/semantic-router/pkg/classification`, imported live via `go.mod replace`, not reimplemented | `control-plane/software_baseline_bench/` |
| Hardware | The DPA kernel's case-folded exact-substring matcher, on the lab's real BlueField-3 DPA | `data-plane/dpa_decision_fastpath/` |

## Results — large real corpus (23,448 Chatbot Arena prompts)

| Configuration | Mean | Median | Throughput |
|---|---|---|---|
| **Software** (real router code, single-core) | 145,701 ns/op | — | ~6,864 ops/sec (1/mean) |
| **DPA, single-request-per-launch** (event reused) | 1,323,051 ns | 1,260,743 ns | 755.7 launches/sec |
| **DPA, batched** (128 requests/launch — the real hardware max, see below) | 42,617 ns/request (amortized) | ~15,400 ns/request (p50 launch ÷ 128) | **23,335.6 requests/sec (wall-clock, includes a real p99 tail — see caveats)** |

Raw output: `control-plane/software_baseline_bench/real_run_output.txt`,
`data-plane/dpa_decision_fastpath/bench_large_corpus_output.txt`.

**Why software gets *slower*, not faster, on realistic traffic:** on the
34-query set, software averaged 42µs because that set was weighted toward
queries that hit a cheap rule (bm25/ngram) on the *first* check.
Real traffic is different: **80.4% of real Chatbot Arena prompts match none
of the 4 rules at all** (measured directly —
`control-plane/software_baseline_bench/matchrate/`). A non-match still has
to fail the bm25 check, fail the ngram check, and run the expensive
per-word Levenshtein fuzzy fallback before giving up — so most real queries
pay software's *worst* case, not its best. The DPA kernel has no such
asymmetry: it scans all 4 rules unconditionally regardless of outcome, so
its cost is close to flat whether a query matches or not. That's the real
mechanism behind the gap widening on realistic data, not an artifact of the
new corpus being "easier" for the DPA.

## Real hardware limit found while building this: 128 threads/kernel launch

Probing batch sizes on real hardware (128 → works, 192/256/512/1024 → all
fail with `Exceeded valid max number of threads per kernel`, narrowed by
binary search to exactly 128 as the ceiling) surfaced a genuine BlueField-3
DPA hardware constraint, not documented anywhere obvious beforehand: **a
single DPA kernel launch on this hardware cannot exceed 128 threads.** The
batched benchmark above chunks the 23,448-query corpus into 184
128-request launches for exactly this reason.

## Results — small hand-curated corpus (34 queries, for mechanism analysis)

| Configuration | Mean latency | Throughput | Notes |
|---|---|---|---|
| **Software** (real router code, single-core) | 42,025 ns/op | ~23,800 ops/sec | 980 B/op, 22 allocs/op. Per-query range: 3.7µs (bm25 hit) to 225µs (fuzzy-fallback miss). |
| **DPA, naive** (fresh sync event per launch) | 19,697,070 ns | 50.8 launches/sec | First working version. ~470x slower than software. |
| **DPA, event reused** | 1,247,063 ns | 801.8 launches/sec | 15.8x faster than naive, from one fix. |
| **DPA, batched** (34 requests/launch, event reused) | 13,745–18,745 ns/request (amortized) | 72,747–53,343 requests/sec | Faster than software's mean on this smaller set too. |

Raw output: `data-plane/dpa_decision_fastpath/bench_real_run_output.txt`.

## Accuracy on the same large real corpus

`control-plane/accuracy_study/large_corpus_accuracy.py` extends the
original 34-query accuracy study to all 23,448 real queries, using the real
software classifier's actual output as ground truth:

- **87.50% raw agreement** (20,518/23,448) between the DPA-realistic
  (case-folded) algorithm and the real software classifier.
- Of that: **80.19%** is both sides correctly finding no match; only
  **7.31%** is both sides agreeing on an actual matched rule.
- **DPA recall is only 38.7%** (binary any-match framing) — the DPA missed
  2,816 real software matches entirely. Precision is high (97.0%): when the
  DPA does claim a match, it's almost always right; it just misses a lot.
- **Root cause, inspected directly in the mismatches:** many of software's
  real bm25/ngram matches are **not literal substring hits** — e.g. "Why did
  my parent not invite me to their wedding?" matches `urgent_keywords` via
  ngram character-similarity scoring (no literal "urgent"/"immediate"/
  "asap"/"emergency" substring anywhere in that sentence), and "How to train
  concentration and memory" matches `machine_learning` via bm25's token
  scoring on "train". The DPA's exact-substring approach cannot replicate
  this by construction — it's not a tuning gap, it's a structural
  difference between exact matching and statistical similarity matching.
  Whether software's own matches here are themselves desirable (a human
  reviewer might call some of these false positives too) is a separate,
  interesting question this comparison doesn't resolve — it measures
  agreement with the real system as shipped, not with human judgment.

Raw output: `control-plane/accuracy_study/` run output (regenerate with
`python3 large_corpus_accuracy.py` after generating the ground truth file
per that script's header).

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
   per request, via `doca_dpa_dev_thread_rank()`, capped at the real
   128-thread hardware limit above) amortizes that dispatch cost across the
   batch — this is the single lever that took the DPA path from "still
   30x slower" to "faster than software."

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
- **Not the same algorithm.** See the accuracy section above — 87.5% raw
  agreement, 38.7% recall, precision 97.0%. The speed numbers have to be
  read together with this, not instead of it.
- **Coverage ceiling.** Only 2 of 23 decisions in this repo's own reference
  config are keyword-only at all — the DPA fast path (in its current,
  keyword-only scope) is only ever eligible for a minority of real traffic
  regardless of how fast it is.
- **A real, large p99 tail in the batched-launch numbers.** At the
  184-batch scale, batched-launch p99 hit 100.9ms — two orders of magnitude
  above the mean. The wall-clock throughput figure above already reflects
  this (it isn't hidden), but it's not root-caused yet; a production SLA
  claim would need to understand this tail, not just cite the mean.
- **Single-node, single-request-stream measurement.** Neither side was
  tested under concurrent multi-client load, connection/queueing overhead,
  or realistic production traffic shape.
- **GPU path (Experiment 2)** has no numbers in this comparison at all —
  blocked on a host-level GPUDirect RDMA registration issue, not an
  application bug (see that experiment's doc). **Flow-table steering
  (Experiment 3)** also has no performance numbers — only functional
  correctness (rules install and hold) was verified, latency/throughput of
  hardware flow lookup vs. a software equivalent was never measured.

## What would make this a complete, production-credible comparison

1. Root-cause the batched-launch p99 tail.
2. Wire the DPA path to a real RDMA-triggered arrival path (BF3 receiving
   actual request bytes over the wire) instead of a host-driven benchmark
   loop, and re-measure — the numbers above are DPA-launch cost only, not
   full network-to-result latency.
3. Unblock and benchmark the GPU path the same way, for the (much larger)
   share of decisions that need BERT-classifier signals, not just keywords.
4. Actually measure Experiment 3's flow-table latency/throughput against a
   software tenant-lookup equivalent — currently unmeasured.
5. Report a single blended number weighted by the real fast-path hit rate
   (currently ceilinged at 2/23 decisions), not just the keyword-only-path
   number in isolation — that's the number that actually reflects
   end-to-end impact on real traffic.

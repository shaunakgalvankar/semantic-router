# Software baseline: the real router's keyword classifier, benchmarked for real

This is the software side of the routeNIC apples-to-apples comparison. It
imports and benchmarks the **actual production code**
(`src/semantic-router/pkg/classification.KeywordClassifier`) via `go.mod`
`replace` directives pointing at the real package in place — nothing here is
reimplemented or approximated. It uses the router's own 4 real keyword rules
from `config/config.yaml` against two corpora, both mirrored identically on
the DPA side:

- **`data/chatbot_arena_queries.txt`** (not committed — see `data/download_chatbot_arena.py`):
  23,448 real, unique, English, non-flagged user prompts from the
  [LMSYS Chatbot Arena Conversations dataset](https://huggingface.co/datasets/lmsys/chatbot_arena_conversations)
  (Zheng et al., NeurIPS 2023) — real traffic to real chatbots, the number
  that matters for a "does this generalize" argument. This is the primary
  benchmark; see `BenchmarkRealKeywordClassifyLargeCorpus`.
- The 34-query hand-curated corpus from `control-plane/accuracy_study/workload.py`
  (`realQueries` in `keyword_bench_test.go`) — smaller, but deliberately
  probes each engine's distinctive behavior one mechanism at a time (bm25
  stemming, ngram typo tolerance, fuzzy edit distance, case sensitivity),
  which the large real corpus can't cleanly isolate. Useful for
  understanding *why* the large-corpus numbers look the way they do, not a
  replacement for them.

See `routeNIC/docs/BENCHMARK_COMPARISON.md` for the full side-by-side
writeup against the DPA hardware numbers, on both corpora.

## A real bug this surfaced in the router's own reference config

`config/config.yaml`'s `fuzzy_sensitive_keywords` rule is declared with
`method: fuzzy`. Reading `NewKeywordClassifier`'s dispatch switch directly
(`src/semantic-router/pkg/classification/keyword_classifier.go`) confirms
only `"bm25"`, `"ngram"`, and `"regex"` (default) are recognized — `"fuzzy"`
hits the `default` case and returns
`fmt.Errorf("unsupported keyword rule method: %q ...")`. **The router's own
shipped reference config would fail to load this rule as literally
written.** The real fuzzy-match code path exists and works
(`matchesWithCount`/`hasFuzzyMatch` in `keyword_classifier_match.go`) — it's
just reached via `method: "regex"` (or omitted) plus `fuzzy_match: true` and
an integer `fuzzy_threshold` (max Levenshtein edit distance), not a
`method: "fuzzy"` value. This benchmark constructs the rule the way that
works; `keyword_bench_test.go`'s header comment and
`control-plane/accuracy_study/workload.py` both document this same finding
independently.

## Run it

```
# One-time: get the real large corpus (needs your own accepted HF access —
# see data/download_chatbot_arena.py's header)
HF_TOKEN=hf_... python3 data/download_chatbot_arena.py

export LD_LIBRARY_PATH="$(pwd)/../../../candle-binding/target/release:$(pwd)/../../../nlp-binding/target/release:$(pwd)/../../../ml-binding/target/release"
go test -bench=. -benchmem -run=^$ -cpu=1
```
(`BenchmarkRealKeywordClassifyLargeCorpus` skips cleanly if the corpus
file hasn't been generated yet, rather than failing.)
(`-cpu=1`: a single decision computation isn't parallel work in the real
router's request path, so this measures single-core latency rather than
letting Go's benchmark harness fan out across cores, which would understate
real per-request cost.)

`LD_LIBRARY_PATH` needs all three Rust `.so`s because `pkg/classification`
transitively imports classifiers (BERT domain/jailbreak/etc.) that link
against `candle-binding` and `ml-binding`, even though this benchmark only
exercises the keyword path.

## Real results, large corpus (23,448 real Chatbot Arena prompts)

**145,701 ns/op** — over 3x the small-corpus number below, and the
direction matters: real traffic is *harder* for software, not easier.
**80.42% of real queries match none of the 4 rules** (measured directly via
`matchrate/main.go`), and a non-match pays the *full* fallback chain (failed
bm25, failed ngram, then the expensive per-word Levenshtein scan) before
giving up — so most real traffic hits software's worst case, not its best.
See `routeNIC/docs/BENCHMARK_COMPARISON.md` for the full breakdown including
the DPA side and the accuracy comparison on this same corpus (87.5%
agreement, 38.7% DPA recall).

## Real results, small hand-curated corpus (last run: `real_run_output.txt`)

**Aggregate, mixed workload: 42,025 ns/op, 980 B/op, 22 allocs/op**
(single-core, `-benchtime=3s`, 165,596 iterations). Lower than the large-corpus
number above because this 34-query set is weighted toward queries that hit a
cheap rule on the first check — see the per-query breakdown below for why.

**Per-query breakdown reveals a ~60x spread** depending on which rule
actually matches — this is a real, structural property of the router's own
rule-evaluation order (`ClassifyWithKeywordsAndCount` in
`keyword_classifier_dispatch.go`), not noise:

- **Cheapest (~3.7–8µs):** queries that hit `code_keywords` or
  `machine_learning` (bm25, native Rust FFI, cached per-call) on the
  *first* rule checked — one FFI call, done.
- **Mid (~15–33µs):** queries that only hit `urgent_keywords` (ngram) — pay
  for a failed bm25 check first, then one ngram FFI call.
- **Most expensive (~40–225µs):** queries that only hit
  `fuzzy_sensitive_keywords`, or match nothing at all — pay for a failed
  bm25 check, a failed ngram check, *and* the regex engine's per-word
  Levenshtein fuzzy-match fallback (`extractLowerWords` + edit-distance
  scan against every word in the text). The single worst case
  ("Emergency: the password reset endpoint...", a longer prompt that
  matches nothing) hit **224,824 ns** — the words-times-keywords
  Levenshtein scan cost compounds with input length.

This matters directly for the DPA comparison: the DPA kernel's exact-substring
matcher has no equivalent to this rule-order-dependent cost structure — every
query costs roughly the same on the DPA (a fixed nested-loop scan over a fixed
rule/keyword table). So the real speedup story isn't a single number; it's
"how much faster is the DPA path *specifically for the expensive,
fuzzy-triggering tail* that the software engine handles worst."

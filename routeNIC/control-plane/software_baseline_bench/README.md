# Software baseline: the real router's keyword classifier, benchmarked for real

This is the software side of the routeNIC apples-to-apples comparison. It
imports and benchmarks the **actual production code**
(`src/semantic-router/pkg/classification.KeywordClassifier`) via `go.mod`
`replace` directives pointing at the real package in place — nothing here is
reimplemented or approximated. It uses the router's own 4 real keyword rules
from `config/config.yaml` and the identical 34-query corpus
`control-plane/accuracy_study/workload.py` already uses, so this benchmark
and the DPA hardware numbers are measuring the same inputs.

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
export LD_LIBRARY_PATH="$(pwd)/../../../candle-binding/target/release:$(pwd)/../../../nlp-binding/target/release:$(pwd)/../../../ml-binding/target/release"
go test -bench=. -benchmem -run=^$ -cpu=1
```
(`-cpu=1`: a single decision computation isn't parallel work in the real
router's request path, so this measures single-core latency rather than
letting Go's benchmark harness fan out across cores, which would understate
real per-request cost.)

`LD_LIBRARY_PATH` needs all three Rust `.so`s because `pkg/classification`
transitively imports classifiers (BERT domain/jailbreak/etc.) that link
against `candle-binding` and `ml-binding`, even though this benchmark only
exercises the keyword path.

## Real results (last run: `real_run_output.txt` in this directory)

**Aggregate, mixed workload: 42,025 ns/op, 980 B/op, 22 allocs/op**
(single-core, `-benchtime=3s`, 165,596 iterations).

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

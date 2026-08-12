# Experiment 1 (control side): DPA-adaptation accuracy/tolerance study

No hardware required — this runs anywhere with Python 3.10+ and no
dependencies outside the standard library.

```bash
python3 run_study.py
```

## What this measures

The router's real keyword-signal engines (`bm25`, `ngram`, per-word
Levenshtein fuzzy matching, plain substring — see `software_baseline.py`'s
docstring for exact semantics, verified against the Go/Rust source, not
assumed) are not things a BlueField-3 DPA core can run: they need
floating-point corpus statistics, character n-gram set operations, or
per-word edit-distance computation, none of which fit a constrained
data-path accelerator's execution model. `dpa_adapted.py` replaces all four
with a single mechanism that does fit — Aho-Corasick multi-pattern exact
substring matching, the DPA kernel in
`data-plane/dpa_decision_fastpath/` actually runs.

This study quantifies exactly what that substitution costs, using the
software engines as ground truth, over a 34-query workload hand-curated to
exercise each engine's distinctive behavior (see `workload.py`).

## Results (this run, `results.json` has the full data)

| Case mode | Decision accuracy | Precision | Recall | F1 |
| --- | --- | --- | --- | --- |
| `strict` (per-rule case handling, matches software exactly) | 0.853 | **1.000** | 0.750 | 0.857 |
| `folded` (single global lowercase automaton — the DPA-realistic simplification) | 0.765 | 0.833 | 0.750 | 0.789 |

**Strict mode is precision-perfect and recall-limited.** Every decision
mismatch in strict mode is a false negative — the DPA fast path never
claims a match the software engines wouldn't have made; it only misses
fuzzy/stemmed variants that a soft-matching engine catches and exact
substring can't:
  - `ngram`'s character-overlap tolerance for typos (`urgnet`, `emergancy`,
    `asp`, `immediat`) — 4 of 5 strict-mode misses.
  - `bm25`'s token-stemming (`"the model was trained"` scoring against the
    keyword `"model training"` via the stem `train`) — 1 of 5 misses.

That's a specific, explainable, and — for a system whose failure mode is
"send an easy request to the GPU anyway" rather than "wrongly skip the GPU
for something that needed it" — a fairly benign shape of accuracy loss: the
fast path is conservative, not overconfident.

**Folded mode adds a second, distinct failure mode.** The
`case_sensitive_secrets` fixture rule (case_sensitive=True; not from the
real config — see `workload.py`'s docstring for why it was added) drops
from perfect accuracy to 0.912 specifically because folding case handling
into one global automaton makes `aws_secret`/`api_key`/`private_key`
(lowercase) match a rule that should only fire on the uppercase forms.
Three new false positives, decision-level precision drops from 1.000 to
0.833. This is the "tolerable accuracy loss from a specific hardware
simplification" the project asked this study to measure and isolate: it's
attributable to exactly one implementation choice (one automaton vs. two),
not to the exact-substring-vs-fuzzy substitution itself, and it's the
concrete number that should inform whether the fast-path DPA kernel is
worth building with `strict` two-automaton case handling (more DPA program
complexity, cheaper accuracy) or `folded` single-automaton case handling
(simpler DPA program, real security-relevant false-positive risk if any
DPA-fast-pathed decision is case-sensitivity-dependent).

## Coverage: how much real traffic could this even apply to

Separately from matching accuracy, `dpa_decision_fastpath`'s whole premise
only helps for decisions whose *entire* rule tree is keyword-only — any
leaf that's a BERT-classifier signal (`domain`, `pii`, `jailbreak`, ...)
still has to reach the GPU regardless of what the DPA does. Parsing this
repo's own `config/config.yaml` directly:

```
23 decisions total
 2 are keyword-only (DPA-fast-path-eligible as currently scoped)
21 reference at least one non-keyword (GPU-classifier-only) signal
```

**8.7% coverage in this specific reference config.** That's a real,
config-grounded number, reported as-is rather than a rosier estimate. It's
also a fairly direct argument for `cascade_classifier`'s scope
(`data-plane/cascade_classifier/`): the DPA-tractable signal set used here
is deliberately narrow (keyword only); folding in the other cheap,
non-learned signal types the router already runs on CPU today — `structure`,
`context`, `authz` (3 more of the 15 leaf types actually used in this
config) — would raise this coverage number substantially without touching
anything BERT-based, and is the natural next measurement once this
keyword-only baseline is validated on real hardware.

## Known simplifications in the software baseline

`software_baseline.py`'s bm25 tokenizer approximates the real Rust
implementation's English Snowball stemmer with a small fixed suffix list
rather than porting Snowball exactly. This doesn't change what the study
measures (the DPA-adaptation cost), but exact bm25 scores here may differ
in their third decimal from the router's own; the threshold-gated
match/no-match decisions used everywhere in this study are far less
sensitive to that than the raw scores would be.

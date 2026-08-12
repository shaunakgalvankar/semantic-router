"""Ruleset and query corpus for the accuracy study.

The rules are taken directly from this repo's own reference config
(config/config.yaml), not invented, with one documented exception and one
documented addition:

  - `fuzzy_sensitive_keywords` in config.yaml is declared with
    `method: fuzzy` and `fuzzy_threshold: 82`. The keyword-matching research
    for this study found that `"fuzzy"` is not a recognized `method` value
    in the router's dispatch code (src/semantic-router/pkg/classification/
    keyword_classifier.go) — real fuzzy behavior is a `fuzzy_match: true`
    boolean layered on the *default* engine, gated by an integer max
    edit-distance (`fuzzy_threshold`), not a 0-100 score. Taken literally,
    this rule would fail to load in the real router. This study reinterprets
    it as what it evidently intended — default engine + fuzzy_match overlay
    with a realistic edit-distance threshold — because fuzzy matching is a
    real, distinct mechanism worth measuring even though this specific
    example is misconfigured upstream. That upstream inconsistency is a
    genuine finding, reported as-is, not silently fixed in this study.

  - `case_sensitive_secrets` does not exist in config.yaml. It's added here
    specifically to exercise the case-mode ablation (strict vs. folded) in
    dpa_adapted.py, because every real keyword rule in config.yaml has
    case_sensitive: false — without at least one case-sensitive rule, that
    ablation would trivially show zero difference and prove nothing.
"""

from __future__ import annotations

from decision_model import Decision, DecisionGroup, KeywordLeaf, KeywordRule

RULES: list[KeywordRule] = [
    KeywordRule(
        name="code_keywords",
        operator="OR",
        keywords=["code", "function", "debug", "algorithm", "refactor"],
        case_sensitive=False,
        method="bm25",
        bm25_threshold=0.1,
    ),
    KeywordRule(
        name="machine_learning",
        operator="OR",
        keywords=["machine learning", "model training", "gradient", "neural network", "classifier"],
        case_sensitive=False,
        method="bm25",
        bm25_threshold=0.1,
    ),
    KeywordRule(
        name="urgent_keywords",
        operator="OR",
        keywords=["urgent", "immediate", "asap", "emergency"],
        case_sensitive=False,
        method="ngram",
        ngram_threshold=0.4,
        ngram_arity=3,
    ),
    KeywordRule(
        name="fuzzy_sensitive_keywords",
        operator="OR",
        keywords=["social security", "credit card", "password", "api key"],
        case_sensitive=False,
        method="",
        fuzzy_match=True,
        fuzzy_threshold=2,
    ),
    # Added fixture — see module docstring.
    KeywordRule(
        name="case_sensitive_secrets",
        operator="OR",
        keywords=["AWS_SECRET", "API_KEY", "PRIVATE_KEY"],
        case_sensitive=True,
        method="",
    ),
]

# A decision purely over keyword-type signals (the DPA-tractable subset).
# Structurally similar to real nested decisions in config.yaml (e.g.
# safe_hybrid_route), but keyword-only since domain/jailbreak/pii signals
# are BERT-classifier-based and out of scope for a DPA fast path.
FAST_PATH_DECISION = Decision(
    name="dpa_fastpath_candidate",
    root=DecisionGroup(
        operator="OR",
        children=[
            KeywordLeaf("code_keywords"),
            KeywordLeaf("machine_learning"),
            DecisionGroup(
                operator="AND",
                children=[
                    KeywordLeaf("urgent_keywords"),
                    DecisionGroup(operator="NOT", children=[KeywordLeaf("fuzzy_sensitive_keywords")]),
                ],
            ),
            KeywordLeaf("case_sensitive_secrets"),
        ],
    ),
)

# Hand-curated to exercise each engine's distinctive behavior, not random
# text — see the comment on each block for what it's meant to probe.
QUERIES: list[str] = [
    # --- exact hits: every engine should agree ---
    "please review this code for bugs",
    "can you debug this function for me",
    "explain gradient descent in machine learning",
    "this is urgent, please respond immediately",
    "what is my credit card balance",
    "AWS_SECRET is exposed in the log file",
    # --- true negatives: every engine should agree ---
    "what's the weather like today",
    "tell me a joke about cats",
    "how do I bake sourdough bread",
    "recommend a good science fiction novel",
    # --- bm25 stemming cases: exact-substring may miss, bm25 should hit ---
    "I am debugging the algorithm right now",
    "we are refactoring the codebase this week",
    "the model was trained on a large dataset",
    "our classifiers need retraining",
    # --- bm25 multi-token accumulation: no single token is the keyword ---
    "gradient updates during training are unstable",
    # --- ngram typo/variant cases: exact-substring should miss, ngram catches nearby spellings ---
    "this is urgnet, please help",
    "we have an emergancy on the production system",
    "asp, need this done now",
    "immediat action required",
    # --- fuzzy typo cases: exact-substring misses, Levenshtein overlay catches ---
    "I forgot my passwrod again",
    "please don't share your cerdit card number",
    "my social securty number was stolen",
    "the api ky was rotated yesterday",
    # --- case-sensitivity probe: exact case vs wrong case ---
    "AWS_SECRET must never be logged",
    "aws_secret must never be logged",
    "API_KEY rotation policy",
    "api_key rotation policy",
    "PRIVATE_KEY leaked in the repo",
    "private_key leaked in the repo",
    # --- combined AND-rule probe (urgent AND NOT fuzzy_sensitive) ---
    "urgent: server is down, respond immediately",
    "urgent: please update my credit card on file",  # urgent AND fuzzy_sensitive -> should NOT fast-path match
    # --- longer, more realistic mixed prose ---
    "Hi team, this is urgent — the neural network training job crashed and "
    "I need someone to debug the gradient computation asap before the demo.",
    "Not urgent, just wondering if anyone has refactored the classifier code "
    "recently, no rush at all.",
    "Emergency: the password reset endpoint is leaking session tokens, "
    "please look at this immediately.",
]

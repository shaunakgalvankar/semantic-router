#!/usr/bin/env python3
"""Runs the DPA-adaptation accuracy/tolerance study end to end.

For each query in the workload, evaluates every keyword rule with both the
software baseline (the router's real bm25/ngram/fuzzy/default engines) and
the DPA-adapted matcher (Aho-Corasick, in both "strict" and "folded" case
modes), then evaluates the same decision tree over both sets of per-rule
results. The software baseline is treated as ground truth — this study is
not asking "which is more correct," it's asking "how much does the DPA
adaptation cost relative to what the router actually does today."

Usage:
    python3 run_study.py [--out results.json]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from decision_model import DecisionOutcome, evaluate_decision_tree
from dpa_adapted import DpaMatcher
import software_baseline
from workload import FAST_PATH_DECISION, QUERIES, RULES


def _confusion(baseline: list[bool], adapted: list[bool]) -> dict[str, int]:
    tp = sum(1 for b, a in zip(baseline, adapted) if b and a)
    fp = sum(1 for b, a in zip(baseline, adapted) if not b and a)
    fn = sum(1 for b, a in zip(baseline, adapted) if b and not a)
    tn = sum(1 for b, a in zip(baseline, adapted) if not b and not a)
    return {"tp": tp, "fp": fp, "fn": fn, "tn": tn}


def _prf1(c: dict[str, int]) -> dict[str, float]:
    precision = c["tp"] / (c["tp"] + c["fp"]) if (c["tp"] + c["fp"]) else 1.0
    recall = c["tp"] / (c["tp"] + c["fn"]) if (c["tp"] + c["fn"]) else 1.0
    f1 = 2 * precision * recall / (precision + recall) if (precision + recall) else 0.0
    accuracy = (c["tp"] + c["tn"]) / sum(c.values()) if sum(c.values()) else 1.0
    return {"precision": precision, "recall": recall, "f1": f1, "accuracy": accuracy}


def run(case_mode: str) -> dict:
    matcher = DpaMatcher(case_mode=case_mode, rules=RULES)  # type: ignore[arg-type]

    per_rule_results: dict[str, dict[str, list[bool]]] = {
        r.name: {"baseline": [], "adapted": []} for r in RULES
    }
    decision_baseline: list[bool] = []
    decision_adapted: list[bool] = []

    for text in QUERIES:
        baseline_results = {r.name: software_baseline.evaluate_rule(r, text) for r in RULES}
        adapted_results = {r.name: matcher.evaluate_rule(r, text) for r in RULES}

        for r in RULES:
            per_rule_results[r.name]["baseline"].append(baseline_results[r.name].matched)
            per_rule_results[r.name]["adapted"].append(adapted_results[r.name].matched)

        base_outcome = evaluate_decision_tree(FAST_PATH_DECISION.root, baseline_results)
        adapted_outcome = evaluate_decision_tree(FAST_PATH_DECISION.root, adapted_results)
        decision_baseline.append(base_outcome.matched)
        decision_adapted.append(adapted_outcome.matched)

    per_rule_metrics = {}
    for name, results in per_rule_results.items():
        c = _confusion(results["baseline"], results["adapted"])
        per_rule_metrics[name] = {**c, **_prf1(c)}

    decision_confusion = _confusion(decision_baseline, decision_adapted)
    decision_metrics = {**decision_confusion, **_prf1(decision_confusion)}

    mismatched_queries = [
        {"query": q, "baseline": b, "adapted": a}
        for q, b, a in zip(QUERIES, decision_baseline, decision_adapted)
        if b != a
    ]

    return {
        "case_mode": case_mode,
        "num_queries": len(QUERIES),
        "per_rule": per_rule_metrics,
        "decision": decision_metrics,
        "decision_mismatches": mismatched_queries,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path(__file__).parent / "results.json")
    args = parser.parse_args()

    results = {mode: run(mode) for mode in ("strict", "folded")}

    print(f"routeNIC accuracy study — {len(QUERIES)} queries, {len(RULES)} keyword rules\n")
    for mode, r in results.items():
        d = r["decision"]
        print(f"[{mode} case mode] decision-level agreement with software baseline:")
        print(
            f"  accuracy={d['accuracy']:.3f}  precision={d['precision']:.3f}  "
            f"recall={d['recall']:.3f}  f1={d['f1']:.3f}  "
            f"(tp={d['tp']} fp={d['fp']} fn={d['fn']} tn={d['tn']})"
        )
        print("  per-rule accuracy:")
        for name, m in r["per_rule"].items():
            print(f"    {name:28s} accuracy={m['accuracy']:.3f}  f1={m['f1']:.3f}")
        if r["decision_mismatches"]:
            print("  decision-level mismatches vs. software baseline:")
            for m in r["decision_mismatches"]:
                print(f"    baseline={m['baseline']!s:5} adapted={m['adapted']!s:5}  {m['query'][:70]!r}")
        print()

    args.out.write_text(json.dumps(results, indent=2))
    print(f"Full results written to {args.out}")


if __name__ == "__main__":
    main()

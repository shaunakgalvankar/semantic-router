"""Shared types for the DPA-adapted keyword/decision accuracy study.

These mirror the real router's config and decision-engine shapes closely
enough to be a faithful comparison target:
  - KeywordRule fields match src/semantic-router/pkg/config/signal_config.go
    (KeywordRule: Operator, Keywords, CaseSensitive, Method, FuzzyMatch,
    FuzzyThreshold, BM25Threshold, NgramThreshold, NgramArity).
  - DecisionRule mirrors src/semantic-router/pkg/decision/engine.go's
    evalAND/evalOR/evalNOT: AND requires every child to match and reports
    the average child confidence; OR requires any child to match and
    reports the best matching child's confidence; NOT requires exactly one
    child and inverts its match state.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Literal, Sequence, Union

KeywordOperator = Literal["OR", "AND", "NOR"]
KeywordMethod = Literal["bm25", "ngram", "regex", ""]


@dataclass(frozen=True)
class KeywordRule:
    name: str
    operator: KeywordOperator
    keywords: Sequence[str]
    case_sensitive: bool = False
    method: KeywordMethod = ""
    fuzzy_match: bool = False
    fuzzy_threshold: int = 2  # integer max Levenshtein distance, NOT a 0-100 score
    bm25_threshold: float = 0.1
    ngram_threshold: float = 0.4
    ngram_arity: int = 3  # character n-gram size; router clamps this to >= 2


@dataclass(frozen=True)
class KeywordLeaf:
    rule_name: str


@dataclass(frozen=True)
class DecisionGroup:
    operator: Literal["AND", "OR", "NOT"]
    children: Sequence["DecisionNode"]


DecisionNode = Union[KeywordLeaf, DecisionGroup]


@dataclass(frozen=True)
class Decision:
    name: str
    root: DecisionNode


@dataclass
class MatchResult:
    matched: bool
    confidence: float


@dataclass
class DecisionOutcome:
    decision_name: str
    matched: bool
    confidence: float


def evaluate_decision_tree(
    node: DecisionNode,
    rule_results: dict[str, MatchResult],
) -> MatchResult:
    """Evaluates a decision's rule tree given already-computed per-keyword-rule
    match results. Semantics mirror evalAND/evalOR/evalNOT in the router's
    Go decision engine exactly (see src/semantic-router/pkg/decision/engine.go),
    so this function is identical regardless of which matcher (software
    baseline or DPA-adapted) produced `rule_results` — only the matcher
    changes between the two arms of this study, never the combination logic.
    """
    if isinstance(node, KeywordLeaf):
        return rule_results[node.rule_name]

    children_results = [evaluate_decision_tree(child, rule_results) for child in node.children]

    if node.operator == "AND":
        if not children_results:
            return MatchResult(True, 0.0)
        if all(r.matched for r in children_results):
            avg = sum(r.confidence for r in children_results) / len(children_results)
            return MatchResult(True, avg)
        return MatchResult(False, 0.0)

    if node.operator == "OR":
        matched = [r for r in children_results if r.matched]
        if matched:
            best = max(matched, key=lambda r: r.confidence)
            return MatchResult(True, best.confidence)
        return MatchResult(False, 0.0)

    if node.operator == "NOT":
        if len(children_results) != 1:
            return MatchResult(False, 0.0)
        child = children_results[0]
        if not child.matched:
            return MatchResult(True, 1.0)
        return MatchResult(False, child.confidence)

    raise ValueError(f"unknown operator {node.operator!r}")

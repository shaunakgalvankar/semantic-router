"""The DPA-suited replacement for the software keyword engines.

This BlueField-3 does not have the hardware RegEx accelerator block, and
even if it did, a DPA core's programming model (small, fixed memory, no
backtracking, no floating point corpus statistics) cannot host BM25 scoring,
character n-gram Jaccard similarity, or per-word Levenshtein distance the
way the software engines in `software_baseline.py` do. What a constrained
data-path core genuinely can do well is a compiled multi-pattern exact
substring automaton: single pass over the text, O(1) work per byte, fixed
compact memory, no backtracking. That's Aho-Corasick, and it's what this
module implements — as a stand-in for *all four* software methods
(bm25/ngram/fuzzy/default), not just the plain substring one, because on the
DPA there is no cheaper way to ask "is this pattern present" than to ask it
exactly. The accuracy study's job is to quantify what that substitution
costs.

Two case-handling modes are provided as an explicit ablation:
  - "strict":  one automaton per case mode, mirroring the software engine's
               per-rule case_sensitive flag exactly (case-sensitive patterns
               scanned against untouched text, case-insensitive patterns
               against lowercased text). This is the accuracy-preserving
               choice and the more expensive one to implement in a
               constrained kernel (two automatons, two passes).
  - "folded":  a single automaton, all patterns lowercased, one pass over
               lowercased text — the simpler, more DPA-realistic choice,
               and the one expected to cost accuracy specifically on any
               rule that has case_sensitive=True.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from typing import Literal

from decision_model import KeywordRule, MatchResult

CaseMode = Literal["strict", "folded"]


@dataclass
class _ACNode:
    children: dict[str, int] = field(default_factory=dict)
    fail: int = 0
    output: list[int] = field(default_factory=list)  # pattern ids ending here


class AhoCorasick:
    """Minimal, dependency-free Aho-Corasick automaton: build once from a
    fixed pattern set, then scan any text in a single pass reporting which
    pattern ids occur as substrings. This is the algorithm this experiment
    claims is DPA-tractable — it needs nothing but a byte-indexed table walk
    and integer transitions, which is exactly what fits a constrained
    data-path core's instruction set.
    """

    def __init__(self, patterns: list[str]):
        self.patterns = patterns
        self._nodes: list[_ACNode] = [_ACNode()]
        for pid, pattern in enumerate(patterns):
            self._insert(pattern, pid)
        self._build_fail_links()

    def _insert(self, pattern: str, pid: int) -> None:
        node = 0
        for ch in pattern:
            if ch not in self._nodes[node].children:
                self._nodes.append(_ACNode())
                self._nodes[node].children[ch] = len(self._nodes) - 1
            node = self._nodes[node].children[ch]
        self._nodes[node].output.append(pid)

    def _build_fail_links(self) -> None:
        root = 0
        queue: deque[int] = deque()
        for ch, child in self._nodes[root].children.items():
            self._nodes[child].fail = root
            queue.append(child)

        while queue:
            node = queue.popleft()
            for ch, child in self._nodes[node].children.items():
                queue.append(child)
                fail = self._nodes[node].fail
                while fail != root and ch not in self._nodes[fail].children:
                    fail = self._nodes[fail].fail
                self._nodes[child].fail = self._nodes[fail].children.get(ch, root) if ch in self._nodes[fail].children else root
                self._nodes[child].output += self._nodes[self._nodes[child].fail].output

    def scan(self, text: str) -> set[int]:
        """Returns the set of pattern ids that occur anywhere in `text`."""
        node = 0
        found: set[int] = set()
        for ch in text:
            while node != 0 and ch not in self._nodes[node].children:
                node = self._nodes[node].fail
            node = self._nodes[node].children.get(ch, 0)
            if self._nodes[node].output:
                found.update(self._nodes[node].output)
        return found


@dataclass
class DpaMatcher:
    """Compiles a fixed rule set into one or two Aho-Corasick automatons
    (depending on `case_mode`) and answers per-rule OR/AND/NOR queries
    against arrivals, exactly the shape the fast-path DPA kernel in
    data-plane/dpa_decision_fastpath/ implements in C.
    """

    case_mode: CaseMode
    rules: list[KeywordRule]
    _pattern_owner: list[tuple[str, int]] = field(default_factory=list)  # (rule_name, keyword_index)
    _cs_automaton: AhoCorasick | None = None
    _ci_automaton: AhoCorasick | None = None
    _rule_keyword_pattern_ids: dict[str, list[int]] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if self.case_mode == "folded":
            self._build_folded()
        else:
            self._build_strict()

    def _build_folded(self) -> None:
        patterns: list[str] = []
        rule_keyword_ids: dict[str, list[int]] = {}
        for rule in self.rules:
            ids = []
            for kw in rule.keywords:
                ids.append(len(patterns))
                patterns.append(kw.lower())
            rule_keyword_ids[rule.name] = ids
        self._ci_automaton = AhoCorasick(patterns)
        self._rule_keyword_pattern_ids = rule_keyword_ids

    def _build_strict(self) -> None:
        cs_patterns: list[str] = []
        ci_patterns: list[str] = []
        rule_keyword_ids: dict[str, list[tuple[bool, int]]] = {}
        for rule in self.rules:
            ids = []
            for kw in rule.keywords:
                if rule.case_sensitive:
                    ids.append((True, len(cs_patterns)))
                    cs_patterns.append(kw)
                else:
                    ids.append((False, len(ci_patterns)))
                    ci_patterns.append(kw.lower())
            rule_keyword_ids[rule.name] = ids
        self._cs_automaton = AhoCorasick(cs_patterns)
        self._ci_automaton = AhoCorasick(ci_patterns)
        self._rule_keyword_pattern_ids = rule_keyword_ids  # type: ignore[assignment]

    def evaluate_rule(self, rule: KeywordRule, text: str) -> MatchResult:
        if self.case_mode == "folded":
            found = self._ci_automaton.scan(text.lower())  # type: ignore[union-attr]
            ids = self._rule_keyword_pattern_ids[rule.name]
            hits = [pid in found for pid in ids]
        else:
            cs_found = self._cs_automaton.scan(text)  # type: ignore[union-attr]
            ci_found = self._ci_automaton.scan(text.lower())  # type: ignore[union-attr]
            entries = self._rule_keyword_pattern_ids[rule.name]
            hits = []
            for is_cs, pid in entries:  # type: ignore[misc]
                hits.append(pid in (cs_found if is_cs else ci_found))

        if not hits:
            return MatchResult(False, 0.0)
        if rule.operator == "OR":
            matched = any(hits)
        elif rule.operator == "AND":
            matched = all(hits)
        elif rule.operator == "NOR":
            matched = not any(hits)
        else:
            raise ValueError(f"unknown keyword operator {rule.operator!r}")
        return MatchResult(matched, 1.0 if matched else 0.0)

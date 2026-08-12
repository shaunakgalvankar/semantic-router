"""Faithful reimplementation of the router's keyword-matching engines.

Grounded directly in the real Go/Rust implementation, not assumed:
  - bm25:   nlp-binding/src/bm25_classifier.rs — each rule's keyword list is
            a mini corpus (one keyword = one "document"); the input text is
            the query; standard Okapi BM25, k1=1.2, b=0.75; IDF uses the
            "+1" floor variant: idf(t) = ln(1 + (N - df(t) + 0.5)/(df(t)+0.5)).
            Gate: per-keyword score >= rule.bm25_threshold (default 0.1).
  - ngram:  nlp-binding/src/ngram_classifier.rs — CHARACTER n-grams of size
            `ngram_arity` (clamped >= 2), matched per text-word plus a
            whole-text fallback pass; similarity is
            (allgrams**2 - diffgrams**2) / allgrams**2 with
            diffgrams = allgrams - samegrams. Gate: similarity >=
            rule.ngram_threshold (default 0.4).
  - fuzzy:  NOT a `method` value — src/semantic-router/pkg/classification/
            keyword_classifier_match.go. An overlay on the default engine
            only: a keyword also counts as matched if `fuzzy_match` is set
            and some text word is within Levenshtein distance
            `fuzzy_threshold` (an integer edit-distance count, default 2 —
            NOT a 0-100 similarity score) of the keyword.
  - default: keyword_classifier_regex.go — literal substring, word-boundary
            wrapped only when the keyword has a word character AND no Han
            character (Go RE2's \\b is ASCII-only and breaks on CJK, so CJK
            keywords fall back to raw substring); case sensitivity picks
            between precompiled case-sensitive/insensitive variants.

One deliberate, documented approximation: bm25's real tokenizer runs an
English Snowball stemmer over query/document tokens regardless of language.
This reimplementation lowercases and strips a small set of common English
suffixes as a stand-in stemmer rather than porting Snowball verbatim — exact
stemmer fidelity doesn't change which *hardware-adaptation* effects this
study is measuring, and is called out here rather than silently assumed.
"""

from __future__ import annotations

import math
import re
import unicodedata
from collections import Counter

from decision_model import KeywordRule, MatchResult

_ENGLISH_STOPWORDS = {
    "a", "an", "the", "is", "are", "was", "were", "be", "been", "to", "of",
    "and", "or", "in", "on", "at", "for", "with", "this", "that", "it",
}
_SUFFIXES = ("ing", "edly", "ed", "es", "s", "ly")


def _approx_stem(token: str) -> str:
    for suffix in _SUFFIXES:
        if len(token) > len(suffix) + 2 and token.endswith(suffix):
            return token[: -len(suffix)]
    return token


def _bm25_tokenize(text: str) -> list[str]:
    words = re.findall(r"\w+", text.lower(), flags=re.UNICODE)
    return [_approx_stem(w) for w in words if w not in _ENGLISH_STOPWORDS]


def _bm25_score(rule: KeywordRule, keyword: str, text: str) -> float:
    """Score `keyword` (as a one-keyword "document") against `text` (the
    "query"), corpus-of-N = the rule's full keyword list, per
    bm25_classifier.rs.
    """
    corpus_docs = [_bm25_tokenize(k) for k in rule.keywords]
    n_docs = len(corpus_docs)
    if n_docs == 0:
        return 0.0
    avgdl = sum(len(d) for d in corpus_docs) / n_docs

    doc_tokens = _bm25_tokenize(keyword)
    doc_len = len(doc_tokens)
    doc_tf = Counter(doc_tokens)

    query_tokens = _bm25_tokenize(text)
    k1, b = 1.2, 0.75

    score = 0.0
    for token in set(query_tokens):
        df = sum(1 for d in corpus_docs if token in d)
        idf = math.log(1.0 + (n_docs - df + 0.5) / (df + 0.5))
        f = doc_tf.get(token, 0)
        if f == 0:
            continue
        denom = f + k1 * (1 - b + b * (doc_len / avgdl if avgdl else 1))
        tf_sat = (f * (k1 + 1)) / denom if denom else 0.0
        score += idf * tf_sat
    return score


def _char_ngrams(s: str, arity: int) -> Counter[str]:
    arity = max(arity, 2)
    padded = (" " * (arity - 1)) + s + (" " * (arity - 1))
    return Counter(padded[i : i + arity] for i in range(len(padded) - arity + 1))


def _ngram_similarity(a_grams: Counter[str], b_grams: Counter[str]) -> float:
    allgrams = sum(a_grams.values()) + sum(b_grams.values())
    if allgrams == 0:
        return 0.0
    samegrams = sum(min(a_grams[g], b_grams[g]) for g in a_grams if g in b_grams)
    diffgrams = allgrams - samegrams
    warp = 2.0
    return (allgrams**warp - diffgrams**warp) / (allgrams**warp)


_WORD_RE = re.compile(r"[\w-]+", re.UNICODE)


def _extract_words(text: str) -> list[str]:
    return _WORD_RE.findall(text)


def _ngram_keyword_matches(rule: KeywordRule, keyword: str, text: str) -> bool:
    kw_grams = _char_ngrams(keyword.lower(), rule.ngram_arity)
    for word in _extract_words(text.lower()) + [text.lower()]:
        if _ngram_similarity(kw_grams, _char_ngrams(word, rule.ngram_arity)) >= rule.ngram_threshold:
            return True
    return False


def _levenshtein(a: str, b: str) -> int:
    if a == b:
        return 0
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i] + [0] * len(b)
        for j, cb in enumerate(b, 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb))
        prev = cur
    return prev[-1]


def _has_han(s: str) -> bool:
    return any("CJK" in unicodedata.name(ch, "") for ch in s)


def _default_keyword_matches(rule: KeywordRule, keyword: str, text: str) -> bool:
    haystack = text if rule.case_sensitive else text.lower()
    needle = keyword if rule.case_sensitive else keyword.lower()

    has_word_char = bool(re.search(r"[A-Za-z0-9_]", keyword, re.ASCII)) and not _has_han(keyword)
    if has_word_char:
        pattern = r"\b" + re.escape(needle) + r"\b"
        exact = re.search(pattern, haystack, re.ASCII) is not None
    else:
        exact = needle in haystack

    if exact:
        return True
    if rule.fuzzy_match:
        text_words = _extract_words(haystack)
        return any(_levenshtein(w, needle) <= rule.fuzzy_threshold for w in text_words)
    return False


def keyword_matches(rule: KeywordRule, keyword: str, text: str) -> bool:
    if rule.method == "bm25":
        return _bm25_score(rule, keyword, text) >= rule.bm25_threshold
    if rule.method == "ngram":
        return _ngram_keyword_matches(rule, keyword, text)
    return _default_keyword_matches(rule, keyword, text)


def evaluate_rule(rule: KeywordRule, text: str) -> MatchResult:
    """Combines per-keyword results with the rule's operator, mirroring
    matchAND/matchOR/matchNOR in keyword_classifier_match.go (and the Rust
    equivalents for bm25/ngram): OR = any keyword matches, AND = every
    keyword matches, NOR = no keyword matches.
    """
    hits = [keyword_matches(rule, kw, text) for kw in rule.keywords]
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

    confidence = 1.0 if matched else 0.0
    return MatchResult(matched, confidence)

"""Pure statistics functions for routeNIC bench reports.

No hardware, no experiment-specific knowledge — just turns a list of
per-request stage timestamps into the numbers docs/architecture.md and each
experiment doc promise to report. Kept dependency-free (stdlib only) so this
runs identically on the DGX Spark and on a laptop with no DOCA install.
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass


@dataclass(frozen=True)
class LatencyStats:
    count: int
    min_ms: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    max_ms: float
    mean_ms: float


def latency_stats(latencies_ms: list[float]) -> LatencyStats:
    if not latencies_ms:
        raise ValueError("latencies_ms must be non-empty")
    ordered = sorted(latencies_ms)
    return LatencyStats(
        count=len(ordered),
        min_ms=ordered[0],
        p50_ms=_percentile(ordered, 0.50),
        p95_ms=_percentile(ordered, 0.95),
        p99_ms=_percentile(ordered, 0.99),
        max_ms=ordered[-1],
        mean_ms=statistics.fmean(ordered),
    )


def _percentile(ordered: list[float], q: float) -> float:
    """Nearest-rank percentile over an already-sorted list."""
    if len(ordered) == 1:
        return ordered[0]
    rank = q * (len(ordered) - 1)
    lo = int(rank)
    hi = min(lo + 1, len(ordered) - 1)
    frac = rank - lo
    return ordered[lo] + (ordered[hi] - ordered[lo]) * frac


def throughput_per_sec(request_count: int, wall_clock_seconds: float) -> float:
    if wall_clock_seconds <= 0:
        raise ValueError("wall_clock_seconds must be positive")
    return request_count / wall_clock_seconds


def jitter_ms(latencies_ms: list[float]) -> float:
    """Population stddev of per-request latency — the tail-smoothness
    number docs/architecture.md flags as the expected standout result for
    Experiment 2 (no OS scheduling jitter between arrival and GPU pickup)."""
    if len(latencies_ms) < 2:
        return 0.0
    return statistics.pstdev(latencies_ms)


def cpu_free_fraction(stage_touches_cpu: dict[str, bool], stage_request_counts: dict[str, int]) -> float:
    """The fraction of *per-request stage executions* that never touch the
    host CPU, per docs/architecture.md's definition: "for a given
    experiment, the fraction of requests whose data-plane path (excluding
    one-time setup) never executes a host-CPU instruction." One-time setup
    stages should simply not be included in either dict.
    """
    total = sum(stage_request_counts.values())
    if total == 0:
        raise ValueError("stage_request_counts must sum to a positive total")
    cpu_free = sum(count for stage, count in stage_request_counts.items() if not stage_touches_cpu.get(stage, True))
    return cpu_free / total

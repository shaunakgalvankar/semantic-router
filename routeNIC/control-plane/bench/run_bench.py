#!/usr/bin/env python3
"""routeNIC bench harness — turns a JSON trace of per-request timings into
the latency/throughput/jitter/CPU-free-fraction/energy numbers each
experiment doc promises to report.

Two ways to get a trace in:

1. Real: point --trace at a JSON file an experiment's own instrumentation
   wrote (schema below). Nothing in this repo currently produces that file,
   because none of the data-plane experiments have run against live BF3
   hardware yet — see each experiment's own "what's not implemented"
   section. This harness is the consumer waiting for that producer.

2. Demo: --demo generates a synthetic trace so this file is runnable and
   testable *today*, with no hardware. Every demo report is stamped
   `"synthetic": true` at the top level and refuses to be silently confused
   with a real run — see --out.

Trace JSON schema (top-level object):
{
  "experiment": "<key into profiles.EXPERIMENT_PROFILES>",
  "wall_clock_seconds": <float, optional — enables throughput>,
  "records": [
    {
      "latency_ms": <float>,
      "stages": ["<stage name from the experiment's profile>", ...],
      "cpu_touches": {"<stage>": <bool>, ...}   // optional: MEASURED per-request
                                                  // observation. If omitted for a
                                                  // stage, this harness falls back
                                                  // to that stage's *design-intent*
                                                  // value from profiles.py and
                                                  // labels the report accordingly —
                                                  // it does not pretend an assumption
                                                  // is a measurement.
    },
    ...
  ]
}
"""

from __future__ import annotations

import argparse
import json
import random
import sys
from dataclasses import asdict

from energy import GpuPowerSampler
from metrics import cpu_free_fraction, jitter_ms, latency_stats, throughput_per_sec
from profiles import EXPERIMENT_PROFILES, stage_touches_cpu_map


def _synthetic_trace(experiment: str, num_requests: int, seed: int) -> dict:
    """Deterministic, clearly-fake trace for exercising this harness without
    hardware. Latencies are made up plausible-looking numbers, NOT
    projections of what the real hardware would achieve — do not report
    these as experiment results."""
    if experiment not in EXPERIMENT_PROFILES:
        raise SystemExit(f"unknown experiment key {experiment!r}; choices: {sorted(EXPERIMENT_PROFILES)}")
    rng = random.Random(seed)
    profile = EXPERIMENT_PROFILES[experiment]
    stages = list(profile.per_request_stages)
    records = []
    for _ in range(num_requests):
        records.append({
            "latency_ms": max(0.01, rng.gauss(mu=0.25, sigma=0.08)),
            "stages": stages,
        })
    return {
        "experiment": experiment,
        "wall_clock_seconds": num_requests / 50_000.0,  # arbitrary synthetic rate
        "records": records,
    }


def _report(trace: dict, synthetic: bool, energy_sample) -> dict:
    experiment = trace["experiment"]
    records = trace["records"]
    if not records:
        raise SystemExit("trace has no records")

    profile_default_touches = stage_touches_cpu_map(experiment)
    latencies = [r["latency_ms"] for r in records]

    stage_touches_assumed: dict[str, bool] = {}
    stage_counts: dict[str, int] = {}
    measured_any = False
    for r in records:
        overrides = r.get("cpu_touches", {})
        for stage in r["stages"]:
            stage_counts[stage] = stage_counts.get(stage, 0) + 1
            if stage in overrides:
                measured_any = True
                touches = overrides[stage]
            else:
                touches = profile_default_touches.get(stage, True)
            # a stage is only "assumed CPU-free" if EVERY occurrence lacked a measurement
            stage_touches_assumed.setdefault(stage, touches)
            if touches:
                stage_touches_assumed[stage] = True

    cff = cpu_free_fraction(stage_touches_assumed, stage_counts)
    stats = latency_stats(latencies)

    report = {
        "experiment": experiment,
        "experiment_name": EXPERIMENT_PROFILES[experiment].name,
        "synthetic": synthetic,
        "request_count": len(records),
        "latency_ms": asdict(stats),
        "jitter_ms_pstdev": jitter_ms(latencies),
        "cpu_free_fraction": cff,
        "cpu_free_fraction_basis": "measured" if measured_any else "assumed from design-intent profile (no cpu_touches observed in trace)",
    }
    if "wall_clock_seconds" in trace:
        report["throughput_per_sec"] = throughput_per_sec(len(records), trace["wall_clock_seconds"])
    if energy_sample is not None:
        report["energy"] = asdict(energy_sample)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--trace", help="path to a real trace JSON file (see schema in this file's docstring)")
    parser.add_argument("--demo", metavar="EXPERIMENT_KEY", help=f"generate a synthetic trace instead of reading one; choices: {sorted(EXPERIMENT_PROFILES)}")
    parser.add_argument("--demo-requests", type=int, default=1000)
    parser.add_argument("--demo-seed", type=int, default=0)
    parser.add_argument("--sample-energy", action="store_true", help="sample real GPU power via nvidia-smi during report generation (only meaningful with a real, running workload — see energy.py)")
    parser.add_argument("--out", help="write the JSON report here instead of stdout")
    args = parser.parse_args()

    if bool(args.trace) == bool(args.demo):
        parser.error("pass exactly one of --trace or --demo")

    sampler = GpuPowerSampler() if args.sample_energy else None
    if sampler:
        sampler.start()

    if args.demo:
        trace = _synthetic_trace(args.demo, args.demo_requests, args.demo_seed)
        synthetic = True
    else:
        with open(args.trace) as f:
            trace = json.load(f)
        synthetic = False

    energy_sample = sampler.stop() if sampler else None
    report = _report(trace, synthetic, energy_sample)

    text = json.dumps(report, indent=2)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

# control-plane/bench

Turns a JSON trace of per-request timings into the latency, throughput,
jitter, CPU-free-fraction, and energy numbers every experiment doc in
`docs/experiments/` promises to report. Pure Python stdlib, no DOCA/CUDA
dependency — runs on any machine, not just this DGX Spark.

## Try it now (no hardware required)

```
python3 run_bench.py --demo 2-gpunetio-persistent-classifier --demo-requests 2000
```

This generates a synthetic trace and prints a full report. Every synthetic
report is stamped `"synthetic": true` — **these numbers are made up for the
sole purpose of exercising this harness's code paths, not projections of
real hardware performance.** Do not cite them as results.

## Feeding it real data

None of the data-plane experiments have produced a real trace file yet —
that requires live BF3/CX7 hardware runs, which is exactly the "what's not
implemented" gap called out in each experiment's own doc. This harness is
the consumer waiting for that producer. Once an experiment binary is
instrumented to emit per-request `{latency_ms, stages, cpu_touches}`
records (schema documented in full at the top of `run_bench.py`), point
this at it:

```
python3 run_bench.py --trace /path/to/real_trace.json --out report.json
```

## Why `cpu_free_fraction` distinguishes "measured" from "assumed"

`profiles.py` encodes each experiment's *design intent* — which stages are
supposed to never touch the host CPU, taken directly from
`docs/architecture.md`'s per-experiment CPU-touch table. But design intent
isn't a measurement: code that's supposed to be CPU-free can still have a
bug that makes it not. If a trace record includes a real `cpu_touches`
observation (e.g. a counter an experiment's host code increments only when
it actually executes on a per-request path), this harness uses that
instead of the assumed value and labels the report
`"cpu_free_fraction_basis": "measured"`. If no trace record ever supplies
one, the report says so explicitly
(`"assumed from design-intent profile..."`) rather than silently presenting
an assumption as a result — this is deliberate: it's what stops a demo run
from being mistaken for hardware evidence.

## Energy sampling

`--sample-energy` samples real GPU package power via `nvidia-smi
--query-gpu=power.draw` on a background thread (confirmed working on this
host) for the duration of report generation. It is only meaningful wrapped
around an actual running workload — sampling idle power during a
`--demo` run (as the example above effectively does) just measures GPU
idle draw, not anything about the experiment. There is no NIC-side (BF3 or
CX7) power telemetry wired up here; DOCA doesn't expose per-device NIC
power the way `nvidia-smi` does for the GPU on this host, so total
system-energy figures are out of scope for this sampler and it says so in
its own report output rather than fabricating a number.

## Files

- `metrics.py` — pure statistics functions (latency percentiles,
  throughput, jitter, CPU-free fraction). No I/O, fully unit-testable.
- `profiles.py` — the CPU-touch profile table, one entry per experiment,
  kept as data so it can't silently drift from `docs/architecture.md`'s
  table.
- `energy.py` — background `nvidia-smi` power sampler.
- `run_bench.py` — CLI: trace in, report out.

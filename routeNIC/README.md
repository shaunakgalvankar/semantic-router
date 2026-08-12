# routeNIC

Offloading pieces of the vLLM Semantic Router's signal/decision pipeline off
the host CPU and onto the network: a BlueField-3 DPU (Arm cores + DPA) and a
ConnectX-7 SmartNIC, feeding a persistent GPU kernel on this DGX Spark (GB10)
over RDMA into unified memory.

This is a research/measurement project, not a semantic-router feature
contribution. It lives in its own top-level directory and its own branch so
it never mixes with the router's actual source tree.

## Hardware this targets

| Device | Role here |
| --- | --- |
| GB10 (DGX Spark, this host) | Grace Arm CPU + Blackwell GPU (`sm_121`) over coherent NVLink-C2C unified memory. The GPU is the only place model inference happens; the question this project asks is how data reaches it without the host CPU in the loop. |
| ConnectX-7 (on this host, `enp1s0f0np0` / `enp1s0f1np1`, two ports directly to the BF3) | RDMA/RoCE transport, GPUDirect RDMA target, ASAP² hardware flow steering. No Arm cores, no DPA — treated as a transport + flow-steering target, not a general compute target. |
| BlueField-3 DPU (external, QSFP-looped to this host: one port at 40G, one at 100G) | 16x Arm Cortex-A78 (a real Linux box in the data path) plus the DPA (`doca_sdk_dpa`, RISC-V-based programmable data-path accelerator, compiled via `dpacc`). **This particular BF3 unit does not have the hardware RegEx accelerator block** — see `data-plane/dpa_decision_fastpath/` for how that constraint was designed around instead of assumed away. |

Per the project's own instruction: there is no fixed rule that a given
experiment must run on the BF3 specifically vs. the ConnectX-7 specifically.
Where an experiment only needs RDMA transport + flow steering, either NIC is
a legitimate target; where it needs Arm cores or the DPA, it has to be the
BF3. Each experiment's own README says which device it actually targets and
why.

## Directory layout

```
routeNIC/
├── control-plane/     # orchestration, config, accuracy studies, metrics — mostly host-side Python/C, no DOCA hardware required to run
│   ├── accuracy_study/         software-only accuracy/tolerance study for the DPA-adapted decision algorithm (Experiment 1's control-side proof)
│   ├── software_baseline_bench/ real benchmark of the ACTUAL router's KeywordClassifier (imported live via go.mod replace, not reimplemented) — the software side of docs/BENCHMARK_COMPARISON.md
│   └── bench/                   latency/throughput/jitter/CPU-free-fraction/GPU-energy measurement harness
├── data-plane/        # the actual offloaded code: DPA kernels, CUDA/GPUNetIO, DOCA Flow rules
│   ├── dpa_decision_fastpath/           Experiment 1 — adapted decision/keyword algorithm running on the BF3 DPA
│   ├── gpunetio_persistent_classifier/  Experiment 2 — persistent CUDA kernel + GPU-triggered RDMA receive
│   ├── asap2_flow_routing/              Experiment 3 — hardware tenant/recipe steering via DOCA Flow (ASAP²)
│   ├── cascade_classifier/              Experiment 4 — cheap DPU-side gate before the full GPU classifier (scaffolded, README only)
│   └── response_path_offload/           Experiment 5 — symmetric outbound offload (scaffolded, README only)
└── docs/
    ├── architecture.md       full design + how these experiments compose
    └── experiments/          one doc per experiment: mechanism, why it's novel, risk, metrics it produces
```

No `control-plane/common/`, `config/`, `orchestrator/`, or top-level `scripts/`
yet — those are natural extension points once a data-plane experiment is
actually running on live hardware and needs shared device config or a
run-everything driver, but building them now, before there's a second real
consumer to generalize from, would be structure with nothing to justify it.

## Experiment status

| # | Experiment | Directory | Status |
| --- | --- | --- | --- |
| 1 | DPA-adapted decision fast path + accuracy/tolerance study | `data-plane/dpa_decision_fastpath/`, `control-plane/accuracy_study/` | **Hardware-validated + benchmarked.** 6/6 correctness tests pass on real BF3 DPA. Real throughput benchmark against the real software baseline: naive 19.7ms/launch → 1.25ms after reusing the completion sync event (15.8x) → **13.7–18.7µs/request amortized after batching (beats the 42µs/op software baseline)**. See `docs/BENCHMARK_COMPARISON.md`. RDMA-triggered (as opposed to benchmark-driven) invocation still not wired up. |
| 2 | GPUNetIO persistent classifier kernel | `data-plane/gpunetio_persistent_classifier/` | Built and run for real against live GB10+ConnectX-7 hardware — 3 real bugs found and fixed (device never opened, no flow/pipe/packet-buffer setup, kernel too large to link). Blocked on a host-level GPUDirect RDMA registration failure — confirmed via a clean control test to also affect NVIDIA's own unmodified sample, and `sudo modprobe nvidia-peermem` fails at the kernel level with no diagnosable cause (version/vermagic all match). Likely a genuine GB10 platform limitation, not an application bug. See experiment doc. |
| 3 | ASAP² tenant/recipe flow steering | `data-plane/asap2_flow_routing/` | **Hardware-validated.** Pipe/entry install actually run against a live ConnectX-7 port — PASS, all entries confirmed installed (`data-plane/asap2_flow_routing/hw_test/`). Sending real VLAN-tagged traffic through the installed rules to confirm the steering decision itself is still open. |
| 4 | Cascade classifier (DPU gate + GPU fallback) | `data-plane/cascade_classifier/` | Scaffolded; depends on Experiment 1's adapted classifier as the cheap gate. |
| 5 | Symmetric response-path offload | `data-plane/response_path_offload/` | Scaffolded; mirrors Experiment 2's mechanism in the outbound direction. |
| — | Metrics harness (latency, throughput, jitter, CPU-free fraction, GPU energy) | `control-plane/bench/` | Implemented and runnable now (`run_bench.py --demo <experiment>`, no hardware required). Waiting on a real trace producer from any data-plane experiment — see that directory's README. |

**Explicitly out of scope:** tokenizer offload to the BF3. That's a separate,
already-in-progress project (`~/docaTokenizer`) and nothing in `routeNIC/`
should depend on it or duplicate it.

## Why no hardware RegEx offload

The original brainstorm's "hardware RegEx engine for keyword signals" idea
assumed a capability this specific BF3 doesn't have. Rather than drop the
idea, Experiment 1 reframes it into the more interesting question the project
actually wants answered: *what's the least-lossy keyword/decision algorithm
that fits the DPA's programming model, and how much accuracy does adapting
it cost?* That's a real research question (algorithm-hardware co-design)
instead of a hardware-availability workaround, and it's exactly what
`control-plane/accuracy_study/` measures.

## Getting started

See `docs/architecture.md` for the full system design, and each
`docs/experiments/NN-*.md` for a single experiment's mechanism, risk, and
metrics. `control-plane/accuracy_study/` is the one piece with no hardware
dependency at all — run it first. For the real software-vs-hardware
performance comparison, see `docs/BENCHMARK_COMPARISON.md`.

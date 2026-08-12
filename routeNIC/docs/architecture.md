# routeNIC architecture

## The pipeline being offloaded

The vLLM Semantic Router evaluates, per request, a set of *signals* and then
a *decision tree* of boolean logic over those signals (see
`src/semantic-router/pkg/decision/engine.go` and `trace.go` in the main
repo). Two properties of that pipeline matter for hardware offload:

1. **Signals split cleanly into two compute classes.** `keyword` signals are
   pattern-matching over raw text — no learned model, no floating-point
   model weights, just string/pattern algorithms. `domain`, `pii`,
   `jailbreak`, `fact_check`, `user_feedback`, `modality` signals are BERT
   classifier inference (mmBERT/ModernBERT LoRA fine-tunes) — GPU's job,
   full stop, on any hardware in this project.
2. **The decision tree itself is cheap.** `evalAND`/`evalOR`/`evalNOT` in the
   router's decision engine are pure boolean combination over
   already-computed signal results — negligible compute, but it's the piece
   that decides whether a request even needs a GPU-classified signal at all.

That split is what makes a DPU-shaped fast path plausible: if a decision's
entire rule subtree only references DPA-tractable signal types, the DPA can
resolve it end-to-end without ever waking the GPU classifier. If any leaf in
the subtree needs a BERT signal, the DPA can't help with *that decision*, and
the request has to reach the GPU regardless of what else is offloaded.

## Two independent axes of offload

This project has two mostly-independent axes, and it's worth keeping them
mentally separate:

**Axis 1 — what runs where (compute placement).** Keyword/decision logic on
the BF3's Arm cores or DPA instead of the DGX Spark's Grace cores. This axis
doesn't require RDMA into the GPU at all; it's a pure "move the CPU-shaped
work off the host CPU" story, and Experiment 1 (`dpa_decision_fastpath`) and
Experiment 3 (`asap2_flow_routing`) live here.

**Axis 2 — how data reaches the GPU (transport placement).** Getting request
data from the wire into GPU-visible unified memory without the host CPU
touching it, then having a *persistent* CUDA kernel — launched once, so the
"one CPU touch" the project allows — do the actual model inference by
polling for arrivals itself. Experiment 2 (`gpunetio_persistent_classifier`)
and Experiment 5 (`response_path_offload`) live here.

Experiment 4 (`cascade_classifier`) is where the two axes meet: a DPA-hosted
cheap gate (axis 1) decides whether a request needs the full GPU classifier
at all, and if it does, it's handed off via the axis-2 mechanism.

## Why "no fixed NIC" is the right call here

ConnectX-7 and BlueField-3 differ in compute (no Arm cores / no DPA on
ConnectX-7), but for pure RDMA transport and ASAP² flow steering they're
close enough to interchangeable. Given this lab only has one of each, every
experiment that only needs transport + flow steering (parts of Experiment 2,
all of Experiment 3) should be written against a *device handle chosen by
config*, not hardcoded to one PCI device — so the same binary can be pointed
at either NIC and the comparison itself becomes a data point ("does it
matter which NIC does the steering here?") rather than an implementation
accident.

## What "avoid the CPU" means concretely, per experiment

| Experiment | Host CPU's job | What it must *not* do |
| --- | --- | --- |
| 1 — DPA decision fast path | Load the compiled DPA program once at startup | Must not touch per-request keyword/decision evaluation |
| 2 — GPUNetIO persistent classifier | Launch the persistent CUDA kernel once at startup | Must not touch per-request RDMA polling, batching, or inference |
| 3 — ASAP² flow steering | Install flow rules once at startup | Must not touch per-packet steering decisions |
| 4 — Cascade classifier | Same as 1 + 2, once each | Must not touch the DPU→GPU handoff decision |
| 5 — Response-path offload | Same as 2, mirrored outbound | Must not touch response post-processing |

The **CPU-free fraction** metric (see `control-plane/bench/`) is defined
precisely against this table: for a given experiment, the fraction of
requests whose *data-plane* path (excluding the one-time setup above) never
executes a host-CPU instruction.

## Data flow for Experiment 2 (the centerpiece)

```
client
  │  HTTP/gRPC request
  ▼
BlueField-3 (Arm cores)              — parses request envelope, extracts prompt text
  │  RDMA WRITE (raw prompt bytes) into a pre-registered
  │  ring buffer in GB10 unified memory (GPUDirect RDMA target)
  ▼
DGX Spark — GB10 unified memory (NVLink-C2C coherent)
  │  polled directly by GPU registers — no host CPU wakeup
  ▼
Persistent CUDA kernel (launched once, DOCA GPUNetIO-driven)
  │  batches arrivals, runs the BERT classifier, evaluates decisions
  ▼
Result written back (RDMA or host-visible ring) → routing decision
```

See `docs/experiments/02-gpunetio-persistent-classifier.md` for the DOCA
GPUNetIO API specifics and what's validated on this host vs. what needs the
BF3 peer live.

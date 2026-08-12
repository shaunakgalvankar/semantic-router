# Experiment 2: GPUNetIO persistent classifier

**Device:** GB10 GPU (persistent kernel) + ConnectX-7 or BlueField-3 as the
RDMA/Ethernet transport (device-agnostic by config — see the launcher's
note on why).

## The question

Can the GPU own its own request arrivals — polling for new work, batching,
and classifying — with the host CPU's only involvement being the one-time
kernel launch at startup? This is the mechanism idea #4 from the original
brainstorm, and it's real, existing NVIDIA technology (DOCA GPUNetIO /
GPUDirect Async), not something invented for this project.

## What's implemented

- `device/gpunetio_classifier_kernel.cu` — the persistent kernel.
  **Compiles clean against the real DOCA GPUNetIO headers on this host at
  `-arch=sm_121` (GB10's actual compute capability)**, verified with
  `nvcc -c ... -gencode arch=compute_121,code=sm_121`. Structure follows
  NVIDIA's own installed `gpunetio_simple_receive` sample exactly (same
  `doca_gpu_dev_eth_rxq_recv` call, same `DOCA_GPUNETIO_VOLATILE` exit-flag
  loop) — that sample stops at "packet received"; this one batches arrivals
  into the SPSC ring in `gpunetio_classifier_common.h` and calls a
  `classify_batch()` hook entirely inside the kernel.
- `host/gpunetio_classifier_launcher.c` — device/mmap/rxq bootstrap,
  syntax-verified against the real headers on this host. Allocates the
  request ring as GPU memory registered for GPUDirect RDMA
  (`DOCA_ACCESS_FLAG_RDMA_WRITE`), so a peer can write directly into
  GPU-visible unified memory.

## The one deliberate scoping decision here

`classify_batch()` does real, deterministic, GPU-side keyword matching
(exercising the same "keyword signal" concept as Experiment 1) — not a full
BERT forward pass. Wiring an actual transformer inference engine into a raw
persistent CUDA kernel (rather than through a host-orchestrated inference
server) is a substantial project on its own; conflating it with the
RDMA/batching mechanics this experiment is actually measuring would make it
impossible to tell which part of any latency result came from which change.
The ring buffer, batching, and polling code do not need to change to
support swapping this hook for a compiled inference engine call later.

## What's not implemented yet (needs the live BF3)

- No RDMA WRITE producer exists yet on the BF3 side — the ring is allocated
  and registered as an RDMA target, but nothing is currently writing to it
  from across the QSFP loop. That's the actual end-to-end validation this
  experiment needs the lab hardware for.
- `gpu_exit_condition` is a placeholder host write; a real shutdown path
  (not yet built — a natural future addition to `control-plane/`, alongside
  `bench/`) needs to signal it cleanly for benchmark runs of bounded
  duration.
- The raw-Ethernet-receive code path in the kernel (the more aggressive
  "Experiment 8" variant with no BF3 involvement at all) shares this same
  kernel and `classify_batch()` call but hasn't been driven through its own
  DOCA Flow pipe setup yet — see `asap2_flow_routing/` for the flow-steering
  half of that.

## Metrics this experiment feeds

- End-to-end latency broken down by stage (network hop, GPU poll-to-batch,
  classify, write-back) — this experiment is the one place in the project
  where all of those stages are actually inside one measurable kernel
  execution.
- CPU-free fraction, directly: after kernel launch, this experiment's whole
  design goal is zero host-CPU involvement in the per-request path.
- Tail latency / jitter, which is expected to be the standout result here
  relative to a host-scheduled classifier — no OS scheduling jitter between
  "data arrived" and "GPU starts processing it," since the kernel is
  already spinning on the arrival check.

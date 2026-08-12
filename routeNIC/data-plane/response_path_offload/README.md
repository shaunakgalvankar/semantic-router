# Experiment 5: Symmetric response-path offload

**Device:** GB10 GPU + ConnectX-7 or BlueField-3 (device-agnostic transport,
same rationale as Experiment 2 — see that experiment's launcher note on why
the NIC is chosen by config, not hardcoded).

## Status: scaffolded, not yet implemented

Experiment 2 (`gpunetio_persistent_classifier`) gets request data from the
wire into GPU-visible unified memory without the host CPU in the loop, and
runs classification there via a persistent kernel. This experiment asks the
symmetric question for the *response* path: once the GPU has a routing
decision (and, in a real deployment, once the downstream model server has
produced a response), can that result reach the wire again without the host
CPU touching it either?

## Why this mirrors Experiment 2's mechanism instead of inventing a new one

The transport primitive is the same in both directions — GPUDirect RDMA
into/out of unified memory registered on a `doca_mmap` — the only thing
that changes is which side initiates the RDMA operation. Experiment 2's
ring (`gpunetio_persistent_classifier/gpunetio_classifier_common.h`,
`routenic_request_ring`) is written by an external peer and read by the
GPU; this experiment needs the reverse: a response ring the GPU's
persistent kernel writes and an external peer (or the NIC's own hardware,
via GPUNetIO's send path — `doca_gpu_dev_eth_txq_*`, not yet touched by
this project) reads and puts on the wire.

## Why this is scaffolded rather than built now

The same persistent-kernel structure Experiment 2 already implements and
compiled clean (`persistent_classify<exec_scope>()` in
`gpunetio_classifier_kernel.cu`) is the natural place to add a symmetric
write-side loop, but doing that meaningfully requires Experiment 2's
receive side to actually be exercised end-to-end first — there is currently
no real classification result flowing out of the kernel to send back
(`gpu_exit_condition`-gated correctness testing hasn't happened on live
hardware yet, per that experiment's own "what's not implemented" section).
Building the outbound half before the inbound half has a real result to
carry would again be unverified scaffolding with nothing to check it
against.

## What this experiment will need, once Experiment 2 is hardware-validated

- A `doca_eth_txq` (or GPUNetIO send-queue equivalent) counterpart to
  Experiment 2's `doca_eth_rxq`, GPU-datapath-enabled the same way
  (`doca_ctx_set_datapath_on_gpu`).
- A response ring symmetric to `routenic_request_ring`, sized for whatever
  the actual routing-decision payload turns out to be (almost certainly
  much smaller than the request ring's prompt-text slots).

## Metrics this experiment feeds

- Round-trip latency (request arrival → decision → response on the wire),
  decomposed into the inbound half (already measured by Experiment 2) and
  this experiment's outbound addition — isolating whether the response
  path adds disproportionate latency relative to the receive path.
- CPU-free fraction for the full round trip, the natural completion of
  Experiment 2's inbound-only CPU-free measurement.

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

## What's implemented, and what's actually been run

- `device/gpunetio_classifier_kernel.cu` — the persistent kernel. Structure
  follows NVIDIA's own installed `gpunetio_simple_receive` sample exactly
  (same `doca_gpu_dev_eth_rxq_recv` call, same `DOCA_GPUNETIO_VOLATILE`
  exit-flag loop) — that sample stops at "packet received"; this one batches
  arrivals into the SPSC ring in `gpunetio_classifier_common.h` and calls a
  `classify_batch()` hook entirely inside the kernel.
- `host/gpunetio_classifier_launcher.c` + `gpunetio_classifier_main.c` — a
  bounded (12s, watchdog-enforced) bring-up test, **actually built and run
  against real GB10 + ConnectX-7 hardware**, not just compiled. Getting a
  real run — not `nvcc -c`, which never device-links and so never catches
  most of what's below — required fixing three genuine defects:
  1. **`ddev` was never opened.** The original code declared
     `struct doca_dev *ddev = NULL;` and passed it straight to
     `doca_eth_rxq_create()` and friends — it stayed NULL the whole run.
     Fixed by adding `routenic_open_nic_device()`
     (`open_doca_device_with_pci`, the same helper every DOCA sample on this
     host uses).
  2. **No DOCA Flow port/pipe setup existed at all.** Without it, nothing
     ever steers an arriving packet into this rxq — the kernel would poll
     forever and legitimately never see a packet, which would have looked
     identical to "the kernel doesn't work" without this being the real
     cause. Fixed by porting `gpunetio_simple_receive_sample.c`'s
     `init_doca_flow`/`start_doca_flow`/`create_udp_pipe`/`create_root_pipe`
     sequence (UDP-matching root + rxq pipe) into the launcher, plus the
     packet-buffer `doca_mmap`/dmabuf setup in `create_rxq()` that the
     original version of this file also omitted entirely.
  3. **The kernel could not link.** `__shared__ struct routenic_request_slot
     batch[ROUTENIC_RING_CAPACITY]` sized a per-iteration batch buffer to
     the *entire ring's capacity* (4096 × ~536 bytes ≈ 2.1MB) against this
     GPU's 48KB per-block shared-memory limit. `nvlink` refused to link the
     kernel outright — a check `nvcc -c` alone never runs. Fixed by
     introducing `ROUTENIC_MAX_BATCH_PER_ITER` (64, a real per-launch cap
     both `batch[]` and `out_attr[]` are now sized to, with `max_batch_size`
     clamped to it defensively inside the kernel).
- All three fixes verified: the kernel now compiles, device-links, and the
  full executable builds and runs. **What it hits now is a real,
  host-level blocker, isolated with a clean control test**: it fails at
  GPUDirect RDMA memory registration (`doca_mmap_start` →
  `DOCA_ERROR_DRIVER`, `errno=14` at the `devx` layer) — and **NVIDIA's own
  unmodified `gpunetio_simple_receive` sample, built fresh via its real
  `meson`/`ninja` project and pointed at the exact same GPU/NIC PCI
  addresses, fails with the byte-for-byte identical error.** That rules out
  application code as the cause. The kernel `nvidia-peermem.ko` exists on
  this system but is not currently loaded (`lsmod` confirms); loading it
  (`sudo modprobe nvidia-peermem`) is the next concrete thing to try, but
  is a system-level kernel module change on shared lab hardware, not
  something to do without asking first. Raw output from both runs (this
  experiment's own test and the control test):
  `real_run_output.txt` in this experiment's directory.

Build and run it yourself with `./build_test.sh` then
`sudo build/routenic_gpunetio_test <gpu-pci-addr> <nic-pci-addr>` (PCI
addresses via `nvidia-smi -q | grep Bus.Id` and `lspci -d 15b3:`). The run is
bounded to 12 seconds by a watchdog thread regardless of outcome — see
`routenic_watchdog_thread()` in the launcher.

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

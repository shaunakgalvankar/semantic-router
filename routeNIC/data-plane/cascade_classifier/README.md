# Experiment 4: Cascade classifier (DPA gate + GPU fallback)

**Device:** BlueField-3 DPA for the gate, GB10 GPU for the fallback — this
is the one experiment that's inherently two-device by construction, since
it's specifically about the handoff between Axis 1 and Axis 2 (see
`docs/architecture.md`, "Two independent axes of offload").

## Status: scaffolded, not yet implemented

This experiment is a composition of Experiments 1 and 2, not new mechanism:

1. Every arriving request first hits Experiment 1's DPA fast path
   (`data-plane/dpa_decision_fastpath/`, `routenic_fastpath_evaluate`).
2. If the request's decision resolves entirely from DPA-tractable
   (keyword-only) signals, it's done — this is the "gate closed" case, and
   the GPU never wakes for this request.
3. If any signal in the decision tree needs a BERT classifier (the DPA
   can't evaluate it — see `docs/architecture.md`'s signal/compute-class
   split), the request needs to reach the GPU. In the cascade, this handoff
   reuses Experiment 2's RDMA-into-unified-memory mechanism
   (`data-plane/gpunetio_persistent_classifier/`) rather than inventing a
   third transport.

## Why this is scaffolded rather than built now

Both halves of this composition (`dpa_decision_fastpath` and
`gpunetio_persistent_classifier`) are independently implemented and
compile/syntax-verified against real DOCA headers on this host (see their
own experiment docs), but neither has been driven through live BF3
hardware yet — there is no real "gate decided no, hand off to GPU" event to
wire up until Experiment 1's fast path is actually receiving requests over
RDMA rather than being fed by the launcher directly. Building the handoff
logic before either side has a real trigger would be code with nothing to
verify it against — exactly the kind of unverified scaffolding this project
has otherwise avoided (see the compile/syntax-check discipline in
Experiments 1-3).

## What this experiment will need, once 1 and 2 are hardware-validated

- A shared "gate result" struct the DPA kernel writes and the GPU-side
  arrival path can read — most naturally as a tagged variant of
  `routenic_fastpath_evaluate`'s existing `out_matched` output
  (`dpa_kernel/fastpath_shared.h`) plus an explicit "needs GPU" flag for
  the case where no DPA-tractable rule subtree matched.
- The handoff path itself: does the DPA forward the request onward via
  RDMA WRITE into the GPU's ring (reusing Experiment 2's ring exactly), or
  does the GPU's persistent kernel poll a DPA-written "pending" queue
  directly? Both are legitimate; picking one needs the fast-path hit-rate
  data from Experiment 1 running live (a very low hit rate makes the gate
  overhead not worth it; this repo's own config only has 2/23 decisions
  keyword-only per the accuracy study, so this is a real open question, not
  a formality).

## Metrics this experiment feeds

- End-to-end latency for gated (DPA-only) vs. cascaded (DPA-then-GPU)
  requests, and specifically the *added* latency the gate itself imposes on
  requests that end up needing the GPU anyway (the gate must not make the
  common case slower to save the rare case work).
- CPU-free fraction across the whole cascade, not just one stage — the
  interesting number here is whether composing two independently CPU-free
  experiments stays CPU-free, or whether the handoff itself needs a CPU
  touch neither experiment required alone.

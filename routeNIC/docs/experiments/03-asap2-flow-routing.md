# Experiment 3: ASAP² tenant/recipe flow steering

**Device:** either NIC by config (`routenic_asap2_tenant_steering_setup`
takes a `struct doca_flow_port *` opened against whichever device's PCI BDF
is passed in) — this experiment only needs flow-table hardware and RDMA
transport, both of which ConnectX-7 and BlueField-3 have, per the project's
"no fixed NIC" instruction.

## The question

The router resolves which recipe a request's virtual model name maps to in
software, per request (`RecipeForRequestModel`, a linear scan over
configured entrypoints — see `src/semantic-router/pkg/config/recipes.go` in
the main repo). Issue #2868 in that repo is actively discussing extending
this into full tenant-aware, header-matched entrypoint rules. This
experiment asks the hardware-side version of the same question: if
tenant→recipe steering happened in the NIC's flow table instead of in Go,
does lookup cost stay flat as the number of tenants grows, where a software
dictionary/list scan degrades?

## Honest scoping decision

The router's real tenant identifier (`x-authz-tenant-id`) is an HTTP
header — DOCA Flow's match engine works on L2-L4 fields and tunnel/VLAN
tags, not application-layer headers, without a custom parser definition.
Rather than either fake HTTP-header hardware matching or drop the idea,
this experiment uses the realistic proxy every real ASAP²-based multi-tenant
deployment actually relies on: an upstream gateway/ext_authz tags each
tenant's traffic with a distinct VLAN ID before it reaches this NIC, and
hardware steers on that tag. Extending to genuine custom-header parsing is
a documented follow-up (`doca_flow`'s custom header parser feature), not
assumed away.

## What's implemented

- `asap2_tenant_steering.c` — creates one `DOCA_FLOW_PIPE_CONTROL` pipe
  (the only DOCA Flow pipe type whose entries can each carry a distinct
  forward action, required since every tenant steers to a different recipe
  queue) and adds one hardware entry per tenant via
  `doca_flow_pipe_control_add_entry`, matching VLAN TCI and forwarding to a
  single deterministic RSS queue per tenant. **Syntax-verified against the
  real DOCA Flow headers on this host** — every API call, struct field, and
  enum name in this file was checked against `/opt/mellanox/doca/include/`
  and NVIDIA's own installed samples (`flow_hash_pipe`,
  `flow_control_pipe`, `flow_ct_udp_query`, `flow_lpm`) rather than assumed;
  two API guesses (a nonexistent `doca_flow_pipe_add_entry` and a
  `DOCA_FLOW_RSS_HASH` constant that doesn't exist in this DOCA version)
  were caught and corrected by that verification, not silently left wrong.
- A fail-closed default forward (`DOCA_FLOW_FWD_DROP`) for traffic that
  matches no configured tenant VLAN — deliberately mirrors the
  "ClaimedNoMatch must never become passthrough" invariant from the
  tenant-rules design discussion in issue #2868, one layer lower in the
  stack.

## What's not implemented yet (needs the live BF3/CX7 + a gateway)

- Nothing currently assigns the VLAN tags this experiment matches on — that
  requires either a real upstream gateway doing the tagging, or a synthetic
  traffic generator standing in for one during bring-up.
- Port/queue bring-up (`doca_flow_init`, `doca_flow_port_start`, RSS queue
  array provisioning) isn't included here — this file is the steering logic
  specifically, meant to be called after the standard DOCA Flow port
  bootstrap every DOCA Flow sample on this host already demonstrates.

## Metrics this experiment feeds

- Flow-table lookup latency as a function of tenant count (the actual
  point of this experiment) — compare against a software equivalent (a
  simple Go map/list scan over the same tenant count) to see whether
  hardware steering's flat-lookup-cost property actually holds in practice
  on this hardware.
- Throughput at saturation, and whether it holds steady as tenant count
  scales up (a software dictionary lookup should show throughput
  degradation the hardware path shouldn't).

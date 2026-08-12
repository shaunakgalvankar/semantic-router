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

## What's implemented, and what's actually been run

- `asap2_tenant_steering.c` — creates one `DOCA_FLOW_PIPE_CONTROL` pipe
  (the only DOCA Flow pipe type whose entries can each carry a distinct
  forward action, required since every tenant steers to a different recipe
  queue) and adds one hardware entry per tenant via
  `doca_flow_pipe_control_add_entry`, matching VLAN TCI and forwarding to a
  single deterministic RSS queue per tenant, plus a fail-closed wildcard
  catch-all entry (`DOCA_FLOW_FWD_DROP`, lower precedence than every tenant
  entry) for traffic matching no configured tenant VLAN — deliberately
  mirrors the "ClaimedNoMatch must never become passthrough" invariant from
  the tenant-rules design discussion in issue #2868, one layer lower in the
  stack.
- `hw_test/` — **actually run against a live ConnectX-7 port on this lab's
  DGX Spark** (`enp1s0f0np0`, taken into DPDK-managed mode for the duration
  of the run, confirmed to return cleanly to normal kernel networking
  afterward). Built via `meson`/`ninja` against the real DOCA Flow/DPDK
  toolchain (the only way DOCA Flow programs are ever actually built —
  there's no simpler correct path). **Result: PASS** — pipe created, all 4
  entries (3 tenant + 1 catch-all) installed and confirmed via
  `doca_flow_entries_process`. Raw output: `hw_test/real_run_output.txt`.

### Two real bugs this run caught that syntax-checking never would have

1. **`fwd should be null for control pipe`** (a real DOCA Flow engine
   error, not a header/syntax issue). The original code set a pipe-level
   default `fwd` at `doca_flow_pipe_create()` time for a control pipe — the
   engine rejects this unconditionally; control pipes require `fwd = NULL`
   at creation, since per-entry `fwd` is the entire reason to use this pipe
   type. Fixed by moving the fail-closed drop behavior into an explicit
   wildcard catch-all *entry* instead of a pipe-level default.
2. **Entries were fire-and-forget.** `doca_flow_pipe_control_add_entry()`
   only queues an entry — nothing is confirmed installed in hardware until
   `doca_flow_entries_process()` is pumped and a `usr_ctx` status struct is
   checked. The original code passed `NULL` for `usr_ctx` and never called
   `doca_flow_entries_process()` — every add_entry call would report
   success regardless of whether hardware actually accepted anything. Fixed
   by threading a status context through and processing entries for real.

See `hw_test/README.md` for the full writeup, including the (also
empirically-discovered, not documented anywhere obvious) real device-address
CLI format.

## What's not implemented yet (needs the live BF3/CX7 + a gateway)

- Nothing currently assigns the VLAN tags this experiment matches on — that
  requires either a real upstream gateway doing the tagging, or a synthetic
  traffic generator standing in for one during bring-up. `hw_test/` proves
  the pipe/entries install correctly in hardware; it doesn't yet send actual
  VLAN-tagged traffic through them to confirm the steering decision itself.
- Port/queue bring-up beyond what `hw_test/` already does (RSS queue array
  provisioning tuned for a real multi-queue deployment, not just enough
  queues to prove the mechanism) is still a simplification.

## Metrics this experiment feeds

- Flow-table lookup latency as a function of tenant count (the actual
  point of this experiment) — compare against a software equivalent (a
  simple Go map/list scan over the same tenant count) to see whether
  hardware steering's flat-lookup-cost property actually holds in practice
  on this hardware.
- Throughput at saturation, and whether it holds steady as tenant count
  scales up (a software dictionary lookup should show throughput
  degradation the hardware path shouldn't).

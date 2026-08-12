# Experiment 3 real-hardware test

Unlike `asap2_tenant_steering.c`'s earlier `gcc -fsyntax-only` verification,
this is an actual runnable program: it opens a real `doca_flow_port` on a
live ConnectX-7 port, calls `routenic_asap2_tenant_steering_setup()`, and
reports whether the hardware actually accepted the pipe and entries.

It needs the full DOCA Flow + DPDK toolchain (`doca-flow`, `doca-dpdk-bridge`,
`libdpdk`), which is why it's built with `meson`/`ninja` against the DOCA
SDK's own sample-common sources in place, rather than the ad hoc single-file
`gcc`/`dpacc` invocations the rest of this experiment's build scripts use —
DOCA Flow samples are never built any other way; there's no simpler correct
path.

## Building

```
meson setup build
ninja -C build
```

Override `-Ddoca_prefix=...` if DOCA isn't installed at `/opt/mellanox/doca`
on your host.

## Running

**This takes over a real NIC port from the kernel's normal network stack for
the duration of the run** (DPDK-managed mode). Do not point this at a port
carrying traffic you care about. Needs root and DPDK hugepages reserved
(`echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`
if none are reserved yet).

```
sudo ./build/routenic_asap2_test -- -a pci/<BB:DD.F>
```

The device address format (`pci/01:00.0`, not the raw `0000:01:00.0` BDF
`lspci` prints) came from `/opt/mellanox/doca/samples/doca_flow/README.md` —
not obvious from the `--help` output alone, which only shows this after the
`--` separator (`-a`/`--device` here is DOCA's own app-level flag, distinct
from the identically-named DPDK EAL `-a`/`--allow` that comes *before* `--`).

## Real result (last run: see `real_run_output.txt` in this directory)

**PASS** — pipe created, all 4 entries (3 tenant VLAN routes + 1 catch-all
drop) installed and confirmed via `doca_flow_entries_process`, against a
live ConnectX-7 port (`enp1s0f0np0`, verified afterward to have returned
cleanly to normal kernel networking — mlx5's bifurcated driver model doesn't
need an explicit unbind/rebind step).

## Two real bugs this run caught in `asap2_tenant_steering.c`

Both were invisible to `gcc -fsyntax-only` — they're runtime engine
invariants, not syntax:

1. **`fwd should be null for control pipe`** — the original code passed a
   `default_fwd` (`DOCA_FLOW_FWD_DROP`) to `doca_flow_pipe_create()` for a
   `DOCA_FLOW_PIPE_CONTROL` pipe. The real DOCA Flow engine rejects this
   outright: control pipes must have `fwd = NULL` at creation, since the
   entire point of the pipe type is that each *entry* supplies its own
   `fwd`. Fixed by removing the pipe-level default and instead adding an
   explicit wildcard-match, `DOCA_FLOW_FWD_DROP`, lower-precedence
   (priority 1 vs. tenant entries' priority 0) catch-all *entry* — the
   fail-closed behavior is unchanged, just implemented the way the engine
   actually requires.
2. **Entries were fire-and-forget.** `doca_flow_pipe_control_add_entry()`
   only queues an entry; nothing is confirmed installed until
   `doca_flow_entries_process()` is pumped and the `usr_ctx` status struct
   (DOCA Flow's own `entries_status` convention) is checked. The original
   code passed `NULL` for `usr_ctx` and never called
   `doca_flow_entries_process()` at all — every entry call would report
   `DOCA_SUCCESS` regardless of whether hardware actually accepted it. Fixed
   by threading a caller-supplied status context through and processing
   entries before returning.

Also needed, found empirically rather than from source-reading (see this
directory's real command above): the exact device-address string format
(`pci/BB:DD.F`) and the fact that the DOCA-level `-a` flag only exists after
`--`, separate from DPDK EAL's own `-a`.

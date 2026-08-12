"""Per-experiment CPU-touch profiles.

Mirrors docs/architecture.md's "What 'avoid the CPU' means concretely, per
experiment" table exactly — that table is the actual spec for this data.
Kept as data here, not duplicated logic, so run_bench.py's cpu-free-fraction
report and that doc can't silently drift apart; if the table changes, this
is the one place to update.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ExperimentProfile:
    name: str
    setup_stages: tuple[str, ...]  # one-time, CPU-touching by design, excluded from cpu-free-fraction
    per_request_stages: dict[str, bool]  # stage name -> touches host CPU


EXPERIMENT_PROFILES: dict[str, ExperimentProfile] = {
    "1-dpa-decision-fastpath": ExperimentProfile(
        name="DPA decision fast path",
        setup_stages=("load_dpa_program",),
        per_request_stages={
            "dpa_keyword_decision_eval": False,
        },
    ),
    "2-gpunetio-persistent-classifier": ExperimentProfile(
        name="GPUNetIO persistent classifier",
        setup_stages=("launch_persistent_kernel",),
        per_request_stages={
            "rdma_arrival_poll": False,
            "gpu_batch_classify": False,
        },
    ),
    "3-asap2-flow-routing": ExperimentProfile(
        name="ASAP2 tenant/recipe flow steering",
        setup_stages=("install_flow_rules",),
        per_request_stages={
            "hw_flow_steer": False,
        },
    ),
    "4-cascade-classifier": ExperimentProfile(
        name="Cascade classifier (DPA gate + GPU fallback)",
        setup_stages=("load_dpa_program", "launch_persistent_kernel"),
        per_request_stages={
            "dpa_gate_eval": False,
            "dpu_to_gpu_handoff": False,  # the actual open question this experiment measures — see its README
            "gpu_batch_classify": False,
        },
    ),
    "5-response-path-offload": ExperimentProfile(
        name="Symmetric response-path offload",
        setup_stages=("launch_persistent_kernel",),
        per_request_stages={
            "gpu_response_write": False,
            "rdma_response_send": False,
        },
    ),
}


def stage_touches_cpu_map(profile_key: str) -> dict[str, bool]:
    return dict(EXPERIMENT_PROFILES[profile_key].per_request_stages)

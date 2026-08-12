"""GPU power sampling via nvidia-smi.

Real telemetry, not a projected/estimated number: this shells out to
`nvidia-smi --query-gpu=power.draw` at a fixed interval on a background
thread for the duration of a bench run. Confirmed working on this host
(GB10, `nvidia-smi --query-gpu=power.draw --format=csv,noheader` returns a
real wattage). If nvidia-smi isn't present or the query fails, this reports
that honestly (`available=False`) rather than fabricating a number — no
NIC-side (BF3/CX7) power telemetry is wired up here, since neither DOCA nor
this host exposes per-device NIC power the way nvidia-smi does for the GPU,
so total system energy figures are out of scope for this sampler.
"""

from __future__ import annotations

import shutil
import subprocess
import threading
import time
from dataclasses import dataclass


@dataclass
class EnergySample:
    available: bool
    sample_count: int = 0
    mean_watts: float = 0.0
    max_watts: float = 0.0
    joules_estimate: float = 0.0  # trapezoid-integrated over the sampling window
    note: str = ""


class GpuPowerSampler:
    """Background nvidia-smi power sampler. Usage:

        sampler = GpuPowerSampler(interval_seconds=0.5)
        sampler.start()
        ... run the workload being benchmarked ...
        sample = sampler.stop()
    """

    def __init__(self, interval_seconds: float = 0.5):
        self._interval = interval_seconds
        self._readings: list[tuple[float, float]] = []  # (timestamp, watts)
        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None
        self._nvidia_smi = shutil.which("nvidia-smi")

    def start(self) -> None:
        if self._nvidia_smi is None:
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._poll_loop, daemon=True)
        self._thread.start()

    def _poll_loop(self) -> None:
        while not self._stop_event.is_set():
            watts = self._read_power_once()
            if watts is not None:
                self._readings.append((time.monotonic(), watts))
            self._stop_event.wait(self._interval)

    def _read_power_once(self) -> float | None:
        try:
            out = subprocess.run(
                [self._nvidia_smi, "--query-gpu=power.draw", "--format=csv,noheader,nounits"],
                capture_output=True,
                text=True,
                timeout=5,
                check=True,
            )
            return float(out.stdout.strip().splitlines()[0])
        except (subprocess.SubprocessError, ValueError, IndexError, OSError):
            return None

    def stop(self) -> EnergySample:
        if self._nvidia_smi is None:
            return EnergySample(available=False, note="nvidia-smi not found on PATH")
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=self._interval + 5)
        if not self._readings:
            return EnergySample(available=False, note="no successful power readings during the run")

        watts = [w for _, w in self._readings]
        joules = 0.0
        for (t0, w0), (t1, w1) in zip(self._readings, self._readings[1:]):
            joules += (w0 + w1) / 2.0 * (t1 - t0)

        return EnergySample(
            available=True,
            sample_count=len(self._readings),
            mean_watts=sum(watts) / len(watts),
            max_watts=max(watts),
            joules_estimate=joules,
            note="GPU package power only (nvidia-smi power.draw) — no NIC-side power telemetry available on this host",
        )

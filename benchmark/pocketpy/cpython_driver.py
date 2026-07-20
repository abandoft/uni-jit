#!/usr/bin/env python3
"""Measure the shared numeric-loop workload in one CPython JIT mode."""

from __future__ import annotations

import argparse
import json
import platform
import runpy
import statistics
import struct
import sys
import time
from pathlib import Path
from typing import Callable


def checksum_bits(value: float) -> str:
    return f"0x{struct.unpack('>Q', struct.pack('>d', value))[0]:016x}"


def measure(workload: Callable[[int], float], iterations: int) -> tuple[float, str]:
    started = time.perf_counter_ns()
    result = workload(iterations)
    elapsed = time.perf_counter_ns() - started
    return elapsed / iterations, checksum_bits(result)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", type=Path, required=True)
    parser.add_argument("--expect-version", default="3.14.6")
    parser.add_argument(
        "--expect-jit", choices=("enabled", "disabled"), required=True
    )
    parser.add_argument("--warmup", type=int, default=100000)
    parser.add_argument("--iterations", type=int, default=1000000)
    parser.add_argument("--samples", type=int, default=7)
    arguments = parser.parse_args()

    if platform.python_version() != arguments.expect_version:
        parser.error(
            f"CPython {arguments.expect_version} required, got "
            f"{platform.python_version()}"
        )
    if min(arguments.warmup, arguments.iterations, arguments.samples) <= 0:
        parser.error("iteration and sample counts must be positive")
    if not arguments.workload.is_file():
        parser.error(f"workload not found: {arguments.workload}")

    jit = getattr(sys, "_jit", None)
    if jit is None or not jit.is_available():
        parser.error("this CPython executable does not include the experimental JIT")
    enabled = bool(jit.is_enabled())
    if enabled != (arguments.expect_jit == "enabled"):
        parser.error(
            f"expected JIT {arguments.expect_jit}, got "
            f"{'enabled' if enabled else 'disabled'}"
        )

    namespace = runpy.run_path(str(arguments.workload))
    workload = namespace["numeric_workload"]
    workload(arguments.warmup)

    timings: list[float] = []
    checksum = ""
    for _ in range(arguments.samples):
        current_timing, current_checksum = measure(workload, arguments.iterations)
        if checksum and current_checksum != checksum:
            raise RuntimeError("CPython benchmark samples produced different checksums")
        timings.append(current_timing)
        checksum = current_checksum

    print(
        json.dumps(
            {
                "schema": "unijit.python-numeric-loop.v2",
                "engine": "CPython",
                "mode": "jit" if enabled else "interpreter",
                "python_version": platform.python_version(),
                "implementation": platform.python_implementation(),
                "warmup_iterations": arguments.warmup,
                "measurement_iterations": arguments.iterations,
                "samples": arguments.samples,
                "median_ns": statistics.median(timings),
                "checksum": checksum,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

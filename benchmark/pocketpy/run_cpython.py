#!/usr/bin/env python3
"""Compare one Python numeric loop across PocketPy, UniJIT, and CPython."""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKLOAD = ROOT / "benchmark/pocketpy/numeric_call.py"
DRIVER = ROOT / "benchmark/pocketpy/cpython_driver.py"


def revision(path: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def benchmark_json(command: list[str], environment: dict[str, str] | None = None) -> dict[str, object]:
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
        env=environment,
    )
    output = completed.stdout.strip()
    if not output:
        raise RuntimeError(f"{' '.join(command)} produced no benchmark record")
    return json.loads(output[output.find("{") :])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--unijit", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--python-version", default="3.14.6")
    parser.add_argument("--warmup", type=int, default=100000)
    parser.add_argument("--iterations", type=int, default=1000000)
    parser.add_argument("--samples", type=int, default=7)
    arguments = parser.parse_args()

    for executable in (arguments.unijit, arguments.python):
        if not executable.is_file():
            parser.error(f"runtime not found: {executable}")
    if min(arguments.warmup, arguments.iterations, arguments.samples) <= 0:
        parser.error("iteration and sample counts must be positive")

    policy = [
        "--warmup",
        str(arguments.warmup),
        "--iterations",
        str(arguments.iterations),
        "--samples",
        str(arguments.samples),
    ]
    embedded = benchmark_json(
        [
            str(arguments.unijit),
            "--script",
            str(WORKLOAD),
            *policy,
        ]
    )
    driver = [
        str(arguments.python),
        str(DRIVER),
        "--workload",
        str(WORKLOAD),
        "--expect-version",
        arguments.python_version,
    ]
    interpreter_environment = os.environ.copy()
    interpreter_environment["PYTHON_JIT"] = "0"
    cpython = benchmark_json(
        [*driver, "--expect-jit", "disabled", *policy],
        interpreter_environment,
    )
    jit_environment = os.environ.copy()
    jit_environment["PYTHON_JIT"] = "1"
    cpython_jit = benchmark_json(
        [*driver, "--expect-jit", "enabled", *policy],
        jit_environment,
    )

    checksums = {
        str(embedded.get("checksum")),
        str(cpython.get("checksum")),
        str(cpython_jit.get("checksum")),
    }
    if len(checksums) != 1:
        raise RuntimeError("PocketPy, UniJIT, and CPython checksums differ")

    unijit_median = float(embedded["unijit_median_ns"])
    record = {
        "schema": "unijit.pocketpy-target-comparison.v1",
        "system": platform.system().lower(),
        "machine": platform.machine().lower(),
        "workload_source": str(WORKLOAD.relative_to(ROOT)),
        "measurement_boundary": "complete_numeric_loop",
        "unijit_revision": revision(ROOT),
        "pocketpy_revision": revision(ROOT / "third/pocketpy"),
        "pocketpy_and_unijit": embedded,
        "cpython": cpython,
        "cpython_jit": cpython_jit,
        "unijit_speedup_over_cpython": float(cpython["median_ns"])
        / unijit_median,
        "unijit_speedup_over_cpython_jit": float(cpython_jit["median_ns"])
        / unijit_median,
    }
    print(json.dumps(record, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

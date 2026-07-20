#!/usr/bin/env python3
"""Compare one JavaScript numeric loop across QuickJS, UniJIT, and V8."""

from __future__ import annotations

import argparse
import json
import platform
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKLOAD = ROOT / "benchmark/quickjs/numeric_call.js"


def revision(path: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def last_json(command: list[str]) -> dict[str, object]:
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    output = completed.stdout.strip()
    if not output:
        raise RuntimeError(f"{' '.join(command)} produced no benchmark record")
    document = output[output.find("{") :]
    return json.loads(document)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--unijit", type=Path, required=True)
    parser.add_argument("--node", type=Path, required=True)
    parser.add_argument("--warmup", type=int, default=100000)
    parser.add_argument("--iterations", type=int, default=1000000)
    parser.add_argument("--samples", type=int, default=7)
    arguments = parser.parse_args()

    for executable in (arguments.unijit, arguments.node):
        if not executable.is_file():
            parser.error(f"runtime not found: {executable}")
    if min(arguments.warmup, arguments.iterations, arguments.samples) <= 0:
        parser.error("iteration and sample counts must be positive")

    policy = [
        str(arguments.warmup),
        str(arguments.iterations),
        str(arguments.samples),
    ]
    embedded = last_json(
        [
            str(arguments.unijit),
            "--script",
            str(WORKLOAD),
            "--warmup",
            policy[0],
            "--iterations",
            policy[1],
            "--samples",
            policy[2],
        ]
    )
    v8 = last_json([str(arguments.node), str(WORKLOAD), *policy])
    v8_jitless = last_json(
        [str(arguments.node), "--jitless", str(WORKLOAD), *policy]
    )

    if v8.get("mode") != "jit" or v8_jitless.get("mode") != "jitless":
        raise RuntimeError("V8 benchmark modes were not applied as requested")
    checksums = {
        str(embedded.get("checksum")),
        str(v8.get("checksum")),
        str(v8_jitless.get("checksum")),
    }
    if len(checksums) != 1:
        raise RuntimeError("QuickJS, UniJIT, V8, and V8 Jitless checksums differ")

    unijit_median = float(embedded["unijit_median_ns"])
    record = {
        "schema": "unijit.quickjs-target-comparison.v1",
        "system": platform.system().lower(),
        "machine": platform.machine().lower(),
        "workload_source": str(WORKLOAD.relative_to(ROOT)),
        "measurement_boundary": "complete_numeric_loop",
        "unijit_revision": revision(ROOT),
        "quickjs_revision": revision(ROOT / "third/quickjs"),
        "quickjs_and_unijit": embedded,
        "v8_jitless": v8_jitless,
        "v8": v8,
        "unijit_speedup_over_v8_jitless": float(v8_jitless["median_ns"])
        / unijit_median,
        "unijit_speedup_over_v8": float(v8["median_ns"]) / unijit_median,
    }
    print(json.dumps(record, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

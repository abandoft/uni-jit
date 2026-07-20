#!/usr/bin/env python3
"""Run identical Lua workloads on pinned stock Lua and LuaJIT binaries."""

from __future__ import annotations

import argparse
import json
import math
import platform
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def revision(path: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def run_engine(
    executable: Path, script: Path, warmup: int, iterations: int, samples: int
) -> dict[str, object]:
    completed = subprocess.run(
        [
            str(executable),
            str(script),
            str(warmup),
            str(iterations),
            str(samples),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError(f"{executable} produced no benchmark record")
    return json.loads(lines[-1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lua", type=Path, required=True)
    parser.add_argument("--luajit", type=Path, required=True)
    parser.add_argument(
        "--script", type=Path, default=ROOT / "benchmark/lua/numeric_recurrence.lua"
    )
    parser.add_argument("--warmup", type=int, default=100000)
    parser.add_argument("--iterations", type=int, default=1000000)
    parser.add_argument("--samples", type=int, default=7)
    arguments = parser.parse_args()

    for executable in (arguments.lua, arguments.luajit):
        if not executable.is_file():
            parser.error(f"runtime not found: {executable}")
    script = arguments.script.resolve()
    if not script.is_file():
        parser.error(f"workload not found: {script}")
    if min(arguments.warmup, arguments.iterations, arguments.samples) <= 0:
        parser.error("iteration and sample counts must be positive")

    stock = run_engine(
        arguments.lua,
        script,
        arguments.warmup,
        arguments.iterations,
        arguments.samples,
    )
    luajit = run_engine(
        arguments.luajit,
        script,
        arguments.warmup,
        arguments.iterations,
        arguments.samples,
    )
    if not math.isclose(
        float(stock["checksum"]),
        float(luajit["checksum"]),
        rel_tol=1e-12,
        abs_tol=1e-9,
    ):
        raise RuntimeError("stock Lua and LuaJIT checksums differ")

    record = {
        "schema": "unijit.lua-reference-comparison.v1",
        "system": platform.system().lower(),
        "machine": platform.machine().lower(),
        "workload_source": str(script.relative_to(ROOT)),
        "lua_revision": revision(ROOT / "third/lua"),
        "luajit_revision": revision(ROOT / "third/luajit"),
        "stock_lua": stock,
        "luajit": luajit,
        "luajit_speedup_over_stock": float(stock["median_ns"])
        / float(luajit["median_ns"]),
    }
    print(json.dumps(record, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

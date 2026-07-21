#!/usr/bin/env python3
"""Compare one numeric Lua workload across stock Lua, UniJIT, and LuaJIT."""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def revision(path: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def run_engine(
    executable: Path,
    script: Path,
    warmup: int,
    iterations: int,
    samples: int,
    mode: str,
) -> dict[str, object]:
    completed = subprocess.run(
        [
            str(executable),
            str(script),
            str(warmup),
            str(iterations),
            str(samples),
            mode,
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError(f"{executable} produced no benchmark record")
    return json.loads(lines[-1])


def aggregate_records(
    records: list[dict[str, object]], engine: str
) -> dict[str, object]:
    if not records:
        raise RuntimeError(f"{engine} produced no benchmark trials")
    invariant_fields = (
        "schema",
        "workload",
        "engine",
        "warmup_iterations",
        "measurement_iterations",
        "inner_loop_iterations",
        "samples",
        "checksum",
    )
    first = records[0]
    for record in records[1:]:
        for field in invariant_fields:
            if record.get(field) != first.get(field):
                raise RuntimeError(
                    f"{engine} benchmark trials disagree on {field}"
                )
    medians = [float(record["median_ns"]) for record in records]
    if any(value <= 0 for value in medians):
        raise RuntimeError(f"{engine} benchmark trial is not positive")
    aggregate = dict(first)
    aggregate["median_ns"] = statistics.median(medians)
    aggregate["trials"] = len(records)
    aggregate["samples_per_trial"] = first["samples"]
    aggregate["trial_median_ns"] = medians
    return aggregate


def rotated_order(
    engines: list[tuple[str, Path, str]], trial: int
) -> list[tuple[str, Path, str]]:
    rotation = trial % len(engines)
    return engines[rotation:] + engines[:rotation]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lua", type=Path, required=True)
    parser.add_argument("--unijit", type=Path, required=True)
    parser.add_argument("--luajit", type=Path, required=True)
    parser.add_argument(
        "--script", type=Path, default=ROOT / "benchmark/lua/integer_call.lua"
    )
    parser.add_argument("--warmup", type=int, default=100000)
    parser.add_argument("--iterations", type=int, default=1000000)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--trials", type=int, default=1)
    arguments = parser.parse_args()

    for executable in (arguments.lua, arguments.unijit, arguments.luajit):
        if not executable.is_file():
            parser.error(f"runtime not found: {executable}")
    script = arguments.script.resolve()
    if not script.is_file():
        parser.error(f"workload not found: {script}")
    if min(
        arguments.warmup,
        arguments.iterations,
        arguments.samples,
        arguments.trials,
    ) <= 0:
        parser.error("iteration, sample, and trial counts must be positive")
    if arguments.trials != 1 and arguments.trials % 3 != 0:
        parser.error("multiple benchmark trials must form complete groups of three")

    engines = [
        ("stock_lua", arguments.lua, "reference"),
        ("unijit", arguments.unijit, "unijit"),
        ("luajit", arguments.luajit, "reference"),
    ]
    trial_records: dict[str, list[dict[str, object]]] = {
        name: [] for name, _, _ in engines
    }
    trial_orders: list[list[str]] = []
    for trial in range(arguments.trials):
        order = rotated_order(engines, trial)
        trial_orders.append([name for name, _, _ in order])
        for name, executable, mode in order:
            trial_records[name].append(
                run_engine(
                    executable,
                    script,
                    arguments.warmup,
                    arguments.iterations,
                    arguments.samples,
                    mode,
                )
            )

    stock = aggregate_records(trial_records["stock_lua"], "stock Lua")
    unijit = aggregate_records(trial_records["unijit"], "UniJIT")
    luajit = aggregate_records(trial_records["luajit"], "LuaJIT")

    checksums = {result["checksum"] for result in (stock, unijit, luajit)}
    if len(checksums) != 1:
        raise RuntimeError("stock Lua, UniJIT, and LuaJIT checksums differ")

    stock_median = float(stock["median_ns"])
    unijit_median = float(unijit["median_ns"])
    luajit_median = float(luajit["median_ns"])
    measurement_boundary = (
        "complete_numeric_loop"
        if stock.get("workload")
        in {
            "integer_loop",
            "integer_parameter_loop",
            "integer_guarded_loop",
            "integer_conditional_loop",
            "float_parameter_loop",
        }
        else "language_call"
    )
    record = {
        "schema": "unijit.lua-integer-comparison.v1",
        "system": platform.system().lower(),
        "machine": platform.machine().lower(),
        "workload_source": str(script.relative_to(ROOT)),
        "measurement_boundary": measurement_boundary,
        "unijit_revision": revision(ROOT),
        "lua_revision": revision(ROOT / "third/lua"),
        "luajit_revision": revision(ROOT / "third/luajit"),
        "balanced_trials": arguments.trials,
        "trial_orders": trial_orders,
        "stock_lua": stock,
        "unijit": unijit,
        "luajit": luajit,
        "unijit_speedup_over_stock": stock_median / unijit_median,
        "luajit_speedup_over_stock": stock_median / luajit_median,
        "unijit_speedup_over_luajit": luajit_median / unijit_median,
    }
    print(json.dumps(record, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

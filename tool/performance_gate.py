#!/usr/bin/env python3
"""Enforce commercial target ratios from retained benchmark records."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


class GateError(RuntimeError):
    """Raised when a benchmark record cannot satisfy its target contract."""


def number_at(document: dict[str, Any], *path: str) -> float:
    value: Any = document
    for component in path:
        if not isinstance(value, dict) or component not in value:
            raise GateError(f"benchmark record is missing {'.'.join(path)}")
        value = value[component]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise GateError(f"benchmark field {'.'.join(path)} is not numeric")
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise GateError(
            f"benchmark field {'.'.join(path)} must be finite and positive"
        )
    return result


def evaluate(
    target: str, document: dict[str, Any], thresholds: dict[str, float]
) -> dict[str, Any]:
    if document.get("measurement_boundary") != "complete_numeric_loop":
        raise GateError("performance gates require the complete numeric-loop boundary")

    if target == "quickjs":
        expected_schema = "unijit.quickjs-target-comparison.v1"
        observed = {
            "stock_quickjs": number_at(
                document, "quickjs_and_unijit", "speedup"
            ),
            "v8_jitless": number_at(
                document, "unijit_speedup_over_v8_jitless"
            ),
        }
    elif target == "pocketpy":
        expected_schema = "unijit.pocketpy-target-comparison.v1"
        observed = {
            "stock_pocketpy": number_at(
                document, "pocketpy_and_unijit", "speedup"
            ),
            "cpython": number_at(document, "unijit_speedup_over_cpython"),
            "cpython_jit": number_at(
                document, "unijit_speedup_over_cpython_jit"
            ),
        }
    else:
        raise GateError(f"unsupported performance target: {target}")

    if document.get("schema") != expected_schema:
        raise GateError(
            f"expected {expected_schema}, found {document.get('schema')!r}"
        )
    if set(observed) != set(thresholds):
        raise GateError("performance-gate threshold set does not match the target")

    requirements: dict[str, dict[str, float]] = {}
    failures: list[str] = []
    for competitor, minimum in thresholds.items():
        if not math.isfinite(minimum) or minimum <= 0:
            raise GateError(f"minimum ratio for {competitor} must be positive")
        ratio = observed[competitor]
        requirements[competitor] = {"minimum": minimum, "observed": ratio}
        if ratio < minimum:
            failures.append(
                f"{competitor} ratio {ratio:.6f} is below {minimum:.6f}"
            )
    if failures:
        raise GateError("; ".join(failures))

    return {
        "schema": "unijit.performance-gate.v1",
        "target": target,
        "source_schema": expected_schema,
        "measurement_boundary": "complete_numeric_loop",
        "requirements": requirements,
    }


def load_record(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GateError(f"unable to read benchmark record {path}: {error}") from error
    if not isinstance(document, dict):
        raise GateError("benchmark record root must be a JSON object")
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="target", required=True)

    quickjs = subparsers.add_parser("quickjs")
    quickjs.add_argument("record", type=Path)
    quickjs.add_argument("--minimum-stock-speedup", type=float, default=1.25)
    quickjs.add_argument(
        "--minimum-v8-jitless-speedup", type=float, default=1.10
    )

    pocketpy = subparsers.add_parser("pocketpy")
    pocketpy.add_argument("record", type=Path)
    pocketpy.add_argument("--minimum-stock-speedup", type=float, default=1.25)
    pocketpy.add_argument("--minimum-cpython-speedup", type=float, default=1.10)
    pocketpy.add_argument(
        "--minimum-cpython-jit-speedup", type=float, default=1.10
    )

    arguments = parser.parse_args()
    try:
        document = load_record(arguments.record)
        if arguments.target == "quickjs":
            thresholds = {
                "stock_quickjs": arguments.minimum_stock_speedup,
                "v8_jitless": arguments.minimum_v8_jitless_speedup,
            }
        else:
            thresholds = {
                "stock_pocketpy": arguments.minimum_stock_speedup,
                "cpython": arguments.minimum_cpython_speedup,
                "cpython_jit": arguments.minimum_cpython_jit_speedup,
            }
        result = evaluate(arguments.target, document, thresholds)
    except GateError as error:
        print(f"performance gate failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

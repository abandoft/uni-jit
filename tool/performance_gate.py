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

    if target == "lua":
        expected_schema = "unijit.lua-integer-comparison.v1"
        observed = {
            "stock_lua": number_at(document, "unijit_speedup_over_stock"),
            "luajit": number_at(document, "unijit_speedup_over_luajit"),
        }
    elif target == "quickjs":
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
        "passed": True,
        "source_schema": expected_schema,
        "measurement_boundary": "complete_numeric_loop",
        "requirements": requirements,
    }


def evaluate_cfg_float64(
    document: dict[str, Any], minimum_speedup: float, maximum_code_bytes: float
) -> dict[str, Any]:
    expected_schema = "unijit.cfg-float64-benchmark.v1"
    if document.get("schema") != expected_schema:
        raise GateError(
            f"expected {expected_schema}, found {document.get('schema')!r}"
        )
    if document.get("benchmark") != "float64_register_residency":
        raise GateError("CFG performance gate requires the register-residency workload")
    if document.get("measurement_boundary") != "native_cfg_loop_iteration":
        raise GateError("CFG performance gate requires the native loop-iteration boundary")
    if document.get("architecture") not in {"aarch64", "x86_64", "riscv64"}:
        raise GateError("CFG performance record has an unsupported architecture")

    samples = number_at(document, "samples")
    loop_iterations = number_at(document, "loop_iterations")
    speedup = number_at(document, "speedup")
    code_bytes = number_at(document, "native_code_bytes")
    number_at(document, "native_median_ns_per_loop_iteration")
    number_at(document, "interpreter_median_ns_per_loop_iteration")
    if samples < 7:
        raise GateError("CFG performance gate requires at least seven samples")
    if loop_iterations < 1000:
        raise GateError("CFG performance gate requires at least 1,000 loop iterations")
    if not math.isfinite(minimum_speedup) or minimum_speedup <= 0:
        raise GateError("minimum CFG speedup must be finite and positive")
    if not math.isfinite(maximum_code_bytes) or maximum_code_bytes <= 0:
        raise GateError("maximum CFG code size must be finite and positive")

    failures: list[str] = []
    if speedup < minimum_speedup:
        failures.append(
            f"CFG speedup {speedup:.6f} is below {minimum_speedup:.6f}"
        )
    if code_bytes > maximum_code_bytes:
        failures.append(
            f"CFG native code size {code_bytes:.0f} exceeds "
            f"{maximum_code_bytes:.0f} bytes"
        )
    if failures:
        raise GateError("; ".join(failures))

    return {
        "schema": "unijit.performance-gate.v1",
        "target": "cfg-float64",
        "passed": True,
        "source_schema": expected_schema,
        "measurement_boundary": "native_cfg_loop_iteration",
        "requirements": {
            "interpreter_speedup": {
                "minimum": minimum_speedup,
                "observed": speedup,
            },
            "native_code_bytes": {
                "maximum": maximum_code_bytes,
                "observed": code_bytes,
            },
        },
    }


def evaluate_cfg_simd(
    document: dict[str, Any],
    minimum_scalar_speedup: float,
    minimum_interpreter_speedup: float,
    maximum_code_bytes: float,
) -> dict[str, Any]:
    expected_schema = "unijit.cfg-simd-benchmark.v1"
    if document.get("schema") != expected_schema:
        raise GateError(
            f"expected {expected_schema}, found {document.get('schema')!r}"
        )
    if document.get("benchmark") != "strict_i32x4_recurrence":
        raise GateError("CFG SIMD gate requires the strict I32x4 recurrence")
    if document.get("measurement_boundary") != "native_cfg_loop_iteration":
        raise GateError("CFG SIMD gate requires the native loop-iteration boundary")

    architecture = document.get("architecture")
    if architecture not in {"aarch64", "x86_64", "riscv64"}:
        raise GateError("CFG SIMD record has an unsupported architecture")
    expected_lowering = "scalarized" if architecture == "riscv64" else "native"
    if document.get("lowering_mode") != expected_lowering:
        raise GateError(
            f"CFG SIMD {architecture} record must report {expected_lowering} lowering"
        )

    samples = number_at(document, "samples")
    loop_iterations = number_at(document, "loop_iterations")
    warmup_invocations = number_at(document, "warmup_invocations")
    measurement_invocations = number_at(document, "measurement_invocations")
    vector_bits = number_at(document, "vector_bits")
    lanes = number_at(document, "lanes")
    scalar_speedup = number_at(document, "vector_speedup_over_scalar")
    interpreter_speedup = number_at(
        document, "vector_speedup_over_interpreter"
    )
    code_bytes = number_at(document, "vector_native_code_bytes")
    number_at(document, "vector_native_median_ns_per_loop_iteration")
    number_at(document, "scalar_native_median_ns_per_loop_iteration")
    number_at(document, "vector_interpreter_median_ns_per_loop_iteration")
    if samples < 7:
        raise GateError("CFG SIMD gate requires at least seven samples")
    if loop_iterations != 1000 or measurement_invocations != 500:
        raise GateError(
            "CFG SIMD gate requires exactly 1,000 loop iterations and "
            "500 measured invocations"
        )
    if warmup_invocations < 100:
        raise GateError("CFG SIMD gate requires at least 100 warmup invocations")
    if vector_bits != 128 or lanes != 4:
        raise GateError("CFG SIMD gate requires the fixed 128-bit I32x4 shape")
    if document.get("checksum") != "0x15424b4ac53c353c":
        raise GateError("CFG SIMD record has an unexpected deterministic checksum")
    for name, threshold in {
        "minimum scalar speedup": minimum_scalar_speedup,
        "minimum interpreter speedup": minimum_interpreter_speedup,
        "maximum code size": maximum_code_bytes,
    }.items():
        if not math.isfinite(threshold) or threshold <= 0:
            raise GateError(f"{name} must be finite and positive")

    failures: list[str] = []
    if scalar_speedup < minimum_scalar_speedup:
        failures.append(
            f"CFG SIMD scalar speedup {scalar_speedup:.6f} is below "
            f"{minimum_scalar_speedup:.6f}"
        )
    if interpreter_speedup < minimum_interpreter_speedup:
        failures.append(
            f"CFG SIMD interpreter speedup {interpreter_speedup:.6f} is below "
            f"{minimum_interpreter_speedup:.6f}"
        )
    if code_bytes > maximum_code_bytes:
        failures.append(
            f"CFG SIMD native code size {code_bytes:.0f} exceeds "
            f"{maximum_code_bytes:.0f} bytes"
        )
    if failures:
        raise GateError("; ".join(failures))

    return {
        "schema": "unijit.performance-gate.v1",
        "target": "cfg-simd",
        "passed": True,
        "source_schema": expected_schema,
        "measurement_boundary": "native_cfg_loop_iteration",
        "architecture": architecture,
        "lowering_mode": expected_lowering,
        "requirements": {
            "scalar_speedup": {
                "minimum": minimum_scalar_speedup,
                "observed": scalar_speedup,
            },
            "interpreter_speedup": {
                "minimum": minimum_interpreter_speedup,
                "observed": interpreter_speedup,
            },
            "native_code_bytes": {
                "maximum": maximum_code_bytes,
                "observed": code_bytes,
            },
        },
    }


def evaluate_patch_cell(
    document: dict[str, Any],
    maximum_overhead_ratio: float,
    maximum_code_bytes: float,
) -> dict[str, Any]:
    expected_schema = "unijit.patch-cell-benchmark.v1"
    if document.get("schema") != expected_schema:
        raise GateError(
            f"expected {expected_schema}, found {document.get('schema')!r}"
        )
    if document.get("benchmark") != "managed_acquire_load":
        raise GateError("patch-cell gate requires the managed acquire-load workload")
    if document.get("measurement_boundary") != "managed_compiled_invocation":
        raise GateError(
            "patch-cell gate requires the complete managed invocation boundary"
        )
    if document.get("architecture") not in {"aarch64", "x86_64", "riscv64"}:
        raise GateError("patch-cell record has an unsupported architecture")

    samples = number_at(document, "samples")
    warmup_iterations = number_at(document, "warmup_iterations")
    measurement_iterations = number_at(document, "measurement_iterations")
    reported_overhead_ratio = number_at(document, "managed_overhead_ratio")
    code_bytes = number_at(document, "patch_native_code_bytes")
    number_at(document, "constant_native_code_bytes")
    constant_latency = number_at(document, "constant_managed_median_ns")
    patch_latency = number_at(document, "patch_managed_median_ns")
    number_at(document, "mutation_round_trip_median_ns")
    overhead_ratio = patch_latency / constant_latency
    if not math.isclose(
        reported_overhead_ratio, overhead_ratio, rel_tol=0.01, abs_tol=0.01
    ):
        raise GateError("patch-cell overhead ratio is inconsistent with latencies")
    if samples < 7:
        raise GateError("patch-cell gate requires at least seven samples")
    if warmup_iterations < 10000 or measurement_iterations < 200000:
        raise GateError(
            "patch-cell gate requires 10,000 warmups and 200,000 measured invocations"
        )
    for name, threshold in {
        "maximum managed overhead": maximum_overhead_ratio,
        "maximum code size": maximum_code_bytes,
    }.items():
        if not math.isfinite(threshold) or threshold <= 0:
            raise GateError(f"{name} must be finite and positive")

    failures: list[str] = []
    if overhead_ratio > maximum_overhead_ratio:
        failures.append(
            f"patch-cell managed overhead {overhead_ratio:.6f} exceeds "
            f"{maximum_overhead_ratio:.6f}"
        )
    if code_bytes > maximum_code_bytes:
        failures.append(
            f"patch-cell native code size {code_bytes:.0f} exceeds "
            f"{maximum_code_bytes:.0f} bytes"
        )
    if failures:
        raise GateError("; ".join(failures))

    return {
        "schema": "unijit.performance-gate.v1",
        "target": "patch-cell",
        "passed": True,
        "source_schema": expected_schema,
        "measurement_boundary": "managed_compiled_invocation",
        "requirements": {
            "managed_overhead_ratio": {
                "maximum": maximum_overhead_ratio,
                "observed": overhead_ratio,
            },
            "native_code_bytes": {
                "maximum": maximum_code_bytes,
                "observed": code_bytes,
            },
        },
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

    lua = subparsers.add_parser("lua")
    lua.add_argument("record", type=Path)
    lua.add_argument("--minimum-stock-speedup", type=float, default=1.25)
    lua.add_argument("--minimum-luajit-speedup", type=float, default=1.10)

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

    cfg_float64 = subparsers.add_parser("cfg-float64")
    cfg_float64.add_argument("record", type=Path)
    cfg_float64.add_argument("--minimum-speedup", type=float, default=5.0)
    cfg_float64.add_argument(
        "--maximum-native-code-bytes", type=float, default=400.0
    )

    cfg_simd = subparsers.add_parser("cfg-simd")
    cfg_simd.add_argument("record", type=Path)
    cfg_simd.add_argument("--minimum-scalar-speedup", type=float, default=1.10)
    cfg_simd.add_argument(
        "--minimum-interpreter-speedup", type=float, default=10.0
    )
    cfg_simd.add_argument(
        "--maximum-native-code-bytes", type=float, default=1024.0
    )

    patch_cell = subparsers.add_parser("patch-cell")
    patch_cell.add_argument("record", type=Path)
    patch_cell.add_argument(
        "--maximum-managed-overhead", type=float, default=2.5
    )
    patch_cell.add_argument(
        "--maximum-native-code-bytes", type=float, default=128.0
    )

    arguments = parser.parse_args()
    try:
        document = load_record(arguments.record)
        if arguments.target == "cfg-float64":
            result = evaluate_cfg_float64(
                document,
                arguments.minimum_speedup,
                arguments.maximum_native_code_bytes,
            )
        elif arguments.target == "cfg-simd":
            result = evaluate_cfg_simd(
                document,
                arguments.minimum_scalar_speedup,
                arguments.minimum_interpreter_speedup,
                arguments.maximum_native_code_bytes,
            )
        elif arguments.target == "patch-cell":
            result = evaluate_patch_cell(
                document,
                arguments.maximum_managed_overhead,
                arguments.maximum_native_code_bytes,
            )
        elif arguments.target == "lua":
            thresholds = {
                "stock_lua": arguments.minimum_stock_speedup,
                "luajit": arguments.minimum_luajit_speedup,
            }
        elif arguments.target == "quickjs":
            thresholds = {
                "stock_quickjs": arguments.minimum_stock_speedup,
                "v8_jitless": arguments.minimum_v8_jitless_speedup,
            }
        elif arguments.target == "pocketpy":
            thresholds = {
                "stock_pocketpy": arguments.minimum_stock_speedup,
                "cpython": arguments.minimum_cpython_speedup,
                "cpython_jit": arguments.minimum_cpython_jit_speedup,
            }
        if arguments.target not in {
            "cfg-float64",
            "cfg-simd",
            "patch-cell",
        }:
            result = evaluate(arguments.target, document, thresholds)
    except GateError as error:
        print(
            json.dumps(
                {
                    "schema": "unijit.performance-gate.v1",
                    "target": arguments.target,
                    "passed": False,
                    "error": str(error),
                },
                indent=2,
                sort_keys=True,
            )
        )
        print(f"performance gate failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

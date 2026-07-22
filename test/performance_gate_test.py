#!/usr/bin/env python3

import math
import sys
import unittest
from pathlib import Path


sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tool"))

from performance_gate import (  # noqa: E402
    GateError,
    evaluate,
    evaluate_cfg_float64,
    evaluate_cfg_simd,
    evaluate_patch_cell,
)


class PerformanceGateTest(unittest.TestCase):
    def patch_cell_record(self) -> dict[str, object]:
        return {
            "schema": "unijit.patch-cell-benchmark.v1",
            "benchmark": "managed_acquire_load",
            "measurement_boundary": "managed_compiled_invocation",
            "architecture": "x86-64",
            "warmup_iterations": 10000,
            "measurement_iterations": 200000,
            "samples": 7,
            "constant_native_code_bytes": 16,
            "patch_native_code_bytes": 42,
            "constant_managed_median_ns": 8.0,
            "patch_managed_median_ns": 10.0,
            "managed_overhead_ratio": 1.25,
            "mutation_round_trip_median_ns": 7.0,
        }

    def test_patch_cell_target_passes(self) -> None:
        result = evaluate_patch_cell(self.patch_cell_record(), 2.5, 128.0)
        self.assertEqual(result["target"], "patch-cell")
        self.assertTrue(result["passed"])

    def test_patch_cell_target_rejects_managed_overhead(self) -> None:
        record = self.patch_cell_record()
        record["patch_managed_median_ns"] = 20.08
        record["managed_overhead_ratio"] = 2.51
        with self.assertRaisesRegex(GateError, "managed overhead"):
            evaluate_patch_cell(record, 2.5, 128.0)

    def test_patch_cell_target_rejects_code_growth(self) -> None:
        record = self.patch_cell_record()
        record["patch_native_code_bytes"] = 129
        with self.assertRaisesRegex(GateError, "code size"):
            evaluate_patch_cell(record, 2.5, 128.0)

    def test_patch_cell_target_rejects_short_sampling(self) -> None:
        record = self.patch_cell_record()
        record["samples"] = 3
        with self.assertRaisesRegex(GateError, "seven samples"):
            evaluate_patch_cell(record, 2.5, 128.0)

    def cfg_record(self) -> dict[str, object]:
        return {
            "schema": "unijit.cfg-float64-benchmark.v1",
            "benchmark": "float64_register_residency",
            "measurement_boundary": "native_cfg_loop_iteration",
            "architecture": "x86_64",
            "loop_iterations": 1000,
            "samples": 7,
            "native_code_bytes": 363,
            "native_median_ns_per_loop_iteration": 3.5,
            "interpreter_median_ns_per_loop_iteration": 55.0,
            "speedup": 15.7,
        }

    def test_cfg_float64_target_passes(self) -> None:
        result = evaluate_cfg_float64(self.cfg_record(), 5.0, 400.0)
        self.assertEqual(result["target"], "cfg-float64")
        self.assertTrue(result["passed"])

    def test_cfg_float64_target_rejects_code_growth(self) -> None:
        record = self.cfg_record()
        record["native_code_bytes"] = 401
        with self.assertRaisesRegex(GateError, "code size"):
            evaluate_cfg_float64(record, 5.0, 400.0)

    def test_cfg_float64_target_rejects_slow_native_code(self) -> None:
        record = self.cfg_record()
        record["speedup"] = 4.99
        with self.assertRaisesRegex(GateError, "speedup"):
            evaluate_cfg_float64(record, 5.0, 400.0)

    def test_cfg_float64_target_rejects_short_sampling(self) -> None:
        record = self.cfg_record()
        record["samples"] = 3
        with self.assertRaisesRegex(GateError, "seven samples"):
            evaluate_cfg_float64(record, 5.0, 400.0)

    def cfg_simd_record(self) -> dict[str, object]:
        return {
            "schema": "unijit.cfg-simd-benchmark.v1",
            "benchmark": "strict_i32x4_recurrence",
            "measurement_boundary": "native_cfg_loop_iteration",
            "architecture": "x86_64",
            "lowering_mode": "native",
            "vector_bits": 128,
            "lanes": 4,
            "loop_iterations": 1000,
            "warmup_invocations": 100,
            "measurement_invocations": 500,
            "samples": 7,
            "vector_native_code_bytes": 725,
            "vector_native_median_ns_per_loop_iteration": 0.8,
            "scalar_native_median_ns_per_loop_iteration": 4.4,
            "vector_interpreter_median_ns_per_loop_iteration": 70.0,
            "vector_speedup_over_scalar": 5.5,
            "vector_speedup_over_interpreter": 87.5,
            "checksum": "0x15424b4ac53c353c",
        }

    def test_cfg_simd_target_passes(self) -> None:
        result = evaluate_cfg_simd(self.cfg_simd_record(), 1.10, 10.0, 1024.0)
        self.assertEqual(result["target"], "cfg-simd")
        self.assertEqual(result["lowering_mode"], "native")
        self.assertTrue(result["passed"])

    def test_cfg_simd_target_accepts_riscv_scalarization(self) -> None:
        record = self.cfg_simd_record()
        record["architecture"] = "riscv64"
        record["lowering_mode"] = "scalarized"
        result = evaluate_cfg_simd(record, 1.10, 10.0, 1024.0)
        self.assertEqual(result["lowering_mode"], "scalarized")

    def test_cfg_simd_target_rejects_wrong_lowering_mode(self) -> None:
        record = self.cfg_simd_record()
        record["architecture"] = "riscv64"
        with self.assertRaisesRegex(GateError, "scalarized lowering"):
            evaluate_cfg_simd(record, 1.10, 10.0, 1024.0)

    def test_cfg_simd_target_rejects_scalar_regression(self) -> None:
        record = self.cfg_simd_record()
        record["vector_speedup_over_scalar"] = 1.09
        with self.assertRaisesRegex(GateError, "scalar speedup"):
            evaluate_cfg_simd(record, 1.10, 10.0, 1024.0)

    def test_cfg_simd_target_rejects_interpreter_regression(self) -> None:
        record = self.cfg_simd_record()
        record["vector_speedup_over_interpreter"] = 9.99
        with self.assertRaisesRegex(GateError, "interpreter speedup"):
            evaluate_cfg_simd(record, 1.10, 10.0, 1024.0)

    def test_cfg_simd_target_rejects_code_growth(self) -> None:
        record = self.cfg_simd_record()
        record["vector_native_code_bytes"] = 1025
        with self.assertRaisesRegex(GateError, "code size"):
            evaluate_cfg_simd(record, 1.10, 10.0, 1024.0)

    def test_cfg_simd_target_rejects_wrong_checksum(self) -> None:
        record = self.cfg_simd_record()
        record["checksum"] = "0x0"
        with self.assertRaisesRegex(GateError, "deterministic checksum"):
            evaluate_cfg_simd(record, 1.10, 10.0, 1024.0)

    def test_lua_target_passes(self) -> None:
        result = evaluate(
            "lua",
            {
                "schema": "unijit.lua-integer-comparison.v1",
                "measurement_boundary": "complete_numeric_loop",
                "unijit_speedup_over_stock": 4.0,
                "unijit_speedup_over_luajit": 1.5,
            },
            {"stock_lua": 1.25, "luajit": 1.10},
        )
        self.assertEqual(result["target"], "lua")
        self.assertTrue(result["passed"])

    def test_lua_target_fails_below_luajit(self) -> None:
        with self.assertRaisesRegex(GateError, "luajit ratio"):
            evaluate(
                "lua",
                {
                    "schema": "unijit.lua-integer-comparison.v1",
                    "measurement_boundary": "complete_numeric_loop",
                    "unijit_speedup_over_stock": 4.0,
                    "unijit_speedup_over_luajit": 0.95,
                },
                {"stock_lua": 1.25, "luajit": 1.10},
            )

    def test_quickjs_target_passes(self) -> None:
        result = evaluate(
            "quickjs",
            {
                "schema": "unijit.quickjs-target-comparison.v1",
                "measurement_boundary": "complete_numeric_loop",
                "quickjs_and_unijit": {"speedup": 3.0},
                "unijit_speedup_over_v8_jitless": 2.0,
            },
            {"stock_quickjs": 1.25, "v8_jitless": 1.10},
        )
        self.assertEqual(result["schema"], "unijit.performance-gate.v1")
        self.assertEqual(result["target"], "quickjs")

    def test_quickjs_target_fails_below_jitless(self) -> None:
        with self.assertRaisesRegex(GateError, "v8_jitless ratio"):
            evaluate(
                "quickjs",
                {
                    "schema": "unijit.quickjs-target-comparison.v1",
                    "measurement_boundary": "complete_numeric_loop",
                    "quickjs_and_unijit": {"speedup": 3.0},
                    "unijit_speedup_over_v8_jitless": 0.9,
                },
                {"stock_quickjs": 1.25, "v8_jitless": 1.10},
            )

    def test_pocketpy_target_passes(self) -> None:
        result = evaluate(
            "pocketpy",
            {
                "schema": "unijit.pocketpy-target-comparison.v1",
                "measurement_boundary": "complete_numeric_loop",
                "pocketpy_and_unijit": {"speedup": 4.0},
                "unijit_speedup_over_cpython": 2.5,
                "unijit_speedup_over_cpython_jit": 2.0,
            },
            {
                "stock_pocketpy": 1.25,
                "cpython": 1.10,
                "cpython_jit": 1.10,
            },
        )
        self.assertEqual(result["target"], "pocketpy")

    def test_nonfinite_observation_is_rejected(self) -> None:
        with self.assertRaisesRegex(GateError, "finite and positive"):
            evaluate(
                "quickjs",
                {
                    "schema": "unijit.quickjs-target-comparison.v1",
                    "measurement_boundary": "complete_numeric_loop",
                    "quickjs_and_unijit": {"speedup": math.inf},
                    "unijit_speedup_over_v8_jitless": 2.0,
                },
                {"stock_quickjs": 1.25, "v8_jitless": 1.10},
            )

    def test_narrow_measurement_boundary_is_rejected(self) -> None:
        with self.assertRaisesRegex(GateError, "complete numeric-loop"):
            evaluate(
                "quickjs",
                {
                    "schema": "unijit.quickjs-target-comparison.v1",
                    "measurement_boundary": "native_call_only",
                },
                {"stock_quickjs": 1.25, "v8_jitless": 1.10},
            )


if __name__ == "__main__":
    unittest.main()

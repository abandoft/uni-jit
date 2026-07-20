#!/usr/bin/env python3

import math
import sys
import unittest
from pathlib import Path


sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tool"))

from performance_gate import GateError, evaluate  # noqa: E402


class PerformanceGateTest(unittest.TestCase):
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

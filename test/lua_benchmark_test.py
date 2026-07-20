#!/usr/bin/env python3

import sys
import unittest
from pathlib import Path


sys.dont_write_bytecode = True
sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / "benchmark" / "lua")
)

from run_integer import aggregate_records, rotated_order  # noqa: E402


def record(median_ns: float, checksum: int = 42) -> dict[str, object]:
    return {
        "schema": "unijit.lua-benchmark.v1",
        "workload": "integer_loop",
        "engine": "fixture",
        "warmup_iterations": 10,
        "measurement_iterations": 100,
        "inner_loop_iterations": 1000,
        "samples": 7,
        "median_ns": median_ns,
        "checksum": checksum,
    }


class LuaBenchmarkTest(unittest.TestCase):
    def test_aggregates_three_trial_medians(self) -> None:
        result = aggregate_records(
            [record(0.8), record(1.2), record(1.0)], "fixture"
        )
        self.assertEqual(result["median_ns"], 1.0)
        self.assertEqual(result["trials"], 3)
        self.assertEqual(result["samples_per_trial"], 7)
        self.assertEqual(result["trial_median_ns"], [0.8, 1.2, 1.0])

    def test_rejects_semantically_different_trials(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "checksum"):
            aggregate_records([record(1.0), record(1.0, 43)], "fixture")

    def test_rotates_every_engine_through_every_position(self) -> None:
        engines = [
            ("stock", Path("stock"), "reference"),
            ("unijit", Path("unijit"), "unijit"),
            ("luajit", Path("luajit"), "reference"),
        ]
        orders = [rotated_order(engines, trial) for trial in range(3)]
        for position in range(3):
            self.assertEqual(
                {orders[trial][position][0] for trial in range(3)},
                {"stock", "unijit", "luajit"},
            )


if __name__ == "__main__":
    unittest.main()

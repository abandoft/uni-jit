#!/usr/bin/env python3

import sys
import unittest
from pathlib import Path


sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from release import ReleaseError, Version  # noqa: E402


class VersionTest(unittest.TestCase):
    def test_patch_increment(self) -> None:
        self.assertEqual(str(Version.parse("1.1.8").next()), "1.1.9")

    def test_patch_carry(self) -> None:
        self.assertEqual(str(Version.parse("1.1.9").next()), "1.2.0")

    def test_minor_carry(self) -> None:
        self.assertEqual(str(Version.parse("1.9.9").next()), "2.0.0")

    def test_multi_digit_minor_is_rejected(self) -> None:
        with self.assertRaises(ReleaseError):
            Version.parse("1.10.0")


if __name__ == "__main__":
    unittest.main()

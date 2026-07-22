#!/usr/bin/env python3

import pathlib
import sys
import unittest


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPOSITORY_ROOT / "tool"))

import c_api_exports  # noqa: E402


class CApiExportsTest(unittest.TestCase):
    def test_committed_manifest_is_versioned_and_unique(self) -> None:
        manifest = c_api_exports.load_manifest(
            REPOSITORY_ROOT / "tool" / "c_api_v1_symbols.txt"
        )
        self.assertEqual(67, len(manifest))
        self.assertTrue(all(symbol.startswith("unijit_v1_") for symbol in manifest))

    def test_parses_unix_and_windows_export_formats(self) -> None:
        expected = {"unijit_v1_alpha", "unijit_v1_beta"}
        output = "\n".join(
            [
                "0000000000001000 T _unijit_v1_alpha",
                "      2    1 00002000 unijit_v1_beta",
            ]
        )
        self.assertEqual((set(), set()), c_api_exports.validate(output, expected))

    def test_reports_missing_and_unexpected_versioned_symbols(self) -> None:
        missing, unexpected = c_api_exports.validate(
            "00001000 T unijit_v1_extra", {"unijit_v1_required"}
        )
        self.assertEqual({"unijit_v1_required"}, missing)
        self.assertEqual({"unijit_v1_extra"}, unexpected)

    def test_rejects_unversioned_c_exports(self) -> None:
        missing, unexpected = c_api_exports.validate(
            "00001000 T internal_helper", set()
        )
        self.assertEqual(set(), missing)
        self.assertEqual({"internal_helper"}, unexpected)

    def test_rejects_exported_cpp_implementation_symbols(self) -> None:
        with self.assertRaisesRegex(ValueError, "C\\+\\+ implementation"):
            c_api_exports.validate("00001000 T __ZN6unijit3jit7compileEv", set())


if __name__ == "__main__":
    unittest.main()

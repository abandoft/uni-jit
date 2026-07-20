#!/usr/bin/env python3

import sys
import tempfile
import unittest
from pathlib import Path


sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tool"))

from release import (  # noqa: E402
    ReleaseError,
    Version,
    validate_bilingual_alignment,
    validate_changelog,
)


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


class ChangelogTest(unittest.TestCase):
    def validate(self, contents: str) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "CHANGELOG.md"
            path.write_text(contents, encoding="utf-8")
            validate_changelog(path)

    def test_single_line_updates_are_accepted(self) -> None:
        self.validate("## 0.1.2\n\n- First complete update.\n- Second update.\n")

    def test_wrapped_update_is_rejected(self) -> None:
        with self.assertRaisesRegex(ReleaseError, "one physical line"):
            self.validate("## 0.1.2\n\n- One update that was manually\n  wrapped.\n")

    def test_non_bullet_release_text_is_rejected(self) -> None:
        with self.assertRaisesRegex(ReleaseError, "one complete '- ' update"):
            self.validate("## 0.1.2\n\nRelease paragraph.\n")

    def test_empty_bullet_is_rejected(self) -> None:
        with self.assertRaisesRegex(ReleaseError, "one complete '- ' update"):
            self.validate("## 0.1.2\n\n- Complete update.\n- \n")

    def test_bilingual_update_count_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            english_path = root / "CHANGELOG.md"
            chinese_path = root / "CHANGELOG-ZH.md"
            english_path.write_text(
                "## 0.1.2\n\n- First update.\n- Second update.\n",
                encoding="utf-8",
            )
            chinese_path.write_text(
                "## 0.1.2\n\n- 第一条更新。\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(ReleaseError, "update counts differ"):
                validate_bilingual_alignment(
                    validate_changelog(english_path),
                    validate_changelog(chinese_path),
                )


if __name__ == "__main__":
    unittest.main()

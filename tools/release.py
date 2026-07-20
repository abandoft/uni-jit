#!/usr/bin/env python3
"""Validate UniJIT versions and extract canonical GitHub release notes."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERSION_PATTERN = re.compile(r"^(\d+)\.([0-9])\.([0-9])$")
HEADING_PATTERN = re.compile(r"^## (\d+\.[0-9]\.[0-9])[ \t]*$", re.MULTILINE)


class ReleaseError(RuntimeError):
    """A release invariant was violated."""


@dataclass(frozen=True, order=True)
class Version:
    major: int
    minor: int
    patch: int

    @classmethod
    def parse(cls, text: str) -> "Version":
        match = VERSION_PATTERN.fullmatch(text)
        if match is None:
            raise ReleaseError(
                f"invalid decimal version {text!r}; expected MAJOR.DIGIT.DIGIT"
            )
        return cls(*(int(component) for component in match.groups()))

    def next(self) -> "Version":
        major, minor, patch = self.major, self.minor, self.patch + 1
        if patch == 10:
            patch = 0
            minor += 1
        if minor == 10:
            minor = 0
            major += 1
        return Version(major, minor, patch)

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"


@dataclass(frozen=True)
class ChangelogSection:
    version: Version
    body: str


def project_version() -> Version:
    contents = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"project\s*\(\s*UniJIT\s+VERSION\s+(\d+\.[0-9]\.[0-9])",
        contents,
        re.DOTALL,
    )
    if match is None:
        raise ReleaseError("CMakeLists.txt has no decimal UniJIT project version")
    return Version.parse(match.group(1))


def changelog_sections(path: Path) -> list[ChangelogSection]:
    contents = path.read_text(encoding="utf-8")
    headings = list(HEADING_PATTERN.finditer(contents))
    if not headings:
        raise ReleaseError(f"{path.name} has no version headings")

    sections: list[ChangelogSection] = []
    for index, heading in enumerate(headings):
        body_start = heading.end()
        body_end = headings[index + 1].start() if index + 1 < len(headings) else len(contents)
        body = contents[body_start:body_end].strip()
        if not body or body in {"-", "- TBD", "- TODO"}:
            raise ReleaseError(
                f"{path.name} section {heading.group(1)} has no release notes"
            )
        sections.append(ChangelogSection(Version.parse(heading.group(1)), body))
    return sections


def validate_changelog(path: Path) -> list[ChangelogSection]:
    sections = changelog_sections(path)
    for newer, older in zip(sections, sections[1:]):
        expected = older.version.next()
        if newer.version != expected:
            raise ReleaseError(
                f"{path.name} versions must be newest-first and consecutive: "
                f"expected {expected} above {older.version}, found {newer.version}"
            )
    return sections


def check(tag: str | None) -> tuple[Version, list[ChangelogSection]]:
    current = project_version()
    english = validate_changelog(ROOT / "CHANGELOG.md")
    chinese = validate_changelog(ROOT / "CHANGELOG-ZH.md")

    if english[0].version != current:
        raise ReleaseError(
            f"CHANGELOG.md newest version {english[0].version} "
            f"does not match project version {current}"
        )
    if [section.version for section in english] != [
        section.version for section in chinese
    ]:
        raise ReleaseError("English and Chinese changelog versions do not match")
    if tag is not None and tag != f"v{current}":
        raise ReleaseError(f"tag {tag!r} does not match project version v{current}")
    return current, english


def main() -> int:
    parser = argparse.ArgumentParser()
    subcommands = parser.add_subparsers(dest="command", required=True)
    check_parser = subcommands.add_parser("check")
    check_parser.add_argument("--tag")
    subcommands.add_parser("next")
    notes_parser = subcommands.add_parser("notes")
    notes_parser.add_argument("version")
    arguments = parser.parse_args()

    try:
        current, sections = check(
            arguments.tag if arguments.command == "check" else None
        )
        if arguments.command == "check":
            print(f"release metadata valid for {current}")
        elif arguments.command == "next":
            print(current.next())
        else:
            requested = Version.parse(arguments.version.removeprefix("v"))
            for section in sections:
                if section.version == requested:
                    print(section.body)
                    break
            else:
                raise ReleaseError(f"CHANGELOG.md has no section for {requested}")
    except ReleaseError as error:
        print(f"release error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

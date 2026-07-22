#!/usr/bin/env python3
"""Validate the exact exported surface of a UniJIT shared library."""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys


UNIX_SYMBOL_PATTERN = re.compile(r"^\s*[0-9A-Fa-f]+\s+[A-Za-z]\s+(\S+)\s*$")
WINDOWS_SYMBOL_PATTERN = re.compile(
    r"^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)"
)


def find_dumpbin() -> str:
    executable = shutil.which("dumpbin")
    if executable is not None:
        return executable
    program_files = os.environ.get("ProgramFiles(x86)")
    if not program_files:
        raise FileNotFoundError("dumpbin and ProgramFiles(x86) are unavailable")
    vswhere = (
        pathlib.Path(program_files)
        / "Microsoft Visual Studio"
        / "Installer"
        / "vswhere.exe"
    )
    process = subprocess.run(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    installation = process.stdout.strip()
    if process.returncode != 0 or not installation:
        raise FileNotFoundError("unable to locate an MSVC x64 installation")
    candidates = sorted(
        (pathlib.Path(installation) / "VC" / "Tools" / "MSVC").glob(
            "*/bin/Hostx64/x64/dumpbin.exe"
        ),
        reverse=True,
    )
    if not candidates:
        raise FileNotFoundError("unable to locate the MSVC x64 dumpbin executable")
    return str(candidates[0])


def inspect_library(library: pathlib.Path) -> str:
    if sys.platform == "win32" or library.suffix.lower() == ".dll":
        command = [find_dumpbin(), "/nologo", "/exports", str(library)]
    elif sys.platform == "darwin" or library.suffix.lower() == ".dylib":
        command = ["nm", "-gU", str(library)]
    else:
        command = ["nm", "-D", "--defined-only", str(library)]
    process = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if process.returncode != 0:
        detail = process.stderr.strip() or process.stdout.strip()
        raise RuntimeError(f"symbol inspection failed: {detail}")
    return process.stdout


def load_manifest(path: pathlib.Path) -> set[str]:
    symbols = {
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    if not symbols or any(not symbol.startswith("unijit_v1_") for symbol in symbols):
        raise ValueError("the ABI manifest must contain only unijit_v1 symbols")
    return symbols


def exported_symbols(output: str) -> set[str]:
    symbols: set[str] = set()
    for line in output.splitlines():
        match = WINDOWS_SYMBOL_PATTERN.match(line) or UNIX_SYMBOL_PATTERN.match(line)
        if match is None:
            continue
        symbol = match.group(1).split("@", 1)[0]
        if symbol.startswith("_unijit_v1_") or symbol.startswith("__Z"):
            symbol = symbol[1:]
        symbols.add(symbol)
    return symbols


def validate(output: str, expected: set[str]) -> tuple[set[str], set[str]]:
    actual = exported_symbols(output)
    leaked_cpp = {symbol for symbol in actual if symbol.startswith("_Z") or symbol.startswith("?")}
    if leaked_cpp:
        sample = ", ".join(sorted(leaked_cpp)[:5])
        raise ValueError(f"C++ implementation symbols escaped the shared ABI: {sample}")
    return expected - actual, actual - expected


def main() -> int:
    parser = argparse.ArgumentParser(
        description="validate the exact versioned UniJIT C ABI exports"
    )
    parser.add_argument("library", type=pathlib.Path)
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("c_api_v1_symbols.txt"),
    )
    arguments = parser.parse_args()
    try:
        expected = load_manifest(arguments.manifest)
        missing, unexpected = validate(inspect_library(arguments.library), expected)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"C ABI export validation failed: {error}", file=sys.stderr)
        return 1
    if missing or unexpected:
        if missing:
            print("missing exports: " + ", ".join(sorted(missing)), file=sys.stderr)
        if unexpected:
            print(
                "unexpected exports: " + ", ".join(sorted(unexpected)),
                file=sys.stderr,
            )
        return 1
    print(f"validated {len(expected)} exact UniJIT C ABI exports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mo
"""Run normalization or an explicitly configured GNU MIPS assembler."""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path

from abumasn64.normalize import normalize, validate_version


def assemble(text: str, output: Path, assembler: Path, asflags: list[str], *, asn64_version: str) -> None:
    """Assemble without adding or translating any assembler options."""
    content = normalize(text, asn64_version=asn64_version)
    for flag in asflags:
        if flag in {"-g", "--gen-debug"} or (flag.startswith("-G") and flag != "-G0"):
            raise ValueError(f"GNU-as flag {flag}: unsupported by the proven ASN64 2.81 configuration")
        if flag == "-" or flag.startswith(("-o", "--output")):
            raise ValueError(f"GNU-as flag {flag}: use the explicit output argument")
    output.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run([str(assembler), *asflags, "-o", str(output), "-"], input=content, capture_output=True)
    if completed.returncode:
        raise ValueError(f"GNU as exited {completed.returncode}: {completed.stderr.decode(errors='replace')}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Normalize SN cc1 assembly for GNU MIPS as")
    parser.add_argument("source", nargs="?", type=Path, help="input assembly file. Omit to read stdin")
    parser.add_argument("--asn64-version", required=True, help="SN assembler version. Only 2.81 is proven")
    parser.add_argument("--run-assembler", action="store_true")
    parser.add_argument("--gnu-as-path", type=Path)
    parser.add_argument("--asflags", help="explicit GNU-as options as one shell-quoted string")
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()
    if args.run_assembler:
        for name in ("gnu_as_path", "asflags", "output"):
            if getattr(args, name) is None:
                parser.error(f"--run-assembler requires --{name.replace('_', '-')}")
    elif any(value is not None for value in (args.gnu_as_path, args.asflags, args.output)):
        parser.error("--gnu-as-path, --asflags and --output require --run-assembler")
    try:
        validate_version(args.asn64_version)
        text = args.source.read_text() if args.source else sys.stdin.read()
        if args.run_assembler:
            assemble(text, args.output, args.gnu_as_path, shlex.split(args.asflags), asn64_version=args.asn64_version)
        else:
            sys.stdout.buffer.write(normalize(text, asn64_version=args.asn64_version))
    except (OSError, ValueError) as error:
        parser.exit(1, f"abumasn64: {error}\n")

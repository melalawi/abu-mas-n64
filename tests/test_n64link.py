# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mo
"""n64link asn64 builds fixture bytes exactly; refusals are named."""

from __future__ import annotations

import json
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "build/n64link"
ASFLAGS = ["-march=vr4300", "-mabi=32", "-EB", "-G0", "--no-pad-sections"]


def assembler() -> str:
    path = os.environ.get("N64LINK_GNU_AS")
    if not path:
        raise RuntimeError("set N64LINK_GNU_AS to GNU MIPS as")
    return path


def run(*args: str, text: str = "") -> subprocess.CompletedProcess[bytes]:
    return subprocess.run([str(BINARY), *args], input=text.encode(), capture_output=True)


def sections(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    offset = struct.unpack_from(">I", data, 32)[0]
    count, names_index = struct.unpack_from(">HH", data, 48)
    headers = [struct.unpack_from(">10I", data, offset + 40 * index) for index in range(count)]

    def payload(header: tuple[int, ...]) -> bytes:
        return b"\0" * header[5] if header[1] == 8 else data[header[4] : header[4] + header[5]]

    names = payload(headers[names_index])
    return {names[h[0] : names.index(b"\0", h[0])].decode(): payload(h) for h in headers}


class Asn64Tests(unittest.TestCase):
    def test_fixtures_assemble_to_reference_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            for source in sorted((ROOT / "tests/fixtures").glob("*.s")):
                with self.subTest(fixture=source.stem):
                    output = Path(temporary) / (source.stem + ".o")
                    result = run("asn64", "--as", assembler(), *ASFLAGS, str(source), "-o", str(output))
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    actual = sections(output)
                    for name, content in json.loads(source.with_suffix(".json").read_text()).items():
                        self.assertEqual(actual.get(name, b""), bytes.fromhex(content), name)

    def test_string_literals_lower_to_exact_bytes(self) -> None:
        cases = (
            ('"%s"', b"%s\0"),
            ('""', b"\0"),
            (r'"\0007\001\177\200\377"', bytes.fromhex("00 37 01 7f 80 ff 00")),
            (r'"\a\b\f\n\r\t\v\"\\"', bytes.fromhex("07 08 0c 0a 0d 09 0b 22 5c 00")),
            ('".rodata,#", "tail" # comment', b".rodata,#\0tail\0"),
            ('"0123456789abcdefghijkl"', b"0123456789abcdefghijkl\0"),
        )
        for operand, expected in cases:
            with self.subTest(operand=operand):
                result = run("asn64", text=f"LC0: .string {operand}\n")
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                rows = [line.split(".byte ")[1] for line in result.stdout.decode().splitlines() if ".byte " in line]
                self.assertEqual(bytes(int(value) for row in rows for value in row.split(",")), expected)

    def test_refusals_are_named(self) -> None:
        for args, text, diagnostic in (
            (["asn64"], ".string\n", ".string"),
            (["asn64"], '.string "\\q"\n', "unsupported escape"),
            (["asn64", "--as", "as", "-G8", "x.s", "-o", "x.o"], "", "-G8"),
            (["asn64", "--as", "as", "-g", "x.s", "-o", "x.o"], "", "-g"),
            (["asn64", "-G0"], "", "need --as"),
            (["place", "x.o"], "", "usage"),
        ):
            with self.subTest(args=args, text=text):
                if "x.s" in args:
                    with tempfile.TemporaryDirectory() as temporary:
                        source = Path(temporary) / "x.s"
                        source.write_text(".text\n")
                        args = [str(source) if arg == "x.s" else arg for arg in args]
                        result = run(*args)
                else:
                    result = run(*args, text=text)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr.decode())

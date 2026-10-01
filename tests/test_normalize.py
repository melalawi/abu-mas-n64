# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mo
"""Compiler string literals survive the SN64 assembly normaliser byte-exactly."""

from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from abumasn64.assemble import assemble
from abumasn64.normalize import directives, normalize

ASFLAGS = ["-march=vr4300", "-mabi=32", "-EB", "-G0", "--no-pad-sections"]


class StringLiteralTests(unittest.TestCase):
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
                result = directives(f".rodata\n.align 2\nLC0: .string {operand}\n.text\n")
                lines = result.decode().splitlines()
                self.assertEqual(lines[:2], [".rdata", ".align 2"])
                self.assertTrue(lines[2].startswith("LC0: .byte "))
                self.assertEqual(lines[-1], ".text")
                actual = bytes(int(value) for line in lines[2:-1] for value in line.split(".byte ")[1].split(","))
                self.assertEqual(actual, expected)
        for operand in ("", '"unterminated', r'"\q"', '"ok",', '"ok" junk'):
            with self.subTest(invalid_operand=operand), self.assertRaisesRegex(ValueError, r"\.string:"):
                directives(f".string {operand}\n")


class AssemblyRegressionTests(unittest.TestCase):
    def test_real_assembly_rule_families(self) -> None:
        fixtures = Path(__file__).parent / "fixtures"
        assembler = assembler_path()
        with tempfile.TemporaryDirectory() as temporary:
            for source in sorted(fixtures.glob("*.s")):
                with self.subTest(family=source.stem):
                    output = Path(temporary) / (source.stem + ".o")
                    assemble(source.read_text(), output, assembler, ASFLAGS, asn64_version="2.81")
                    sections, symbols = read_object(output)
                    if source.stem == "common-bss":
                        self.assertEqual(
                            {name: symbols[name] for name in ("local_words", "shared_small", "shared_large")},
                            {"local_words": 0, "shared_small": 12, "shared_large": 16},
                        )
                    expected = json.loads(source.with_suffix(".json").read_text())
                    for name, content in expected.items():
                        payload = sections.get(name, b"")
                        self.assertEqual(payload, bytes.fromhex(content), name)


def assembler_path() -> Path:
    executable = os.environ.get("ABUMASN64_GNU_AS") or shutil.which("mips-linux-gnu-as")
    if executable is None:
        raise RuntimeError("tests require GNU MIPS as. Set ABUMASN64_GNU_AS or install mips-linux-gnu-as")
    return Path(executable)


def read_object(path: Path) -> tuple[dict[str, bytes], dict[str, int]]:
    """Read the ELF32 big-endian sections and symbols used by fixture assertions."""
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x01\x02":
        raise ValueError("expected ELF32 big-endian object")
    offset = struct.unpack_from(">I", data, 32)[0]
    size, count, names_index = struct.unpack_from(">HHH", data, 46)
    headers = [struct.unpack_from(">10I", data, offset + size * index) for index in range(count)]

    def payload(index: int) -> bytes:
        header = headers[index]
        return b"\0" * header[5] if header[1] == 8 else data[header[4] : header[4] + header[5]]

    def name(table: bytes, index: int) -> str:
        return table[index : table.index(b"\0", index)].decode()

    names = payload(names_index)
    sections = {name(names, header[0]): payload(index) for index, header in enumerate(headers)}
    symbols: dict[str, int] = {}
    for index, header in enumerate(headers):
        if header[1] != 2:
            continue
        strings = payload(header[6])
        table = payload(index)
        for position in range(0, len(table), header[9]):
            symbol_name, value = struct.unpack_from(">II", table, position)
            symbols[name(strings, symbol_name)] = value
    return sections, symbols


class CliTests(unittest.TestCase):
    def invoke(self, *args: str, text: str = ".text\nli $2,-1\n") -> subprocess.CompletedProcess[bytes]:
        return subprocess.run([sys.executable, "-m", "abumasn64", *args], input=text.encode(), capture_output=True)

    def test_stdin_and_file_produce_identical_normalized_assembly(self) -> None:
        source = ".text\nli $2,-1\n"
        expected = normalize(source, asn64_version="2.81")
        stdin = self.invoke("--asn64-version", "2.81", text=source)
        self.assertEqual(stdin.returncode, 0, stdin.stderr)
        self.assertEqual(stdin.stdout, expected)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "input.s"
            path.write_text(source)
            file = self.invoke("--asn64-version", "2.81", str(path), text="")
            self.assertEqual(file.returncode, 0, file.stderr)
            self.assertEqual(file.stdout, expected)

    def test_version_and_assembler_configuration_are_required(self) -> None:
        for args, diagnostic in (
            ([], "--asn64-version"),
            (["--asn64-version", "2.80"], "ASN64 version 2.80"),
            (["--asn64-version", "2.81", "--run-assembler"], "--gnu-as-path"),
            (["--asn64-version", "2.81", "--run-assembler", "--gnu-as-path", "as"], "--asflags"),
            (["--asn64-version", "2.81", "--run-assembler", "--gnu-as-path", "as", "--asflags="], "--output"),
        ):
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr.decode())
        with self.assertRaisesRegex(ValueError, "ASN64 version 2.90"):
            normalize("", asn64_version="2.90")

    def test_cli_assembler_produces_reference_sections(self) -> None:
        source = Path(__file__).parent / "fixtures/common-bss.s"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "result.o"
            result = self.invoke(
                "--asn64-version",
                "2.81",
                str(source),
                "--run-assembler",
                "--gnu-as-path",
                str(assembler_path()),
                "--asflags=" + " ".join(ASFLAGS),
                "--output",
                str(output),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            actual, _ = read_object(output)
            for name, content in json.loads(source.with_suffix(".json").read_text()).items():
                self.assertEqual(actual.get(name, b""), bytes.fromhex(content), name)

    def test_unproven_small_data_and_debug_flags_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            for flag in ("-G8", "-g"):
                with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, "unsupported"):
                    assemble(".text\n", Path(temporary) / "out.o", assembler_path(), [flag], asn64_version="2.81")

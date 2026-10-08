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


JUMP_TABLE = """\
\t.set noreorder
\t.set noat
\t.text
\t.globl func
func:
\tlui $at, %hi($jt)
\tlw $at, %lo($jt)($at)
\tjr $at
\tnop
$L1:
\tjr $ra
\tnop
$L2:
\tjr $ra
\tnop
\t.section .rdata,""
\t.align 2
$jt:
\t.word $L1
\t.word $L2
"""
TEXT_VRAM, TABLE_VRAM = 0x80001000, 0x80001100


class PlaceTests(unittest.TestCase):
    """KMC gcc emits a jump table as an unallocated .rdata whose R_MIPS_32 words point into .text."""

    def place(self, table: tuple[int, int]) -> tuple[subprocess.CompletedProcess[bytes], dict[str, bytes]]:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "unit.s").write_text(JUMP_TABLE)
            built = subprocess.run([assembler(), *ASFLAGS, str(directory / "unit.s"), "-o", str(directory / "unit.o")], capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr.decode())
            text = bytearray(sections(directory / "unit.o")[".text"])
            struct.pack_into(">H", text, 2, TABLE_VRAM >> 16)
            struct.pack_into(">H", text, 6, TABLE_VRAM & 0xFFFF)
            rom = bytearray(0x200)
            rom[: len(text)] = text
            struct.pack_into(">II", rom, TABLE_VRAM - TEXT_VRAM, *table)
            (directory / "game.z64").write_bytes(bytes(rom))
            result = run(
                "place", str(directory / "unit.o"), "-o", str(directory / "placed.o"), "--rom", str(directory / "game.z64"),
                "--text", f"{TEXT_VRAM:#x}:0x0:{len(text):#x}", "--map", f"{TEXT_VRAM:#x}:0x0:0x200",
            )
            placed = sections(directory / "placed.o") if result.returncode == 0 else {}
            return result, placed

    def test_jump_table_words_are_relocated_before_the_rom_proof(self) -> None:
        result, placed = self.place((TEXT_VRAM + 0x10, TEXT_VRAM + 0x18))
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(struct.unpack_from(">HxxH", placed[".text"], 2), (TABLE_VRAM >> 16, TABLE_VRAM & 0xFFFF))

    def test_unrelocated_jump_table_in_the_rom_is_refused(self) -> None:
        result, _ = self.place((0x10, 0x18))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(".rdata+0x0: ROM word 0x00000010 is not 0x80001010", result.stderr.decode())


FLOATS_THEN_TABLE = """\
\t.set noreorder
\t.set noat
\t.text
\t.globl func
func:
\tlui $at, %hi($c0)
\tlwc1 $f0, %lo($c0)($at)
\tlui $at, %hi($c1)
\tlwc1 $f2, %lo($c1)($at)
\tlui $at, %hi($jt)
\tlw $at, %lo($jt)($at)
\tjr $at
\tnop
$L1:
\tjr $ra
\tnop
$L2:
\tjr $ra
\tnop
\t.section .rdata,""
\t.align 3
$c0:
\t.word 0x3f800000
$c1:
\t.word 0x40000000
ALIGN8_MARK_0:
\t.align 2
$jt:
\t.word $L1
\t.word $L2
"""
FLOAT_BASE = 0x80001104  # 4 mod 8: the original link padded the table to 8


class AlignedConstantTests(unittest.TestCase):
    """An 8-aligned item in a section placed at a 4 mod 8 base sits after 4 bytes of zero padding."""

    def place(self, pad: bytes) -> subprocess.CompletedProcess[bytes]:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "unit.s").write_text(FLOATS_THEN_TABLE)
            built = subprocess.run([assembler(), *ASFLAGS, str(directory / "unit.s"), "-o", str(directory / "unit.o")], capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr.decode())
            text = bytearray(sections(directory / "unit.o")[".text"])
            addresses = (FLOAT_BASE, FLOAT_BASE + 4, FLOAT_BASE + 0xC)
            for index, address in enumerate(addresses):
                struct.pack_into(">H", text, 8 * index + 2, address >> 16)
                struct.pack_into(">H", text, 8 * index + 6, address & 0xFFFF)
            rom = bytearray(0x200)
            rom[: len(text)] = text
            at = FLOAT_BASE - TEXT_VRAM
            struct.pack_into(">II", rom, at, 0x3F800000, 0x40000000)
            rom[at + 8 : at + 12] = pad
            struct.pack_into(">II", rom, at + 12, TEXT_VRAM + 0x20, TEXT_VRAM + 0x28)
            (directory / "game.z64").write_bytes(bytes(rom))
            return run(
                "place", str(directory / "unit.o"), "-o", str(directory / "placed.o"), "--rom", str(directory / "game.z64"),
                "--text", f"{TEXT_VRAM:#x}:0x0:{len(text):#x}", "--map", f"{TEXT_VRAM:#x}:0x0:0x200",
            )

    def test_padded_table_at_a_misaligned_base_is_proved_exact(self) -> None:
        result = self.place(b"\0\0\0\0")
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_missing_pad_is_refused_by_name(self) -> None:
        result = self.place(bytes.fromhex("DEADBEEF"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(".rdata+0x8: alignment pad at 0x8000110C is not 4 zero bytes in the ROM", result.stderr.decode())

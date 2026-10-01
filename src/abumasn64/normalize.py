# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mo
"""Normalize cc1 assembly for the selected SN ASN64 version."""

from __future__ import annotations

import re

from abumasn64.schedule import schedule


def validate_version(asn64_version: str) -> None:
    if asn64_version != "2.81":
        raise ValueError(f"ASN64 version {asn64_version}: unsupported. Only 2.81 is proven")


def string_bytes(operands: str) -> bytes:
    """Decode compiler assembly strings, including one NUL per operand."""
    result = bytearray()
    escapes = {
        "a": 7,
        "b": 8,
        "f": 12,
        "n": 10,
        "r": 13,
        "t": 9,
        "v": 11,
        '"': 34,
        "\\": 92,
    }
    remaining = operands.strip()
    while remaining:
        match = re.match(r'"((?:\\.|[^"\\])*)"', remaining)
        if match is None:
            raise ValueError(f".string: invalid operand {remaining!r}")
        content = match[1]
        position = 0
        while position < len(content):
            char = content[position]
            position += 1
            if char != "\\":
                result.extend(char.encode())
                continue
            escape = content[position]
            position += 1
            if escape in "01234567":
                digits = escape
                while position < len(content) and len(digits) < 3 and content[position] in "01234567":
                    digits += content[position]
                    position += 1
                result.append(int(digits, 8) & 0xFF)
            elif escape in escapes:
                result.append(escapes[escape])
            else:
                raise ValueError(f".string: unsupported escape \\{escape}")
        result.append(0)
        remaining = remaining[match.end() :].strip()
        if not remaining or remaining.startswith("#"):
            break
        if not remaining.startswith(",") or not remaining[1:].strip():
            raise ValueError(f".string: invalid trailing operand {remaining!r}")
        remaining = remaining[1:].strip()
    if not result:
        raise ValueError(".string: missing operand")
    return bytes(result)


def directives(text: str) -> bytes:
    lines = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == ".set gp=64":
            continue
        if stripped.startswith((".version", ".size", ".type", ".ident")):
            continue
        literal = re.match(r"^(\s*(?:[.\w]+:\s*)?)\.string\s+(.*)$", line)
        if literal:
            content = string_bytes(literal[2])
            for offset in range(0, len(content), 16):
                prefix = literal[1] if offset == 0 else "\t"
                lines.append(prefix + ".byte " + ",".join(str(byte) for byte in content[offset : offset + 16]))
            continue
        line = line.replace(".rodata", ".rdata")
        lines.append(line)
    return ("\r\n".join(lines) + "\r\n").encode()


def normalize(text: str, *, asn64_version: str) -> bytes:
    validate_version(asn64_version)
    return schedule(directives(text).decode()).encode()

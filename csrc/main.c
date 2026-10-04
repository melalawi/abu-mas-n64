/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mo */
#include <stdio.h>
#include <string.h>

#include "n64link.h"

static const char USAGE[] =
    "usage:\n"
    "  n64link asn64 [SOURCE.s]                         normalise SN64 cc1 assembly to stdout\n"
    "  n64link asn64 --as AS [ASFLAGS...] SOURCE.s -o OBJECT\n"
    "                                                   normalise, then assemble with GNU as\n"
    "  n64link place OBJECT -o OUTPUT --rom ROM --text VRAM:ROM:SIZE\n"
    "                [--map VRAM:ROM:SIZE]... [--symbols FILE] [--trim] [--score]\n"
    "                                                   prove constants against the ROM; make .text linkable alone\n"
    "  n64link --version\n";

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "asn64") == 0) return asn64_main(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "place") == 0) return place_main(argc - 2, argv + 2);
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("n64link " N64LINK_VERSION " (SN ASN64 2.81 rules)");
        return 0;
    }
    fputs(USAGE, stderr);
    return argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) ? 0 : 2;
}

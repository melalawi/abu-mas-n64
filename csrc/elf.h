/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mo */
/* ELF32 big-endian MIPS relocatable objects: read, edit in memory, write back with fresh offsets. */
#ifndef N64LINK_ELF_H
#define N64LINK_ELF_H

#include <stddef.h>
#include <stdint.h>

enum {
    SHT_PROGBITS = 1,
    SHT_SYMTAB = 2,
    SHT_RELA = 4,
    SHT_NOBITS = 8,
    SHT_REL = 9,
    SHF_ALLOC = 2,
    SHN_UNDEF = 0,
    SHN_ABS = 0xfff1,
    STT_FUNC = 2,
    STT_SECTION = 3,
    R_MIPS_32 = 2,
    R_MIPS_26 = 4,
    R_MIPS_HI16 = 5,
    R_MIPS_LO16 = 6,
};

typedef struct {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
    const char *label;
    uint8_t *data; /* NULL for SHT_NOBITS */
} Section;

typedef struct {
    uint32_t name, value, size;
    uint8_t info, other;
    uint16_t shndx;
    const char *label;
} Symbol;

typedef struct {
    uint32_t offset;
    uint32_t symbol;
    uint32_t type;
} Rel;

typedef struct {
    const char *path;
    uint8_t header[52];
    Section *sections;
    size_t section_count;
    size_t symtab; /* section index of SHT_SYMTAB */
    Symbol *symbols;
    size_t symbol_count;
} Elf;

Elf elf_read(const char *path);
size_t elf_section(const Elf *elf, const char *name); /* 0 when absent */
Rel *elf_rels(const Elf *elf, size_t rel_section, size_t *count);
void elf_set_rels(Elf *elf, size_t rel_section, const Rel *rels, size_t count);
void elf_store_symbols(Elf *elf);
void elf_write(const Elf *elf, const char *path);

#endif

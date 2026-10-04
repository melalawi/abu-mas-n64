/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mo */
#include "elf.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static const char *label_at(const Elf *elf, size_t table, uint32_t offset) {
    const Section *strings = &elf->sections[table];
    if (strings->data == NULL || offset >= strings->size) die("%s: string offset %u outside section %zu", elf->path, offset, table);
    if (memchr(strings->data + offset, '\0', strings->size - offset) == NULL) die("%s: unterminated string", elf->path);
    return (const char *)strings->data + offset;
}

Elf elf_read(const char *path) {
    Elf elf = {.path = path};
    size_t len;
    uint8_t *data = read_file(path, &len);
    if (len < 52 || memcmp(data, "\x7f" "ELF\x01\x02", 6) != 0) die("%s: not an ELF32 big-endian object", path);
    if (be16(data + 16) != 1 || be16(data + 18) != 8) die("%s: not a MIPS relocatable object", path);
    memcpy(elf.header, data, 52);
    uint32_t table = be32(data + 32);
    uint16_t entry = be16(data + 46), count = be16(data + 48), names = be16(data + 50);
    if (entry != 40 || (uint64_t)table + (uint64_t)count * 40 > len || names >= count) die("%s: bad section table", path);
    elf.section_count = count;
    elf.sections = calloc(count, sizeof(Section));
    if (elf.sections == NULL) die("out of memory");
    for (size_t i = 0; i < count; i++) {
        const uint8_t *h = data + table + i * 40;
        Section *s = &elf.sections[i];
        s->name = be32(h), s->type = be32(h + 4), s->flags = be32(h + 8), s->addr = be32(h + 12);
        s->offset = be32(h + 16), s->size = be32(h + 20), s->link = be32(h + 24), s->info = be32(h + 28);
        s->addralign = be32(h + 32), s->entsize = be32(h + 36);
        if (s->type != SHT_NOBITS && i != 0) {
            if ((uint64_t)s->offset + s->size > len) die("%s: section %zu outside file", path, i);
            s->data = xmalloc(s->size);
            memcpy(s->data, data + s->offset, s->size);
        }
    }
    for (size_t i = 0; i < count; i++) elf.sections[i].label = label_at(&elf, names, elf.sections[i].name);
    for (size_t i = 1; i < count; i++) {
        if (elf.sections[i].type != SHT_SYMTAB) continue;
        if (elf.symtab) die("%s: more than one symbol table", path);
        elf.symtab = i;
    }
    if (elf.symtab) {
        Section *s = &elf.sections[elf.symtab];
        if (s->entsize != 16 || s->size % 16) die("%s: bad symbol table", path);
        elf.symbol_count = s->size / 16;
        elf.symbols = calloc(elf.symbol_count ? elf.symbol_count : 1, sizeof(Symbol));
        if (elf.symbols == NULL) die("out of memory");
        for (size_t i = 0; i < elf.symbol_count; i++) {
            const uint8_t *e = s->data + i * 16;
            Symbol *y = &elf.symbols[i];
            y->name = be32(e), y->value = be32(e + 4), y->size = be32(e + 8);
            y->info = e[12], y->other = e[13], y->shndx = be16(e + 14);
            y->label = label_at(&elf, s->link, y->name);
        }
    }
    free(data);
    return elf;
}

size_t elf_section(const Elf *elf, const char *name) {
    for (size_t i = 1; i < elf->section_count; i++)
        if (strcmp(elf->sections[i].label, name) == 0) return i;
    return 0;
}

Rel *elf_rels(const Elf *elf, size_t index, size_t *count) {
    const Section *s = &elf->sections[index];
    if (s->type != SHT_REL || s->entsize != 8 || s->size % 8) die("%s: %s: expected REL entries", elf->path, s->label);
    *count = s->size / 8;
    Rel *rels = xmalloc(*count * sizeof(Rel));
    for (size_t i = 0; i < *count; i++) {
        uint32_t info = be32(s->data + i * 8 + 4);
        rels[i] = (Rel){be32(s->data + i * 8), info >> 8, info & 0xff};
        if (rels[i].symbol >= elf->symbol_count) die("%s: %s: symbol index %u outside table", elf->path, s->label, rels[i].symbol);
    }
    return rels;
}

void elf_set_rels(Elf *elf, size_t index, const Rel *rels, size_t count) {
    Section *s = &elf->sections[index];
    free(s->data);
    s->size = (uint32_t)(count * 8);
    s->data = xmalloc(s->size);
    for (size_t i = 0; i < count; i++) {
        put_be32(s->data + i * 8, rels[i].offset);
        put_be32(s->data + i * 8 + 4, rels[i].symbol << 8 | rels[i].type);
    }
}

void elf_store_symbols(Elf *elf) {
    Section *s = &elf->sections[elf->symtab];
    for (size_t i = 0; i < elf->symbol_count; i++) {
        uint8_t *e = s->data + i * 16;
        const Symbol *y = &elf->symbols[i];
        put_be32(e + 4, y->value);
        put_be32(e + 8, y->size);
        e[12] = y->info;
        e[13] = y->other;
        put_be16(e + 14, y->shndx);
    }
}

void elf_write(const Elf *elf, const char *path) {
    Buf out = {0};
    buf_add(&out, (const char *)elf->header, 52);
    uint32_t *offsets = xmalloc(elf->section_count * sizeof(uint32_t));
    offsets[0] = 0;
    for (size_t i = 1; i < elf->section_count; i++) {
        const Section *s = &elf->sections[i];
        uint32_t align = s->addralign ? s->addralign : 1;
        while (out.len % align) buf_add(&out, "", 1);
        offsets[i] = (uint32_t)out.len;
        if (s->type != SHT_NOBITS) buf_add(&out, (const char *)s->data, s->size);
    }
    while (out.len % 4) buf_add(&out, "", 1);
    uint32_t table = (uint32_t)out.len;
    for (size_t i = 0; i < elf->section_count; i++) {
        const Section *s = &elf->sections[i];
        uint8_t h[40];
        uint32_t fields[10] = {s->name, s->type, s->flags, s->addr, i ? offsets[i] : 0, s->size, s->link, s->info, s->addralign, s->entsize};
        for (int f = 0; f < 10; f++) put_be32(h + f * 4, fields[f]);
        buf_add(&out, (const char *)h, 40);
    }
    put_be32((uint8_t *)out.data + 32, table);
    write_file(path, out.data, out.len);
    free(out.data);
    free(offsets);
}

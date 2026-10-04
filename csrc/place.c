/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mo */
/*
 * place: prove a compiled C unit's constants against the ROM and make its .text linkable alone.
 *
 * Every %hi/%lo pair in .text that refers to a constant section (.rodata, .data, .bss, ...) is checked
 * against the ROM words at the unit's address: the opcode bits must agree, and the ROM pair gives the
 * address the original program used. All references to one section must agree on its base. The section's
 * bytes must equal the ROM bytes at that base (R_MIPS_32 words inside it are compared after relocation).
 * The pair is then written as absolute %hi/%lo and its relocations are dropped, so the linker never sees
 * the constant sections; their bytes come from the ROM slice that holds them. Symbols defined in a
 * constant section become absolute at their proved address. .text can be trimmed of trailing zero
 * padding and is padded with zeros to the row size.
 *
 * Without --score any failed proof is refused by name. With --score the object is written anyway with the
 * best-supported bases, and each failed proof is reported on stderr (for scoring drafts that do not match).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf.h"
#include "n64link.h"
#include "util.h"

typedef struct {
    uint32_t vram, rom, size;
} Window;

typedef struct {
    char *name;
    uint32_t value;
} External;

typedef struct {
    uint32_t hi, lo; /* .text offsets */
    uint32_t symbol;
    size_t section;
    uint32_t section_offset;
    bool checked;
    uint32_t base;
} Reference;

typedef struct {
    const uint8_t *rom;
    size_t rom_len;
    Window *windows;
    size_t window_count;
    External *externals;
    size_t external_count;
    StrVec problems;
} Context;

static void problem(Context *context, char *text) { vec_push(&context->problems, text); }

static uint32_t number(const char *text, const char *what) {
    long long value;
    if (!parse_int0(text, &value) || value < 0 || value > 0xFFFFFFFFLL) die("place: %s: %s is not a 32-bit number", what, text);
    return (uint32_t)value;
}

static Window window(const char *text, const char *what) {
    StrVec parts = split_on(text, ':', 0);
    if (parts.len != 3) die("place: %s: expected VRAM:ROM:SIZE, got %s", what, text);
    Window result = {number(parts.items[0], what), number(parts.items[1], what), number(parts.items[2], what)};
    vec_free(&parts);
    return result;
}

static int external_order(const void *a, const void *b) { return strcmp(((const External *)a)->name, ((const External *)b)->name); }

static void read_externals(Context *context, const char *path) {
    size_t len;
    char *text = (char *)read_file(path, &len);
    StrVec lines = split_on(text, '\n', 0);
    for (size_t i = 0; i < lines.len; i++) {
        char *line = strip(lines.items[i]);
        if (!line[0]) {
            free(line);
            continue;
        }
        size_t end = strlen(line);
        if (line[end - 1] != ';') die("place: %s:%zu: expected NAME = ADDRESS; or PROVIDE(NAME = ADDRESS);", path, i + 1);
        line[--end] = '\0';
        char *body = line;
        if (starts_with(body, "PROVIDE(")) {
            if (end < 9 || body[end - 1] != ')') die("place: %s:%zu: unclosed PROVIDE(", path, i + 1);
            body[end - 1] = '\0';
            body += 8;
        }
        char *equals = strchr(body, '=');
        if (equals == NULL) die("place: %s:%zu: expected NAME = ADDRESS", path, i + 1);
        *equals = '\0';
        char *name = strip(body), *value = strip(equals + 1);
        context->externals = xrealloc(context->externals, (context->external_count + 1) * sizeof(External));
        context->externals[context->external_count++] = (External){name, number(value, path)};
        free(value);
        free(line);
    }
    qsort(context->externals, context->external_count, sizeof(External), external_order);
    vec_free(&lines);
    free(text);
}

static const External *external(const Context *context, const char *name) {
    External key = {(char *)name, 0};
    return bsearch(&key, context->externals, context->external_count, sizeof(External), external_order);
}

/* ROM bytes at VRAM for LEN bytes, or NULL when no window holds the whole range. */
static const uint8_t *rom_at(const Context *context, uint32_t vram, uint32_t len) {
    for (size_t i = 0; i < context->window_count; i++) {
        const Window *w = &context->windows[i];
        if (vram < w->vram || (uint64_t)vram + len > (uint64_t)w->vram + w->size) continue;
        uint64_t offset = (uint64_t)w->rom + (vram - w->vram);
        if (offset + len > context->rom_len) return NULL;
        return context->rom + offset;
    }
    return NULL;
}

static bool is_constant(const Elf *elf, size_t index, size_t text) {
    const Section *s = &elf->sections[index];
    /* KMC gcc 2.7.2's assembler emits its read-only constants as .rdata without SHF_ALLOC. */
    bool allocated = (s->flags & SHF_ALLOC) || strcmp(s->label, ".rdata") == 0;
    return index != text && allocated && (s->type == SHT_PROGBITS || s->type == SHT_NOBITS) && s->size > 0;
}

static size_t rel_section_for(const Elf *elf, size_t target) {
    size_t found = 0;
    for (size_t i = 1; i < elf->section_count; i++) {
        const Section *s = &elf->sections[i];
        if (s->info != target || (s->type != SHT_REL && s->type != SHT_RELA)) continue;
        if (s->type == SHT_RELA) die("place: %s: %s: RELA relocations are unsupported", elf->path, s->label);
        if (found) die("place: %s: more than one relocation section for %s", elf->path, elf->sections[target].label);
        found = i;
    }
    return found;
}

static uint32_t symbol_offset(const Symbol *symbol) { return (symbol->info & 15) == STT_SECTION ? 0 : symbol->value; }

int place_main(int argc, char **argv) {
    const char *object = NULL, *output = NULL, *rom_path = NULL, *symbols_path = NULL;
    Window text_window = {0};
    bool have_text = false, trim = false, score = false;
    Context context = {0};
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        bool valued = strcmp(arg, "-o") == 0 || strcmp(arg, "--rom") == 0 || strcmp(arg, "--text") == 0 ||
                      strcmp(arg, "--map") == 0 || strcmp(arg, "--symbols") == 0;
        if (valued && i + 1 >= argc) die("place: %s needs a value", arg);
        if (strcmp(arg, "-o") == 0) output = argv[++i];
        else if (strcmp(arg, "--rom") == 0) rom_path = argv[++i];
        else if (strcmp(arg, "--symbols") == 0) symbols_path = argv[++i];
        else if (strcmp(arg, "--text") == 0) text_window = window(argv[++i], "--text"), have_text = true;
        else if (strcmp(arg, "--map") == 0) {
            context.windows = xrealloc(context.windows, (context.window_count + 1) * sizeof(Window));
            context.windows[context.window_count++] = window(argv[++i], "--map");
        } else if (strcmp(arg, "--trim") == 0) trim = true;
        else if (strcmp(arg, "--score") == 0) score = true;
        else if (arg[0] == '-') die("place: unknown option %s", arg);
        else if (object != NULL) die("place: one object expected, got %s and %s", object, arg);
        else object = arg;
    }
    if (object == NULL || output == NULL || rom_path == NULL || !have_text)
        die("place: usage: n64link place OBJECT -o OUTPUT --rom ROM --text VRAM:ROM:SIZE [--map VRAM:ROM:SIZE]... [--symbols FILE] [--trim] [--score]");
    context.rom = read_file(rom_path, &context.rom_len);
    context.windows = xrealloc(context.windows, (context.window_count + 1) * sizeof(Window));
    context.windows[context.window_count++] = text_window;
    if (symbols_path) read_externals(&context, symbols_path);

    Elf elf = elf_read(object);
    if (!elf.symtab) die("place: %s: no symbol table", object);
    size_t text = elf_section(&elf, ".text");
    if (!text || elf.sections[text].type != SHT_PROGBITS) die("place: %s: no .text", object);
    Section *code = &elf.sections[text];
    size_t text_rels_index = rel_section_for(&elf, text);
    size_t rel_count = 0;
    Rel *rels = text_rels_index ? elf_rels(&elf, text_rels_index, &rel_count) : NULL;
    bool *drop = calloc(rel_count ? rel_count : 1, sizeof(bool));

    /* Pair HI16/LO16 relocations against constant sections (MIPS ABI order: HI16s, then their LO16). */
    Reference *refs = NULL;
    size_t ref_count = 0;
    size_t *pending = xmalloc((rel_count ? rel_count : 1) * sizeof(size_t));
    size_t pending_count = 0;
    long long *last_hi = xmalloc(elf.symbol_count * sizeof(long long));
    for (size_t i = 0; i < elf.symbol_count; i++) last_hi[i] = -1;
    for (size_t i = 0; i < rel_count; i++) {
        const Rel *r = &rels[i];
        const Symbol *symbol = &elf.symbols[r->symbol];
        if (symbol->shndx >= elf.section_count || !is_constant(&elf, symbol->shndx, text)) continue;
        if (r->offset + 4 > code->size) die("place: %s: relocation at +0x%X outside .text", object, r->offset);
        drop[i] = true;
        if (r->type == R_MIPS_HI16) {
            pending[pending_count++] = i;
            continue;
        }
        if (r->type != R_MIPS_LO16) {
            problem(&context, xformat("relocation type %u at .text+0x%X against %s is unsupported", r->type, r->offset, elf.sections[symbol->shndx].label));
            drop[i] = false;
            continue;
        }
        size_t matched = 0;
        for (size_t p = 0; p < pending_count;) {
            if (rels[pending[p]].symbol != r->symbol) {
                p++;
                continue;
            }
            refs = xrealloc(refs, (ref_count + 1) * sizeof(Reference));
            refs[ref_count++] = (Reference){rels[pending[p]].offset, r->offset, r->symbol, symbol->shndx, 0, false, 0};
            last_hi[r->symbol] = rels[pending[p]].offset;
            memmove(pending + p, pending + p + 1, (pending_count - p - 1) * sizeof(size_t));
            pending_count--;
            matched++;
        }
        if (!matched) {
            if (last_hi[r->symbol] < 0) {
                problem(&context, xformat("LO16 at .text+0x%X has no HI16", r->offset));
                continue;
            }
            refs = xrealloc(refs, (ref_count + 1) * sizeof(Reference));
            refs[ref_count++] = (Reference){(uint32_t)last_hi[r->symbol], r->offset, r->symbol, symbol->shndx, 0, false, 0};
        }
    }
    for (size_t p = 0; p < pending_count; p++) problem(&context, xformat("HI16 at .text+0x%X has no LO16", rels[pending[p]].offset));

    /* Each reference proposes a section base from the ROM pair. */
    for (size_t i = 0; i < ref_count; i++) {
        Reference *ref = &refs[i];
        uint32_t hi = be32(code->data + ref->hi), lo = be32(code->data + ref->lo);
        int32_t addend = (int32_t)((hi & 0xFFFF) << 16) + (int16_t)(lo & 0xFFFF);
        ref->section_offset = symbol_offset(&elf.symbols[ref->symbol]) + (uint32_t)addend;
        const char *name = elf.sections[ref->section].label;
        if (ref->hi + 4 > text_window.size || ref->lo + 4 > text_window.size) {
            problem(&context, xformat("%s reference at .text+0x%X lies past the row", name, ref->lo));
            continue;
        }
        const uint8_t *rom_hi = rom_at(&context, text_window.vram + ref->hi, 4);
        const uint8_t *rom_lo = rom_at(&context, text_window.vram + ref->lo, 4);
        if (rom_hi == NULL || rom_lo == NULL) die("place: --text window lies outside the ROM");
        uint32_t want_hi = be32(rom_hi), want_lo = be32(rom_lo);
        if ((want_hi ^ hi) & 0xFFFF0000 || (want_lo ^ lo) & 0xFFFF0000) {
            problem(&context, xformat("%s reference at .text+0x%X/0x%X: opcode differs from the ROM", name, ref->hi, ref->lo));
            continue;
        }
        uint32_t address = ((want_hi & 0xFFFF) << 16) + (uint32_t)(int32_t)(int16_t)(want_lo & 0xFFFF);
        ref->checked = true;
        ref->base = address - ref->section_offset;
    }

    /* Bases per section: references first, then named symbols known to the symbol table. */
    size_t count = elf.section_count;
    bool *known = calloc(count, sizeof(bool));
    uint32_t *base = calloc(count, sizeof(uint32_t));
    for (size_t s = 1; s < count; s++) {
        if (!is_constant(&elf, s, text)) continue;
        uint32_t *candidates = NULL;
        size_t candidate_count = 0;
        for (size_t i = 0; i < ref_count; i++) {
            if (refs[i].section != s || !refs[i].checked) continue;
            candidates = xrealloc(candidates, (candidate_count + 1) * sizeof(uint32_t));
            candidates[candidate_count++] = refs[i].base;
        }
        for (size_t y = 0; y < elf.symbol_count; y++) {
            const Symbol *symbol = &elf.symbols[y];
            if (symbol->shndx != s || (symbol->info & 15) == STT_SECTION || !symbol->label[0]) continue;
            const External *known_symbol = external(&context, symbol->label);
            if (known_symbol == NULL) continue;
            candidates = xrealloc(candidates, (candidate_count + 1) * sizeof(uint32_t));
            candidates[candidate_count++] = known_symbol->value - symbol->value;
        }
        if (!candidate_count) {
            free(candidates);
            continue;
        }
        size_t best = 0, best_votes = 0;
        bool agree = true;
        for (size_t i = 0; i < candidate_count; i++) {
            size_t votes = 0;
            for (size_t j = 0; j < candidate_count; j++) votes += candidates[j] == candidates[i];
            if (votes > best_votes) best = i, best_votes = votes;
            agree = agree && candidates[i] == candidates[0];
        }
        if (!agree)
            problem(&context, xformat("references to %s disagree on its address (0x%08X has %zu of %zu)", elf.sections[s].label, candidates[best], best_votes, candidate_count));
        known[s] = true;
        base[s] = candidates[best];
        free(candidates);
    }

    /* Prove every constant section's bytes; R_MIPS_32 words compare after relocation. */
    for (size_t s = 1; s < count; s++) {
        if (!is_constant(&elf, s, text)) continue;
        const Section *section = &elf.sections[s];
        if (!known[s]) {
            bool data = false;
            for (uint32_t i = 0; section->data && i < section->size; i++) data = data || section->data[i];
            if (data) problem(&context, xformat("%s has data but nothing gives its address", section->label));
            continue;
        }
        if (section->type == SHT_NOBITS) continue;
        const uint8_t *rom = rom_at(&context, base[s], section->size);
        if (rom == NULL) {
            problem(&context, xformat("%s at 0x%08X (0x%X bytes) lies outside every mapped window", section->label, base[s], section->size));
            continue;
        }
        bool *relocated = calloc(section->size, sizeof(bool));
        size_t data_rels_index = rel_section_for(&elf, s), data_rel_count = 0;
        Rel *data_rels = data_rels_index ? elf_rels(&elf, data_rels_index, &data_rel_count) : NULL;
        long long bias = -1;
        for (size_t i = 0; i < data_rel_count; i++) {
            const Rel *r = &data_rels[i];
            const Symbol *symbol = &elf.symbols[r->symbol];
            if (r->type != R_MIPS_32 || r->offset % 4 || r->offset + 4 > section->size) {
                problem(&context, xformat("%s+0x%X: relocation type %u is unsupported", section->label, r->offset, r->type));
                continue;
            }
            for (int k = 0; k < 4; k++) relocated[r->offset + k] = true;
            uint32_t target;
            if (symbol->shndx == text) target = text_window.vram + symbol_offset(symbol);
            else if (symbol->shndx == SHN_ABS) target = symbol->value;
            else if (symbol->shndx < count && known[symbol->shndx]) target = base[symbol->shndx] + symbol_offset(symbol);
            else if (symbol->shndx == SHN_UNDEF) {
                const External *found = external(&context, symbol->label);
                if (found == NULL) {
                    problem(&context, xformat("%s+0x%X refers to %s, which the symbol table does not define", section->label, r->offset, symbol->label));
                    continue;
                }
                target = found->value;
            } else {
                problem(&context, xformat("%s+0x%X refers to a section with no proved address", section->label, r->offset));
                continue;
            }
            uint32_t expected = target + be32(section->data + r->offset);
            uint32_t delta = be32(rom + r->offset) - expected;
            if ((delta != 0 && delta != 0x80000000u) || (bias >= 0 && delta != (uint32_t)bias)) {
                problem(&context, xformat("%s+0x%X: ROM word 0x%08X is not 0x%08X", section->label, r->offset, be32(rom + r->offset), expected));
                continue;
            }
            bias = delta;
        }
        for (uint32_t i = 0; i < section->size; i++) {
            if (relocated[i] || section->data[i] == rom[i]) continue;
            problem(&context, xformat("%s+0x%X: byte 0x%02X differs from ROM 0x%02X at 0x%08X", section->label, i, section->data[i], rom[i], base[s] + i));
            break;
        }
        free(data_rels);
        free(relocated);
    }

    /* Write absolute %hi/%lo pairs; drop their relocations. */
    for (size_t i = 0; i < ref_count; i++) {
        const Reference *ref = &refs[i];
        if (!known[ref->section]) continue;
        uint32_t address = base[ref->section] + ref->section_offset;
        uint32_t hi = be32(code->data + ref->hi), lo = be32(code->data + ref->lo);
        uint32_t new_hi = (hi & 0xFFFF0000) | (((address + 0x8000) >> 16) & 0xFFFF);
        for (size_t j = 0; j < i; j++)
            if (refs[j].hi == ref->hi && known[refs[j].section] && be32(code->data + ref->hi) != new_hi)
                problem(&context, xformat("HI16 at .text+0x%X is shared by references with different upper halves", ref->hi));
        put_be32(code->data + ref->hi, new_hi);
        put_be32(code->data + ref->lo, (lo & 0xFFFF0000) | (address & 0xFFFF));
    }
    if (text_rels_index) {
        size_t kept = 0;
        for (size_t i = 0; i < rel_count; i++) {
            bool resolved = drop[i] && known[elf.symbols[rels[i].symbol].shndx];
            if (!resolved) rels[kept++] = rels[i];
        }
        elf_set_rels(&elf, text_rels_index, rels, kept);
    }
    for (size_t y = 0; y < elf.symbol_count; y++) {
        Symbol *symbol = &elf.symbols[y];
        if (symbol->shndx >= count || !known[symbol->shndx] || (symbol->info & 15) == STT_SECTION) continue;
        symbol->value += base[symbol->shndx];
        symbol->shndx = SHN_ABS;
    }
    elf_store_symbols(&elf);

    /* Trim trailing zero padding after the last function, then pad to the row. */
    if (trim) {
        uint32_t end = 0;
        for (size_t y = 0; y < elf.symbol_count; y++) {
            const Symbol *symbol = &elf.symbols[y];
            if (symbol->shndx == text && (symbol->info & 15) == STT_FUNC && symbol->size && symbol->value + symbol->size > end)
                end = symbol->value + symbol->size;
        }
        if (end > code->size) die("place: %s: function size exceeds .text", object);
        bool zeros = end > 0;
        for (uint32_t i = end; zeros && i < code->size; i++) zeros = code->data[i] == 0;
        if (zeros) code->size = end;
    }
    if (code->size > text_window.size) {
        problem(&context, xformat(".text is 0x%X bytes; the row holds 0x%X", code->size, text_window.size));
    } else if (code->size < text_window.size) {
        code->data = xrealloc(code->data, text_window.size);
        memset(code->data + code->size, 0, text_window.size - code->size);
        code->size = text_window.size;
    }
    /* The row fixes .text's address. A coarser input alignment (IDO and gcc use 16) would make the linker
       move .text up and leave a gap before the function. */
    if (code->addralign > 4) code->addralign = 4;

    if (context.problems.len && !score) {
        Buf message = {0};
        for (size_t i = 0; i < context.problems.len; i++) buf_format(&message, "\n  %s", context.problems.items[i]);
        die("place: %s: not proved:%s", object, message.data);
    }
    for (size_t i = 0; i < context.problems.len; i++) fprintf(stderr, "n64link: place: unproved: %s\n", context.problems.items[i]);
    elf_write(&elf, output);
    return 0;
}

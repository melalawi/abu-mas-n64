/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mo */
/* Scheduling and macro rules derive from RocketRet/modern-asn64 (Unlicense). See NOTICE. */
/* asn64: turn SN64 cc1 assembly into GNU MIPS assembly that builds the bytes SN ASN64 2.81 builds. */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "n64link.h"
#include "util.h"

static const char *const HEADER =
    ".macro glabel label\n    .global \\label\n    \\label:\n.endm\n"
    ".macro dlabel label\n    .global \\label\n    \\label:\n.endm\n"
    ".macro move a, b\n    addu \\a, \\b, $0\n.endm\n"
    ".macro b target\n    bgez $0, \\target\n.endm\n\t.set noreorder\n";

static const char *const BRANCHES[] = {"bc1f", "bc1fl", "bc1t", "bc1tl", "beq", "beql", "bgez", "bgezal", "bgezall",
                                       "bgezl", "bgtz", "bgtzl", "blez", "blezl", "bltz", "bltzal", "bltzall", "bltzl",
                                       "bne", "bnel", "j", "jal", "jalr", "jr", "blt", "ble", "bgt", "bge", NULL};
static const char *const COP1_BRANCHES[] = {"bc1f", "bc1fl", "bc1t", "bc1tl", NULL};
static const char *const COMPARES[] = {"c.lt.s", "c.le.s", "c.eq.s", "c.lt.d", "c.le.d", "c.eq.d", NULL};
static const char *const LOAD_STORES[] = {"lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "sb", "sh", "sw",
                                          "sd", "lwc1", "swc1", "ldc1", "sdc1", "l.s", "l.d", "s.s", "s.d", NULL};

static bool member(const char *word, const char *const *set) {
    for (; *set; set++)
        if (strcmp(word, *set) == 0) return true;
    return false;
}

/* ---- literals: SN64 decimal conversion with seventeen significant digits ---- */

static const struct {
    int step;
    double power;
} POWERS[] = {{216, 0x1.734e940c6f9c6p+717}, {108, 0x1.b403dcc834e12p+358}, {54, 0x1.4e1878814c9cep+179},
              {27, 0x1.9d971e4fe8402p+89},   {14, 0x1.6bcc41e900000p+46},   {8, 0x1.7d78400000000p+26},
              {4, 0x1.3880000000000p+13},    {1, 0x1.4000000000000p+3}};
static const double E216 = 0x1.734e940c6f9c6p+717;

static char *literal_words(const char *text, bool is_double) {
    const char *p = text;
    char sign = 0;
    if (*p == '+' || *p == '-') sign = *p++;
    const char *whole = p;
    while (isdigit((unsigned char)*p)) p++;
    size_t whole_len = (size_t)(p - whole);
    const char *fraction = "";
    size_t fraction_len = 0;
    if (*p == '.') {
        fraction = ++p;
        while (isdigit((unsigned char)*p)) p++;
        fraction_len = (size_t)(p - fraction);
    }
    long long exponent = 0;
    if (*p == 'e' || *p == 'E') {
        const char *e = p + 1;
        const char *digits = e + (*e == '+' || *e == '-');
        if (isdigit((unsigned char)*digits)) {
            const char *end = digits;
            while (isdigit((unsigned char)*end)) end++;
            if (end - digits > 6) die("unsupported floating literal '%s'", text);
            exponent = strtoll(e, NULL, 10);
            p = end;
        }
    }
    if (*p != '\0' || (whole_len == 0 && fraction_len == 0)) die("unsupported floating literal '%s'", text);
    Buf all = {0};
    buf_add(&all, whole, whole_len);
    buf_add(&all, fraction, fraction_len);
    char *digits = buf_take(&all);
    const char *significant = digits + strspn(digits, "0");
    exponent -= (long long)fraction_len;
    size_t total = strlen(significant);
    size_t kept = total < 17 ? total : 17;
    exponent += (long long)(total - kept);
    size_t trimmed = kept;
    while (trimmed > 0 && significant[trimmed - 1] == '0') trimmed--;
    exponent += (long long)(kept - trimmed);
    uint64_t integer = 0;
    for (size_t i = 0; i < trimmed; i++) integer = integer * 10 + (uint64_t)(significant[i] - '0');
    free(digits);
    int bits = 0;
    for (uint64_t v = integer; v; v >>= 1) bits++;
    int shift = bits > 53 ? bits - 53 : 0;
    if (shift) integer = ((integer + ((uint64_t)1 << (shift - 1))) >> shift) << shift;
    double value = (double)integer;
    if (value != 0.0) {
        if (exponent > 308) {
            value *= E216;
            exponent -= 216;
        } else if (exponent < -308) {
            value /= E216;
            exponent += 216;
        }
        double power = 1.0;
        long long remaining = exponent < 0 ? -exponent : exponent;
        for (size_t i = 0; i < sizeof POWERS / sizeof POWERS[0]; i++) {
            while (remaining >= POWERS[i].step) {
                power *= POWERS[i].power;
                remaining -= POWERS[i].step;
            }
        }
        value = exponent < 0 ? value / power : value * power;
    }
    if (sign == '-' && value != 0.0) value = -value;
    uint8_t data[8];
    size_t size;
    if (is_double) {
        uint64_t raw;
        memcpy(&raw, &value, 8);
        put_be32(data, (uint32_t)(raw >> 32));
        put_be32(data + 4, (uint32_t)raw);
        size = 8;
    } else {
        float single = (float)value;
        if (isinf(single) && !isinf(value)) die("float too large to pack with f format: '%s'", text);
        uint32_t raw;
        memcpy(&raw, &single, 4);
        put_be32(data, raw);
        size = 4;
    }
    Buf out = {0};
    for (size_t i = 0; i < size; i += 4) buf_format(&out, "%s\t.word 0x%08x", i ? "\n" : "", be32(data + i));
    return buf_take(&out);
}

/* ---- macros ---- */

static bool symbol_base(const char *address, char **symbol, char **base) {
    const char *p = address;
    if (!(isalpha((unsigned char)*p) || *p == '_' || *p == '.')) return false;
    p++;
    while (isalnum((unsigned char)*p) || *p == '_' || *p == '.') p++;
    const char *symbol_end = p;
    if (*p == '+' || *p == '-') {
        const char *q = p + 1;
        if (q[0] == '0' && q[1] == 'x' && isxdigit((unsigned char)q[2])) {
            q += 2;
            while (isxdigit((unsigned char)*q)) q++;
        } else if (isdigit((unsigned char)*q)) {
            while (isdigit((unsigned char)*q)) q++;
        } else q = NULL;
        if (q != NULL && q[0] == '(') symbol_end = p = q;
    }
    if (p[0] != '(' || p[1] != '$') return false;
    const char *register_start = p + 2, *r = register_start;
    while (isalnum((unsigned char)*r)) r++;
    if (r == register_start || r[0] != ')' || r[1] != '\0') return false;
    *symbol = xstrndup(address, (size_t)(symbol_end - address));
    *base = xstrndup(register_start, (size_t)(r - register_start));
    return true;
}

static bool wide_offset(const char *address, char **value_text, char **base) {
    const char *p = address;
    if (*p == '-') p++;
    if (p[0] == '0' && p[1] == 'x' && isxdigit((unsigned char)p[2])) {
        p += 2;
        while (isxdigit((unsigned char)*p)) p++;
    } else if (isdigit((unsigned char)*p)) {
        while (isdigit((unsigned char)*p)) p++;
    } else return false;
    const char *value_end = p;
    if (p[0] != '(' || p[1] != '$') return false;
    const char *r = p + 2;
    while (isalnum((unsigned char)*r) || *r == '_') r++;
    if (r == p + 2 || r[0] != ')' || r[1] != '\0') return false;
    *value_text = xstrndup(address, (size_t)(value_end - address));
    *base = xstrndup(p + 1, (size_t)(r - p - 1));
    return true;
}

static char *memory(const char *opcode, const char *operand, const char *original) {
    StrVec parts = split_on(operand, ',', 1);
    char *reg = strip(parts.items[0]);
    char *address = strip(parts.items[1]);
    vec_free(&parts);
    char *symbol, *base, *result = NULL;
    if (symbol_base(address, &symbol, &base)) {
        char *add = (strcmp(base, "0") == 0 || strcmp(base, "zero") == 0) ? xstrdup("") : xformat("\taddu $at, $at, $%s\n", base);
        result = xformat("\t.set noat\n\tlui $at, %%hi(%s)\n%s\t%s %s, %%lo(%s)($at)\n\t.set at\n", symbol, add, opcode, reg, symbol);
        free(add);
        free(symbol);
        free(base);
    } else if (wide_offset(address, &symbol, &base)) {
        long long value;
        if (!parse_int0(symbol, &value)) die("invalid literal for int() with base 0: '%s'", symbol);
        if (value < -32768 || value > 32767) {
            long long low = ((value + 32768) & 65535) - 32768;
            long long high = ((value - low) / 65536) & 65535;
            result = xformat("\t.set noat\n\tlui $at,%lld\n\taddu $at,%s,$at\n\t%s %s,%lld($at)\n\t.set at\n", high, base, opcode, reg, low);
        }
        free(symbol);
        free(base);
    }
    free(reg);
    free(address);
    return result ? result : xstrdup(original);
}

/* mark >= 0 labels the 8-byte alignment point of a double for place. */
static char *floating(const char *opcode, StrVec *operands, long long number, long long mark) {
    if (operands->len < 2) die("%s: expected two operands", opcode);
    bool is_double = strcmp(opcode, "li.d") == 0;
    char *words = literal_words(operands->items[1], is_double);
    char *label = mark >= 0 ? xformat("ALIGN8_MARK_%lld:\n", mark) : xstrdup("");
    char *result = xformat("\t.section .rdata\nRODATA_SYM_%lld:\n\t.align %d\n%s%s\n\t.text\n\t.set noat\n"
                           "\tlui $at, %%hi(RODATA_SYM_%lld)\n\t%s %s, %%lo(RODATA_SYM_%lld)($at)\n\t.set at\n",
                           number, is_double ? 3 : 2, label, words, number, is_double ? "ldc1" : "lwc1", operands->items[0], number);
    free(words);
    free(label);
    return result;
}

static char *division(const char *opcode, StrVec *operands, long long number) {
    if (operands->len != 3) die("%s: expected three operands", opcode);
    const char *target = operands->items[0], *dividend = operands->items[1], *divisor = operands->items[2];
    bool is_unsigned = strcmp(opcode, "divu") == 0 || strcmp(opcode, "remu") == 0;
    bool remainder = strcmp(opcode, "rem") == 0 || strcmp(opcode, "remu") == 0;
    Buf text = {0};
    buf_format(&text, "\t.set noat\n\t%s $0,%s,%s\n\tbnez %s,BRANCH_LABEL_%lld\n\tnop\n\tbreak 0x7\nBRANCH_LABEL_%lld:\n",
               is_unsigned ? "divu" : "div", dividend, divisor, divisor, number, number);
    if (!is_unsigned)
        buf_format(&text, "\taddiu $1,$0,-1\n\tbne %s,$1,BRANCH_LABEL_%lld\n\tlui $1,0x8000\n\tbne %s,$1,BRANCH_LABEL_%lld\n\tnop\n\tbreak 0x6\nBRANCH_LABEL_%lld:\n",
                   divisor, number + 1, dividend, number + 1, number + 1);
    buf_format(&text, "\t%s %s\n\t.set at\n", remainder ? "mfhi" : "mflo", target);
    return buf_take(&text);
}

/* ---- directives ---- */

static StrVec python_lines(const char *text, bool keep_ends) {
    StrVec lines = {0};
    const char *p = text;
    while (*p) {
        const char *start = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        const char *end = p;
        if (*p == '\r' && p[1] == '\n') p += 2;
        else if (*p) p++;
        vec_push(&lines, xstrndup(start, (size_t)((keep_ends ? p : end) - start)));
    }
    return lines;
}

static void string_bytes(const char *operands, Buf *result) {
    char *stripped = strip(operands);
    const char *remaining = stripped;
    while (*remaining) {
        if (*remaining != '"') die(".string: invalid operand '%s'", remaining);
        const char *p = remaining + 1;
        while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
        if (*p != '"') die(".string: invalid operand '%s'", remaining);
        for (const char *c = remaining + 1; c < p;) {
            if (*c != '\\') {
                buf_add(result, c++, 1);
                continue;
            }
            char escape = c[1];
            c += 2;
            uint8_t byte;
            if (escape >= '0' && escape <= '7') {
                int value = escape - '0', count = 1;
                while (c < p && count < 3 && *c >= '0' && *c <= '7') value = value * 8 + (*c++ - '0'), count++;
                byte = (uint8_t)(value & 0xFF);
            } else {
                switch (escape) {
                case 'a': byte = 7; break;
                case 'b': byte = 8; break;
                case 'f': byte = 12; break;
                case 'n': byte = 10; break;
                case 'r': byte = 13; break;
                case 't': byte = 9; break;
                case 'v': byte = 11; break;
                case '"': byte = 34; break;
                case '\\': byte = 92; break;
                default: die(".string: unsupported escape \\%c", escape);
                }
            }
            buf_add(result, (const char *)&byte, 1);
        }
        buf_add(result, "", 1);
        const char *rest = p + 1;
        while (*rest && is_space((unsigned char)*rest)) rest++;
        if (!*rest || *rest == '#') break;
        const char *after = rest + 1;
        while (*after && is_space((unsigned char)*after)) after++;
        if (*rest != ',' || !*after) die(".string: invalid trailing operand '%s'", rest);
        remaining = after;
    }
    if (result->len == 0) die(".string: missing operand");
    free(stripped);
}

/* Match ^(\s*(?:[.\w]+:\s*)?)\.string(?:\s+(.*))?$ and return the prefix length and operand start. */
static bool string_directive(const char *line, size_t *prefix, const char **operand) {
    const char *p = line;
    while (*p && is_space((unsigned char)*p)) p++;
    const char *after_space = p;
    for (int with_label = 1; with_label >= 0; with_label--) {
        const char *q = after_space;
        if (with_label) {
            const char *label = q;
            while (isalnum((unsigned char)*q) || *q == '_' || *q == '.') q++;
            if (q == label || *q != ':') continue;
            q++;
            while (*q && is_space((unsigned char)*q)) q++;
        }
        if (!starts_with(q, ".string") || (q[7] && !is_space((unsigned char)q[7]))) continue;
        *prefix = (size_t)(q - line);
        q += 7;
        while (*q && is_space((unsigned char)*q)) q++;
        *operand = q;
        return true;
    }
    return false;
}

static char *replace_all(const char *text, const char *from, const char *to) {
    Buf out = {0};
    size_t from_len = strlen(from);
    for (const char *p = text;;) {
        const char *hit = strstr(p, from);
        if (hit == NULL) {
            buf_str(&out, p);
            break;
        }
        buf_add(&out, p, (size_t)(hit - p));
        buf_str(&out, to);
        p = hit + from_len;
    }
    return buf_take(&out);
}

static char *directives(const char *text) {
    StrVec lines = python_lines(text, false);
    Buf out = {0};
    size_t emitted = 0;
    for (size_t i = 0; i < lines.len; i++) {
        const char *line = lines.items[i];
        char *stripped = strip(line);
        bool skip = strcmp(stripped, ".set gp=64") == 0 || starts_with(stripped, ".version") ||
                    starts_with(stripped, ".size") || starts_with(stripped, ".type") || starts_with(stripped, ".ident");
        free(stripped);
        if (skip) continue;
        size_t prefix;
        const char *operand;
        if (string_directive(line, &prefix, &operand)) {
            Buf content = {0};
            string_bytes(operand, &content);
            for (size_t offset = 0; offset < content.len; offset += 16) {
                if (offset == 0) buf_add(&out, line, prefix);
                else buf_str(&out, "\t");
                buf_str(&out, ".byte ");
                for (size_t j = offset; j < content.len && j < offset + 16; j++)
                    buf_format(&out, "%s%u", j == offset ? "" : ",", (unsigned char)content.data[j]);
                buf_str(&out, "\r\n");
                emitted++;
            }
            free(content.data);
            continue;
        }
        char *replaced = replace_all(line, ".rodata", ".rdata");
        buf_str(&out, replaced);
        buf_str(&out, "\r\n");
        emitted++;
        free(replaced);
    }
    if (emitted == 0) buf_str(&out, "\r\n");
    vec_free(&lines);
    return buf_take(&out);
}

/* ---- schedule ---- */

static StrVec tokens_for(const char *line) {
    char *stripped = strip(line);
    char *hash = strchr(stripped, '#');
    if (hash) *hash = '\0';
    StrVec tokens = split_whitespace(stripped);
    free(stripped);
    return tokens;
}

static bool previous_is_compare(const StrVec *preprocessed) {
    /* The last non-empty line that is not a comment, directive or label. */
    for (size_t i = preprocessed->len; i-- > 0;) {
        StrVec lines = python_lines(preprocessed->items[i], false);
        for (size_t j = lines.len; j-- > 0;) {
            char *item = strip(lines.items[j]);
            bool usable = item[0] && item[0] != '#' && item[0] != '.' && !ends_with(item, ":");
            if (usable) {
                StrVec words = split_whitespace(item);
                bool compare = words.len && member(words.items[0], COMPARES);
                vec_free(&words);
                free(item);
                vec_free(&lines);
                return compare;
            }
            free(item);
        }
        vec_free(&lines);
    }
    return false;
}

typedef struct {
    char *symbol;
    char *size;
} Common;

static char *schedule(const char *text) {
    StrVec input = python_lines(text, true);
    StrVec out = {0};
    vec_push(&out, xstrdup(HEADER));
    bool is_reorder = true, prev_mul = false, delay_slot = false;
    long long generated = 0, marks = 0;
    bool in_constant = false;
    int delay_count = 0;
    size_t delay_location = 0, prev_instruction = 0, file_count = 0;
    long long last_file = -1;
    StrVec locals = {0};
    Common *comm = NULL, *lcomm = NULL;
    size_t comm_len = 0, lcomm_len = 0;
    for (size_t index = 0; index < input.len; index++) {
        char *line = xstrdup(input.items[index]);
        StrVec tokens = tokens_for(line);
        if (strlen(line) >= 5 && strncmp(line + 1, "#nop", 4) == 0) {
            StrVec previous = tokens_for(input.items[prev_instruction]);
            if (previous.len && member(previous.items[0], COMPARES)) {
                free(line);
                line = xstrdup("\tnop\n");
            }
            vec_free(&previous);
        }
        if (tokens.len == 0) {
            vec_push(&out, line);
            vec_free(&tokens);
            continue;
        }
        const char *identifier = tokens.items[0];
        bool new_prev_mul = false, is_branch = false, dropped = false, mark = false;
        if (identifier[0] == '.') {
            const char *directive = identifier + 1;
            if (strcmp(directive, "section") == 0 || strcmp(directive, "text") == 0 || strcmp(directive, "data") == 0 ||
                strcmp(directive, "rdata") == 0 || strcmp(directive, "rodata") == 0 || strcmp(directive, "sdata") == 0 ||
                strcmp(directive, "bss") == 0) {
                bool section = strcmp(directive, "section") == 0;
                in_constant = section ? tokens.len >= 2 && (strstr(tokens.items[1], "rdata") || strstr(tokens.items[1], "rodata"))
                                      : strcmp(directive, "rdata") == 0 || strcmp(directive, "rodata") == 0;
            } else if (strcmp(directive, "align") == 0) {
                long long power;
                mark = in_constant && tokens.len >= 2 && parse_int0(tokens.items[1], &power) && power >= 3;
            } else if (strcmp(directive, "set") == 0) {
                if (tokens.len < 2) die(".set: missing operand");
                if (strcmp(tokens.items[1], "noreorder") == 0 || strcmp(tokens.items[1], "reorder") == 0) {
                    is_reorder = strcmp(tokens.items[1], "reorder") == 0;
                    dropped = true;
                }
            } else if (strcmp(directive, "local") == 0) {
                if (tokens.len < 2) die(".local: missing operand");
                vec_push(&locals, xstrdup(tokens.items[1]));
            } else if (strcmp(directive, "comm") == 0 || strcmp(directive, "lcomm") == 0) {
                if (tokens.len < 2) die("%s: missing operand", identifier);
                StrVec parts = split_on(tokens.items[1], ',', 0);
                bool local_comm = strcmp(directive, "lcomm") == 0;
                if (local_comm ? parts.len != 2 : parts.len < 2) die("%s: expected SYMBOL,SIZE", identifier);
                Common item = {strip(parts.items[0]), strip(parts.items[1])};
                bool is_local = local_comm;
                for (size_t i = 0; !is_local && i < locals.len; i++) is_local = strcmp(locals.items[i], item.symbol) == 0;
                if (is_local) {
                    lcomm = xrealloc(lcomm, (lcomm_len + 1) * sizeof *lcomm);
                    lcomm[lcomm_len++] = item;
                } else {
                    comm = xrealloc(comm, (comm_len + 1) * sizeof *comm);
                    comm[comm_len++] = item;
                }
                vec_free(&parts);
                free(line);
                line = xstrdup("");
            } else if (strcmp(directive, "file") == 0) {
                if (tokens.len < 3) die(".file: expected NUMBER NAME");
                file_count++;
                free(line);
                line = xformat("\t.file\t%zu %s\n", file_count + 1, tokens.items[2]);
                last_file = (long long)out.len;
            } else if (strcmp(directive, "def") == 0 || strcmp(directive, "begin") == 0 || strcmp(directive, "bend") == 0) {
                free(line);
                line = xstrdup("");
            } else if (strcmp(directive, "word") == 0) {
                if (tokens.len < 2) die(".word: missing operand");
                if (tokens.items[1][0] == '$') {
                    free(line);
                    line = xformat("\t.word\t.%s\n", tokens.items[1] + 1);
                }
            }
        } else if (identifier[strlen(identifier) - 1] == ':') {
            /* label */
        } else {
            if (member(identifier, BRANCHES)) {
                is_branch = true;
                if (member(identifier, COP1_BRANCHES) && previous_is_compare(&out)) vec_push(&out, xstrdup("\tnop\n"));
                if (is_reorder) {
                    char *with_nop = xformat("%s\tnop\n", line);
                    free(line);
                    line = with_nop;
                }
            } else if (member(identifier, LOAD_STORES) && tokens.len >= 2 && strchr(tokens.items[1], ',')) {
                char *expanded = memory(identifier, tokens.items[1], line);
                free(line);
                line = expanded;
            } else if (strcmp(identifier, "li") == 0) {
                if (tokens.len < 2) die("li: missing operands");
                StrVec operands = split_on(tokens.items[1], ',', 0);
                if (operands.len < 2) die("li: expected REGISTER,VALUE");
                long long value;
                if (!parse_int0(operands.items[1], &value)) die("invalid literal for int() with base 0: '%s'", operands.items[1]);
                if (value >= -32768 && value <= 32767) {
                    free(line);
                    line = xformat("\taddiu %s,$0,%lld\n", operands.items[0], value);
                }
                vec_free(&operands);
            } else if (strcmp(identifier, "li.s") == 0 || strcmp(identifier, "li.d") == 0) {
                if (tokens.len < 2) die("%s: missing operands", identifier);
                StrVec raw = split_on(tokens.items[1], ',', 0), operands = {0};
                for (size_t i = 0; i < raw.len; i++) vec_push(&operands, strip(raw.items[i]));
                vec_free(&raw);
                if (starts_with(operands.items[0], "$f")) {
                    free(line);
                    bool is_double = strcmp(identifier, "li.d") == 0;
                    line = floating(identifier, &operands, generated, is_double ? marks : -1);
                    if (is_double) marks++;
                    generated++;
                    in_constant = false;
                }
                vec_free(&operands);
            } else if (strcmp(identifier, "div") == 0 || strcmp(identifier, "divu") == 0 || strcmp(identifier, "rem") == 0 ||
                       strcmp(identifier, "remu") == 0) {
                for (; delay_count > 0; delay_count--) vec_push(&out, xstrdup("\tnop\n"));
                if (tokens.len < 2) die("%s: missing operands", identifier);
                StrVec raw = split_on(tokens.items[1], ',', 0), operands = {0};
                for (size_t i = 0; i < raw.len; i++) vec_push(&operands, strip(raw.items[i]));
                vec_free(&raw);
                if (strcmp(operands.items[0], "$0") != 0) {
                    free(line);
                    line = division(identifier, &operands, generated);
                    bool is_unsigned = strcmp(identifier, "divu") == 0 || strcmp(identifier, "remu") == 0;
                    generated += is_unsigned ? 1 : 2;
                    delay_count = 3;
                    delay_location = out.len + 1;
                }
                vec_free(&operands);
            } else if (strcmp(identifier, "mflo") == 0 || strcmp(identifier, "mfhi") == 0) {
                delay_count = 3;
                delay_location = out.len + 1;
            } else if (strcmp(identifier, "mult") == 0 || strcmp(identifier, "mul.s") == 0 || strcmp(identifier, "mul.d") == 0) {
                bool is_mult = strcmp(identifier, "mult") == 0;
                if (delay_slot && !is_reorder && delay_count <= 0) {
                    vec_insert(&out, out.len - 1, line);
                    line = xstrdup("\tnop\n");
                }
                if (is_mult) {
                    for (; delay_count > 0; delay_count--) {
                        if (delay_slot) vec_insert(&out, delay_location, xstrdup("\tnop\n"));
                        else vec_push(&out, xstrdup("\tnop\n"));
                    }
                }
                if (prev_mul) {
                    char *with_nop = xformat("\tnop\n%s", line);
                    free(line);
                    line = with_nop;
                }
                new_prev_mul = true;
            }
            if (delay_count > 0) delay_count--;
            prev_mul = new_prev_mul;
            delay_slot = is_branch;
            prev_instruction = index;
        }
        vec_free(&tokens);
        if (dropped) {
            free(line);
            continue;
        }
        vec_push(&out, line);
        if (mark) vec_push(&out, xformat("ALIGN8_MARK_%lld:\n", marks++));
    }
    if (comm_len || lcomm_len) vec_push(&out, xstrdup("\t.section\t.bss\n"));
    for (int pass = 0; pass < 2; pass++) {
        Common *items = pass == 0 ? lcomm : comm;
        size_t len = pass == 0 ? lcomm_len : comm_len;
        for (size_t i = 0; i < len; i++) {
            long long size;
            if (!parse_int10(items[i].size, &size)) die("invalid literal for int() with base 10: '%s'", items[i].size);
            if (size > 4) vec_push(&out, xstrdup("\t.align 3\n"));
            vec_push(&out, xformat("\t.globl %s\n%s:\n\t.space %s\n", items[i].symbol, items[i].symbol, items[i].size));
            free(items[i].symbol);
            free(items[i].size);
        }
    }
    free(comm);
    free(lcomm);
    if (last_file != -1) {
        StrVec file_tokens = tokens_for(out.items[last_file]);
        if (file_tokens.len < 3) die(".file: directive moved; cannot rewrite");
        char *directive = xformat("\t.file\t1 %s\n", file_tokens.items[2]);
        vec_free(&file_tokens);
        free(out.items[last_file]);
        out.items[last_file] = xstrdup("");
        vec_insert(&out, 0, directive);
    }
    Buf result = {0};
    for (size_t i = 0; i < out.len; i++) buf_str(&result, out.items[i]);
    vec_free(&out);
    vec_free(&input);
    vec_free(&locals);
    return buf_take(&result);
}

char *asn64_normalize(const char *text) {
    char *lowered = directives(text);
    char *scheduled = schedule(lowered);
    free(lowered);
    return scheduled;
}

static void assemble(const char *content, const char *assembler, char **flags, size_t flag_count, const char *output) {
    for (size_t i = 0; i < flag_count; i++) {
        const char *flag = flags[i];
        if (strcmp(flag, "-g") == 0 || strcmp(flag, "--gen-debug") == 0 || (starts_with(flag, "-G") && strcmp(flag, "-G0") != 0))
            die("asn64: GNU-as flag %s: unsupported by the proven ASN64 2.81 configuration", flag);
        if (strcmp(flag, "-") == 0 || starts_with(flag, "-o") || starts_with(flag, "--output"))
            die("asn64: GNU-as flag %s: use the explicit output argument", flag);
    }
    make_parents(output);
    char **argv = xmalloc((flag_count + 5) * sizeof(char *));
    size_t n = 0;
    argv[n++] = (char *)assembler;
    for (size_t i = 0; i < flag_count; i++) argv[n++] = flags[i];
    argv[n++] = "-o";
    argv[n++] = (char *)output;
    argv[n++] = "-";
    argv[n] = NULL;
    int input[2];
    if (pipe(input) != 0) die("asn64: pipe: %s", strerror(errno));
    pid_t child = fork();
    if (child < 0) die("asn64: fork: %s", strerror(errno));
    if (child == 0) {
        dup2(input[0], 0);
        close(input[0]);
        close(input[1]);
        execvp(assembler, argv); /* --as may be a PATH name (the generated Makefile passes one) */
        fprintf(stderr, "n64link: asn64: %s: %s\n", assembler, strerror(errno));
        _exit(127);
    }
    close(input[0]);
    size_t len = strlen(content), written = 0;
    while (written < len) {
        ssize_t got = write(input[1], content + written, len - written);
        if (got < 0) {
            if (errno == EINTR) continue;
            break; /* the assembler exited early; its status reports why */
        }
        written += (size_t)got;
    }
    close(input[1]);
    int status;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) die("asn64: wait: %s", strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        die("asn64: GNU as exited %d", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    free(argv);
}

int asn64_main(int argc, char **argv) {
    const char *assembler = NULL, *source = NULL, *output = NULL;
    char **flags = xmalloc((size_t)argc * sizeof(char *));
    size_t flag_count = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--as") == 0) {
            if (++i >= argc) die("asn64: --as needs a path");
            assembler = argv[i];
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) die("asn64: -o needs a path");
            output = argv[i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            flags[flag_count++] = argv[i];
        } else {
            if (source != NULL) die("asn64: one source file expected, got %s and %s", source, argv[i]);
            source = argv[i];
        }
    }
    if ((assembler == NULL) != (output == NULL)) die("asn64: --as and -o go together");
    if (assembler == NULL && flag_count) die("asn64: assembler flags need --as and -o");
    size_t len;
    char *text = (char *)read_file(source ? source : "-", &len);
    if (strlen(text) != len) die("asn64: %s: NUL byte in assembly", source ? source : "stdin");
    char *normalized = asn64_normalize(text);
    if (assembler == NULL) {
        fputs(normalized, stdout);
        if (fflush(stdout) != 0) die("asn64: stdout: %s", strerror(errno));
    } else {
        assemble(normalized, assembler, flags, flag_count, output);
    }
    free(normalized);
    free(text);
    free(flags);
    return 0;
}

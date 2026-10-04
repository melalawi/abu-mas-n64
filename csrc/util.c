/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mo */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void die(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("n64link: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

void *xmalloc(size_t size) {
    void *pointer = malloc(size ? size : 1);
    if (pointer == NULL) die("out of memory");
    return pointer;
}

void *xrealloc(void *pointer, size_t size) {
    void *result = realloc(pointer, size ? size : 1);
    if (result == NULL) die("out of memory");
    return result;
}

char *xstrndup(const char *text, size_t len) {
    char *copy = xmalloc(len + 1);
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

char *xstrdup(const char *text) { return xstrndup(text, strlen(text)); }

char *xformat(const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (len < 0) die("format failed");
    char *text = xmalloc((size_t)len + 1);
    vsnprintf(text, (size_t)len + 1, format, args);
    va_end(args);
    return text;
}

void buf_add(Buf *buf, const char *data, size_t len) {
    if (buf->len + len + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 256;
        while (cap < buf->len + len + 1) cap *= 2;
        buf->data = xrealloc(buf->data, cap);
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

void buf_str(Buf *buf, const char *text) { buf_add(buf, text, strlen(text)); }

void buf_format(Buf *buf, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (len < 0) die("format failed");
    char *text = xmalloc((size_t)len + 1);
    vsnprintf(text, (size_t)len + 1, format, args);
    va_end(args);
    buf_add(buf, text, (size_t)len);
    free(text);
}

char *buf_take(Buf *buf) {
    if (buf->data == NULL) buf_add(buf, "", 0);
    char *data = buf->data;
    buf->data = NULL;
    buf->len = buf->cap = 0;
    return data;
}

void vec_push(StrVec *vec, char *item) { vec_insert(vec, vec->len, item); }

void vec_insert(StrVec *vec, size_t index, char *item) {
    if (index > vec->len) index = vec->len; /* Python list.insert clamps. */
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 64;
        vec->items = xrealloc(vec->items, vec->cap * sizeof(char *));
    }
    memmove(vec->items + index + 1, vec->items + index, (vec->len - index) * sizeof(char *));
    vec->items[index] = item;
    vec->len++;
}

void vec_free(StrVec *vec) {
    for (size_t i = 0; i < vec->len; i++) free(vec->items[i]);
    free(vec->items);
    vec->items = NULL;
    vec->len = vec->cap = 0;
}

bool is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= 0x1c && c <= 0x1f); }

char *strip(const char *text) {
    const char *start = text;
    while (*start && is_space((unsigned char)*start)) start++;
    const char *end = start + strlen(start);
    while (end > start && is_space((unsigned char)end[-1])) end--;
    return xstrndup(start, (size_t)(end - start));
}

StrVec split_whitespace(const char *text) {
    StrVec result = {0};
    const char *p = text;
    while (*p) {
        while (*p && is_space((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !is_space((unsigned char)*p)) p++;
        vec_push(&result, xstrndup(start, (size_t)(p - start)));
    }
    return result;
}

StrVec split_on(const char *text, char separator, size_t limit) {
    StrVec result = {0};
    const char *start = text;
    for (const char *p = text;; p++) {
        if ((*p == separator && (limit == 0 || result.len < limit)) || *p == '\0') {
            vec_push(&result, xstrndup(start, (size_t)(p - start)));
            if (*p == '\0') break;
            start = p + 1;
        }
    }
    return result;
}

bool starts_with(const char *text, const char *prefix) { return strncmp(text, prefix, strlen(prefix)) == 0; }

bool ends_with(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    return a >= b && memcmp(text + a - b, suffix, b) == 0;
}

static bool digits_in(const char *p, int base, long long *value) {
    if (!*p) return false;
    unsigned long long result = 0;
    for (; *p; p++) {
        int digit;
        if (isdigit((unsigned char)*p)) digit = *p - '0';
        else if (isalpha((unsigned char)*p)) digit = tolower((unsigned char)*p) - 'a' + 10;
        else return false;
        if (digit >= base) return false;
        if (result > (unsigned long long)INT64_MAX / (unsigned)base) return false;
        result = result * (unsigned)base + (unsigned)digit;
    }
    if (result > (unsigned long long)INT64_MAX) return false;
    *value = (long long)result;
    return true;
}

static bool parse_int(const char *text, bool base0, long long *value) {
    char *stripped = strip(text);
    const char *p = stripped;
    bool negative = false;
    if (*p == '+' || *p == '-') negative = *p++ == '-';
    bool ok;
    if (base0 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ok = digits_in(p + 2, 16, value);
    else if (base0 && p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) ok = digits_in(p + 2, 8, value);
    else if (base0 && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) ok = digits_in(p + 2, 2, value);
    else if (base0 && p[0] == '0' && p[1] != '\0') {
        /* Python refuses leading zeros in base 0 unless every digit is zero. */
        ok = strspn(p, "0") == strlen(p) && digits_in(p, 10, value);
    } else ok = digits_in(p, 10, value);
    free(stripped);
    if (ok && negative) *value = -*value;
    return ok;
}

bool parse_int0(const char *text, long long *value) { return parse_int(text, true, value); }
bool parse_int10(const char *text, long long *value) { return parse_int(text, false, value); }

uint8_t *read_file(const char *path, size_t *len) {
    FILE *file = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (file == NULL) die("%s: %s", path, strerror(errno));
    Buf buf = {0};
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, file)) > 0) buf_add(&buf, chunk, got);
    if (ferror(file)) die("%s: read failed", path);
    if (file != stdin) fclose(file);
    *len = buf.len;
    return (uint8_t *)buf_take(&buf);
}

void make_parents(const char *path) {
    char *copy = xstrdup(path);
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, 0777) != 0 && errno != EEXIST) die("%s: %s", copy, strerror(errno));
        *p = '/';
    }
    free(copy);
}

void write_file(const char *path, const void *data, size_t len) {
    make_parents(path);
    char *temporary = xformat("%s.tmp.%ld", path, (long)getpid());
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) die("%s: %s", temporary, strerror(errno));
    if (fwrite(data, 1, len, file) != len || fclose(file) != 0) die("%s: write failed", temporary);
    if (rename(temporary, path) != 0) die("%s: %s", path, strerror(errno));
    free(temporary);
}

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

void put_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

void put_be16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

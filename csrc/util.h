/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mo */
#ifndef N64LINK_UTIL_H
#define N64LINK_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StrVec;

_Noreturn void die(const char *format, ...) __attribute__((format(printf, 1, 2)));
void *xmalloc(size_t size);
void *xrealloc(void *pointer, size_t size);
char *xstrdup(const char *text);
char *xstrndup(const char *text, size_t len);
char *xformat(const char *format, ...) __attribute__((format(printf, 1, 2)));

void buf_add(Buf *buf, const char *data, size_t len);
void buf_str(Buf *buf, const char *text);
void buf_format(Buf *buf, const char *format, ...) __attribute__((format(printf, 2, 3)));
char *buf_take(Buf *buf);

void vec_push(StrVec *vec, char *item);
void vec_insert(StrVec *vec, size_t index, char *item);
void vec_free(StrVec *vec);

/* Whitespace as Python's str.strip() and str.split() see it for ASCII text. */
bool is_space(int c);
char *strip(const char *text);
StrVec split_whitespace(const char *text);
StrVec split_on(const char *text, char separator, size_t limit);
bool starts_with(const char *text, const char *prefix);
bool ends_with(const char *text, const char *suffix);

/* Python int(text, 0) and int(text): false when Python would raise ValueError. */
bool parse_int0(const char *text, long long *value);
bool parse_int10(const char *text, long long *value);

uint8_t *read_file(const char *path, size_t *len);
void write_file(const char *path, const void *data, size_t len);
void make_parents(const char *path);

uint32_t be32(const uint8_t *p);
uint16_t be16(const uint8_t *p);
void put_be32(uint8_t *p, uint32_t value);
void put_be16(uint8_t *p, uint16_t value);

#endif

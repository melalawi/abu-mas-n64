# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2
WARNINGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
SOURCES = csrc/main.c csrc/asn64.c csrc/util.c
HEADERS = csrc/n64link.h csrc/util.h

build/n64link: $(SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) -static -o $@ $(SOURCES) -lm

.PHONY: clean
clean:
	rm -f build/n64link

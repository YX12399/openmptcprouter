# SPDX-License-Identifier: GPL-2.0
#
# Standalone dev build for ratan_telemetryd.
# Requirements (Ubuntu 24.04):
#   apt install build-essential libsqlite3-dev

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-array-bounds
PREFIX  ?= /usr/local
DESTDIR ?=

all: ratan_telemetryd

ratan_telemetryd: ratan_telemetryd.c discover.c discover.h ../prober/ratan_proto.h
	$(CC) $(CFLAGS) -I. -I../prober -pthread \
	    -o $@ ratan_telemetryd.c discover.c -lsqlite3

install: all
	install -d $(DESTDIR)$(PREFIX)/sbin $(DESTDIR)$(PREFIX)/share/ratan
	install -m 0755 ratan_telemetryd $(DESTDIR)$(PREFIX)/sbin/
	install -m 0644 schema.sql       $(DESTDIR)$(PREFIX)/share/ratan/schema.sql

clean:
	rm -f ratan_telemetryd

.PHONY: all install clean

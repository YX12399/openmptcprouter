# SPDX-License-Identifier: GPL-2.0
#
# Standalone dev build for ratan_prober + ratan_probe_responder.
# NOT the OpenWrt package Makefile.
#
# Requirements (Ubuntu 24.04):
#   apt install build-essential libbpf-dev

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
PREFIX  ?= /usr/local
DESTDIR ?=

VPS_SRC := ../vps/ratan_probe_responder.c

# Allow the responder source to live either alongside the prober (single
# dev box) or in src/vps/ (the in-repo layout).
ifneq ($(wildcard $(VPS_SRC)),)
RESPONDER_SRC := $(VPS_SRC)
else
RESPONDER_SRC := ratan_probe_responder.c
endif

all: ratan_prober ratan_probe_responder

ratan_prober: ratan_prober.c ratan_proto.h
	$(CC) $(CFLAGS) -I. -pthread -o $@ $< -lbpf

ratan_probe_responder: $(RESPONDER_SRC) ratan_proto.h
	$(CC) $(CFLAGS) -I. -o $@ $(RESPONDER_SRC)

install: all
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 0755 ratan_prober           $(DESTDIR)$(PREFIX)/sbin/
	install -m 0755 ratan_probe_responder  $(DESTDIR)$(PREFIX)/sbin/

clean:
	rm -f ratan_prober ratan_probe_responder

.PHONY: all install clean

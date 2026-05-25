# SPDX-License-Identifier: GPL-2.0
#
# Standalone dev build for ratan_sched.bpf.o + ratan_loader.
# NOT the OpenWrt package Makefile — that one will live in
# ratan/packaging/openwrt/ratan-sched/Makefile when STEP 2 lands.
#
# Requirements (Ubuntu 24.04):
#   apt install clang llvm libbpf-dev linux-tools-common linux-tools-$(uname -r) bpftool
# Kernel must be >=6.5 (mainline MPTCP BPF struct_ops scheduler support).

CLANG    ?= clang
CC       ?= cc
BPFTOOL  ?= bpftool
LLVM_STRIP ?= llvm-strip

PREFIX   ?= /usr/local
DESTDIR  ?=

VMLINUX_H := vmlinux.h

# Map uname -m to BPF __TARGET_ARCH_* macro
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
  TARGET_ARCH := x86
else ifeq ($(UNAME_M),aarch64)
  TARGET_ARCH := arm64
else
  TARGET_ARCH := $(UNAME_M)
endif

BPF_CFLAGS := -O2 -g -Wall \
              -target bpf \
              -D__TARGET_ARCH_$(TARGET_ARCH) \
              -I.

CFLAGS   ?= -O2 -Wall -Wextra
LDLIBS   := -lbpf

all: ratan_sched.bpf.o ratan_loader

$(VMLINUX_H):
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

ratan_sched.bpf.o: ratan_sched.bpf.c $(VMLINUX_H)
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@
	$(LLVM_STRIP) --strip-debug $@

ratan_loader: ratan_loader.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: all
	install -d $(DESTDIR)$(PREFIX)/lib/bpf $(DESTDIR)$(PREFIX)/sbin
	install -m 0644 ratan_sched.bpf.o $(DESTDIR)$(PREFIX)/lib/bpf/
	install -m 0755 ratan_loader     $(DESTDIR)$(PREFIX)/sbin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/bpf/ratan_sched.bpf.o
	rm -f $(DESTDIR)$(PREFIX)/sbin/ratan_loader

clean:
	rm -f ratan_sched.bpf.o ratan_loader $(VMLINUX_H)

.PHONY: all install uninstall clean

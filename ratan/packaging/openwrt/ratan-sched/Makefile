# SPDX-License-Identifier: GPL-2.0
#
# OpenWrt package recipe for ratan-sched: the BPF struct_ops MPTCP packet
# scheduler plus its libbpf-based loader. Source of truth lives at
# ratan/src/sched/ in this repo; this Makefile pulls those files in and
# compiles them with the OpenWrt clang/llvm-bpf toolchain (enabled by
# OMR's build.sh for kernel >=6.6 via CONFIG_BPF_TOOLCHAIN[_HOST]=y).
#
# vmlinux.h is generated at build time from the kernel's BTF using
# host bpftool. The kernel must be built before this package runs --
# OpenWrt's standard dependency ordering ensures that.

include $(TOPDIR)/rules.mk

PKG_NAME:=ratan-sched
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=RATAN team

PKG_BUILD_DEPENDS:=bpftool/host

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/kernel.mk

# Locate the in-repo source. PKG_DIR is the directory holding this Makefile;
# the cross-distro source lives three levels up at ratan/src/sched.
RATAN_PKG_DIR := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
RATAN_SRC_DIR := $(realpath $(RATAN_PKG_DIR)/../../../src/sched)

# Map OpenWrt's ARCH to clang's __TARGET_ARCH_* macro for BPF CO-RE.
ifeq ($(ARCH),aarch64)
  RATAN_BPF_TARGET_ARCH:=arm64
else ifeq ($(ARCH),x86_64)
  RATAN_BPF_TARGET_ARCH:=x86
else ifeq ($(ARCH),arm)
  RATAN_BPF_TARGET_ARCH:=arm
else
  RATAN_BPF_TARGET_ARCH:=$(ARCH)
endif

# Use recursive (=) assignment so STAGING_DIR_HOST is resolved at rule expansion.
RATAN_BPF_CLANG = $(firstword \
  $(wildcard $(STAGING_DIR_HOST)/llvm-bpf/bin/clang) \
  $(wildcard $(TOPDIR)/llvm-bpf/bin/clang) \
  $(shell command -v clang 2>/dev/null))

RATAN_BPF_LLVM_STRIP = $(firstword \
  $(wildcard $(STAGING_DIR_HOST)/llvm-bpf/bin/llvm-strip) \
  $(wildcard $(TOPDIR)/llvm-bpf/bin/llvm-strip) \
  $(shell command -v llvm-strip 2>/dev/null))

RATAN_BPFTOOL = $(firstword \
  $(wildcard $(STAGING_DIR_HOST)/bin/bpftool) \
  $(shell command -v bpftool 2>/dev/null))

define Package/ratan-sched
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=RATAN
  TITLE:=RATAN MPTCP BPF struct_ops scheduler + loader
  URL:=https://github.com/YX12399/openmptcprouter
  DEPENDS:=+libbpf @KERNEL_BPF_SYSCALL @KERNEL_DEBUG_INFO_BTF \
           @!arc @!TARGET_ramips
endef

define Package/ratan-sched/description
  RATAN's MPTCP BPF struct_ops packet scheduler.

  At boot the ratan_loader binary pins the scheduler's weight map at
  /sys/fs/bpf/ratan/path_weights and registers the struct_ops with
  the kernel. The companion ratan-predictor daemon (separate package)
  writes per-subflow weights into the map; the scheduler reads them
  per packet to pick the active subflow.

  Requires kernel >=6.5 (mainline MPTCP BPF struct_ops support); OMR
  ships the 999-mptcp-bpf.patch backport for kernels 6.6 and 6.12.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	[ -d "$(RATAN_SRC_DIR)" ] || { \
		echo "ERROR: RATAN source dir not found at $(RATAN_SRC_DIR)"; \
		exit 1; \
	}
	$(CP) $(RATAN_SRC_DIR)/ratan_sched.bpf.c $(PKG_BUILD_DIR)/
	$(CP) $(RATAN_SRC_DIR)/ratan_loader.c    $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

define Build/Compile
	@[ -n "$(RATAN_BPF_CLANG)" ] || { \
		echo "ERROR: no clang for BPF compilation."; \
		echo "  Expected $(STAGING_DIR_HOST)/llvm-bpf/bin/clang."; \
		echo "  Ensure CONFIG_BPF_TOOLCHAIN=y and CONFIG_BPF_TOOLCHAIN_HOST=y."; \
		exit 1; \
	}
	@[ -n "$(RATAN_BPFTOOL)" ] || { \
		echo "ERROR: bpftool not found (needed to dump kernel BTF)."; \
		exit 1; \
	}
	@[ -f $(LINUX_DIR)/vmlinux ] || { \
		echo "ERROR: $(LINUX_DIR)/vmlinux missing; build the kernel first."; \
		exit 1; \
	}
	$(RATAN_BPFTOOL) btf dump file $(LINUX_DIR)/vmlinux format c \
		> $(PKG_BUILD_DIR)/vmlinux.h
	$(RATAN_BPF_CLANG) -O2 -g -Wall \
		-target bpf \
		-D__TARGET_ARCH_$(RATAN_BPF_TARGET_ARCH) \
		-I$(PKG_BUILD_DIR) \
		-c $(PKG_BUILD_DIR)/ratan_sched.bpf.c \
		-o $(PKG_BUILD_DIR)/ratan_sched.bpf.o
	$(RATAN_BPF_LLVM_STRIP) --strip-debug $(PKG_BUILD_DIR)/ratan_sched.bpf.o
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) $(TARGET_LDFLAGS) \
		-Wall -Wextra \
		-o $(PKG_BUILD_DIR)/ratan_loader \
		$(PKG_BUILD_DIR)/ratan_loader.c \
		-lbpf
endef

define Package/ratan-sched/install
	$(INSTALL_DIR) $(1)/usr/lib/bpf
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/ratan_sched.bpf.o \
		$(1)/usr/lib/bpf/ratan_sched.bpf.o
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ratan_loader \
		$(1)/usr/sbin/ratan_loader
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ratan-sched.init \
		$(1)/etc/init.d/ratan-sched
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/ratan-sched.defaults \
		$(1)/etc/uci-defaults/99-ratan-sched
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/ratan.config $(1)/etc/config/ratan
endef

define Package/ratan-sched/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/ratan-sched enable
exit 0
endef

define Package/ratan-sched/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/ratan-sched disable
	/etc/init.d/ratan-sched stop
}
exit 0
endef

$(eval $(call BuildPackage,ratan-sched))

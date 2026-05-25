# SPDX-License-Identifier: GPL-2.0
#
# OpenWrt package recipe for ratan-prober: the per-WAN UDP probe daemon
# that owns the sub-200ms fast-failover path. Compiled against libbpf
# (writes weight=0 directly into the pinned ratan_path_weights map on
# loss-streak trigger).

include $(TOPDIR)/rules.mk

PKG_NAME:=ratan-prober
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=RATAN team

include $(INCLUDE_DIR)/package.mk

RATAN_PKG_DIR := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
RATAN_PROBER_SRC := $(realpath $(RATAN_PKG_DIR)/../../../src/prober)
RATAN_VPS_SRC    := $(realpath $(RATAN_PKG_DIR)/../../../src/vps)

define Package/ratan-prober
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=RATAN
  TITLE:=RATAN per-WAN UDP prober + fast-failover
  URL:=https://github.com/YX12399/openmptcprouter
  DEPENDS:=+libbpf +libpthread +ratan-sched
endef

define Package/ratan-prober/description
  Per-WAN UDP probe daemon. Sends 50ms-cadence probes from each WAN
  interface to the VPS-side responder, measures RTT/loss/jitter,
  emits samples to the predictor over /run/ratan/samples.sock.

  On N consecutive losses (default 3, ~150ms), writes weight=0 into
  the pinned ratan_path_weights map for the affected subflow -- this
  is the path that owns the sub-200ms KPI. An all-zero guard prevents
  zeroing the last live subflow.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	[ -d "$(RATAN_PROBER_SRC)" ] || { echo "ERROR: prober src missing"; exit 1; }
	$(CP) $(RATAN_PROBER_SRC)/ratan_prober.c $(PKG_BUILD_DIR)/
	$(CP) $(RATAN_PROBER_SRC)/ratan_proto.h  $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) $(TARGET_LDFLAGS) \
		-Wall -Wextra -Wno-unused-parameter -pthread \
		-I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/ratan_prober \
		$(PKG_BUILD_DIR)/ratan_prober.c \
		-lbpf
endef

define Package/ratan-prober/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ratan_prober $(1)/usr/sbin/ratan_prober
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ratan-prober.init $(1)/etc/init.d/ratan-prober
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/ratan-prober.config $(1)/etc/config/ratan-prober
endef

define Package/ratan-prober/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/ratan-prober enable
exit 0
endef

define Package/ratan-prober/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/ratan-prober disable
	/etc/init.d/ratan-prober stop
}
exit 0
endef

$(eval $(call BuildPackage,ratan-prober))

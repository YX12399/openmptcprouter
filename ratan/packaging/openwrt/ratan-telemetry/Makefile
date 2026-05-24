# SPDX-License-Identifier: GPL-2.0
#
# OpenWrt package recipe for ratan-telemetry:
#   - ratan_telemetryd: the SQLite-backed telemetry daemon
#   - schema.sql: applied at first start
#   - procd init + UCI config
#
# Provides HTTP API on 127.0.0.1:9180 consumed by:
#   - LuCI app (Step 11)
#   - ratan-relay-client (Step 12 -> Vercel dashboard)
#   - the ratan-test harness (Step 4b)

include $(TOPDIR)/rules.mk

PKG_NAME:=ratan-telemetry
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=RATAN team

include $(INCLUDE_DIR)/package.mk

RATAN_PKG_DIR := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
RATAN_TEL_SRC := $(realpath $(RATAN_PKG_DIR)/../../../src/telemetry)
RATAN_PROTO_H := $(realpath $(RATAN_PKG_DIR)/../../../src/prober/ratan_proto.h)

define Package/ratan-telemetry
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=RATAN
  TITLE:=RATAN telemetry daemon (SQLite + HTTP API + SSE)
  URL:=https://github.com/YX12399/openmptcprouter
  DEPENDS:=+libsqlite3
endef

define Package/ratan-telemetry/description
  Sole reader of /run/ratan/telemetry.sock. Persists samples, state
  transitions, weight changes, discovery + per-flow + MOS events to
  /var/lib/ratan/telemetry.db (SQLite WAL).

  Exposes HTTP on 127.0.0.1:9180:
    GET  /healthz
    GET  /metrics                       Prometheus exposition
    GET  /stream                        Server-Sent Events live feed
    GET  /sessions                      list past test sessions
    POST /sessions   {name, metadata?}  start recording session
    POST /sessions/<id>/stop            stop, compute summary
    GET  /sessions/<id>                 details + summary
    GET  /sessions/<id>/download        full JSON export (attachment)
    DEL  /sessions/<id>                 remove session + linked rows

  Consumed by luci-app-ratan, the Vercel dashboard (via the relay client),
  and the ratan-test harness.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	[ -d "$(RATAN_TEL_SRC)" ] || { echo "ERROR: telemetry src missing"; exit 1; }
	$(CP) $(RATAN_TEL_SRC)/ratan_telemetryd.c $(PKG_BUILD_DIR)/
	$(CP) $(RATAN_TEL_SRC)/schema.sql         $(PKG_BUILD_DIR)/
	$(CP) $(RATAN_PROTO_H)                    $(PKG_BUILD_DIR)/ratan_proto.h
endef

define Build/Configure
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) $(TARGET_LDFLAGS) \
		-Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-array-bounds \
		-I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/ratan_telemetryd \
		$(PKG_BUILD_DIR)/ratan_telemetryd.c \
		-lsqlite3
endef

define Package/ratan-telemetry/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ratan_telemetryd $(1)/usr/sbin/ratan_telemetryd
	$(INSTALL_DIR) $(1)/usr/share/ratan
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/schema.sql $(1)/usr/share/ratan/schema.sql
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ratan-telemetry.init $(1)/etc/init.d/ratan-telemetry
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/ratan-telemetry.config $(1)/etc/config/ratan-telemetry
endef

define Package/ratan-telemetry/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/ratan-telemetry enable
exit 0
endef

define Package/ratan-telemetry/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/ratan-telemetry disable
	/etc/init.d/ratan-telemetry stop
}
exit 0
endef

$(eval $(call BuildPackage,ratan-telemetry))

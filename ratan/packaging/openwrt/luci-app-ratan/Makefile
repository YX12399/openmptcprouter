# SPDX-License-Identifier: GPL-2.0
#
# luci-app-ratan -- web dashboard for the RATAN aggregation engine.
#
# Pure static HTML/JS + a tiny shell CGI proxy. No Lua, no build step.
# Pages live at http://<router>/ratan/ ; a LuCI menu entry under
# Network -> RATAN deep-links into them.
#
# The handover-status.json file written by ratan-classifier lives at
# /run/ratan/handover-status.json -- we expose it at /ratan-data/
# via a uci-defaults-installed symlink so the static pages can
# fetch it without a CGI hop (it's a single-file read).

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-ratan
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=RATAN team

include $(INCLUDE_DIR)/package.mk

RATAN_PKG_DIR := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))

define Package/luci-app-ratan
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=RATAN dashboard
  URL:=https://github.com/YX12399/openmptcprouter
  DEPENDS:=+luci-base +curl +ratan-telemetry
  PKGARCH:=all
endef

define Package/luci-app-ratan/description
  Web UI for the RATAN MPTCP aggregation engine. Pages:
    Overview   per-WAN cards, daemon KPIs
    Live       polling stats + scrolling event log
    Sessions   list / start / stop / download / delete test recordings
    Handover   per-WAN Starlink handover predictor status

  Accessible at:
    http://<router>/ratan/                 (direct)
    http://<router>/luci -> Network -> RATAN  (LuCI menu)

  Talks to ratan_telemetryd on 127.0.0.1:9180 via a small shell CGI
  proxy at /cgi-bin/ratan-api. The handover status file is exposed
  read-only via /ratan-data/handover-status.json.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) -r $(RATAN_PKG_DIR)/root $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/luci-app-ratan/install
	$(INSTALL_DIR) $(1)/www/ratan
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/root/www/ratan/*.html  $(1)/www/ratan/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/root/www/ratan/*.css   $(1)/www/ratan/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/root/www/ratan/*.js    $(1)/www/ratan/

	$(INSTALL_DIR) $(1)/www/cgi-bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/root/www/cgi-bin/ratan-api \
		$(1)/www/cgi-bin/ratan-api

	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/root/usr/share/luci/menu.d/luci-app-ratan.json \
		$(1)/usr/share/luci/menu.d/luci-app-ratan.json

	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/root/usr/share/rpcd/acl.d/luci-app-ratan.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-ratan.json

	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/50_luci-app-ratan $(1)/etc/uci-defaults/50_luci-app-ratan
endef

define Package/luci-app-ratan/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	# expose /run/ratan as /www/ratan-data so the static pages can
	# fetch the handover-status.json without a CGI hop. uhttpd serves
	# /www; we symlink /www/ratan-data -> /run/ratan.
	ln -snf /run/ratan /www/ratan-data 2>/dev/null || true
}
exit 0
endef

define Package/luci-app-ratan/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	[ -L /www/ratan-data ] && rm /www/ratan-data
}
exit 0
endef

$(eval $(call BuildPackage,luci-app-ratan))

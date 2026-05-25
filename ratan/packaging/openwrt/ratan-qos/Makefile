# SPDX-License-Identifier: GPL-2.0
#
# OpenWrt package: ratan-qos -- tc HTB + nft ruleset for the "Teams
# survives WAN drop" moat. See ratan/src/qos/README.md for design.

include $(TOPDIR)/rules.mk

PKG_NAME:=ratan-qos
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=RATAN team

include $(INCLUDE_DIR)/package.mk

RATAN_PKG_DIR := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
RATAN_QOS_SRC := $(realpath $(RATAN_PKG_DIR)/../../../src/qos)

define Package/ratan-qos
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=RATAN
  TITLE:=RATAN QoS: tc HTB + nft classification (realtime/bulk/background)
  URL:=https://github.com/YX12399/openmptcprouter
  DEPENDS:=+kmod-sched +kmod-sched-core +tc +nftables +kmod-nft-core +kmod-nf-conntrack-netlink
  PKGARCH:=all
endef

define Package/ratan-qos/description
  Bandwidth shaping framework for RATAN. Builds an HTB tree on each
  configured WAN egress with three classes (realtime, bulk, background)
  and loads an nftables ruleset that maps ct->mark to tc priority.

  Effect: a real-time class (VideoCall/*, VoIP/*) is guaranteed a
  bandwidth floor (default 1.5 Mbps per WAN) regardless of what bulk
  traffic is doing on the same link. When a WAN drops, the other WAN's
  HTB tree still honors that floor -- this is the "Teams survives a
  WAN drop while iperf3 absorbs the hit" property.

  Assumes ct->mark is populated by OMR's nDPI-netfilter integration
  (the common convention). If first-flash testing shows OMR uses a
  CT extension that nft can't match natively, ratan-qos-marker
  (Step 8b, separate package) bridges the gap.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(RATAN_QOS_SRC)/ratan-qos-classes.json   $(PKG_BUILD_DIR)/
	$(CP) $(RATAN_QOS_SRC)/ratan-qos.nft.in         $(PKG_BUILD_DIR)/
	$(CP) $(RATAN_QOS_SRC)/ratan-qos-apply.sh       $(PKG_BUILD_DIR)/
	$(CP) $(RATAN_QOS_SRC)/ratan-qos-stop.sh        $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef
define Build/Compile
endef

define Package/ratan-qos/install
	# config + classes JSON + the nft template
	$(INSTALL_DIR) $(1)/etc/ratan/qos
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/ratan-qos-classes.json \
		$(1)/etc/ratan/qos/classes.json
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/ratan-qos.nft.in \
		$(1)/etc/ratan/qos/ratan-qos.nft

	# apply / stop scripts
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ratan-qos-apply.sh  $(1)/usr/sbin/ratan-qos-apply
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ratan-qos-stop.sh   $(1)/usr/sbin/ratan-qos-stop

	# procd init + UCI defaults
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ratan-qos.init   $(1)/etc/init.d/ratan-qos
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/ratan-qos.config $(1)/etc/config/ratan-qos
endef

define Package/ratan-qos/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/ratan-qos enable
exit 0
endef

define Package/ratan-qos/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/ratan-qos disable
	/etc/init.d/ratan-qos stop
}
exit 0
endef

$(eval $(call BuildPackage,ratan-qos))

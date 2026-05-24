# SPDX-License-Identifier: GPL-2.0
#
# OpenWrt package for the ratan-test session recorder + recipes.
# Python 3 + pyyaml; no C compile needed.

include $(TOPDIR)/rules.mk

PKG_NAME:=ratan-test
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=RATAN team

include $(INCLUDE_DIR)/package.mk

RATAN_PKG_DIR := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
RATAN_TEST_SRC := $(realpath $(RATAN_PKG_DIR)/../../../src/test)

define Package/ratan-test
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=RATAN
  TITLE:=RATAN test session recorder + recipes
  URL:=https://github.com/YX12399/openmptcprouter
  DEPENDS:=+python3-light +python3-urllib +python3-logging +python3-yaml \
           +ratan-telemetry
  PKGARCH:=all
endef

define Package/ratan-test/description
  Session recorder for RATAN telemetry. Drives the ratan-telemetry HTTP
  API on 127.0.0.1:9180 to start/stop sessions, schedules tc-netem
  injects on a thread timer, writes RATAN_EVENT_INJECT envelopes to
  /run/ratan/telemetry.sock so injects appear as synchronized markers
  on every live graph.

  Includes recipes for:
    - kpi_failover           sub-200ms failover KPI
    - video_call_continuity  Teams/Zoom survives Starlink handovers
    - starlink_baseline      passive 10-min handover-cadence learning
    - qos_isolation          real-time class survives WAN drop

  Usage:
    ratan-test list-recipes
    ratan-test record --recipe kpi_failover --output run.json
    ratan-test record --name custom --duration 60 \
        --inject "at:10s netem_drop:wan0" \
        --inject "at:20s netem_restore:wan0"
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(RATAN_TEST_SRC)/ratan-test $(PKG_BUILD_DIR)/
	$(CP) -r $(RATAN_TEST_SRC)/recipes $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/ratan-test/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ratan-test $(1)/usr/sbin/ratan-test
	$(INSTALL_DIR) $(1)/usr/share/ratan/recipes
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/recipes/*.yaml $(1)/usr/share/ratan/recipes/
endef

$(eval $(call BuildPackage,ratan-test))

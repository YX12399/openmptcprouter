# SPDX-License-Identifier: GPL-2.0
#
# ratan-full metapackage. Pulls in the full OpenMPTCProuter feature set
# plus every RATAN component that's currently shipping. OMR's build.sh,
# when invoked with OMR_DIST=ratan and OMR_PACKAGES=full, auto-emits
# CONFIG_PACKAGE_ratan-full=y -- so this is the metapackage that gates
# the whole build.

include $(TOPDIR)/rules.mk

PKG_NAME:=ratan-full
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=RATAN team

include $(INCLUDE_DIR)/package.mk

define Package/ratan-full
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=RATAN
  TITLE:=RATAN aggregation engine (metapackage)
  URL:=https://github.com/YX12399/openmptcprouter
  DEPENDS:= \
    +openmptcprouter-full \
    +ratan-sched \
    +ratan-prober \
    +ratan-telemetry \
    +ratan-test \
    +ratan-classifier
  # Future components will be added here as they ship:
  #   +ratan-classifier  (Step 5)
  #   +ratan-predictor   (Step 6)
  #   +ratan-qos         (Step 8)
  #   +ratan-handover    (Step 9)
  #   +ratan-cli         (Step 10)
  #   +luci-app-ratan    (Step 11)
  #   +ratan-relay-client (Step 12)
endef

define Package/ratan-full/description
  Metapackage that builds an OpenMPTCProuter image with the full RATAN
  aggregation engine (Starlink-aware MPTCP scheduling, smart prediction,
  sub-200ms failover, QoS, telemetry, LuCI UI).

  Currently includes: ratan-sched, ratan-prober, ratan-telemetry,
  ratan-test, ratan-classifier.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/ratan-full/install
	$(INSTALL_DIR) $(1)/etc
endef

$(eval $(call BuildPackage,ratan-full))

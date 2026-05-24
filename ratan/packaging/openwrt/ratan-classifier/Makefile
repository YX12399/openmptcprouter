# SPDX-License-Identifier: GPL-2.0
#
# OpenWrt package for ratan-classifier: per-WAN Starlink-aware state
# machine that consumes the telemetry daemon's SSE stream and emits
# state transitions + weight changes (BPF map writes via bpftool).
#
# Python 3; no C compile.

include $(TOPDIR)/rules.mk

PKG_NAME:=ratan-classifier
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=RATAN team

include $(INCLUDE_DIR)/package.mk

RATAN_PKG_DIR := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
RATAN_CLS_SRC := $(realpath $(RATAN_PKG_DIR)/../../../src/classifier)

define Package/ratan-classifier
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=RATAN
  TITLE:=RATAN Starlink-aware path classifier
  URL:=https://github.com/YX12399/openmptcprouter
  DEPENDS:=+python3-light +python3-urllib +python3-logging \
           +bpftool +ratan-telemetry +ratan-sched
  PKGARCH:=all
endef

define Package/ratan-classifier/description
  Per-WAN finite state machine that turns prober samples into smart
  scheduler weights.

  States:
    HEALTHY    -- baseline weight; capacity-proportional
    TRANSIENT  -- expected blip (handover signature); weight UNCHANGED
                  -- this is what stops Starlink's 15s-handover thrash
                  from defeating aggregation
    DEGRADING  -- sustained worsening; weight smoothly decayed over ~1.5s
    DOWN       -- 1s+ consecutive loss; weight 0 (the prober owns the
                  sub-200ms fast-failover path independently)

  Inputs: SSE from /run/ratan/telemetry.sock daemon at port 9180.
  Outputs:
    - RATAN_EVENT_STATE + RATAN_EVENT_WEIGHT envelopes back to the
      telemetry socket -> persisted + broadcast to all UIs
    - Direct writes to the pinned BPF weights map via bpftool

  Config: /etc/ratan/classifier.json (one wan entry per uplink, with
  subflow_idx matching the prober's UCI, capacity_mbps for baseline
  weight calc, and optional FSM threshold overrides).

  Reseachable invariants (verified by the unit tests shipped in
  /usr/lib/ratan-classifier/classifier/tests/):
    - 10 back-to-back 250ms blips never decay weight
    - 1.5s blackhole -> DOWN at weight 0
    - DEGRADING decay is monotonic baseline -> floor over decay_ms
    - Steady-state clean stream emits zero transitions
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/classifier
	[ -d "$(RATAN_CLS_SRC)" ] || { echo "ERROR: classifier src missing"; exit 1; }
	$(CP) $(RATAN_CLS_SRC)/ratan-classifier   $(PKG_BUILD_DIR)/
	$(CP) $(RATAN_CLS_SRC)/__init__.py        $(PKG_BUILD_DIR)/classifier/
	$(CP) $(RATAN_CLS_SRC)/fsm.py             $(PKG_BUILD_DIR)/classifier/
	$(CP) $(RATAN_CLS_SRC)/ewma.py            $(PKG_BUILD_DIR)/classifier/
	$(CP) $(RATAN_CLS_SRC)/handover.py        $(PKG_BUILD_DIR)/classifier/
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/ratan-classifier/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ratan-classifier $(1)/usr/sbin/ratan-classifier
	$(INSTALL_DIR) $(1)/usr/lib/ratan-classifier/classifier
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/classifier/__init__.py \
		$(1)/usr/lib/ratan-classifier/classifier/__init__.py
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/classifier/fsm.py \
		$(1)/usr/lib/ratan-classifier/classifier/fsm.py
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/classifier/ewma.py \
		$(1)/usr/lib/ratan-classifier/classifier/ewma.py
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/classifier/handover.py \
		$(1)/usr/lib/ratan-classifier/classifier/handover.py
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ratan-classifier.init $(1)/etc/init.d/ratan-classifier
	$(INSTALL_DIR) $(1)/etc/ratan
	$(INSTALL_CONF) ./files/classifier.json $(1)/etc/ratan/classifier.json
endef

define Package/ratan-classifier/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/ratan-classifier enable
exit 0
endef

define Package/ratan-classifier/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/ratan-classifier disable
	/etc/init.d/ratan-classifier stop
}
exit 0
endef

$(eval $(call BuildPackage,ratan-classifier))

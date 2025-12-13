#
# Copyright (C) 2015 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=pcie_mhi
PKG_VERSION:=1.3.8
PKG_RELEASE:=4

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/pcie_mhi
  SUBMENU:=WWAN Support
  TITLE:=Kernel pcie driver for MHI device
  DEPENDS:=@(TARGET_qualcommax_ipq807x||TARGET_qualcommax_ipq50xx) \
	    @PCI_SUPPORT \
	    +PACKAGE_kmod-rmnet-nss:kmod-rmnet-nss
  FILES:=$(PKG_BUILD_DIR)/pcie_mhi.ko
# AUTOLOAD:=$(call AutoLoad,90,pcie_mhi)
endef

define KernelPackage/pcie_mhi/description
  Kernel module for register a custom pciemhi platform device.
endef

EXTRA_CFLAGS+= \
	-Wno-unused-function \
	-Wno-missing-prototypes \
	-Wno-missing-declarations

ifneq ($(CONFIG_PACKAGE_kmod-rmnet-nss),)
EXTRA_CFLAGS += -I$(STAGING_DIR)/usr/include/qca-nss-rmnet
EXTRA_CFLAGS += -DCONFIG_QCA_NSS_DRV
endif

MAKE_OPTS:= \
	ARCH="$(LINUX_KARCH)" \
	CROSS_COMPILE="$(TARGET_CROSS)" \
	CXXFLAGS="$(TARGET_CXXFLAGS)" \
	M="$(PKG_BUILD_DIR)" \
	EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
	$(EXTRA_KCONFIG)

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) -C "$(LINUX_DIR)" \
		$(MAKE_OPTS) \
	 M="$(PKG_BUILD_DIR)" \
		modules
endef

define KernelPackage/pcie_mhi/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/pcie_mhi $(1)/etc/init.d/pcie_mhi
endef

$(eval $(call KernelPackage,pcie_mhi))

include $(TOPDIR)/rules.mk

PKG_NAME:=uartmuxd
PKG_VERSION:=0.1.0
PKG_RELEASE:=6

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/uartmuxd
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Userspace UART mux daemon
  DEPENDS:=+libpthread
endef

define Package/uartmuxd/description
 Userspace replacement for vendor uartmux.ko.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/uartmuxd $(PKG_BUILD_DIR)/uartmuxd.c
endef

define Package/uartmuxd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/uartmuxd $(1)/usr/sbin/uartmuxd
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/uartmuxd.init $(1)/etc/init.d/uartmuxd
endef

$(eval $(call BuildPackage,uartmuxd))

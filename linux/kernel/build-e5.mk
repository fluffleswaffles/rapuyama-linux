# Nokia E5-00 kernel + dtb + initramfs (busybox + Alpine userspace).
# this Makefile is based on kapina's script (linux/kernel/build-e5.sh)
#
#   make help            what can be built and which variables exist
#   make                 kernel Image + e5.dtb in $(KOUT)
#   make rootfs          only the Alpine userspace tree
#   make initramfs-list  only $(KOUT)/initramfs.list
#   make vars            what this Makefile decided (compiler, paths, ...)
#
# Nothing is hard-wired: every setting below is `?=`, so it can come from the
# command line (make CROSS_COMPILE=arm-none-eabi-), the environment, or an
# untracked local.mk next to this file. local.mk is read first, so plain
# `VAR = value` in it wins. Lists have a *_EXTRA twin so you add instead of
# copying the default.

SHELL       := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.DELETE_ON_ERROR:
.DEFAULT_GOAL := all
# no implicit rules: make must never "rebuild" a source file or a ready made
# binary (the committed init, your busybox) from something next to it
MAKEFLAGS += -r
.SUFFIXES:

HERE := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TOP  ?= $(abspath $(HERE)/../..)

-include $(HERE)/local.mk

# ---------------------------------------------------------------- locations
KSRC   ?= $(HERE)/linux
KOUT   ?= $(HERE)/build-e5
BOOT   ?= $(TOP)/linux/boot
INITD  ?= $(TOP)/linux/initramfs
TOOLS  ?= $(TOP)/tools
WORK   ?= $(TOP)/work
DL     ?= $(WORK)/dl
DTS    ?= $(BOOT)/e5.dts
KCONFIG_BASE ?= $(HERE)/e6.config
OUT    := $(abspath $(KOUT))

# ---------------------------------------------------------------- toolchain
# Any ARM Linux/EABI gcc builds this kernel (the kernel needs no libc). Taken
# from the command line if given, else the first prefix found in PATH.
ifeq ($(origin CROSS_COMPILE),undefined)
CROSS_COMPILE := $(firstword $(foreach p,arm-linux-gnu- arm-linux-gnueabi- arm-none-eabi- arm-linux-gnueabihf-,$(if $(shell command -v $(p)gcc 2>/dev/null),$(p))))
endif
DTC     ?= dtc
PYTHON  ?= python3
CURL    ?= curl -sS -m 120 -fL
JOBS    ?= $(shell nproc 2>/dev/null || echo 1)
KMAKE_FLAGS ?=

KMAKE = $(MAKE) -C $(KSRC) O=$(OUT) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) \
        $(if $(findstring -j,$(MAKEFLAGS)),,-j$(JOBS)) $(KMAKE_FLAGS)

# ------------------------------------------------------------------- kernel
# Applied on top of KCONFIG_BASE with scripts/config (names are used exactly
# as written, so mixed-case ones like FONT_8x8 work). Add yours to *_EXTRA, or
# raw scripts/config arguments (--set-val HZ 250, -m FOO ...) to KCFG_RAW.
KCFG_DISABLE ?= DRM SOUND RAPUYAMA_MODEM FB_RAPUYAMA FB_SIMPLE \
                USB_G_SERIAL USB_ETH BRCMUTIL BRCMFMAC ZD1211RW
KCFG_ENABLE  ?= FB FB_DEVICE FB_RAPUYAMA_E5 FRAMEBUFFER_CONSOLE FONTS \
                FONT_8x8 FONT_8x16 FONT_MINI_4x6 FONT_6x8 FONT_6x10 FONT_6x11 \
                FONT_7x14 FONT_10x18 \
                INPUT_KEYBOARD KEYBOARD_RAPUYAMA_E5 \
                USB_CDC_COMPOSITE \
                WLAN WLAN_VENDOR_TI WL1251 WL1251_SPI SPI_RAPUYAMA_E5 \
                BACKLIGHT_CLASS_DEVICE BACKLIGHT_RAPUYAMA_E5 \
                DMADEVICES RAPUYAMA_GDMA
KCFG_DISABLE_EXTRA ?=
KCFG_ENABLE_EXTRA  ?=
KCFG_RAW ?=
# dir with ti-connectivity/wl1251-{fw,nvs}.bin, built into the Image when both
# exist (not in the repo). Without: the kernel boots, wlan is bound later by
# init.sh with the files the loader copied out of the phone's rom
E5_WLAN_FW ?= $(HOME)/e5/wlan/fw
WLAN_FW_FILES = ti-connectivity/wl1251-fw.bin ti-connectivity/wl1251-nvs.bin
ifneq ($(wildcard $(E5_WLAN_FW)/ti-connectivity/wl1251-nvs.bin),)
KCFG_FW = --set-str EXTRA_FIRMWARE "$(WLAN_FW_FILES)" --set-str EXTRA_FIRMWARE_DIR "$(E5_WLAN_FW)"
endif

KCFG_ARGS = --keep-case \
	$(addprefix -d ,$(KCFG_DISABLE) $(KCFG_DISABLE_EXTRA)) \
	$(addprefix -e ,$(KCFG_ENABLE) $(KCFG_ENABLE_EXTRA)) \
	$(KCFG_FW) --set-str INITRAMFS_SOURCE "$(INITRAMFS_LIST)" $(KCFG_RAW)

# ---------------------------------------------------------------- userspace
BUSYBOX     ?= $(DL)/busybox-armv5l
# official static build, runs on the arm1176 (check the URL if it moved)
BUSYBOX_URL ?= https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-armv5l

ALPINE_MIRROR ?= https://dl-cdn.alpinelinux.org/alpine
ALPINE_REL    ?= v3.24
ALPINE_ARCH   ?= armhf
# arch of the apk-tools-static that runs on THIS pc (x86_64, aarch64, ...)
HOST_ARCH     ?= $(patsubst armv7l,armv7,$(patsubst i%86,x86,$(shell uname -m)))
# set APK=/path/to/apk to use your own instead of downloading apk.static
APK_STATIC    := $(DL)/alpine/apkt/sbin/apk.static
APK           ?= $(APK_STATIC)
# --usermode lets apk run unprivileged by unsharing a user namespace; a
# container (podman, docker) often forbids that, and as root it is not needed.
#   APK_USERMODE=auto  (default) --usermode only when not running as root
#   APK_USERMODE=yes   always      APK_USERMODE=no   never
# APK_FLAGS replaces the base flags, APK_FLAGS_EXTRA adds to them
APK_USERMODE  ?= auto
ifeq ($(APK_USERMODE),auto)
override APK_USERMODE := $(if $(filter 0,$(shell id -u)),no,yes)
endif
APK_FLAGS     ?= $(if $(filter yes,$(APK_USERMODE)),--usermode) --no-scripts --allow-untrusted --timeout 60
APK_FLAGS_EXTRA ?=
APK_ALL_FLAGS = $(APK_FLAGS) $(APK_FLAGS_EXTRA)

ROOTFS        ?= $(WORK)/rootfs-e5-simple
# alpine's busybox is in the apk db but its files stay out of the initramfs
# (see ROOTFS_DIRS): `apk add` must never replace our static /bin/busybox
ALPINE_PKGS   ?= musl wpa_supplicant iw apk-tools alpine-keys \
                 ca-certificates-bundle busybox busybox-binsh
# prune the tree to what these programs load (tools/prune_rootfs.py), or `no`
PRUNE         ?= yes
PRUNE_PY      ?= $(TOOLS)/prune_rootfs.py
PRUNE_ROOTS   ?= sbin/wpa_supplicant sbin/wpa_passphrase usr/sbin/iw sbin/apk \
                 lib/ld-musl-$(ALPINE_ARCH).so.1
# what of the tree goes into the initramfs (relative to ROOTFS) and what is
# skipped inside it (not /bin, /etc/busybox*, usr/share/udhcpc: busybox's)
ROOTFS_DIRS   ?= lib usr sbin etc/apk etc/ssl
ROOTFS_SKIP   ?= usr/share/udhcpc

# e5-keyd (fn key picture, power key screen toggle): a static armv6 musl
# binary built with clang against a small Alpine sysroot. auto = only if clang
# is installed; 0 = leave it out (init.sh starts it only if it exists)
KEYD       ?= auto
ifeq ($(KEYD),auto)
override KEYD := $(if $(shell command -v clang 2>/dev/null),1,0)
endif
KEYD_DIR   ?= $(INITD)/e5-keyd
KEYD_BIN   ?= $(OUT)/e5-keyd/e5-keyd
KEYD_TEST  ?= 1
HOST_CC    ?= clang
KEYD_CC    ?= clang
KEYD_LD    ?= $(shell command -v $(CROSS_COMPILE)ld 2>/dev/null)
SYSROOT    ?= $(WORK)/sysroot-alpine
SYSROOT_PKGS ?= musl-dev linux-headers libgcc gcc

# the initramfs: dirs, then files as  dest|source|mode  (no spaces in paths).
# Add yours with the *_EXTRA twins, or append a ready made list fragment
# (gen_init_cpio syntax) with INITRAMFS_APPEND=/path/to/list
KMAP ?= $(INITD)/e5.kmap
XKB  ?= $(INITD)/e5.xkb
INIT_SH ?= $(INITD)/init.sh
INIT_BIN ?= $(INITD)/init
INITRAMFS_DIRS ?= /dev|755 /bin|755 /sbin|755 /proc|755 /sys|755 /tmp|1777 /etc|755
INITRAMFS_DIRS_EXTRA ?=
INITRAMFS_FILES ?= \
	/bin/busybox|$(BUSYBOX)|755 \
	/init|$(INIT_SH)|755 \
	/sbin/init|$(INIT_BIN)|755 \
	/bin/e6sh|$(INITD)/e6sh.sh|755 \
	/etc/e5.kmap|$(KMAP)|644 \
	/etc/e5.xkb|$(XKB)|644 \
	/bin/keyscan|$(INITD)/keyscan.sh|755 \
	/bin/e5-dhcp|$(INITD)/e5-dhcp.sh|755 \
	/bin/brightness|$(INITD)/e5-brightness.sh|755 \
	/bin/blank|$(INITD)/e5-blank.sh|755 \
	/etc/e5-welcome|$(INITD)/e5-welcome|644
# after the rootfs part (these live in directories that come with it)
INITRAMFS_FILES_ROOTFS ?= \
	/usr/bin/wifi|$(INITD)/e5-wifi.sh|755 \
	/usr/share/udhcpc/default.script|$(INITD)/e5-dhcp.sh|755
INITRAMFS_FILES_EXTRA ?=
INITRAMFS_APPEND ?=

# ------------------------------------------------------------------ derived
INITRAMFS_LIST := $(OUT)/initramfs.list
ifeq ($(PRUNE),yes)
ROOTFS_READY := $(ROOTFS).pruned
else
ROOTFS_READY := $(ROOTFS).done
endif
ifeq ($(KEYD),1)
KEYD_FILES := /bin/e5-keyd|$(KEYD_BIN)|755
endif

# initramfs list lines as quoted shell words (for printf '%s\n'), from dest|src|mode
split     = $(subst |, ,$(1))
dir_word  = 'dir $(word 1,$(call split,$(1))) $(word 2,$(call split,$(1))) 0 0'
file_word = 'file $(word 1,$(call split,$(1))) $(word 2,$(call split,$(1))) $(word 3,$(call split,$(1))) 0 0'
srcs_of   = $(foreach f,$(1),$(word 2,$(call split,$(f))))

ALL_FILES    := $(INITRAMFS_FILES) $(KEYD_FILES) $(INITRAMFS_FILES_EXTRA)
LIST_INPUTS  := $(call srcs_of,$(ALL_FILES) $(INITRAMFS_FILES_ROOTFS)) $(INITRAMFS_APPEND)

# --------------------------------------------------------------- small tools
# `$(call retry,cmd)`: the cdn drops connections now and then
retry = ok=; for i in 1 2 3 4 5 6; do if $(1); then ok=1; break; fi; sleep 3; done; [ -n "$$ok" ]
fetch = $(CURL) -o "$(2)" "$(1)"
# rewrite $@ only when its content changed, so it can be a prerequisite
q = $(subst ','\'',$(1))
vstamp = mkdir -p $(@D); printf '%s\n' '$(call q,$(1))' | cmp -s - $@ || printf '%s\n' '$(call q,$(1))' > $@
need_cross = @[ -n "$(CROSS_COMPILE)" ] && command -v $(CROSS_COMPILE)gcc >/dev/null || { \
	echo "no ARM cross compiler: install one or run  make CROSS_COMPILE=<prefix>-" >&2; exit 1; }

.PHONY: all kernel dtb rootfs initramfs-list busybox config menuconfig \
        clean clean-rootfs distclean help vars FORCE
FORCE:

all: kernel dtb
	@ls -la $(OUT)/arch/arm/boot/Image $(OUT)/e5.dtb

# ------------------------------------------------------------------- kernel
$(OUT)/.kcfg.args: FORCE
	@$(call vstamp,$(KCFG_ARGS))

$(OUT)/.config: $(KCONFIG_BASE) $(OUT)/.kcfg.args
	$(need_cross)
	mkdir -p $(OUT)
	cp $(KCONFIG_BASE) $@
	$(KSRC)/scripts/config --file $@ $(KCFG_ARGS)
	$(KMAKE) olddefconfig

config:
	@rm -f $(OUT)/.config
	@$(MAKE) --no-print-directory $(OUT)/.config

menuconfig: $(OUT)/.config
	$(KMAKE) menuconfig

kernel: $(OUT)/.config $(INITRAMFS_LIST)
	$(need_cross)
	$(KMAKE) Image

dtb: $(OUT)/e5.dtb
$(OUT)/e5.dtb: $(DTS)
	mkdir -p $(@D)
	$(DTC) -I dts -O dtb -o $@ $<

# ----------------------------------------------------------------- initramfs
# the asm /sbin/init fallback, built with the same cross compiler (only when
# INIT_BIN is the default; point it at a ready made binary to skip this)
ifeq ($(INIT_BIN),$(INITD)/init)
$(INIT_BIN): $(INITD)/init.S
	$(need_cross)
	mkdir -p $(@D)
	$(CROSS_COMPILE)gcc -march=armv6k -marm -static -nostdlib -o $@ $<
endif

# fetched only when BUSYBOX does not exist yet
busybox: $(BUSYBOX)
$(BUSYBOX):
	mkdir -p $(@D)
	$(call retry,$(call fetch,$(BUSYBOX_URL),$@))
	chmod +x $@

initramfs-list: $(INITRAMFS_LIST)
$(INITRAMFS_LIST): $(ROOTFS_READY) $(LIST_INPUTS) $(MAKEFILE_LIST) \
                   $(if $(filter 1,$(KEYD)),$(KEYD_BIN)) \
                   $(OUT)/.initramfs.args
	mkdir -p $(@D)
	printf '%s\n' $(foreach d,$(INITRAMFS_DIRS) $(INITRAMFS_DIRS_EXTRA),$(call dir_word,$(d))) \
		'nod /dev/console 600 0 0 c 5 1' 'slink /bin/sh busybox 777 0 0' \
		$(foreach f,$(ALL_FILES),$(call file_word,$(f))) > $@
	cd $(ROOTFS) && find $(ROOTFS_DIRS) $(foreach s,$(ROOTFS_SKIP),-path $(s) -prune -o) \
		-printf '%p\t%y\t%m\t%l\n' | LC_ALL=C sort | awk -F'\t' -v W='$(ROOTFS)' ' \
		$$1 == "sbin" { next } \
		$$2 == "l" { print "slink /" $$1 " " $$4 " 777 0 0"; next } \
		$$2 == "d" { print "dir /" $$1 " " $$3 " 0 0"; next } \
		{ print "file /" $$1 " " W "/" $$1 " " $$3 " 0 0" }' >> $@
	printf '%s\n' $(foreach s,$(ROOTFS_SKIP),'dir /$(s) 755 0 0') \
		$(foreach f,$(INITRAMFS_FILES_ROOTFS),$(call file_word,$(f))) >> $@
	if [ -n '$(INITRAMFS_APPEND)' ]; then cat '$(INITRAMFS_APPEND)' >> $@; fi
	sed -i '/^$$/d' $@
	@echo "$@: $$(wc -l < $@) entries"

$(OUT)/.initramfs.args: FORCE
	@$(call vstamp,$(INITRAMFS_DIRS) $(INITRAMFS_DIRS_EXTRA) $(ALL_FILES) $(INITRAMFS_FILES_ROOTFS) $(ROOTFS_DIRS) $(ROOTFS_SKIP) $(INITRAMFS_APPEND))

# ---------------------------------------------------------- alpine userspace
rootfs: $(ROOTFS_READY)

ifeq ($(APK),$(APK_STATIC))
APK_DEP := $(APK_STATIC)
endif
$(APK_STATIC):
	mkdir -p $(DL)/alpine/apkt
	ad=$(DL)/alpine; \
	$(call retry,$(call fetch,$(ALPINE_MIRROR)/$(ALPINE_REL)/main/$(HOST_ARCH)/APKINDEX.tar.gz,$$ad/idx.tar.gz)); \
	v=$$(tar -xzOf $$ad/idx.tar.gz APKINDEX | awk -v RS= '/\nP:apk-tools-static\n/' | sed -n 's/^V://p'); \
	[ -n "$$v" ] || { echo "apk-tools-static not in the index" >&2; exit 1; }; \
	$(call retry,$(call fetch,$(ALPINE_MIRROR)/$(ALPINE_REL)/main/$(HOST_ARCH)/apk-tools-static-$$v.apk,$$ad/apk-tools-static.apk)); \
	tar -xzf $$ad/apk-tools-static.apk -C $$ad/apkt 2>/dev/null || true; \
	[ -x $@ ] || { echo "no $@ in the apk-tools-static package" >&2; exit 1; }

$(ROOTFS).args: FORCE
	@$(call vstamp,$(ALPINE_MIRROR) $(ALPINE_REL) $(ALPINE_ARCH) $(ALPINE_PKGS) $(APK_ALL_FLAGS))

# stamps live next to the dir, not in it (all of the dir goes into the initramfs)
$(ROOTFS).done: $(APK_DEP) $(ROOTFS).args
	rm -rf $(ROOTFS) $@ $(ROOTFS).pruned
	mkdir -p $(ROOTFS)/etc/apk $(DL)/cache
	printf '%s/%s/main\n%s/%s/community\n' '$(ALPINE_MIRROR)' '$(ALPINE_REL)' \
		'$(ALPINE_MIRROR)' '$(ALPINE_REL)' > $(ROOTFS)/etc/apk/repositories
	$(call retry,$(APK) --root $(ROOTFS) --arch $(ALPINE_ARCH) --initdb $(APK_ALL_FLAGS) \
		--cache-dir $(DL)/cache -U add $(ALPINE_PKGS) > $(DL)/apk.log 2>&1) \
		|| { tail $(DL)/apk.log; exit 1; }
	touch $@

$(ROOTFS).pruned: $(ROOTFS).done $(PRUNE_PY)
	$(PYTHON) $(PRUNE_PY) $(ROOTFS) $(PRUNE_ROOTS)
	touch $@

# ------------------------------------------------------------------- e5-keyd
$(SYSROOT).done: $(APK_DEP)
	rm -rf $(SYSROOT) $@
	mkdir -p $(SYSROOT)/etc/apk $(DL)/cache
	printf '%s/%s/main\n%s/%s/community\n' '$(ALPINE_MIRROR)' '$(ALPINE_REL)' \
		'$(ALPINE_MIRROR)' '$(ALPINE_REL)' > $(SYSROOT)/etc/apk/repositories
	$(call retry,$(APK) --root $(SYSROOT) --arch $(ALPINE_ARCH) --initdb $(APK_ALL_FLAGS) \
		--cache-dir $(DL)/cache -U add $(SYSROOT_PKGS) > $(DL)/apk-sysroot.log 2>&1) \
		|| { tail $(DL)/apk-sysroot.log; exit 1; }
	touch $@

$(KEYD_BIN): $(wildcard $(KEYD_DIR)/*.c $(KEYD_DIR)/*.h) $(SYSROOT).done
	mkdir -p $(@D)
ifeq ($(KEYD_TEST),1)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
		-o $(@D)/test_core $(KEYD_DIR)/core.c $(KEYD_DIR)/test_core.c
	$(@D)/test_core
endif
	$(KEYD_CC) --target=armv6-alpine-linux-musleabihf -march=armv6kz -mfloat-abi=hard \
		-mfpu=vfp --sysroot=$(SYSROOT) --ld-path=$(KEYD_LD) -static-libgcc -static -s \
		-std=gnu11 -Os -Wall -Wextra -Werror -o $@ $(KEYD_DIR)/main.c $(KEYD_DIR)/core.c

# ------------------------------------------------------------------- housekeeping
clean:
	rm -rf $(OUT)
clean-rootfs:
	rm -rf $(ROOTFS) $(ROOTFS).done $(ROOTFS).pruned $(ROOTFS).args $(SYSROOT) $(SYSROOT).done
distclean: clean clean-rootfs
	rm -rf $(DL)/alpine $(DL)/cache $(DL)/apk*.log

vars:
	@echo "CROSS_COMPILE = $(CROSS_COMPILE)   (gcc: $$(command -v $(CROSS_COMPILE)gcc || echo NOT FOUND))"
	@echo "KSRC          = $(KSRC)"
	@echo "KOUT          = $(OUT)"
	@echo "DTS           = $(DTS)"
	@echo "DTC           = $(DTC)   ($$(command -v $(DTC) || echo NOT FOUND))"
	@echo "BUSYBOX       = $(BUSYBOX)   ($$([ -f $(BUSYBOX) ] && echo present || echo missing))"
	@echo "ROOTFS        = $(ROOTFS)   prune=$(PRUNE)"
	@echo "ALPINE        = $(ALPINE_MIRROR) $(ALPINE_REL) $(ALPINE_ARCH), apk-tools for $(HOST_ARCH)"
	@echo "APK           = $(APK)"
	@echo "APK_FLAGS     = $(APK_ALL_FLAGS)   (usermode=$(APK_USERMODE))"
	@echo "KEYD          = $(KEYD)   (HOST_CC=$(HOST_CC) KEYD_CC=$(KEYD_CC))"

help:
	@echo 'targets: all kernel dtb rootfs initramfs-list busybox config menuconfig'
	@echo '         clean clean-rootfs distclean vars help'
	@echo 'toolchain : CROSS_COMPILE DTC PYTHON HOST_CC KEYD_CC APK'
	@echo 'paths     : KSRC KOUT DTS KCONFIG_BASE WORK DL ROOTFS BUSYBOX'
	@echo 'kernel    : KCFG_ENABLE_EXTRA KCFG_DISABLE_EXTRA KCFG_RAW E5_WLAN_FW'
	@echo 'userspace : ALPINE_PKGS ALPINE_REL ALPINE_MIRROR PRUNE PRUNE_ROOTS KEYD'
	@echo 'apk       : APK APK_USERMODE=auto|yes|no APK_FLAGS APK_FLAGS_EXTRA'
	@echo 'initramfs : INITRAMFS_FILES_EXTRA INITRAMFS_DIRS_EXTRA INITRAMFS_APPEND INIT_SH KMAP XKB'
	@echo 'examples  : make CROSS_COMPILE=arm-linux-gnueabi- -j8'
	@echo '            make ALPINE_PKGS="musl busybox nano" PRUNE=no rootfs'
	@echo '            put permanent settings in $(HERE)/local.mk'

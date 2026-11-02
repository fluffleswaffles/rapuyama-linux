#!/usr/bin/env bash
# E5-00 kernel + dtb: e6.config minus drm, sound, the E6 fbdevs, the E6 modem
# keepalive (blind E6 pmic writes) and the E6 wlan chips; plus the E5 rfbi
# fbdev + fbcon, keypad, g_cdc usb, the wl1251 on spi2 and the gazoo light
# pwm. writes the E5 initramfs list into the output dir. own output dir, so
# the E6 trees stay untouched. usage: ./build-e5.sh [config|build]
# E5_WLAN_FW = dir with ti-connectivity/wl1251-{fw,nvs}.bin (see the E5
# README); without them the kernel builds and boots, just without wlan
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
KSRC="$HERE/linux"
KOUT="${KOUT:-$HERE/build-e5}"
export ARCH=arm CROSS_COMPILE=arm-linux-gnu-
mkdir -p "$KOUT"
MAKE=(make -C "$KSRC" O="$KOUT")
E5_WLAN_FW="${E5_WLAN_FW:-$HOME/e5/wlan/fw}"
# initramfs/build.sh's busybox list + the E5 keymap, written with this checkout's paths
initramfs_list() {
	local I BB
	I="$(cd "$HERE/../initramfs" && pwd)"
	BB="$(realpath "$HERE/../../work/dl/busybox-armv5l")"
	cat > "$KOUT/initramfs.list" <<LIST
dir /dev 755 0 0
nod /dev/console 600 0 0 c 5 1
dir /bin 755 0 0
dir /sbin 755 0 0
dir /proc 755 0 0
dir /sys 755 0 0
dir /tmp 1777 0 0
file /bin/busybox $BB 755 0 0
slink /bin/sh busybox 777 0 0
file /init $I/init.sh 755 0 0
file /sbin/init $I/init 755 0 0
file /bin/e6sh $I/e6sh.sh 755 0 0
dir /etc 755 0 0
file /etc/e5.kmap $I/e5.kmap 644 0 0
file /bin/keyscan $I/keyscan.sh 755 0 0
file /bin/e5-dhcp $I/e5-dhcp.sh 755 0 0
file /bin/brightness $I/e5-brightness.sh 755 0 0
file /bin/blank $I/e5-blank.sh 755 0 0
LIST
	"$KSRC/scripts/config" --file "$KOUT/.config" --set-str INITRAMFS_SOURCE "$KOUT/initramfs.list"
}
config() {
	cp "$HERE/e6.config" "$KOUT/.config"
	local S="$KSRC/scripts/config --file $KOUT/.config"
	for o in DRM SOUND RAPUYAMA_MODEM FB_RAPUYAMA FB_SIMPLE; do $S -d "$o"; done
	# D3 display: rfbi fbdev + fbcon (8x8 font = 40x30 text on 320x240)
	for o in FB FB_DEVICE FB_RAPUYAMA_E5 FRAMEBUFFER_CONSOLE FONTS FONT_8x8 FONT_8x16; do $S -e "$o"; done
	# more built-in fonts, fbcon=font:<name> (grid on 320x240): MINI4x6 80x40,
	# 6x8 53x30, 6x10 53x24, ProFont6x11 53x21, 7x14 45x17, 10x18 32x13.
	# --keep-case: scripts/config upper-cases names (FONT_6X10 doesn't exist)
	for o in FONT_MINI_4x6 FONT_6x8 FONT_6x10 FONT_6x11 FONT_7x14 FONT_10x18; do $S --keep-case -e "$o"; done
	# phase 5 keyboard: polled matrix
	for o in INPUT_KEYBOARD KEYBOARD_RAPUYAMA_E5; do $S -e "$o"; done
	# usb: g_cdc (acm serial = ttyGS0 as before + cdc ecm ethernet = usb0)
	# instead of the plain g_serial. only one built-in gadget can bind
	for o in USB_G_SERIAL USB_ETH; do $S -d "$o"; done
	$S -e USB_CDC_COMPOSITE
	# wlan: TI WL1251 on spi2 (polled spi-rapuyama.c), the E6 ones off
	for o in BRCMUTIL BRCMFMAC ZD1211RW; do $S -d "$o"; done
	for o in WLAN WLAN_VENDOR_TI WL1251 WL1251_SPI SPI_RAPUYAMA_E5; do $S -e "$o"; done
	# wl1251 reads its nvs at probe (before the rootfs): both blobs built in
	# from E5_WLAN_FW (cut from the E5 rom, see the E5 README; not in the
	# repo). without them (release Image: E5_WLAN_FW=/none) the wl1251 probe
	# fails, the rest boots, and init.sh binds it later with the files that
	# LinuxE5 copied out of the phone's rom to E:\linux\
	if [ -f "$E5_WLAN_FW/ti-connectivity/wl1251-nvs.bin" ]; then
		$S --set-str EXTRA_FIRMWARE "ti-connectivity/wl1251-fw.bin ti-connectivity/wl1251-nvs.bin"
		$S --set-str EXTRA_FIRMWARE_DIR "$E5_WLAN_FW"
	fi
	# P1: gazoo light pwm as backlight class devices (not the screen; the
	# screen backlight is in the fbdev driver, /sys/class/backlight/panel)
	for o in BACKLIGHT_CLASS_DEVICE BACKLIGHT_RAPUYAMA_E5; do $S -e "$o"; done
	initramfs_list
	"${MAKE[@]}" olddefconfig
}
case "${1:-build}" in
	config) config ;;
	build)  [ -f "$KOUT/.config" ] || config
		initramfs_list
		"${MAKE[@]}" -j"$(nproc)" Image
		dtc -I dts -O dtb -o "$KOUT/e5.dtb" "$HERE/../boot/e5.dts"
		ls -la "$KOUT/arch/arm/boot/Image" "$KOUT/e5.dtb" ;;
	*) echo "usage: $0 [config|build]"; exit 1 ;;
esac

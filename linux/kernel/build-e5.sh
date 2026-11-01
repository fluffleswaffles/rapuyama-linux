#!/usr/bin/env bash
# E5-00 kernel + dtb: e6.config minus drm/sound/wlan, plus the E5 rfbi fbdev + fbcon/the E6 modem keepalive (blind E6 pmic writes), E5 initramfs list (generated in the output dir),
# own output dir so the E6 trees stay untouched. usage: ./build-e5.sh [config|build]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
KSRC="$HERE/linux"
KOUT="${KOUT:-$HERE/build-e5}"
export ARCH=arm CROSS_COMPILE=arm-linux-gnu-
mkdir -p "$KOUT"
MAKE=(make -C "$KSRC" O="$KOUT")
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
LIST
	"$KSRC/scripts/config" --file "$KOUT/.config" --set-str INITRAMFS_SOURCE "$KOUT/initramfs.list"
}
config() {
	cp "$HERE/e6.config" "$KOUT/.config"
	local S="$KSRC/scripts/config --file $KOUT/.config"
	for o in DRM SOUND WLAN RAPUYAMA_MODEM FB_RAPUYAMA FB_SIMPLE; do $S -d "$o"; done
	# D3 display: rfbi fbdev + fbcon (8x8 font = 40x30 text on 320x240)
	for o in FB FB_DEVICE FB_RAPUYAMA_E5 FRAMEBUFFER_CONSOLE FONTS FONT_8x8 FONT_8x16; do $S -e "$o"; done
	# phase 5 keyboard: polled matrix
	for o in INPUT_KEYBOARD KEYBOARD_RAPUYAMA_E5; do $S -e "$o"; done
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

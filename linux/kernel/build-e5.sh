#!/usr/bin/env bash
# E5-00 kernel + dtb: e6.config minus display/sound/wlan/the E6 modem keepalive (blind E6 pmic writes), E5 initramfs list,
# own output dir so the E6 trees stay untouched. usage: ./build-e5.sh [config|build]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
KSRC="$HERE/linux"
KOUT="${KOUT:-$HERE/build-e5}"
export ARCH=arm CROSS_COMPILE=arm-linux-gnu-
mkdir -p "$KOUT"
MAKE=(make -C "$KSRC" O="$KOUT")
config() {
	cp "$HERE/e6.config" "$KOUT/.config"
	local S="$KSRC/scripts/config --file $KOUT/.config"
	$S --set-str INITRAMFS_SOURCE "$HERE/../initramfs/e5.list"
	for o in DRM FB SOUND WLAN RAPUYAMA_MODEM; do $S -d "$o"; done
	"${MAKE[@]}" olddefconfig
}
case "${1:-build}" in
	config) config ;;
	build)  [ -f "$KOUT/.config" ] || config
		"${MAKE[@]}" -j"$(nproc)" Image
		dtc -I dts -O dtb -o "$KOUT/e5.dtb" "$HERE/../boot/e5.dts"
		ls -la "$KOUT/arch/arm/boot/Image" "$KOUT/e5.dtb" ;;
	*) echo "usage: $0 [config|build]"; exit 1 ;;
esac

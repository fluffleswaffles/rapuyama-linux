#!/usr/bin/env bash
# E5 release files for the simple install (README "Install (simple mode)"):
# LinuxE5.sis, Image, e5.dtb + SHA256SUMS, the user copies them to E:\.
# builds the blob-free Image (no Nokia wlan firmware, E5_WLAN_FW=/none: the
# loader copies it from the phone's own rom) with a neutral user@host in the
# version string, then checks that nothing of this pc or of the dev phone
# is in it. the sis is built and signed separately (README "Build", your
# own SHA-1 cert), so it is passed in.
# usage: tools/e5-release.sh <LinuxE5.sis> [out dir]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$(cd "$HERE/.." && pwd)"
SIS="$(realpath "${1:?usage: e5-release.sh <LinuxE5.sis> [out dir]}")"
OUT="${2:-$TOP/out/e5-release}"
KOUT="$TOP/linux/kernel/build-e5-pub"
IMAGE="$KOUT/arch/arm/boot/Image"
DTB="$KOUT/e5.dtb"
CPIO="$KOUT/usr/initramfs_data.cpio"
[ -f "$SIS" ] || { echo "no $SIS"; exit 1; }

export KBUILD_BUILD_USER=e5 KBUILD_BUILD_HOST=rapuyama
KOUT="$KOUT" E5_WLAN_FW=/none "$TOP/linux/kernel/build-e5.sh" config
KOUT="$KOUT" "$TOP/linux/kernel/build-e5.sh" build

fail() { echo "NOT RELEASING: $*"; exit 1; }
# Nokia's wlan blobs must never be in a release Image
grep -q '^CONFIG_EXTRA_FIRMWARE=""' "$KOUT/.config" || fail "firmware built in (EXTRA_FIRMWARE)"
cpio -it < "$CPIO" 2>/dev/null | grep -E '^lib/firmware/|^etc/wpa_supplicant/wpa_supplicant\.conf$|e5-wlan-mac' &&
	fail "^ these are in the initramfs"
grep -aq "$HOME" "$IMAGE" "$CPIO" && fail "$HOME is named in the Image"
grep -aq "$(id -un)@$(hostname)" "$IMAGE" && fail "$(id -un)@$(hostname) in the Image"
# the dtb has to match e5.dts
dtc -q -I dts -O dtb "$TOP/linux/boot/e5.dts" | cmp -s - "$DTB" || fail "$DTB doesn't match linux/boot/e5.dts"

# only what this script makes (the out dir may be given)
mkdir -p "$OUT"
rm -f "$OUT"/{LinuxE5.sis,Image,e5.dtb,SHA256SUMS}
cp "$SIS" "$OUT/LinuxE5.sis"
cp "$IMAGE" "$OUT/Image"
cp "$DTB" "$OUT/e5.dtb"
(cd "$OUT" && sha256sum LinuxE5.sis Image e5.dtb > SHA256SUMS && cat SHA256SUMS)
grep -a -m1 -o 'Linux version [^#]*' "$OUT/Image"
echo "release files in $OUT"

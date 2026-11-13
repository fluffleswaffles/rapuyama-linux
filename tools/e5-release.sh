#!/usr/bin/env bash
# E5 release files for the simple install (README "Install (simple mode)"):
# LinuxE5.sis, 3GLinuxE5.sis, Image, e5.dtb, e5-3g.dtb + SHA256SUMS, the user
# copies them to E:\. (3GLinuxE5 = Linux with mobile data: e5-3g.dtb, the
# modem runtime is on the advanced rootfs only)
# also e5-advanced.tar.gz for the advanced install (README "Advanced
# install"): install.sh (tools/e5-install.sh) + rootfs.tar.gz (tools/sdroot.sh
# e5, built without root, with the mobile data runtime) + the same five
# files, run on a Linux pc.
# builds the blob-free Image (no Nokia wlan firmware, E5_WLAN_FW=/none: the
# loader copies it from the phone's own rom) with a neutral user@host in the
# version string, then checks that nothing of this pc or of the dev phone
# is in it. the sis is built and signed separately (README "Build", your
# own SHA-1 cert), so both are passed in.
# usage: tools/e5-release.sh <LinuxE5.sis> <3GLinuxE5.sis> [out dir]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$(cd "$HERE/.." && pwd)"
USAGE="usage: e5-release.sh <LinuxE5.sis> <3GLinuxE5.sis> [out dir]"
SIS="$(realpath "${1:?$USAGE}")"
SIS3G="$(realpath "${2:?$USAGE}")"
OUT="${3:-$TOP/out/e5-release}"
KOUT="$TOP/linux/kernel/build-e5-pub"
IMAGE="$KOUT/arch/arm/boot/Image"
DTB="$KOUT/e5.dtb"
DTB3G="$KOUT/e5-3g.dtb"
CPIO="$KOUT/usr/initramfs_data.cpio"
[ -f "$SIS" ] || { echo "no $SIS"; exit 1; }
[ -f "$SIS3G" ] || { echo "no $SIS3G"; exit 1; }

[ -z "${E5_FBDEV:-}" ] || { echo "NOT RELEASING: E5_FBDEV is set (a release ships the KMS driver)"; exit 1; }
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
# the dtbs: e5.dtb = e5-cell.dts (the beta kernel is the cellular build),
# e5-3g.dtb = the same + e5.mobile=1
dtc -q -I dts -O dtb "$TOP/linux/boot/e5-cell.dts" | cmp -s - "$DTB" || fail "$DTB doesn't match linux/boot/e5-cell.dts"
"$TOP/linux/kernel/check-e5-dtbs.sh" "$KOUT" || fail "dtb check"
"$TOP/linux/kernel/check-e5-kms-config.sh" "$KOUT" || fail "kms config"

# only what this script makes (the out dir may be given)
mkdir -p "$OUT"
rm -f "$OUT"/{LinuxE5.sis,3GLinuxE5.sis,Image,e5.dtb,e5-3g.dtb,SHA256SUMS,e5-advanced.tar.gz}
cp "$SIS" "$OUT/LinuxE5.sis"
cp "$SIS3G" "$OUT/3GLinuxE5.sis"
cp "$IMAGE" "$OUT/Image"
cp "$DTB" "$OUT/e5.dtb"
cp "$DTB3G" "$OUT/e5-3g.dtb"
FILES="LinuxE5.sis 3GLinuxE5.sis Image e5.dtb e5-3g.dtb"
(cd "$OUT" && sha256sum $FILES > SHA256SUMS && cat SHA256SUMS)
grep -a -m1 -o 'Linux version [^#]*' "$OUT/Image"

# advanced install: the sd rootfs as a tarball (no swapfile: install.sh makes it)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
R="$STAGE/rootfs"
mkdir "$R"
# the mobile data runtime (e5rt, e5gprs, pnping, e5mem.ko for this kernel)
"$TOP/linux/e5-cell/build.sh" "$STAGE/cell" "$KOUT" > "$STAGE/cell.log" 2>&1 ||
	{ tail -20 "$STAGE/cell.log"; fail "linux/e5-cell/build.sh"; }
SDROOT_NOSUDO=1 SWAP_SIZE=0 KOUT="$KOUT" E5_CELL_OUT="$STAGE/cell" "$TOP/tools/sdroot.sh" "$R" e5 > "$STAGE/sdroot.log" 2>&1 ||
	{ cat "$STAGE/sdroot.log"; fail "sdroot.sh"; }
grep -q '^warning' "$STAGE/sdroot.log" && { grep '^warning' "$STAGE/sdroot.log"; fail "mobile data files missing"; }
# apk's install log is from this pc (paths of the build dir): not shipped
rm -f "$R/var/log/apk.log"
# nothing of this pc or of the dev phone in the rootfs
for f in etc/wpa_supplicant/wpa_supplicant.conf etc/e5-wlan-mac etc/e5-apn etc/e5-imei swapfile root/.ash_history; do
	[ -e "$R/$f" ] && fail "$f is in the rootfs"
done
ls "$R"/etc/ssh/ssh_host_* > /dev/null 2>&1 && fail "ssh host keys in the rootfs"
grep -rIl -e "$HOME" -e "$(id -un)@$(hostname)" "$R" && fail "^ these name $HOME or this pc"
# binaries too for the runtime we built (grep -I skips them above)
grep -al -e "$HOME" -e "$(id -un)@$(hostname)" "$R"/usr/libexec/e5/* && fail "^ these name $HOME or this pc"
A="$STAGE/e5-advanced"
mkdir "$A"
tar -C "$R" --owner=0 --group=0 --numeric-owner -czf "$A/rootfs.tar.gz" .
cp "$TOP/tools/e5-install.sh" "$A/install.sh"
(cd "$OUT" && cp $FILES "$A/")
(cd "$A" && sha256sum install.sh rootfs.tar.gz $FILES > SHA256SUMS)
tar -C "$STAGE" --owner=0 --group=0 -czf "$OUT/e5-advanced.tar.gz" e5-advanced
(cd "$OUT" && sha256sum e5-advanced.tar.gz >> SHA256SUMS)
ls -la "$OUT/e5-advanced.tar.gz"
echo "release files in $OUT"

#!/usr/bin/env bash
# E5-00 advanced install, run on a Linux PC from the unpacked e5-advanced
# release tarball (README "Advanced install"). ERASES the whole card and makes
#   p1 FAT32 "E5CARD" = Symbian's E: (default 2 GiB): Image, e5.dtb, e5-3g.dtb,
#                       LinuxE5.sis, 3GLinuxE5.sis
#   p2 ext4 "e5root"  = the rest: the Alpine rootfs + a /swapfile (1 GiB)
# Linux then runs from p2 (normal mode, e6.root=/dev/mmcblk0p2 in e5.dtb).
# usage: sudo ./install.sh /dev/sdX [--fat-size 2G] [--card-size 4900M]
#   --fat-size   size of E: (min 256M)
#   --card-size  use only the start of the card: for fake cards that report
#                more than they hold (check a new card with f3probe first)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
usage() { echo "usage: sudo $0 /dev/sdX [--fat-size 2G] [--card-size SIZE]" >&2; exit 1; }

T=; FAT=2G; CARD=
while [ $# -gt 0 ]; do
	case "$1" in
	--fat-size) FAT="${2:?}"; shift 2 ;;
	--card-size) CARD="${2:?}"; shift 2 ;;
	-h|--help) usage ;;
	/dev/*) T="$1"; shift ;;
	*) usage ;;
	esac
done
[ -n "$T" ] || usage
[ "$(id -u)" = 0 ] || { echo "run it with sudo (it partitions the card)" >&2; exit 1; }
for c in sfdisk mkfs.vfat mkfs.ext4 tar fallocate mkswap blockdev lsblk numfmt sha256sum; do
	command -v "$c" > /dev/null || { echo "missing tool: $c (dosfstools, e2fsprogs, util-linux)" >&2; exit 1; }
done

# the release files, checked before anything is touched
cd "$HERE"
for f in Image e5.dtb e5-3g.dtb LinuxE5.sis 3GLinuxE5.sis rootfs.tar.gz SHA256SUMS; do
	[ -f "$f" ] || { echo "no $f next to install.sh: unpack the whole tarball" >&2; exit 1; }
done
sha256sum --quiet -c SHA256SUMS || { echo "release files damaged (SHA256SUMS)" >&2; exit 1; }

# the target: a whole, removable card, not a partition or a pc disk
[ -b "$T" ] || { echo "$T is not a block device" >&2; exit 1; }
name=$(basename "$(readlink -f "$T")")
[ -e "/sys/class/block/$name/partition" ] && { echo "$T is a partition: give the whole card (e.g. /dev/sdb)" >&2; exit 1; }
if [ "$(cat "/sys/class/block/$name/removable")" != 1 ] && [[ "$name" != mmcblk* ]]; then
	echo "$T is not a removable card, refusing" >&2; exit 1
fi
p=; [[ "$name" =~ [0-9]$ ]] && p=p

dev_bytes=$(blockdev --getsize64 "$T")
use=$dev_bytes
if [ -n "$CARD" ]; then
	use=$(numfmt --from=iec "$CARD")
	[ "$use" -le "$dev_bytes" ] || { echo "--card-size $CARD is bigger than the card" >&2; exit 1; }
fi
fat=$(numfmt --from=iec "$FAT")
[ "$fat" -ge $((256 << 20)) ] || { echo "--fat-size must be at least 256M" >&2; exit 1; }
# sectors of 512 bytes, partitions on 1 MiB boundaries
s1=2048
n1=$(( fat / 512 / 2048 * 2048 ))
s2=$(( s1 + n1 ))
n2=$(( (use / 512 / 2048 * 2048) - s2 ))
min2=$(( (3 << 30) / 2 / 512 ))	# 1.5 GiB: rootfs + swapfile
[ "$n2" -ge "$min2" ] || { echo "card too small: p2 would be $((n2 / 2048)) MiB, needs 1536 MiB" >&2; exit 1; }
swap=1G; [ "$n2" -ge $(( (5 << 30) / 2 / 512 )) ] || swap=512M

echo "card $T: $(numfmt --to=iec "$dev_bytes") reported, using $(numfmt --to=iec "$use")"
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "$T"
echo "new layout: p1 FAT32 E5CARD $((n1 / 2048)) MiB, p2 ext4 e5root $((n2 / 2048)) MiB (swapfile $swap)"
read -rp "EVERYTHING on $T gets erased. Type $name to go on: " ok
[ "$ok" = "$name" ] || { echo "cancelled"; exit 1; }

# one mount point per line (desktop auto-mounts can have spaces: "NO NAME")
lsblk -nlo MOUNTPOINT "$T" | while IFS= read -r m; do
	if [ -n "$m" ]; then umount "$m"; fi
done
sfdisk -q "$T" <<EOF
label: dos
unit: sectors
start=$s1, size=$n1, type=c
start=$s2, size=$n2, type=83
EOF
udevadm settle 2>/dev/null || sleep 2
mkfs.vfat -F 32 -n E5CARD "/dev/$name${p}1" > /dev/null
mkfs.ext4 -q -F -L e5root "/dev/$name${p}2"

M1=$(mktemp -d); M2=$(mktemp -d)
trap 'umount "$M1" "$M2" 2>/dev/null; rmdir "$M1" "$M2"' EXIT
mount -o noatime "/dev/$name${p}2" "$M2"
echo "writing the rootfs (a few minutes on a slow card)..."
tar -xzpf rootfs.tar.gz --numeric-owner -C "$M2"
fallocate -l "$swap" "$M2/swapfile"
chmod 600 "$M2/swapfile"
mkswap -q "$M2/swapfile"
mount "/dev/$name${p}1" "$M1"
cp Image e5.dtb e5-3g.dtb LinuxE5.sis 3GLinuxE5.sis "$M1/"
sync
(cd "$M1" && grep -E ' (Image|e5\.dtb|e5-3g\.dtb|LinuxE5\.sis|3GLinuxE5\.sis)$' "$HERE/SHA256SUMS" | sha256sum --quiet -c -) ||
	{ echo "copy on the card does not match: bad card?" >&2; exit 1; }
umount "$M1" "$M2"
echo "done. Next: card into the phone, in Symbian install E:\\LinuxE5.sis, run LinuxE5"
echo "(first run copies the WLAN firmware from the phone's ROM to E:\\linux, then Linux starts)"
echo "3G mobile data: also install E:\\3GLinuxE5.sis; in Linux run \`mobile setup\` once"
echo "(IMEI + APN), then start Linux with 3GLinuxE5"

#!/usr/bin/env bash
# sd card rootfs for e6.root=/dev/mmcblk0p2: the same userspace the
# initramfs has (alpine + everything build-vc.sh adds), but unpruned.
# needs a finished build-vc.sh (it reads build-vc/initramfs.list).
#   sdroot.sh /dev/sdX   WIPES the whole card: p1 fat32 for symbian, p2 btrfs "e6root"
#   sdroot.sh <dir>      fills an already mounted rootfs (or any dir)
#   ... e5               E5-00: slim alpine (console tools, no gui), the E5
#                        initramfs files (linux/kernel/build-e5.sh) + `font`;
#                        p2 is ext4 "e5root" (new-card test 2026-11-08)
# SWAP_SIZE=0: no /swapfile (the release rootfs tarball: install.sh makes it)
# SDROOT_NOSUDO=1: fill a dir of your own without sudo (tarball builds)
# symbian has to mount p1: a card it can't mount gets its power cut, and
# linux only finds the card when symbian left it powered
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$HERE/.."
LIST="$TOP/linux/kernel/build-vc/initramfs.list"
ROOTFS="$TOP/work/rootfs-sd"
FAT_SIZE=2GiB
SWAP_SIZE=${SWAP_SIZE:-1GiB}
FAT_LABEL=E6CARD
ROOT_LABEL=e6root
ROOT_FS=btrfs
T="${1:?usage: sdroot.sh /dev/sdX | <dir> [e5]}"
DEV="${2:-e6}"
if [ "$DEV" = e5 ]; then
	LIST="${KOUT:-$TOP/linux/kernel/build-e5}/initramfs.list"
	ROOTFS="$TOP/work/rootfs-sd-e5"
	FAT_LABEL=E5CARD
	ROOT_LABEL=e5root
	ROOT_FS=ext4
	# fbdev console only (no drm = no weston). networking later: the apk
	# db stays whole, so `apk add` works once there is a network
	export ALPINE_PKGS="alpine-baselayout apk-tools busybox musl alpine-keys
ca-certificates ca-certificates-bundle nano tmux htop python3 git
openssh-client-default less ncurses-terminfo-base kbd kbd-misc font-terminus"
fi

[ -f "$LIST" ] || { echo "no $LIST, run linux/kernel/build-$([ "$DEV" = e5 ] && echo e5 || echo vc).sh first"; exit 1; }
SUDO=
[ "$(id -u)" = 0 ] || [ -n "${SDROOT_NOSUDO:-}" ] || SUDO=sudo

if [ -b "$T" ]; then
	name=$(basename "$(readlink -f "$T")")
	[ -e "/sys/class/block/$name/partition" ] && { echo "$T is a partition, give the whole card"; exit 1; }
	# usb readers say removable, built in readers are mmcblk
	if [ "$(cat "/sys/class/block/$name/removable")" != 1 ] && [[ "$name" != mmcblk* ]]; then
		echo "$T is not a removable card, refusing"
		exit 1
	fi
	if grep -q "^/dev/$name" /proc/mounts; then
		echo "$T has mounted partitions, unmount them first"
		exit 1
	fi
	lsblk -o NAME,SIZE,MODEL,LABEL,FSTYPE "$T"
	read -rp "everything on $T gets erased, type $name to go on: " ok
	[ "$ok" = "$name" ] || exit 1
fi

"$TOP/linux/initramfs/alpine.sh" "$ROOTFS" full

MNT="$T"
if [ -b "$T" ]; then
	printf 'label: dos\nsize=%s, type=c\ntype=83\n' "$FAT_SIZE" | $SUDO sfdisk -q "$T"
	$SUDO udevadm settle 2>/dev/null || sleep 2
	p=
	[[ "$name" =~ [0-9]$ ]] && p=p
	$SUDO mkfs.vfat -F 32 -n "$FAT_LABEL" "/dev/$name${p}1"
	if [ "$ROOT_FS" = ext4 ]; then
		$SUDO mkfs.ext4 -q -F -L "$ROOT_LABEL" "/dev/$name${p}2"
	else
		$SUDO mkfs.btrfs -q -f -L "$ROOT_LABEL" "/dev/$name${p}2"
	fi
	MNT=$(mktemp -d)
	$SUDO mount -o noatime "/dev/$name${p}2" "$MNT"
	trap '$SUDO umount "$MNT"; rmdir "$MNT"' EXIT
fi

tar -C "$ROOTFS" --owner=0 --group=0 -cf - . | $SUDO tar -C "$MNT" -xpf -
# what build-vc.sh puts on top of alpine: our busybox, /init, firmware, libvcgl ...
# the list has no trailing newline
while read -r kind dst a b _ || [ -n "$kind" ]; do
	case "$kind" in
	dir) $SUDO install -d -m "$a" "$MNT$dst" ;;
	file) $SUDO install -D -m "$b" "$a" "$MNT$dst" ;;
	slink) $SUDO ln -sfn "$a" "$MNT$dst" ;;
	esac
done < "$LIST"
if [ "$DEV" = e5 ]; then
	$SUDO install -D -m 755 "$TOP/linux/initramfs/e5-font.sh" "$MNT/usr/bin/font"
	$SUDO install -D -m 755 "$TOP/linux/initramfs/e5-wifi.sh" "$MNT/usr/bin/wifi"
	# ssh server on demand: the script only, openssh-server + nftables are an
	# apk add away; host keys are made on the phone (ssh-server on)
	$SUDO install -D -m 755 "$TOP/linux/initramfs/e5-ssh.sh" "$MNT/usr/bin/ssh-server"
	# tailscale on demand: the script only (apk add tailscale), the login is
	# made on the phone (tailnet on)
	$SUDO install -D -m 755 "$TOP/linux/initramfs/e5-tailnet.sh" "$MNT/usr/bin/tailnet"
	# optional graphical browser: the launcher + X config only, X itself is
	# an apk add away (see the README, "Graphical browser")
	$SUDO install -D -m 755 "$TOP/linux/initramfs/e5-browser.sh" "$MNT/usr/bin/browser"
	$SUDO install -D -m 644 "$TOP/linux/initramfs/e5-xorg.conf" "$MNT/etc/X11/xorg.conf.d/10-e5.conf"
	# the shells go through `login -f root` (init.sh), which prints this
	# welcome (the helpers) instead of alpine's
	$SUDO install -m 644 "$TOP/linux/initramfs/e5-motd" "$MNT/etc/motd"
fi
$SUDO mkdir -p "$MNT/dev" "$MNT/proc" "$MNT/sys" "$MNT/run" "$MNT/root"
$SUDO install -d -m 1777 "$MNT/tmp"
# swap files must be fully allocated (btrfs: also nocow), init.sh swapon's it
if [ "$SWAP_SIZE" != 0 ] && [ ! -f "$MNT/swapfile" ]; then
	$SUDO truncate -s 0 "$MNT/swapfile"
	[ "$ROOT_FS" = btrfs ] && $SUDO chattr +C "$MNT/swapfile"
	$SUDO fallocate -l "$SWAP_SIZE" "$MNT/swapfile"
	$SUDO chmod 600 "$MNT/swapfile"
	$SUDO mkswap "$MNT/swapfile" > /dev/null
fi
sync
echo "rootfs on $T done, boot with e6.root=/dev/mmcblk0p2"
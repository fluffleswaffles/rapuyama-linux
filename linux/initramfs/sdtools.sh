# /etc/sdtools.sh - SD helpers as shell functions.
# init.sh: export ENV=/etc/sdtools.sh   (interactive sh reads it)
# card rootfs: add ". /etc/sdtools.sh" to /etc/profile

# mntsd [-r] [-p N]   FAT partition (default p1) on /sd, read-write
# SD=/dev/mmcblk1p1 mntsd forces a device
mntsd() {
	local mode=rw p=1 o first= d opts OPTIND=1
	while getopts rp: o; do
		case $o in
		r) mode=ro ;;
		p) p=$OPTARG ;;
		*) echo "usage: mntsd [-r] [-p N]"; return 1 ;;
		esac
	done
	mkdir -p /sd
	if grep -q ' /sd ' /proc/mounts; then
		mount -o remount,$mode /sd && echo "/sd remounted $mode"
		return
	fi
	opts=$mode,exec,noatime,flush
	if [ -n "$SD" ]; then
		mount -t vfat -o $opts "$SD" /sd || return 1
	else
		# emmc (symbian E:) is an mmcblk too: prefer the one holding /linux or /e5
		for d in /dev/mmcblk*p$p; do
			[ -b "$d" ] || continue
			mount -t vfat -o $opts "$d" /sd 2>/dev/null || continue
			if [ -d /sd/linux ] || [ -d /sd/e5 ]; then first=$d; break; fi
			[ -z "$first" ] && first=$d
			umount /sd
		done
		[ -n "$first" ] || { echo "mntsd: no FAT partition $p found"; return 1; }
		grep -q ' /sd ' /proc/mounts || mount -t vfat -o $opts "$first" /sd || return 1
	fi
	echo "$(grep ' /sd ' /proc/mounts | cut -d' ' -f1) on /sd ($mode)"
}

# umntsd   sync + unmount, safe to pull the card
umntsd() {
	sync
	umount /sd && echo "/sd unmounted, safe to remove"
}

# bootsd [/dev/mmcblkNp2]   ask init (pid 1, sd_switch handler) to switch_root
bootsd() {
	local dev=$1 d
	if [ -z "$dev" ]; then
		for d in /dev/mmcblk*p2; do [ -b "$d" ] && { dev=$d; break; }; done
	fi
	[ -b "$dev" ] || { echo "bootsd: no root partition, give one: bootsd /dev/mmcblk1p2"; return 1; }
	[ -z "$E6_ROOT" ] || { echo "bootsd: already on the card rootfs"; return 1; }
	echo "$dev" > /tmp/bootsd.dev
	umount /sd 2>/dev/null
	echo "switching to $dev (see dmesg if you land back here)"
	kill -HUP 1
}

# sdrun NAME [args]   run /sd/e5/NAME (mounts /sd if needed)
sdrun() {
	[ -n "$1" ] || { echo "usage: sdrun NAME [args]"; return 1; }
	grep -q ' /sd ' /proc/mounts || mntsd >/dev/null || return 1
	local n=$1; shift
	[ -f "/sd/e5/$n" ] || { echo "sdrun: /sd/e5/$n not found"; ls /sd/e5 2>/dev/null; return 1; }
	( cd /sd/e5 && exec sh "/sd/e5/$n" "$@" )
}

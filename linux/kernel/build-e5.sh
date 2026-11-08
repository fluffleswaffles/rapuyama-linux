#!/usr/bin/env bash
# E5-00 kernel + dtb: e6.config minus drm, sound, the E6 fbdevs, the E6 modem
# keepalive (blind E6 pmic writes) and the E6 wlan chips; plus the E5 rfbi
# fbdev + fbcon, keypad, g_cdc usb, the wl1251 on spi2 and the gazoo light
# pwm; slim() drops the pi/armv7/E6/pc parts. writes the E5 initramfs list
# into the output dir. own output dir, so the E6 trees stay untouched.
# usage: ./build-e5.sh [config|build]
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
	"$I/e5-keyd/build.sh" "$KOUT/e5-keyd" > /dev/null
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
file /etc/e5.xkb $I/e5.xkb 644 0 0
file /bin/e5-keyd $KOUT/e5-keyd/e5-keyd 755 0 0
file /bin/keyscan $I/keyscan.sh 755 0 0
file /bin/e5-dhcp $I/e5-dhcp.sh 755 0 0
file /bin/brightness $I/e5-brightness.sh 755 0 0
file /bin/blank $I/e5-blank.sh 755 0 0
file /etc/e5-welcome $I/e5-welcome 644 0 0
LIST
	alpine_list >> "$KOUT/initramfs.list"
	"$KSRC/scripts/config" --file "$KOUT/.config" --set-str INITRAMFS_SOURCE "$KOUT/initramfs.list"
}
# simple install (no sd rootfs, linux stays in the initramfs) gets wifi
# (wpa_supplicant + iw) and apk from alpine (the same armhf packages as the
# sd rootfs), pruned to their DT_NEEDED closure, ~+4 MB in the gzipped
# Image. `apk add` installs into RAM, gone at reboot. alpine's busybox is in
# the apk db but its files stay out: `apk add` must never replace our static
# /bin/busybox (init's shells use its cttyhack). cached in
# work/rootfs-e5-simple, delete it and its .done/.pruned stamps to rebuild
alpine_list() {
	local W f
	W="$(realpath -m "$HERE/../../work/rootfs-e5-simple")"
	if [ ! -e "$W.pruned" ]; then
		ALPINE_PKGS="musl wpa_supplicant iw apk-tools alpine-keys
ca-certificates-bundle busybox busybox-binsh" "$HERE/../initramfs/alpine.sh" "$W" full
		python3 "$HERE/../../tools/prune_rootfs.py" "$W" sbin/wpa_supplicant \
			sbin/wpa_passphrase usr/sbin/iw sbin/apk lib/ld-musl-armhf.so.1
		touch "$W.pruned"
	fi
	# not /bin, /etc/busybox*, usr/share/udhcpc: alpine busybox's files
	(cd "$W" && find lib usr sbin etc/apk etc/ssl -path usr/share/udhcpc -prune -o -print | sort) |
	while read -r f; do
		if [ "$f" = sbin ]; then
			continue
		elif [ -L "$W/$f" ]; then
			echo "slink /$f $(readlink "$W/$f") 777 0 0"
		elif [ -d "$W/$f" ]; then
			echo "dir /$f $(stat -c %a "$W/$f") 0 0"
		else
			echo "file /$f $W/$f $(stat -c %a "$W/$f") 0 0"
		fi
	done
	echo 'dir /usr/share/udhcpc 755 0 0'
	echo "file /usr/bin/wifi $I/e5-wifi.sh 755 0 0"
	echo "file /usr/share/udhcpc/default.script $I/e5-dhcp.sh 755 0 0"
}
# slim E5 kernel (plan Phase 14): e6.config is a raspberry pi based,
# multi-platform config. drop what the E5 can't use (pi, armv7, E6 and pc
# hardware, nfs, debug extras), keep and add what the goals need (tailscale,
# sshd firewall, cellular development, the new-card memory test)
slim() {
	local S="$KSRC/scripts/config --file $KOUT/.config"
	# A: platform + cpu: only the armv6 rapuyama, no pi, no armv7
	for o in ARCH_MULTI_V7 ARCH_BCM ARCH_BCM2835 RASPBERRYPI_FIRMWARE BCM2835_MBOX MAILBOX \
		BCM_VIDEOCORE BCM2835_VCHIQ VCHIQ_CDEV CMA DMA_CMA; do $S -d "$o"; done
	# A: E6 and pi hardware
	for o in INPUT_TOUCHSCREEN KEYBOARD_RAPUYAMA KEYBOARD_GPIO_POLLED LEDS_E6_KEYBOARD NEW_LEDS \
		I2C_BCM2835 I2C_BRCMSTB SPI_BCM2835 SPI_BCM2835AUX MMC_BCM2835 MMC_SDHCI MMC_SDHCI_IPROC \
		PWM_BCM2835 PINCTRL_BCM2835 GPIO_RASPBERRYPI_EXP BCM2711_THERMAL BCM2835_THERMAL THERMAL HWMON \
		SERIAL_8250 SERIAL_AMBA_PL011 PWRSEQ_EMMC; do $S -d "$o"; done
	# A: usb host side (the E5 usb is gadget only), wired ethernet, ps/2, bluetooth, media
	for o in USB HID SCSI ETHERNET PHYLIB SERIO INPUT_MOUSE KEYBOARD_ATKBD BT MEDIA_SUPPORT; do $S -d "$o"; done
	for v in ATMEL MEDIATEK; do $S -d "WLAN_VENDOR_$v"; done
	# A: nfs, ext2, unused initramfs decompressors (the initramfs is gzip)
	for o in NFS_FS NFSD IP_PNP EXT2_FS RD_BZIP2 RD_LZMA RD_XZ RD_LZO RD_LZ4 RD_ZSTD; do $S -d "$o"; done
	# A: debug extras + cpufreq (no E5 driver)
	for o in KGDB KPROBES PROFILING PERF_EVENTS CPU_FREQ; do $S -d "$o"; done
	# B: smaller log buffer (128 KB; the ramcon RAM log stays), lighter symbols
	# (function names stay in oopses), no runtime debug switches
	$S --set-val LOG_BUF_SHIFT 17
	for o in KALLSYMS_ALL DYNAMIC_DEBUG SLUB_DEBUG SCHED_DEBUG IO_URING KEXEC IPV6_SIT GPIO_SYSFS; do $S -d "$o"; done
	# B: cgroups: only cpu (sched autogroup selects it); memcg went with zswap
	for o in CGROUP_FREEZER CGROUP_DEVICE CGROUP_CPUACCT CGROUP_PERF; do $S -d "$o"; done
	# group 1: tailscale (tun + policy routing) and a firewall (nftables)
	for o in TUN IP_ADVANCED_ROUTER IP_MULTIPLE_TABLES IPV6_MULTIPLE_TABLES \
		NETFILTER NF_CONNTRACK NF_NAT NF_TABLES NF_TABLES_INET NF_TABLES_IPV4 NF_TABLES_IPV6 \
		NFT_CT NFT_NAT NFT_MASQ NFT_REJECT NFT_LIMIT NFT_LOG; do $S -e "$o"; done
	# new-card test results (2026-11-06/08): swap = zram (lzo-rle) + a
	# swapfile, root = ext4. out: zswap + memcg, zram writeback, the lz4/zstd
	# backends (slower than lzo-rle on the arm11) and btrfs (slower, ~5x the
	# card writes on fsync); kept: loop, mglru (off until enabled at runtime),
	# psi (memory stall numbers)
	for o in ZSWAP MEMCG ZRAM_WRITEBACK ZRAM_TRACK_ENTRY_ACTIME ZRAM_BACKEND_LZ4 ZRAM_BACKEND_ZSTD \
		CRYPTO_LZ4 CRYPTO_ZSTD BTRFS_FS; do $S -d "$o"; done
	for o in EXT4_FS BLK_DEV_LOOP LRU_GEN PSI ZRAM_BACKEND_LZO ZRAM_DEF_COMP_LZORLE; do $S -e "$o"; done
	$S -d ZRAM_DEF_COMP_LZ4
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
	# sd card dma: the general dma (gdma)
	for o in DMADEVICES RAPUYAMA_GDMA; do $S -e "$o"; done
	slim
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

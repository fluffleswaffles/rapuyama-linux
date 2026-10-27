#!/usr/bin/env bash
# kernel + dtb with the VideoCore display driver (CONFIG_SOC_RAPUYAMA_VIDEOCORE)
# usage: KERN_BIN=/path/to/kern.bin ./build-vc.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
KSRC="$HERE/linux"
# KOUT=... builds elsewhere, so two builds never share a directory
KOUT="${KOUT:-$HERE/build-vc}"
# the E6 rom dump, the VideoCore images get pulled out of it
KERN_BIN="${KERN_BIN:?set KERN_BIN=/path/to/kern.bin}"

export ARCH=arm
export CROSS_COMPILE=arm-linux-gnu-

mkdir -p "$KOUT"
cp "$HERE/e6.config" "$KOUT/.config"
arm-linux-gnu-gcc -march=armv6k -marm -static -nostdlib -fno-pie -no-pie \
	-fomit-frame-pointer -fno-stack-protector -Os -Wall -Wextra -Werror \
	-Wl,-e,_start -o "$KOUT/fbtest" "$HERE/../initramfs/fbtest.c" -lgcc
# userspace /dev/mem probe for the VC bus and DSPC (docs/porting-guide.md)
arm-linux-gnu-gcc -march=armv6k -marm -static -nostdlib -fno-pie -no-pie \
	-ffreestanding -fno-builtin -fomit-frame-pointer -fno-stack-protector \
	-Os -Wall -Wextra \
	-Wl,-e,_start -o "$KOUT/vcprobe" "$HERE/../initramfs/vcprobe.c" -lgcc
# userspace for /dev/vc-khrn (musl, clang + alpine sysroot)
"$HERE/../vc-khrn/build.sh" "$KOUT/vc-khrn"
"$HERE/../yamashell/build.sh" "$KOUT/yamashell"
python3 "$HERE/../../tools/vc_extract.py" "$KERN_BIN" "$KOUT/vcfw"
# alpine armhf userspace with weston (cached, see alpine.sh)
ROOTFS="$HERE/../../work/rootfs-alpine"
"$HERE/../initramfs/alpine.sh" "$ROOTFS"

REV=$(git -C "$HERE/../.." describe --always --dirty 2>/dev/null || echo unknown)
printf 'rapuyama-linux %s (%s)\n' "$(date +%d.%m.%Y)" "$REV" > "$KOUT/rapuyama-release"

cp "$HERE/../initramfs/initramfs.list" "$KOUT/initramfs.list"
{
	# the list has no trailing newline, start on a fresh line
	printf '\ndir /usr 755 0 0\ndir /usr/bin 755 0 0\n'
	printf 'file /usr/bin/fbtest %s 755 0 0\n' "$KOUT/fbtest"
	printf 'file /usr/bin/vcprobe %s 755 0 0\n' "$KOUT/vcprobe"
	printf 'file /usr/bin/khrntest %s 755 0 0\n' "$KOUT/vc-khrn/khrntest"
	printf 'file /usr/bin/vctri %s 755 0 0\n' "$KOUT/vc-khrn/vctri"
	printf 'file /usr/bin/vcwl %s 755 0 0\n' "$KOUT/vc-khrn/vcwl"
	printf 'file /usr/bin/yamashell %s 755 0 0\n' "$KOUT/yamashell/yamashell"
	printf 'file /usr/bin/yamashell-settings %s 755 0 0\n' "$KOUT/yamashell/yamashell-settings"
	printf 'file /usr/bin/yamashell-session %s 755 0 0\n' "$HERE/../yamashell/session.sh"
	printf 'dir /usr/share/yamashell 755 0 0\n'
	printf 'dir /usr/share/yamashell/applications 755 0 0\n'
	printf 'dir /usr/share/yamashell/wallpapers 755 0 0\n'
	printf 'dir /usr/share/yamashell/icons 755 0 0\ndir /usr/share/yamashell/icons/hicolor 755 0 0\ndir /usr/share/yamashell/icons/hicolor/scalable 755 0 0\ndir /usr/share/yamashell/icons/hicolor/scalable/apps 755 0 0\n'
	for f in wallpaper.jpg Ubuntu-Regular.ttf Ubuntu-Light.ttf MaterialIcons-Regular.ttf applications/Alacritty.desktop applications/yamashell-settings.desktop icons/hicolor/scalable/apps/yamashell-settings.svg icons/hicolor/scalable/apps/yamashell-device-e6.svg icons/hicolor/scalable/apps/yamashell-device-n8.svg icons/hicolor/scalable/apps/yamashell-device-e7.svg; do
		printf 'file /usr/share/yamashell/%s %s 644 0 0\n' "$f" "$HERE/../yamashell/assets/$f"
	done
	printf 'file /usr/lib/libvcgl.so %s 755 0 0\n' "$KOUT/vc-khrn/libvcgl.so"
	printf 'slink /usr/lib/libEGL.so.1 libvcgl.so 777 0 0\n'
	printf 'slink /usr/lib/libGLESv2.so.2 libvcgl.so 777 0 0\n'
	printf 'dir /lib 755 0 0\ndir /lib/firmware 755 0 0\ndir /lib/firmware/vc 755 0 0\n'
	printf 'dir /etc/xdg 755 0 0\ndir /etc/xdg/weston 755 0 0\n'
	printf 'file /etc/xdg/weston/weston.ini %s 644 0 0\n' "$HERE/../initramfs/weston.ini"
	printf 'file /usr/share/X11/xkb/symbols/e6 %s 644 0 0\n' "$HERE/../initramfs/xkb-e6"
	printf 'file /usr/bin/e6-term %s 755 0 0\n' "$HERE/../initramfs/e6-term.sh"
	printf 'file /usr/bin/fps %s 755 0 0\n' "$HERE/../initramfs/fps.sh"
	printf 'file /etc/e6-check.sh %s 755 0 0\n' "$HERE/../initramfs/e6-check.sh"
	printf 'file /etc/passwd %s 644 0 0\nfile /etc/group %s 644 0 0\n' "$HERE/../initramfs/passwd" "$HERE/../initramfs/group"
	printf 'file /etc/rapuyama-release %s 644 0 0\n' "$KOUT/rapuyama-release"
	printf 'file /etc/fonts/local.conf %s 644 0 0\n' "$HERE/../initramfs/fonts-local.conf"
	printf 'dir /etc/alacritty 755 0 0\nfile /etc/alacritty/alacritty.toml %s 644 0 0\n' "$HERE/../initramfs/alacritty.toml"
	for f in vc-loader-a vc-loader-b vc-main; do
		printf 'file /lib/firmware/vc/%s.bin %s 644 0 0\n' "$f" "$KOUT/vcfw/$f.bin"
		printf 'file /lib/firmware/vc/%s.trl %s 644 0 0\n' "$f" "$KOUT/vcfw/$f.trl"
	done
} >> "$KOUT/initramfs.list"

# the driver reboots the VC, so Symbian's framebuffer area needs no
# protection: the normal PHYS_OFFSET from e6.config stays
"$KSRC/scripts/config" --file "$KOUT/.config" --disable FB_RAPUYAMA
"$KSRC/scripts/config" --file "$KOUT/.config" --enable FB --enable FRAMEBUFFER_CONSOLE
"$KSRC/scripts/config" --file "$KOUT/.config" --enable SOC_RAPUYAMA_VIDEOCORE
# rootfs first, our list after it wins on the same paths. uid/gid map our files to root
"$KSRC/scripts/config" --file "$KOUT/.config" --set-str INITRAMFS_SOURCE "$ROOTFS $KOUT/initramfs.list"
"$KSRC/scripts/config" --file "$KOUT/.config" --set-val INITRAMFS_ROOT_UID "$(id -u)"
"$KSRC/scripts/config" --file "$KOUT/.config" --set-val INITRAMFS_ROOT_GID "$(id -g)"
"$KSRC/scripts/config" --file "$KOUT/.config" --disable INITRAMFS_COMPRESSION_GZIP --enable INITRAMFS_COMPRESSION_XZ
# modem experiments off (drivers/misc/rapuyama_*), they made boots die
"$KSRC/scripts/config" --file "$KOUT/.config" --disable RAPUYAMA_MODEM

make -C "$KSRC" O="$KOUT" olddefconfig
make -C "$KSRC" O="$KOUT" -j"$(nproc)" zImage
make -C "$KSRC" O="$KOUT" scripts_dtc

for v in vc vc-565 vc-565-gpu vc-565-gpu-dma vc-565-gpu-dma-nohlt vc-565-charger vc-khrn vc-gl; do
	"$KOUT/scripts/dtc/dtc" -I dts -O dtb -i "$HERE/../boot" \
		-o "$KOUT/e6-$v.dtb" "$HERE/../boot/e6-$v.dts"
done

echo
echo "zImage: $KOUT/arch/arm/boot/zImage"
echo "DTB:    $KOUT/e6-vc-565.dtb (daily), e6-vc-565-gpu.dtb (yamashell), e6-vc-565-gpu-dma.dtb (yamashell + dma), e6-vc.dtb, e6-vc-khrn.dtb, e6-vc-gl.dtb"
ls -lh "$KOUT/arch/arm/boot/zImage"
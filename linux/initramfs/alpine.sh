#!/usr/bin/env bash
# alpine armhf userspace for the initramfs: weston (drm backend, pixman
# renderer, desktop shell) + weston-terminal + seatd. alpine armhf = armv6
# + vfp hard float = exactly the e6's arm1176, so the stock packages run.
# installed on the pc with the static x86_64 apk (no qemu, no root), then
# pruned to what the programs really load (tools/prune_rootfs.py), ~26 MB.
# the result is cached in work/rootfs-alpine, delete it to rebuild
# full = no prune, for the sd card rootfs (tools/sdroot.sh), apk db stays whole
# usage: alpine.sh <out dir> [full]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$HERE/../.."
OUT="${1:?usage: alpine.sh <out dir> [full]}"
FULL="${2:-}"
DL="$TOP/work/dl/alpine"
APK="$DL/apkt/sbin/apk.static"
MIRROR=https://dl-cdn.alpinelinux.org/alpine
REL=v3.24
PKGS="busybox musl alpine-keys weston weston-backend-drm weston-shell-desktop
weston-terminal seatd libudev-zero font-hack dbus libnotify alacritty
wayland-libs-egl"
# what has to keep working after the prune
ROOTS="usr/bin/weston usr/bin/weston-terminal usr/bin/seatd
usr/libexec/weston-desktop-shell usr/libexec/weston-keyboard
usr/lib/weston/desktop-shell.so usr/lib/libweston-14/drm-backend.so
lib/ld-musl-armhf.so.1
usr/bin/dbus-daemon usr/bin/dbus-uuidgen usr/bin/notify-send usr/lib/libelogind.so.0
usr/bin/alacritty usr/lib/libwayland-egl.so.1
usr/lib/libwayland-client.so.0 usr/lib/libxkbcommon.so.0
usr/lib/libwayland-server.so.0 usr/lib/libfreetype.so.6 usr/lib/libpng16.so.16 usr/lib/libjpeg.so.8"
# alacritty dlopens libwayland-client/-egl and libxkbcommon, yamashell (built
# against the sysroot, not in here) links the rest: listed so the prune keeps them

# stamp next to the dir, not in it (all of it goes into the initramfs)
[ -e "$OUT.done" ] && exit 0

# the cdn drops connections now and then, so everything retries
fetch() {
	local i
	for i in 1 2 3 4 5 6; do
		curl -sS -m 120 -fL -o "$2" "$1" && return 0
		sleep 3
	done
	return 1
}

if [ ! -x "$APK" ]; then
	mkdir -p "$DL/apkt"
	fetch "$MIRROR/$REL/main/x86_64/APKINDEX.tar.gz" "$DL/idx-x86.tar.gz"
	v=$(tar -xzOf "$DL/idx-x86.tar.gz" APKINDEX |
		awk -v RS= '/\nP:apk-tools-static\n/' | sed -n 's/^V://p')
	fetch "$MIRROR/$REL/main/x86_64/apk-tools-static-$v.apk" "$DL/apk-tools-static.apk"
	tar -xzf "$DL/apk-tools-static.apk" -C "$DL/apkt" 2>/dev/null || true
fi

rm -rf "$OUT" "$OUT.done"
mkdir -p "$OUT/etc/apk" "$DL/cache"
printf '%s/%s/main\n%s/%s/community\n' "$MIRROR" "$REL" "$MIRROR" "$REL" \
	> "$OUT/etc/apk/repositories"
ok=
for i in 1 2 3 4 5 6; do
	# --usermode: no chown as a normal user, the initramfs maps us to root
	if "$APK" --root "$OUT" --arch armhf --initdb --usermode --no-scripts \
		--allow-untrusted --cache-dir "$DL/cache" --timeout 60 -U \
		add $PKGS > "$DL/apk.log" 2>&1; then
		ok=1
		break
	fi
	sleep 3
done
[ -n "$ok" ] || { tail "$DL/apk.log"; exit 1; }

[ -n "$FULL" ] || python3 "$TOP/tools/prune_rootfs.py" "$OUT" $ROOTS
# fonts flat in /usr/share/fonts: fontconfig only enters a subdir when
# readdir reports it as one, which qemu-user (tools/weston_qemu.sh) doesn't
mv "$OUT"/usr/share/fonts/*/*.ttf "$OUT/usr/share/fonts/"
find "$OUT/usr/share/fonts" -mindepth 1 -type d -empty -delete
# ncurses-terminfo is ~7 mb, keep the few we use
[ -n "$FULL" ] || find "$OUT/usr/share/terminfo" -type f ! -name alacritty ! -name alacritty-direct \
	! -name xterm-256color ! -name linux ! -name vt100 -delete 2>/dev/null || true
[ -n "$FULL" ] || find "$OUT/usr/share/terminfo" -xtype l -delete 2>/dev/null || true
[ -n "$FULL" ] || find "$OUT/usr/share/terminfo" -mindepth 1 -type d -empty -delete 2>/dev/null || true
touch "$OUT.done"
du -sh "$OUT"
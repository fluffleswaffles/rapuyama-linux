#!/bin/sh
# E5: weston (drm backend, pixman) with the foot terminal on the KMS display,
# back to the console when weston exits.
#   wayland          start weston + foot
#   wayland help     this text
# weston and foot are not preinstalled:
#   apk add weston weston-backend-drm weston-shell-desktop foot font-dejavu
# Keys: the e5 layers as a layout (/etc/xkb/symbols/e5, from tools/e5-xkb.py):
# sym = printed symbols, fn = the Fn layer, one key each; the right soft key
# is Ctrl for one key. Not in weston: pointer mode (Mail), the arrows stay
# arrows. No touch screen, no pointer.
# Exiting foot (Ctrl+D) ends weston and returns to the console.
# LIBSEAT_BACKEND = seat backend (default builtin: weston runs as root; if
# weston dies within 5 s, one retry with noop; an explicit value is kept).
APK="apk add weston weston-backend-drm weston-shell-desktop foot font-dejavu"
case "$1" in
help | -h | --help)
	sed -n '2,/^# LIBSEAT/s/^# \{0,1\}//p' "$0"
	exit 0
	;;
"") ;;
*)
	echo "usage: wayland [help]" >&2
	exit 1
	;;
esac
# overrides for the tests (tests/test-e5-wayland.sh)
SYS=${SYS:-/sys}
TEMPLATE=${TEMPLATE:-/etc/xdg/weston/weston.ini}

if ! command -v weston > /dev/null || ! command -v foot > /dev/null; then
	echo "wayland: weston/foot not installed: $APK" >&2
	exit 1
fi
XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
export XDG_RUNTIME_DIR
mkdir -p "$XDG_RUNTIME_DIR" /var/cache/fontconfig
chmod 700 "$XDG_RUNTIME_DIR"
# the colour format follows the kernel's display mode: rgb565 -> weston draws
# 565 itself, else its default (also an old fbdev kernel without e5_display)
if [ "$(cat "$SYS/module/e5_display/parameters/rgb565" 2>/dev/null)" = Y ]; then
	gbm=gbm-format=rgb565
else
	gbm=
fi
sed "s/^#@GBM@\$/$gbm/" "$TEMPLATE" > "$XDG_RUNTIME_DIR/weston.ini" || exit 1
export SHELL=${SHELL:-/bin/sh}
echo "starting weston + foot..."
conf="$XDG_RUNTIME_DIR/weston.ini"
if [ -n "$LIBSEAT_BACKEND" ]; then
	weston --backend=drm --config="$conf"
	exit $?
fi
# libseat's builtin backend needs root and the vt; a weston that dies right
# away (libseat error) gets one more try without seat management, as the
# e6 init does
t=$(date +%s)
LIBSEAT_BACKEND=builtin weston --backend=drm --config="$conf"
rc=$?
if [ $rc != 0 ] && [ $(($(date +%s) - t)) -lt 5 ]; then
	echo "wayland: weston exited with $rc right away, retrying with LIBSEAT_BACKEND=noop" >&2
	LIBSEAT_BACKEND=noop weston --backend=drm --config="$conf"
	rc=$?
fi
[ $rc = 0 ] || echo "wayland: weston exited with $rc" >&2
exit $rc

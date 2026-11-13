#!/bin/sh
# E5: graphical browser in X on vt2, back to the console when it exits.
#   browser [dillo|links] [url]     (default: dillo, lite.duckduckgo.com)
# Needs X (see /etc/X11/xorg.conf.d/10-e5.conf) and dillo or links-graphics.
# Keys in X = the console layers (/etc/e5.xkb from tools/e5-xkb.py): sym =
# printed symbols, fn = the Fn layer for one key, right soft key = Ctrl for
# one key (rsk+L address, rsk+Q quit in dillo). Pointer: Mail toggles
# pointer mode (XKB MouseKeys), then the navi key moves the pointer and Enter
# (= navi centre) clicks; off, the keys are plain arrows/Enter.
# E5_PTR_STEP = pointer step in pixels (default 2).
if [ "$1" = "help" ]; then
	echo "browser: dillo or links in X on the screen."
	echo "  browser                 dillo, lite.duckduckgo.com"
	echo "  browser [dillo|links] [url]"
	echo "Takes about 30 s to start; closing it returns to the console."
	echo "Mail key = pointer mode: navi key moves, Enter clicks."
	echo "Not preinstalled: apk add xorg-server xf86-input-evdev xinit"
	echo "font-misc-misc dillo font-dejavu (or links-graphics)."
	exit 0
fi
if [ "$1" = "--session" ]; then
	shift
	s=${E5_PTR_STEP:-2}
	# the e5 keymap (built from the standard parts + our layers, not from a
	# dump of the live keymap: that came out with xfree86 keycodes)
	sed "s/@STEP@/$s/g" /etc/e5.xkb | xkbcomp -w 0 - "$DISPLAY"
	exec "$@"
fi

case "$1" in
dillo|links) app=$1; shift ;;
*) app=dillo ;;
esac
url=${1:-https://lite.duckduckgo.com}
# overrides for the tests (tests/test-e5-browser.sh)
SYS=${SYS:-/sys}
BINDIR=${BINDIR:-/usr/bin}
LOG=${LOG:-/tmp/browser-x.log}

case "$app" in
dillo) bin=$BINDIR/dillo; set -- -g 320x240+0+0 "$url" ;;
links) bin=$BINDIR/links; set -- -g -mode 320x240 "$url" ;;
esac

if ! command -v xinit > /dev/null; then
	echo "browser: X is not installed: apk add xorg-server xf86-input-evdev xinit font-misc-misc" >&2
	exit 1
fi
if [ ! -x "$bin" ]; then
	echo "browser: $app is not installed: apk add $([ "$app" = links ] && echo links-graphics || echo "dillo font-dejavu")" >&2
	exit 1
fi
# the keypad's event node depends on probe order (the power key may be
# event0): give X a stable name, found by the device name
for d in $SYS/class/input/event*; do
	if [ "$(cat "$d/device/name" 2>/dev/null)" = "Nokia E5 keyboard" ]; then
		ln -sfn "${d##*/}" /dev/input/e5-keypad
	fi
done
# the colour depth follows the kernel's display mode: rgb565 -> 16, else 24
# (also an old fbdev kernel without the e5_display module)
depth=24
[ "$(cat "$SYS/module/e5_display/parameters/rgb565" 2>/dev/null)" = Y ] && depth=16
echo "starting $app in X (about 30 s)..."
exec xinit "$0" --session "$bin" "$@" -- :0 vt2 -nolisten tcp -depth $depth > "$LOG" 2>&1

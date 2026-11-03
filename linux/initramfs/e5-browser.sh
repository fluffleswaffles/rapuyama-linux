#!/bin/sh
# E5: graphical browser in X on vt2, back to the console when it exits.
#   browser [dillo|links] [url]     (default: dillo, lite.duckduckgo.com)
# Needs X (see /etc/X11/xorg.conf.d/10-e5.conf) and dillo or links-graphics.
# Pointer: the right soft key toggles pointer mode (XKB MouseKeys), then the
# navi key moves the pointer and Enter (= navi centre) clicks; off, the keys
# are plain arrows/Enter. fn (bottom left) held = Ctrl (fn+L address, fn+Q
# quit in dillo). E5_PTR_STEP = pointer step in pixels (default 2).
if [ "$1" = "--session" ]; then
	shift
	s=${E5_PTR_STEP:-2}
	# evdev keycodes + us, plus 6 keys: rsk <LWIN> toggles MouseKeys, the
	# arrows and Enter get pointer actions that only act while MouseKeys is
	# on. built from the standard parts, not from a dump of the live keymap:
	# a dump taken at session start came out with xfree86 keycodes
	xkbcomp -w 0 - "$DISPLAY" <<-XKB
	xkb_keymap {
		xkb_keycodes { include "evdev+aliases(qwerty)" };
		xkb_types { include "complete" };
		xkb_compat { include "complete" };
		xkb_symbols {
			include "pc+us+inet(evdev)"
			key <LWIN> { [ Pointer_EnableKeys ] };
			key <LEFT> { symbols[Group1]= [ Left ], actions[Group1]= [ MovePtr(x=-$s,y=+0) ] };
			key <RGHT> { symbols[Group1]= [ Right ], actions[Group1]= [ MovePtr(x=+$s,y=+0) ] };
			key <UP> { symbols[Group1]= [ Up ], actions[Group1]= [ MovePtr(x=+0,y=-$s) ] };
			key <DOWN> { symbols[Group1]= [ Down ], actions[Group1]= [ MovePtr(x=+0,y=+$s) ] };
			key <RTRN> { symbols[Group1]= [ Return ], actions[Group1]= [ PtrBtn(button=1) ] };
		};
		xkb_geometry { include "pc(pc105)" };
	};
	XKB
	exec "$@"
fi

case "$1" in
dillo|links) app=$1; shift ;;
*) app=dillo ;;
esac
url=${1:-https://lite.duckduckgo.com}

case "$app" in
dillo) bin=/usr/bin/dillo; set -- -g 320x240+0+0 "$url" ;;
links) bin=/usr/bin/links; set -- -g -mode 320x240 "$url" ;;
esac

if ! command -v xinit > /dev/null; then
	echo "browser: X is not installed: apk add xorg-server xf86-video-fbdev xf86-input-evdev xinit font-misc-misc" >&2
	exit 1
fi
if [ ! -x "$bin" ]; then
	echo "browser: $app is not installed: apk add $([ "$app" = links ] && echo links-graphics || echo "dillo font-dejavu")" >&2
	exit 1
fi
echo "starting $app in X (about 30 s)..."
exec xinit "$0" --session "$bin" "$@" -- :0 vt2 -nolisten tcp > /tmp/browser-x.log 2>&1

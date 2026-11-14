#!/bin/sh
# E5: weston (drm backend, pixman) with the foot terminal on the KMS display,
# or sway (tiling, several terminals), back to the console when it exits.
#   wayland          start weston + foot
#   wayland sway     start sway (config /etc/xdg/e5/sway.conf, mod = Ctrl)
#   wayland help     short explanation
# weston, sway, foot, seatd and eudev are not preinstalled:
#   apk add weston weston-backend-drm weston-shell-desktop foot font-dejavu seatd eudev
#   apk add sway foot font-dejavu seatd eudev
# sway: wlroots draws with pixman (WLR_RENDERER), 16 bpp kernel = the config
# gets render_bit_depth 6; Ctrl+x (right soft key, x) exits.
# (foot pulls ncurses-terminfo, its TERM=foot entry)
# Keys: the e5 layers as a layout (/etc/xkb/symbols/e5, from tools/e5-xkb.py):
# sym = printed symbols, fn = the Fn layer, one key each; the right soft key
# is Ctrl for one key. Not in weston: pointer mode (Mail), the arrows stay
# arrows. No touch screen, no pointer.
# Exiting foot (Ctrl+D) ends weston and returns to the console.
# Seat: weston runs with LIBSEAT_BACKEND=seatd; a seatd is started for the
# session (as the e6 init does) unless /run/seatd.sock is there, and stopped
# when weston exits. seatd puts the vt in graphics mode with its keyboard
# off, so keys typed in foot don't also reach the console shell on tty1
# (alpine's libseat has no builtin backend; noop leaves the vt as it is).
# An explicit LIBSEAT_BACKEND is kept (no seatd started unless it is seatd).
# Input: libinput only takes devices udev tagged ID_INPUT*, and the rootfs
# has eudev's libudev without its daemon: udevd runs for the session unless
# one is running (stopped afterwards if this started it), the input and drm
# devices get an add event first. weston.ini has require-input=true: no
# keypad seen = weston exits, back to the console.
APK="apk add weston weston-backend-drm weston-shell-desktop foot font-dejavu seatd eudev"
APK_SWAY="apk add sway foot font-dejavu seatd eudev"
case "$1" in
help | -h | --help)
	echo "wayland: weston + the foot terminal on the screen."
	echo "  wayland        start it (Ctrl+D in foot goes back)"
	echo "  wayland sway   tiling, several terminals: Ctrl, then"
	echo "    Enter terminal, q close, arrows focus, e/s/t layouts,"
	echo "    f fullscreen, . , workspaces, x back to the console"
	echo "  wayland help   this text"
	echo "Not preinstalled: $APK."
	echo "For sway: $APK_SWAY."
	echo "Keys: sym = printed symbols, Fn layer for one key, the"
	echo "right soft key is Ctrl for one key. No pointer or touch."
	echo "Runs seatd (screen, keys) and udevd (keypad) meanwhile;"
	echo "LIBSEAT_BACKEND=<name> picks another seat backend."
	exit 0
	;;
"") mode=weston ;;
sway) mode=sway ;;
*)
	echo "usage: wayland [sway|help]" >&2
	exit 1
	;;
esac
# overrides for the tests (tests/test-e5-wayland.sh)
SYS=${SYS:-/sys}
TEMPLATE=${TEMPLATE:-/etc/xdg/weston/weston.ini}
SWAY_TEMPLATE=${SWAY_TEMPLATE:-/etc/xdg/e5/sway.conf}
SEATD_SOCKET=${SEATD_SOCKET:-/run/seatd.sock}

if [ $mode = sway ]; then
	need="sway foot seatd udevd udevadm" hint="sway/foot/seatd/udevd not installed: $APK_SWAY"
else
	need="weston foot seatd udevd udevadm" hint="weston/foot/seatd/udevd not installed: $APK"
fi
for c in $need; do
	if ! command -v $c > /dev/null; then
		echo "wayland: $hint" >&2
		exit 1
	fi
done
XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
export XDG_RUNTIME_DIR
mkdir -p "$XDG_RUNTIME_DIR" /var/cache/fontconfig
chmod 700 "$XDG_RUNTIME_DIR"
# the colour format follows the kernel's display mode: rgb565 -> weston draws
# 565 itself, else its default (also an old fbdev kernel without e5_display)
if [ "$(cat "$SYS/module/e5_display/parameters/rgb565" 2>/dev/null)" = Y ]; then
	gbm=gbm-format=rgb565 depth='output * render_bit_depth 6'
else
	gbm= depth=
fi
# foot's default window (700x500 px) is bigger than the 320x240 screen and
# weston's autolaunch takes no arguments: a wrapper starts it fullscreen
if [ $mode = sway ]; then
	conf="$XDG_RUNTIME_DIR/sway.conf"
	sed "s/^#@DEPTH@\$/$depth/" "$SWAY_TEMPLATE" > "$conf" || exit 1
else
	foot="$XDG_RUNTIME_DIR/foot-fullscreen"
	printf '#!/bin/sh\nexec foot --fullscreen "$@"\n' > "$foot" && chmod 755 "$foot" || exit 1
	conf="$XDG_RUNTIME_DIR/weston.ini"
	sed -e "s/^#@GBM@\$/$gbm/" -e "s|^path=/usr/bin/foot\$|path=$foot|" "$TEMPLATE" > "$conf" || exit 1
fi
export SHELL=${SHELL:-/bin/sh}

# what this script started, stopped again on the way out
seatd_pid= udevd_started=
cleanup() {
	if [ -n "$seatd_pid" ]; then
		kill "$seatd_pid" 2> /dev/null
		wait "$seatd_pid" 2> /dev/null
	fi
	[ -z "$udevd_started" ] || udevadm control --exit 2> /dev/null
}
trap cleanup EXIT
trap 'exit 130' INT TERM

if ! pidof udevd > /dev/null; then
	udevd --daemon || { echo "wayland: udevd did not start" >&2; exit 1; }
	udevd_started=1
fi
udevadm trigger --action=add --subsystem-match=input --subsystem-match=drm
udevadm settle --timeout=10

seat=${LIBSEAT_BACKEND:-seatd}
if [ "$seat" = seatd ] && [ ! -e "$SEATD_SOCKET" ]; then
	seatd > "$XDG_RUNTIME_DIR/seatd.log" 2>&1 &
	seatd_pid=$!
	n=0
	while [ ! -e "$SEATD_SOCKET" ] && [ $n -lt 10 ] && kill -0 "$seatd_pid" 2> /dev/null; do
		n=$((n + 1))
		sleep 1
	done
	if [ ! -e "$SEATD_SOCKET" ]; then
		echo "wayland: seatd did not start ($XDG_RUNTIME_DIR/seatd.log):" >&2
		cat "$XDG_RUNTIME_DIR/seatd.log" >&2
		exit 1
	fi
fi

if [ $mode = sway ]; then
	echo "starting sway (Ctrl+x goes back)..."
	LIBSEAT_BACKEND=$seat WLR_RENDERER=pixman sway -c "$conf"
else
	echo "starting weston + foot..."
	LIBSEAT_BACKEND=$seat weston --backend=drm --config="$conf"
fi
rc=$?
[ $rc = 0 ] || echo "wayland: $mode exited with $rc" >&2
exit $rc

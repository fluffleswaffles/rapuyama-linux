#!/bin/sh
# screen brightness of the E5 (panel DCS backlight, e5-display.c:
# /sys/class/backlight/panel, 0..255), installed as /bin/brightness (initramfs
# list in linux/kernel/build-e5.sh, copied to the sd rootfs by sdroot.sh).
# the choice is kept in /etc/e5-brightness and init.sh puts it back at boot
# (`brightness apply`); the kernel starts at full.
#   brightness             current level
#   brightness <n> | <n>%  set it (n = 0..255 or a percentage) and keep it
#   brightness up | down [step]  +- step (default 32) and keep it
#   brightness apply       set the saved level (boot)
#   brightness reset       forget the choice (next boot = full)
# levels below 4 are refused: the screen goes dark and you would be typing
# blind (echo 255 > /sys/class/backlight/panel/brightness to get it back)
B=/sys/class/backlight/panel
CONF=/etc/e5-brightness
MIN=4

[ -d "$B" ] || { echo "brightness: no $B (kernel without the panel backlight)" >&2; exit 1; }
MAX=$(cat "$B/max_brightness")
cur() { cat "$B/brightness"; }

set_level() {
	case "$1" in
	*%)	n=${1%\%}; case "$n" in ''|*[!0-9]*) n=x ;; *) n=$((n * MAX / 100)) ;; esac ;;
	*)	n=$1 ;;
	esac
	case "$n" in ''|*[!0-9-]*|*?-*) echo "brightness: '$1' is not a level" >&2; return 1 ;; esac
	[ "$n" -gt "$MAX" ] && n=$MAX
	[ "$n" -lt "$MIN" ] && { echo "brightness: $n is too dark (min $MIN)" >&2; return 1; }
	echo "$n" > "$B/brightness" || return 1
	[ -n "$2" ] || echo "$n" > "$CONF"
	echo "$n/$MAX ($((n * 100 / MAX))%)"
}

case "${1:-}" in
	"")	echo "$(cur)/$MAX ($(($(cur) * 100 / MAX))%)" ;;
	up)	set_level $(($(cur) + ${2:-32})) ;;
	down)	n=$(($(cur) - ${2:-32})); [ "$n" -lt "$MIN" ] && n=$MIN; set_level $n ;;
	apply)	[ -r "$CONF" ] || exit 0; set_level "$(cat "$CONF")" nosave ;;
	reset)	rm -f "$CONF"; echo "saved level removed, full brightness at the next boot" ;;
	[0-9]*)	set_level "$1" ;;
	*)	sed -n '2,/^B=/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 1 ;;
esac

#!/bin/sh
# screen-off delay of the E5 console, installed as /bin/blank (initramfs list
# in linux/kernel/build-e5.sh, copied to the sd rootfs by sdroot.sh). the
# console blank turns the panel and the keyboard light off, any key wakes
# them. the choice is kept in /etc/e5-blank and init.sh puts it back at boot
# (`blank apply`); without it the dtb default is used (consoleblank=, 1 min).
#   blank              current delay
#   blank <minutes>    blank after 1..60 minutes without a key press, keep it
#   blank off | 0      never blank the screen, keep it
#   blank apply        set the saved delay (boot)
#   blank reset        forget the choice (next boot = dtb default)
# mechanism: the console escape ESC[9;N] (N minutes, 0 = never, the kernel
# caps it at 60); the current value is the consoleblank parameter (seconds)
P=/sys/module/kernel/parameters/consoleblank
TTY=/dev/tty1
CONF=/etc/e5-blank

cur() {
	s=$(cat "$P" 2>/dev/null) || { echo "blank: can't read $P" >&2; return 1; }
	if [ "$s" -eq 0 ]; then echo "off (the screen stays on)"
	elif [ $((s % 60)) -eq 0 ]; then echo "$((s / 60)) min"
	else echo "$s s"
	fi
}

set_min() {
	case "$1" in
	off)	n=0 ;;
	''|*[!0-9]*) echo "blank: '$1' is not a number of minutes (1..60) or off" >&2; return 1 ;;
	*)	n=$1
		# leading zeros would make printf read octal (08 = error)
		while case "$n" in 0?*) true ;; *) false ;; esac; do n=${n#0}; done
		[ "$n" -ge 0 ] && [ "$n" -le 60 ] || { echo "blank: $1 is out of range (1..60 minutes, or off)" >&2; return 1; }
		;;
	esac
	printf '\033[9;%d]' "$n" > "$TTY" || return 1
	if [ -z "$2" ]; then
		if [ "$n" -eq 0 ]; then echo off > "$CONF"; else echo "$n" > "$CONF"; fi
	fi
	cur
}

case "${1:-}" in
	help)	echo "blank: when the screen turns off (any key wakes it)."
		echo "  blank              current delay"
		echo "  blank <minutes>    1..60, kept across reboots"
		echo "  blank off          never turn the screen off"
		echo "  blank reset        forget the saved delay (default after reboot)." ;;
	"")	cur ;;
	apply)	[ -r "$CONF" ] || exit 0; set_min "$(cat "$CONF")" nosave ;;
	reset)	rm -f "$CONF"; echo "saved delay removed, the dtb default applies at the next boot" ;;
	off|[0-9]*) set_min "$1" ;;
	*)	sed -n '2,/^P=/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 1 ;;
esac

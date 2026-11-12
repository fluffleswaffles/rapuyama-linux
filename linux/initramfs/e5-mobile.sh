#!/bin/sh
# 3G mobile data on the E5 (advanced setup), installed as /usr/bin/mobile on
# the sd rootfs (tools/sdroot.sh e5). Only on a 3GLinuxE5 boot: the boot step
# (/usr/libexec/e5/mobile-boot) starts the modem and runs `mobile on` itself.
#   mobile        status: modem, network, data, APN, bytes, battery
#   mobile on     data on: PDP context (e5gprs), default route via gprs0
#                 (metric 600: Wi-Fi and the PC link win while up), DNS when
#                 Wi-Fi has no default route
#   mobile off    data off, routes and resolv.conf back; the modem keeps running
#   mobile setup  asks for the IMEI (*#06# in Symbian, checked: 15 digits +
#                 check digit) and the APN, saves them on the card
#                 (E:\linux\e5-imei, E:\linux\e5-apn); Enter keeps a saved one
#   mobile apn <name>   changes only the APN
# Nothing is read from Symbian. /etc/e5-apn wins over the card's APN.
# A stopped modem cannot be restarted in the same boot: reboot.
# overrides (tests/test-e5-mobile.sh): LIBEXEC RUN ETC SYS SLEEP CARD_DIR
LIBEXEC=${LIBEXEC:-/usr/libexec/e5}
RUN=${RUN:-/run/e5-mobile}
ETC=${ETC:-/etc}
SYS=${SYS:-/sys}
SLEEP=${SLEEP:-sleep}

# printf, not echo: some shells' echo turns the \e of E:\linux\e5-... into ESC
say() { printf '%s\n' "$*"; }
alive() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }
state() { cat "$RUN/state" 2>/dev/null; }
# first line, without CR / spaces (files written on Symbian or Windows)
apn() {
	for f in "$ETC/e5-apn" "$RUN/e5-apn"; do
		[ -s "$f" ] || continue
		a=$(head -n 1 "$f" | tr -d '\r \t')
		[ -n "$a" ] && { say "$a"; return; }
	done
}
data_on() { alive "$RUN/e5gprs.pid"; }
# no PDP context left in the modem (GPDS status "1a 00 00"): a new
# activation started while the old one was still going down never came up
# (phone, m2 2026-11-12: off, then on 1 s later). up to 20 tries, 1 s apart
gpds_idle_wait() {
	alive "$RUN/e5rt.pid" || return 0	# no modem, no context to wait for
	n=0
	while [ $n -lt 20 ]; do
		case "$("$LIBEXEC/pnping" --send 31 19 2>&1)" in *" 1a 00 00 "*) return 0 ;; esac
		n=$((n + 1))
		$SLEEP 1
	done
	return 1
}
registered_now() {
	case "$("$LIBEXEC/pnping" --send 0a e000 2>&1)" in
	*"42 e1 00 "*" 00 24 00 "*|*"42 e1 00 "*" 00 24 01 "*) return 0 ;;
	esac
	return 1
}
# put the saved resolv.conf back, but only over the line `mobile on` wrote:
# Wi-Fi's dhcp may have written its own since then (that one stays)
restore_dns() {
	[ -f "$RUN/resolv.conf.saved" ] || return 0
	if [ -f "$RUN/resolv.conf.mobile" ] && cmp -s "$RUN/resolv.conf.mobile" "$ETC/resolv.conf"; then
		cat "$RUN/resolv.conf.saved" > "$ETC/resolv.conf"
	fi
	rm -f "$RUN/resolv.conf.saved" "$RUN/resolv.conf.mobile"
}
field() { sed -n "s/^$1 //p" "$RUN/e5gprs.out" | head -n 1; }
need_root() { [ "$(id -u)" = 0 ] || { say "mobile: run it as root" >&2; exit 1; }; }
modem_line() {
	case "$(state)" in
	'') say "off (not a 3G boot)" ;;
	no-imei) say "not started: no IMEI (run mobile setup)" ;;
	bad-imei) say "not started: the saved IMEI is not 15 digits (run mobile setup, then start 3GLinuxE5 again)" ;;
	too-late) say "not started: too late after boot, reboot with 3GLinuxE5" ;;
	unavailable) say "not available (see $RUN/boot.log)" ;;
	*) if alive "$RUN/e5rt.pid"; then echo running; else say "stopped (reboot to use mobile data)"; fi ;;
	esac
}

status() {
	m=$(modem_line)
	say "modem: $m"
	case "$m" in
	running)
		case "$(state)" in
		registered) say "network: registered" ;;
		not-registered) say "network: not registered (SIM, coverage?)" ;;
		*) say "network: searching" ;;
		esac ;;
	stopped*)
		say "last lines of $RUN/e5rt.log:"
		tail -n 5 "$RUN/e5rt.log" 2>/dev/null ;;
	esac
	if data_on; then
		ifn=$(field IFNAME)
		say "data: on $(field IP)"
		say "$ifn: rx $(cat "$SYS/class/net/$ifn/statistics/rx_bytes" 2>/dev/null) tx $(cat "$SYS/class/net/$ifn/statistics/tx_bytes" 2>/dev/null) bytes"
	else
		say "data: off"
	fi
	a=$(apn)
	say "apn: ${a:-none (run mobile setup)}"
	for c in "$SYS"/class/power_supply/*/capacity; do
		[ -r "$c" ] && { say "battery: $(cat "$c")%"; break; }
	done
}

on() {
	need_root
	m=$(modem_line)
	[ "$m" = running ] || { say "mobile: modem $m" >&2; exit 1; }
	# the boot step gave up after 20 tries: ask the network once more
	if [ "$(state)" = not-registered ] && registered_now; then
		echo registered > "$RUN/state"
	fi
	[ "$(state)" = registered ] || { say "mobile: not registered (SIM, coverage?), try again later" >&2; exit 1; }
	if data_on; then say "mobile: data already on ($(field IP))"; exit 0; fi
	a=$(apn)
	[ -n "$a" ] || { say "mobile: no APN (run mobile setup)" >&2; exit 1; }
	gpds_idle_wait || say "mobile: the modem still reports a data context, trying anyway"
	# 20 s after the attach: an activate right after it was never sent on the air
	E5GPRS_DELAY=20 "$LIBEXEC/e5gprs" "$a" > "$RUN/e5gprs.out" 2>&1 &
	echo $! > "$RUN/e5gprs.pid"
	say "mobile: activating data (apn $a)..."
	n=0
	until grep -q '^CONTEXT ACTIVE' "$RUN/e5gprs.out"; do
		n=$((n + 1))
		if ! data_on || [ $n -gt 180 ]; then
			kill "$(cat "$RUN/e5gprs.pid")" 2>/dev/null
			rm -f "$RUN/e5gprs.pid"
			say "mobile: no data context, last lines of $RUN/e5gprs.out:" >&2
			tail -n 3 "$RUN/e5gprs.out" >&2
			exit 1
		fi
		$SLEEP 1
	done
	ifn=$(field IFNAME) ip4=$(field IP) dns=$(field DNS1)
	ip addr add "$ip4/32" dev "$ifn"
	ip link set "$ifn" up
	ip route add default dev "$ifn" metric 600
	if [ -n "$dns" ] && ! ip route show default | grep -q 'dev wlan0'; then
		[ -f "$RUN/resolv.conf.saved" ] || cp "$ETC/resolv.conf" "$RUN/resolv.conf.saved" 2>/dev/null || : > "$RUN/resolv.conf.saved"
		say "nameserver $dns" > "$RUN/resolv.conf.mobile"
		cat "$RUN/resolv.conf.mobile" > "$ETC/resolv.conf"
	fi
	say "mobile: data on $ip4 ($ifn)"
}

off() {
	need_root
	if ! data_on; then
		rm -f "$RUN/e5gprs.pid"
		restore_dns	# e5gprs may have died with data on
		say "mobile: data already off"
		exit 0
	fi
	ifn=$(field IFNAME)
	kill "$(cat "$RUN/e5gprs.pid")"
	n=0
	while data_on && [ $n -lt 10 ]; do $SLEEP 1; n=$((n + 1)); done
	rm -f "$RUN/e5gprs.pid"
	gpds_idle_wait || say "mobile: the modem still reports a data context"
	ip route del default dev "$ifn" metric 600 2>/dev/null
	ip addr flush dev "$ifn" 2>/dev/null
	restore_dns
	say "mobile: data off"
}

# the card's p1 (Symbian's E:), mounted read-write while setup writes to it
card_open() {
	if [ -n "${CARD_DIR:-}" ]; then C=$CARD_DIR; mkdir -p "$C/linux"; return 0; fi
	r=$(sed -n 's/.*e6\.root=\([^ ]*\).*/\1/p' /proc/cmdline)
	dev=${r%p[0-9]*}p1; [ -n "$r" ] || dev=/dev/mmcblk0p1
	C=/tmp/e5-card
	mkdir -p "$C"
	mount -t vfat "$dev" "$C" || { say "mobile: can't mount the memory card ($dev)" >&2; exit 1; }
	mkdir -p "$C/linux"
}
card_close() { sync; [ -n "${CARD_DIR:-}" ] || umount "$C"; }
# 15 digits whose Luhn check digit is right
imei_ok() {
	printf '%s' "$1" | awk '{ if (length($0) != 15 || $0 !~ /^[0-9]+$/) exit 1
		s = 0; for (i = 1; i <= 15; i++) { d = substr($0, i, 1) + 0
			if (i % 2 == 0) { d *= 2; if (d > 9) d -= 9 } s += d }
		exit (s % 10 != 0) }'
}
clean_apn() { printf '%s' "$1" | tr -d '\r \t'; }
save_apn() {
	printf '%s' "$1" > "$C/linux/e5-apn"
	[ -d "$RUN" ] && printf '%s' "$1" > "$RUN/e5-apn"
	# /etc/e5-apn overrides the card: keep it in step
	[ -f "$ETC/e5-apn" ] && printf '%s' "$1" > "$ETC/e5-apn"
	return 0
}

setup() {
	need_root
	card_open
	old=$(tr -cd 0-9 < "$C/linux/e5-imei" 2>/dev/null)
	imei_ok "$old" || old=
	if [ -n "$old" ]; then
		printf 'IMEI (Enter keeps the saved one): '
	else
		printf 'IMEI (dial *#06# in Symbian, 15 digits): '
	fi
	read -r in || in=
	imei=$(printf '%s' "$in" | tr -cd 0-9)
	[ -z "$imei" ] && imei=$old
	if [ -z "$imei" ]; then
		card_close; say "mobile: no IMEI given, nothing saved" >&2; exit 1
	fi
	if [ ${#imei} != 15 ]; then
		card_close; say "mobile: an IMEI has 15 digits (got ${#imei}), nothing saved" >&2; exit 1
	fi
	if ! imei_ok "$imei"; then
		card_close; say "mobile: the IMEI's check digit is wrong (typo?), nothing saved" >&2; exit 1
	fi
	olda=$(clean_apn "$(head -n 1 "$C/linux/e5-apn" 2>/dev/null)")
	if [ -n "$olda" ]; then
		printf 'APN (Enter keeps %s): ' "$olda"
	else
		printf 'APN (from your operator, e.g. internet): '
	fi
	read -r in || in=
	a=$(clean_apn "$in")
	[ -z "$a" ] && a=$olda
	if [ -z "$a" ]; then
		card_close; say "mobile: no APN given, nothing saved" >&2; exit 1
	fi
	printf '%s' "$imei" > "$C/linux/e5-imei"
	save_apn "$a"
	card_close
	say "saved to E:\\linux\\e5-imei and E:\\linux\\e5-apn (apn $a)"
	case "$(state)" in
	'') say "start Linux with 3GLinuxE5 to use mobile data" ;;
	registered) say "mobile off, then mobile on, to use the new APN" ;;
	*) say "reboot and start Linux with 3GLinuxE5 to use mobile data" ;;
	esac
}

apn_cmd() {
	need_root
	a=$(clean_apn "$1")
	[ -n "$a" ] || { say "usage: mobile apn <name>" >&2; exit 1; }
	card_open
	save_apn "$a"
	card_close
	say "apn: $a (saved to E:\\linux\\e5-apn)"
}

case "$1" in
'') status ;;
on) on ;;
off) off ;;
setup) setup ;;
apn) apn_cmd "$2" ;;
*) say "usage: mobile [on|off|setup|apn <name>]" >&2; exit 1 ;;
esac

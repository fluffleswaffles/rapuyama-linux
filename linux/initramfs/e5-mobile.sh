#!/bin/sh
# 3G mobile data on the E5 (advanced setup), installed as /usr/bin/mobile on
# the sd rootfs (tools/sdroot.sh e5). Only on a 3GLinuxE5 boot: the boot step
# (/usr/libexec/e5/mobile-boot) starts the modem and runs `mobile on` itself.
#   mobile        status: modem, network, data, APN, bytes, battery
#   mobile on     data on: PDP context (e5gprs), default route via gprs0
#                 (metric 600: Wi-Fi and the PC link win while up), DNS when
#                 Wi-Fi has no default route
#   mobile off    data off, routes and resolv.conf back; the modem keeps running
# Needs E:\linux\e5-imei and E:\linux\e5-apn, written by hand (see
# /usr/share/doc/e5/mobile.txt); /etc/e5-apn wins over the card's.
# A stopped modem cannot be restarted in the same boot: reboot.
# overrides (tests/test-e5-mobile.sh): LIBEXEC RUN ETC SYS SLEEP
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
field() { sed -n "s/^$1 //p" "$RUN/e5gprs.out" | head -n 1; }
need_root() { [ "$(id -u)" = 0 ] || { say "mobile: run it as root" >&2; exit 1; }; }
modem_line() {
	case "$(state)" in
	'') say "off (not a 3G boot)" ;;
	no-imei) say "not started: no IMEI (write it to E:\\linux\\e5-imei)" ;;
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
	say "apn: ${a:-none (write it to E:\\linux\\e5-apn)}"
	for c in "$SYS"/class/power_supply/*/capacity; do
		[ -r "$c" ] && { say "battery: $(cat "$c")%"; break; }
	done
}

on() {
	need_root
	m=$(modem_line)
	[ "$m" = running ] || { say "mobile: modem $m" >&2; exit 1; }
	[ "$(state)" = registered ] || { say "mobile: not registered (SIM, coverage?), try again later" >&2; exit 1; }
	if data_on; then say "mobile: data already on ($(field IP))"; exit 0; fi
	a=$(apn)
	[ -n "$a" ] || { say "mobile: no APN (write it to E:\\linux\\e5-apn), see /usr/share/doc/e5/mobile.txt" >&2; exit 1; }
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
		say "nameserver $dns" > "$ETC/resolv.conf"
	fi
	say "mobile: data on $ip4 ($ifn)"
}

off() {
	need_root
	if ! data_on; then
		rm -f "$RUN/e5gprs.pid"
		say "mobile: data already off"
		exit 0
	fi
	ifn=$(field IFNAME)
	kill "$(cat "$RUN/e5gprs.pid")"
	n=0
	while data_on && [ $n -lt 10 ]; do $SLEEP 1; n=$((n + 1)); done
	rm -f "$RUN/e5gprs.pid"
	ip route del default dev "$ifn" metric 600 2>/dev/null
	ip addr flush dev "$ifn" 2>/dev/null
	if [ -f "$RUN/resolv.conf.saved" ]; then
		cat "$RUN/resolv.conf.saved" > "$ETC/resolv.conf"
		rm -f "$RUN/resolv.conf.saved"
	fi
	say "mobile: data off"
}

case "$1" in
'') status ;;
on) on ;;
off) off ;;
*) say "usage: mobile [on|off]" >&2; exit 1 ;;
esac

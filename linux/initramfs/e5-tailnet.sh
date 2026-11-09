#!/bin/sh
# Tailscale on the E5 (advanced setup), installed as /usr/bin/tailnet on the
# sd rootfs (tools/sdroot.sh e5). Off at boot, on when you ask. Real tunnel
# (tailscale0, kernel TUN), so every program reaches the tailnet. Needs
# alpine's tailscale: apk add tailscale
#   tailnet         status: on/off, this phone's tailnet address, the devices
#   tailnet on      start tailscaled + connect (the first time: a login link,
#                   open it on another device and approve this phone)
#   tailnet off     disconnect and stop tailscaled (frees its RAM)
# The login stays in /var/lib/tailscale on the card. Reach devices by their
# 100.x address (tailnet shows them); Tailscale DNS is off, resolv.conf stays
# as wifi set it.
pidf=/run/e5-tailscaled.pid
sock=/run/tailscale/tailscaled.sock
log=/tmp/tailscaled.log

running() {
	[ -f "$pidf" ] && kill -0 "$(cat "$pidf")" 2>/dev/null
}

status() {
	if running; then
		echo "tailnet: on (tailscaled pid $(cat "$pidf"))"
		tailscale status 2>&1 | head -20
	else
		echo "tailnet: off (tailnet on)"
	fi
}

case "$1" in
'')
	status
	;;
on)
	[ "$(id -u)" = 0 ] || { echo "tailnet: run it as root" >&2; exit 1; }
	[ -x /usr/sbin/tailscaled ] || { echo "missing: tailscale (wifi on, then: apk add tailscale)" >&2; exit 1; }
	[ -c /dev/net/tun ] || { echo "tailnet: no /dev/net/tun (kernel without TUN?)" >&2; exit 1; }
	if ! running; then
		mkdir -p /var/lib/tailscale /run/tailscale
		/usr/sbin/tailscaled --statedir=/var/lib/tailscale --socket="$sock" \
			--tun=tailscale0 > "$log" 2>&1 &
		echo $! > "$pidf"
		echo "starting tailscaled (log: $log)..."
		n=0
		while [ ! -S "$sock" ] && [ "$n" -lt 60 ]; do
			running || { echo "tailscaled stopped, last lines of $log:" >&2; tail -5 "$log" >&2; rm -f "$pidf"; exit 1; }
			sleep 1
			n=$((n + 1))
		done
		[ -S "$sock" ] || { echo "tailscaled did not start in 60 s ($log)" >&2; exit 1; }
	fi
	# the first time this prints a login link and waits for the approval
	tailscale --socket="$sock" up --accept-dns=false || {
		echo "not connected: the clock must be right (wifi sets it), see $log" >&2
		exit 1
	}
	echo "tailnet: on, this phone is $(tailscale --socket="$sock" ip -4 2>/dev/null)"
	;;
off)
	[ "$(id -u)" = 0 ] || { echo "tailnet: run it as root" >&2; exit 1; }
	if running; then
		tailscale --socket="$sock" down 2>/dev/null
		p=$(cat "$pidf")
		kill "$p"
		n=0
		while kill -0 "$p" 2>/dev/null && [ "$n" -lt 10 ]; do
			sleep 1
			n=$((n + 1))
		done
		kill -9 "$p" 2>/dev/null
	fi
	rm -f "$pidf"
	echo "tailnet: off"
	;;
*)
	echo "usage: tailnet [on|off]" >&2
	exit 1
	;;
esac

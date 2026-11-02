#!/bin/sh
# wlan setup on the E5, installed as /usr/bin/wifi on the sd rootfs
# (tools/sdroot.sh e5; needs alpine's wpa_supplicant + iw). init.sh
# wlan_net connects the saved network at boot; this saves one and
# connects at once (same steps as wlan_net: own mac, power save off).
#   wifi                      status
#   wifi '<name>' '<password>'  save the network (WPA/WPA2) + connect now
#   wifi on                   connect the saved network now
#   wifi off                  disconnect (it connects again at boot)
#   wifi forget               delete the saved network
conf=/etc/wpa_supplicant/wpa_supplicant.conf

stop() {
	pkill -f 'udhcpc -i wlan0' 2>/dev/null
	pkill -f 'wpa_supplicant .*-i wlan0' 2>/dev/null
	sleep 1
	ip addr flush dev wlan0 2>/dev/null
}

connect() {
	[ -e /sys/class/net/wlan0 ] || {
		echo "no wlan0: run LinuxE5 once in Symbian with the card in" >&2
		exit 1
	}
	stop
	if ! ip link show wlan0 | grep -q '<.*UP'; then
		# the rom's default nvs mac is the same on every E5; LinuxE5
		# saves this phone's own one, set while wlan0 is still down
		[ -r /etc/e5-wlan-mac ] && ip link set wlan0 address "$(cat /etc/e5-wlan-mac)"
		ip link set wlan0 up || exit 1
	fi
	# wl1251 power save misses beacons (see init.sh wlan_net)
	iw dev wlan0 set power_save off 2>/dev/null
	out=$(wpa_supplicant -B -i wlan0 -c "$conf" 2>&1) || { echo "$out" >&2; exit 1; }
	udhcpc -i wlan0 -b -S -t 5 -T 3 -A 10 -s /usr/share/udhcpc/default.script > /dev/null 2>&1
	echo "connecting..."
	n=0
	while [ "$n" -lt 30 ]; do
		ip=$(ip -4 -o addr show wlan0 | awk '{print $4; exit}')
		[ -n "$ip" ] && break
		n=$((n + 1))
		sleep 1
	done
	[ -n "$ip" ] || { echo "no address after 30 s (name/password? \`wifi\` shows the link)" >&2; exit 1; }
	echo "connected, $ip"
	# no rtc: the clock is 1970 until ntp, and apk (tls) fails before that
	if [ "$(date +%Y)" -lt 2025 ]; then
		echo "setting the clock..."
		ntpd -n -q -p pool.ntp.org > /dev/null 2>&1 && date || echo "ntp failed, try again later: ntpd -n -q -p pool.ntp.org" >&2
	fi
}

case "$1" in
"")
	[ -e /sys/class/net/wlan0 ] || { echo "no wlan0"; exit 1; }
	link=$(iw dev wlan0 link 2>/dev/null)
	case "$link" in
	Connected*)
		echo "$link" | sed -n 's/^[[:space:]]*SSID: /network: /p; s/^[[:space:]]*signal: /signal: /p'
		ip -4 -o addr show wlan0 | awk '{print "address: " $4}'
		;;
	*) echo "not connected" ;;
	esac
	[ -f "$conf" ] && sed -n 's/^[[:space:]]*ssid="\(.*\)"/saved: \1/p' "$conf" || echo "saved: none (wifi 'name' 'password')"
	;;
on)
	[ -f "$conf" ] || { echo "no saved network: wifi '<name>' '<password>'" >&2; exit 1; }
	connect
	;;
off)
	stop
	ip link set wlan0 down 2>/dev/null
	echo "off (the saved network connects again at the next boot)"
	;;
forget)
	stop
	rm -f "$conf"
	echo "saved network deleted"
	;;
*)
	[ -n "$2" ] || { echo "usage: wifi '<name>' '<password>' | on | off | forget" >&2; exit 1; }
	mkdir -p "${conf%/*}"
	# wpa_passphrase also writes the password in clear as a comment: drop it
	out=$(wpa_passphrase "$1" "$2") || { echo "$out" >&2; exit 1; }
	(umask 077; echo "$out" | sed '/^[[:space:]]*#psk=/d' > "$conf.tmp") && mv "$conf.tmp" "$conf" || exit 1
	connect
	;;
esac

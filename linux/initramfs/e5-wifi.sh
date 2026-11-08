#!/bin/sh
# wlan setup on the E5, installed as /usr/bin/wifi on the sd rootfs
# (tools/sdroot.sh e5) and in the initramfs (build-e5.sh, simple mode);
# needs alpine's wpa_supplicant + iw. init.sh
# wlan_net connects the saved network at boot; this saves one and
# connects at once (same steps as wlan_net: own mac, power save off).
#   wifi                      status
#   wifi scan                 list the networks in range, numbered
#   wifi '<name>' '<password>'  save the network (WPA/WPA2) + connect now
#   wifi <number> '<password>'  the same for network <number> of the last
#                             scan (exact name, also ones you can't type)
#   wifi on                   connect the saved network now
#   wifi off                  disconnect (it connects again at boot)
#   wifi forget               delete the saved network
conf=/etc/wpa_supplicant/wpa_supplicant.conf
scanf=/tmp/e5-wifi-scan

stop() {
	pkill -f 'udhcpc -i wlan0' 2>/dev/null
	pkill -f 'wpa_supplicant .*-i wlan0' 2>/dev/null
	sleep 1
	ip addr flush dev wlan0 2>/dev/null
}

up() {
	[ -e /sys/class/net/wlan0 ] || {
		echo "no wlan0: run LinuxE5 once in Symbian with the card in" >&2
		exit 1
	}
	if ! ip link show wlan0 | grep -q '<.*UP'; then
		# the rom's default nvs mac is the same on every E5; LinuxE5
		# saves this phone's own one, set while wlan0 is still down
		[ -r /etc/e5-wlan-mac ] && ip link set wlan0 address "$(cat /etc/e5-wlan-mac)"
		ip link set wlan0 up || exit 1
	fi
	# wl1251 power save misses beacons (see init.sh wlan_net)
	iw dev wlan0 set power_save off 2>/dev/null
}

connect() {
	[ -e /sys/class/net/wlan0 ] || up
	stop
	up
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
		# at most 15 s: ntpd -q waits forever when no server answers
		ntpd -n -q -p pool.ntp.org > /dev/null 2>&1 &
		ntp=$!
		( sleep 15; kill "$ntp" 2> /dev/null ) &
		if wait "$ntp"; then date; else echo "ntp failed, try again later: ntpd -n -q -p pool.ntp.org" >&2; fi
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
	if [ -f "$conf" ]; then
		name=$(sed -n 's/^# e5 name: //p' "$conf")
		[ -n "$name" ] && echo "saved: $name" || sed -n 's/^[[:space:]]*ssid="\(.*\)"/saved: \1/p' "$conf"
	else
		echo "saved: none (wifi scan, then wifi <number> 'password')"
	fi
	;;
scan)
	up
	# iw prints names byte-escaped (\xNN): keep that for display, and the
	# exact bytes as hex for wpa_supplicant (ssid=<hex>)
	iw dev wlan0 scan 2> /dev/null | awk '
		BEGIN { h = "0123456789abcdef"; for (i = 32; i < 127; i++) ord[sprintf("%c", i)] = i }
		function hex(s,   o, i, c) {
			o = ""
			for (i = 1; i <= length(s); i++) {
				c = substr(s, i, 1)
				if (c == "\\" && substr(s, i + 1, 1) == "x") { o = o tolower(substr(s, i + 2, 2)); i += 3 }
				else if (c == "\\" && substr(s, i + 1, 1) == "\\") { o = o "5c"; i++ }
				else o = o substr(h, int(ord[c] / 16) + 1, 1) substr(h, ord[c] % 16 + 1, 1)
			}
			return o
		}
		/^BSS / { sig = "" }
		/^[[:space:]]*signal:/ { sig = $2 }
		/^[[:space:]]*SSID: / { n = $0; sub(/^[[:space:]]*SSID: /, "", n)
			if (n != "" && !(n in seen)) { seen[n] = 1; print sig "\t" hex(n) "\t" n } }
	' | sort -t "$(printf '\t')" -k1,1nr | awk -F '\t' '{ print NR "\t" $2 "\t" $3 "\t" $1 }' > "$scanf"
	[ -s "$scanf" ] || { echo "no networks found (try again)"; exit 1; }
	awk -F '\t' '{ printf "%2d  %s dBm  %s\n", $1, $4, $3 }' "$scanf"
	echo "connect: wifi <number> 'password'"
	;;
on)
	[ -f "$conf" ] || { echo "no saved network: wifi '<name>' '<password>'" >&2; exit 1; }
	connect
	;;
off)
	stop
	ip link set wlan0 down 2>/dev/null
	echo "off (wifi on: connect again)"
	;;
forget)
	stop
	rm -f "$conf"
	echo "saved network deleted"
	;;
*)
	[ -n "$2" ] || { echo "usage: wifi scan | wifi <number> '<password>' | wifi '<name>' '<password>' | on | off | forget" >&2; exit 1; }
	mkdir -p "${conf%/*}"
	ssid="$1" hex= name="$1"
	case "$1" in
	*[!0-9]*) ;;
	*)	# a number from the last `wifi scan`: the exact name, as bytes
		line=$(awk -F '\t' -v n="$1" '$1 == n' "$scanf" 2> /dev/null)
		if [ -n "$line" ]; then
			hex=$(echo "$line" | cut -f2)
			name=$(echo "$line" | cut -f3)
			oct=$(echo "$hex" | awk '{ h = "0123456789abcdef"; for (i = 1; i < length($0); i += 2)
				printf "\\%03o", (index(h, substr($0, i, 1)) - 1) * 16 + index(h, substr($0, i + 1, 1)) - 1 }')
			ssid=$(printf "$oct")
		fi
		;;
	esac
	# wpa_passphrase also writes the password in clear as a comment: drop it
	out=$(wpa_passphrase "$ssid" "$2") || { echo "$out" >&2; exit 1; }
	(umask 077
	 { echo "# e5 name: $name"
	   if [ -n "$hex" ]; then echo "$out" | sed -e '/^[[:space:]]*#psk=/d' -e "s/^\([[:space:]]*\)ssid=.*/\1ssid=$hex/"
	   else echo "$out" | sed '/^[[:space:]]*#psk=/d'; fi
	 } > "$conf.tmp") && mv "$conf.tmp" "$conf" || exit 1
	connect
	;;
esac

#!/bin/sh
# udhcpc script for the E5 usb ethernet (usb0, g_cdc). only busybox
# ifconfig/route, no iproute2. the pc side is a NetworkManager "shared"
# connection (dhcp + dns + nat), see the E5 README
case "$1" in
deconfig)
	ifconfig "$interface" 0.0.0.0 up
	;;
bound|renew)
	ifconfig "$interface" "$ip" netmask "${subnet:-255.255.255.0}" up
	while route del default dev "$interface" 2>/dev/null; do :; done
	for r in $router; do
		route add default gw "$r" dev "$interface" && break
	done
	if [ -n "$dns" ]; then
		: > /etc/resolv.conf
		for d in $dns; do echo "nameserver $d" >> /etc/resolv.conf; done
	fi
	echo "e5: usb0 $1 $ip via ${router:-none} dns ${dns:-none}" > /dev/kmsg
	;;
esac
exit 0

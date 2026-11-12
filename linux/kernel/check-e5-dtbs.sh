#!/bin/sh
# checks a build-e5.sh output dir: e5.dtb (wifi-only boots, no e5.mobile) and
# e5-3g.dtb (= e5.dtb + " e5.mobile=1" on the bootargs, for 3GLinuxE5), both
# with the MDI node, and the sim bridge module next to them
# usage: ./check-e5-dtbs.sh <KOUT>
K="${1:?usage: $0 <KOUT>}"
ok=1
bad() { echo "FAIL: $*"; ok=0; }
for d in e5.dtb e5-3g.dtb; do
	[ -f "$K/$d" ] || { bad "$d missing"; continue; }
	fdtget -l "$K/$d" /soc | grep -qx 'mdi@30003ce0' || bad "$d has no mdi@30003ce0"
done
if [ -f "$K/e5.dtb" ]; then
	case "$(fdtget -t s "$K/e5.dtb" /chosen bootargs)" in
	*e5.mobile*) bad "e5.dtb bootargs contain e5.mobile" ;;
	esac
fi
if [ -f "$K/e5-3g.dtb" ]; then
	case "$(fdtget -t s "$K/e5-3g.dtb" /chosen bootargs)" in
	*" e5.mobile=1") ;;
	*) bad "e5-3g.dtb bootargs do not end with ' e5.mobile=1'" ;;
	esac
	[ "$(fdtget -t s "$K/e5-3g.dtb" /chosen bootargs)" = "$(fdtget -t s "$K/e5.dtb" /chosen bootargs) e5.mobile=1" ] ||
		bad "e5-3g.dtb bootargs != e5.dtb bootargs + ' e5.mobile=1'"
fi
[ -f "$K/drivers/misc/rapuyama_sim.ko" ] || bad "drivers/misc/rapuyama_sim.ko missing"
[ $ok = 1 ] && echo "PASS: $K"
[ $ok = 1 ]

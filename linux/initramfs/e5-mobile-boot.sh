#!/bin/sh
# E5 3G boot step (advanced setup), installed as /usr/libexec/e5/mobile-boot;
# init.sh starts it in the background on a 3GLinuxE5 boot (e5.mobile=1 on the
# cmdline). Starts the modem runtime once per boot, waits for the network,
# then `mobile on`. Never on a LinuxE5 boot.
#   1. same-boot ISASW images (e5mem debugfs) -> /run/e5img
#   2. sim bridge module, phonet0 up
#   3. e5rt, lean: only the four E5RT_* settings below (debug traces stall the
#      DSP link), log /run/e5-mobile/e5rt.log (kept under 256 KB)
#   4. registration (pnping, 20 x 5 s), then `mobile on`
# e5rt must start within ~2 min of boot (later: the firmware's dealloc block /
# reset 9) and only once per boot. /run/e5-mobile/state = starting,
# registered, not-registered, no-imei, too-late, unavailable, stopped.
# The IMEI is read by e5rt from its file, never printed here.
# overrides (tests/test-e5-mobile.sh): LIBEXEC RUN CMDLINE UPTIME SLEEP DBG IMG MOBILE KMSG
LIBEXEC=${LIBEXEC:-/usr/libexec/e5}
RUN=${RUN:-/run/e5-mobile}
CMDLINE=${CMDLINE:-/proc/cmdline}
UPTIME=${UPTIME:-/proc/uptime}
SLEEP=${SLEEP:-sleep}
DBG=${DBG:-/sys/kernel/debug}
IMG=${IMG:-/run/e5img}
MOBILE=${MOBILE:-/usr/bin/mobile}
KMSG=${KMSG:-/dev/kmsg}
LOG_MAX=262144 LOG_KEEP=65536

grep -q 'e5\.mobile=1' "$CMDLINE" || exit 0
mkdir -p "$RUN"
log() {
	printf 'mobile: %s\n' "$*" >> "$RUN/boot.log"
	{ printf 'e5: mobile: %s\n' "$*" > "$KMSG"; } 2>/dev/null
}
state() { echo "$1" > "$RUN/state"; log "$1${2:+ ($2)}"; }
up() { cut -d. -f1 "$UPTIME"; }

[ -e "$RUN/started" ] && { log "the modem runtime already ran in this boot"; exit 0; }
[ -s "$RUN/e5-imei" ] || { state no-imei "write the IMEI (*#06# in Symbian) to E:\\linux\\e5-imei"; exit 0; }
[ "$(up)" -gt 90 ] && { state too-late "uptime $(up) s, the modem must start within 90 s of boot: reboot"; exit 0; }
for f in e5rt pnping e5mem.ko rapuyama_sim.ko; do
	[ -e "$LIBEXEC/$f" ] || { state unavailable "$LIBEXEC/$f missing"; exit 0; }
done
touch "$RUN/started"
state starting

mount -t debugfs debugfs "$DBG" 2>/dev/null
insmod "$LIBEXEC/e5mem.ko" || { state unavailable "insmod e5mem.ko failed"; exit 0; }
mkdir -p "$IMG"
for f in 10480000 1fd50000 1ffe0000 1fb60000 100ec000; do
	cat "$DBG/e5mem/$f" > "$IMG/$f.bin" || { state unavailable "image $f unreadable"; exit 0; }
done
# e5mem stays loaded (the runtime reads the uplink puts back through it)
insmod "$LIBEXEC/rapuyama_sim.ko" || { state unavailable "insmod rapuyama_sim.ko failed"; exit 0; }
while [ "$(up)" -lt 60 ]; do $SLEEP 2; done
ip link set phonet0 up

env -i PATH="$PATH" E5RT_QUIET=1 E5RT_IMEI_FILE="$RUN/e5-imei" \
	E5RT_SIM_DEVICE=/dev/rapuyama-sim E5RT_OS_CLOCK=1 \
	"$LIBEXEC/e5rt" "$IMG" --mdi /dev/rapuyama-mdi --backend /dev/rapuyama-isi --seconds 0 \
	>> "$RUN/e5rt.log" 2>&1 &
rt=$!
echo $rt > "$RUN/e5rt.pid"
log "modem runtime started (pid $rt, uptime $(up) s)"

# log cap + death watch for the whole boot
(
	while kill -0 $rt 2>/dev/null; do
		if [ "$(wc -c < "$RUN/e5rt.log")" -gt $LOG_MAX ]; then
			tail -c $LOG_KEEP "$RUN/e5rt.log" > "$RUN/e5rt.log.tmp"
			cat "$RUN/e5rt.log.tmp" > "$RUN/e5rt.log"
			rm -f "$RUN/e5rt.log.tmp"
		fi
		$SLEEP 30
	done
	state stopped "the modem runtime exited: reboot to use mobile data"
) &

$SLEEP 5
reg=
n=0
while [ $n -lt 20 ]; do
	n=$((n + 1))
	case "$("$LIBEXEC/pnping" --send 0a e000 2>&1)" in
	*"42 e1 00 "*" 00 24 00 "*|*"42 e1 00 "*" 00 24 01 "*) reg=1; break ;;
	esac
	kill -0 $rt 2>/dev/null || exit 0
	$SLEEP 5
done
[ -n "$reg" ] || { state not-registered "no network after 100 s (SIM, coverage?)"; exit 0; }
"$LIBEXEC/pnping" --send 0a 3600 > /dev/null 2>&1
state registered
"$MOBILE" on >> "$RUN/boot.log" 2>&1
exit 0

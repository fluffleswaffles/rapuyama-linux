#!/bin/sh
# tests for the E5 mobile data scripts (init.sh e5_mobile_files,
# e5-mobile-boot.sh, e5-mobile.sh) with stub commands, no modem needed.
# run: sh linux/initramfs/tests/test-e5-mobile.sh [test name...]
# runs the scripts with dash: a busybox sh runs its own applets (insmod, ip)
# before the PATH stubs. busybox ash (the phone's shell) parses each script
# in t_syntax_busybox
HERE="$(cd "$(dirname "$0")" && pwd)"
I="$(cd "$HERE/.." && pwd)"
SH="${SH:-dash}"
TMPBASE="$(mktemp -d)"
trap 'rm -rf "$TMPBASE"' EXIT
pass=0 fail=0 failed=
T=
setup() {
	T="$TMPBASE/$1"
	mkdir -p "$T/run" "$T/card/linux" "$T/bin" "$T/etc"
}
ok() { :; }
no() { echo "  $cur: $*"; bad=1; }
check() { [ "$@" ] || no "check failed: [ $* ]"; }

t_syntax_busybox() {
	command -v busybox > /dev/null || { echo "  (no busybox on this PC, skipped)"; return; }
	for f in init.sh e5-mobile-boot.sh e5-mobile.sh; do
		[ -f "$I/$f" ] || continue
		busybox sh -n "$I/$f" || no "$f does not parse in busybox sh"
	done
}

# ---- Task 3: card files ----
# sourcing init.sh without its test switch would run the real /init on this
# machine (busybox --install, mount, mkswap, reboot): never do that
init_fn() {
	grep -q '^\[ -n "\$E5_INIT_TEST" \] && return' "$I/init.sh" || { no "init.sh has no E5_INIT_TEST return, not sourced"; return 1; }
	E5_INIT_TEST=1 RUN="$T/run/e5-mobile" CMDLINE="$T/cmdline" $SH -c \
		". '$I/init.sh'; type e5_mobile_files > /dev/null || exit 9; e5_mobile_files '$T/card'"
	[ $? = 9 ] && no "e5_mobile_files not defined"
}
t_files_copied() {
	setup files_copied
	printf '123456789012345' > "$T/card/linux/e5-imei"
	printf 'internet\n' > "$T/card/linux/e5-apn"
	echo 'console=tty0 e5.mobile=1' > "$T/cmdline"
	init_fn || return
	check -f "$T/run/e5-mobile/e5-imei"
	check -f "$T/run/e5-mobile/e5-apn"
	[ "$(stat -c %a "$T/run/e5-mobile/e5-imei" 2>/dev/null)" = 600 ] || no "imei mode $(stat -c %a "$T/run/e5-mobile/e5-imei" 2>/dev/null) != 600"
	cmp -s "$T/card/linux/e5-apn" "$T/run/e5-mobile/e5-apn" || no "apn differs"
}
t_files_skip_nocell() {
	setup files_skip_nocell
	printf '123456789012345' > "$T/card/linux/e5-imei"
	printf 'internet' > "$T/card/linux/e5-apn"
	echo 'console=tty0 e6.usbsh' > "$T/cmdline"
	init_fn || return
	check ! -e "$T/run/e5-mobile"
}

# ---- Task 4: boot step ----
# stubs: every one logs its argv to $T/calls; e5rt also its E5RT_* env.
# pnping answers "registered" while $T/reg exists
boot_setup() {
	setup "$1"
	mkdir -p "$T/libexec" "$T/dbg/e5mem" "$T/run/e5-mobile"
	for f in 10480000 1fd50000 1ffe0000 1fb60000 100ec000; do echo "img $f" > "$T/dbg/e5mem/$f"; done
	touch "$T/libexec/e5mem.ko" "$T/libexec/rapuyama_sim.ko"
	echo 'console=tty0 e5.mobile=1' > "$T/cmdline"
	echo '30.00 20.00' > "$T/uptime"
	printf '123456789012345' > "$T/run/e5-mobile/e5-imei"
	for c in insmod mount ip; do
		printf '#!/bin/sh\necho "%s $*" >> "%s/calls"\n' "$c" "$T" > "$T/bin/$c"
	done
	cat > "$T/libexec/e5rt" <<STUB
#!/bin/sh
echo "e5rt \$*" >> "$T/calls"
env | grep '^E5RT' | sort > "$T/e5rt.env"
echo "e5rt log line" 
sleep \${E5RT_STUB_SLEEP:-3}
STUB
	cat > "$T/libexec/pnping" <<STUB
#!/bin/sh
echo "pnping \$*" >> "$T/calls"
if [ "\$*" = "--send 0a e000" ] && [ -e "$T/reg" ]; then echo "res 0a: 42 e1 00 01 02 00 24 00 05"; else echo "res 0a: 42 e1 00 01 02 00 22 00 05"; fi
STUB
	printf '#!/bin/sh\necho "mobile $*" >> "%s/calls"\n' "$T" > "$T/bin/mobile"
	printf '#!/bin/sh\nexec sleep 0.02\n' > "$T/bin/fastsleep"
	chmod +x "$T/bin/"* "$T/libexec/e5rt" "$T/libexec/pnping"
}
boot_run() {
	PATH="$T/bin:$PATH" LIBEXEC="$T/libexec" RUN="$T/run/e5-mobile" CMDLINE="$T/cmdline" \
		UPTIME="$T/uptime" SLEEP="$T/bin/fastsleep" DBG="$T/dbg" IMG="$T/run/e5img" MOBILE="$T/bin/mobile" \
		KMSG="$T/kmsg" $SH "$I/e5-mobile-boot.sh"
	rc=$?
}
state_is() { [ "$(cat "$T/run/e5-mobile/state" 2>/dev/null)" = "$1" ] || no "state '$(cat "$T/run/e5-mobile/state" 2>/dev/null)' != '$1'"; }
t_boot_happy() {
	boot_setup boot_happy
	echo '65.00 20.00' > "$T/uptime"; touch "$T/reg"
	boot_run
	check $rc = 0
	state_is registered
	[ "$(grep -c '^mobile on$' "$T/calls")" = 1 ] || no "mobile on called $(grep -c '^mobile on$' "$T/calls") times"
	grep -q '^e5rt .*/run/e5img --mdi /dev/rapuyama-mdi --backend /dev/rapuyama-isi' "$T/calls" || no "e5rt argv: $(grep ^e5rt "$T/calls")"
	[ "$(cat "$T/e5rt.env" 2>/dev/null | tr '\n' ' ')" = "E5RT_IMEI_FILE=$T/run/e5-mobile/e5-imei E5RT_OS_CLOCK=1 E5RT_QUIET=1 E5RT_SIM_DEVICE=/dev/rapuyama-sim " ] ||
		no "e5rt env: $(cat "$T/e5rt.env" 2>/dev/null | tr '\n' ' ')"
	for f in 10480000 1fd50000 1ffe0000 1fb60000 100ec000; do cmp -s "$T/dbg/e5mem/$f" "$T/run/e5img/$f.bin" || no "image $f not copied"; done
	check -f "$T/run/e5-mobile/started"
	check -f "$T/run/e5-mobile/e5rt.pid"
	grep -q 'e5rt log line' "$T/run/e5-mobile/e5rt.log" || no "e5rt.log empty"
	grep -q '0123456789012345\|123456789012345' "$T/run/e5-mobile/e5rt.log" "$T/kmsg" "$T/run/e5-mobile/boot.log" 2>/dev/null && no "IMEI in a log"
	# e5rt exits (stub sleeps 3 s): the step notices
	n=0; while [ "$(cat "$T/run/e5-mobile/state")" != stopped ] && [ $n -lt 80 ]; do sleep 0.1; n=$((n + 1)); done
	state_is stopped
}
t_boot_with_traces_env() {
	boot_setup boot_with_traces_env
	echo '65.00 20.00' > "$T/uptime"; touch "$T/reg"
	E5RT_FW_TRACE=all E5RT_MSG_TRACE=data E5RT_INV_LOG=1 E5RT_STUB_SLEEP=0 boot_run
	grep -q 'TRACE\|INV_LOG\|STUB' "$T/e5rt.env" && no "debug env reached e5rt: $(cat "$T/e5rt.env" | tr '\n' ' ')"
	check -f "$T/e5rt.env"
}
t_boot_no_imei() {
	boot_setup boot_no_imei
	rm "$T/run/e5-mobile/e5-imei"
	boot_run
	check $rc = 0
	state_is no-imei
	grep -q '^e5rt' "$T/calls" 2>/dev/null && no "e5rt started"
	grep -q 'E:\\linux\\e5-imei' "$T/run/e5-mobile/boot.log" || no "boot.log does not name E:\\linux\\e5-imei"
}
t_boot_too_late() {
	boot_setup boot_too_late
	echo '120.00 20.00' > "$T/uptime"
	boot_run
	check $rc = 0
	state_is too-late
	grep -q '^e5rt\|^insmod' "$T/calls" 2>/dev/null && no "modem stack touched"
}
t_boot_not_registered() {
	boot_setup boot_not_registered
	echo '65.00 20.00' > "$T/uptime"
	E5RT_STUB_SLEEP=30 boot_run
	check $rc = 0
	state_is not-registered
	grep -q '^mobile on' "$T/calls" && no "mobile on called"
	[ "$(grep -c '^pnping --send 0a e000' "$T/calls")" = 20 ] || no "$(grep -c '^pnping --send 0a e000' "$T/calls") registration polls, not 20"
	kill "$(cat "$T/run/e5-mobile/e5rt.pid")" 2>/dev/null
}
t_boot_once() {
	boot_setup boot_once
	echo '65.00 20.00' > "$T/uptime"; touch "$T/reg" "$T/run/e5-mobile/started"
	boot_run
	check $rc = 0
	grep -q '^e5rt\|^insmod' "$T/calls" 2>/dev/null && no "modem stack started twice"
}
t_boot_not_3g() {
	boot_setup boot_not_3g
	echo 'console=tty0' > "$T/cmdline"; echo '65.00 20.00' > "$T/uptime"; touch "$T/reg"
	boot_run
	check $rc = 0
	check ! -e "$T/calls"
	check ! -e "$T/run/e5-mobile/state"
}

# init.sh starts the step on a 3G boot only
start_run() {
	grep -q '^\[ -n "\$E5_INIT_TEST" \] && return' "$I/init.sh" || { no "init.sh not sourceable"; return 1; }
	E5_INIT_TEST=1 CMDLINE="$T/cmdline" MOBILE_BOOT="$T/mobile-boot" KMSG="$T/kmsg" $SH -c \
		". '$I/init.sh'; type e5_mobile_start > /dev/null || exit 9; e5_mobile_start; wait"
	[ $? = 9 ] && no "e5_mobile_start not defined"
}
t_start_3g() {
	setup start_3g
	echo 'console=tty0 e5.mobile=1' > "$T/cmdline"
	printf '#!/bin/sh\ntouch "%s/ran"\n' "$T" > "$T/mobile-boot"; chmod +x "$T/mobile-boot"
	start_run || return
	check -f "$T/ran"
}
t_start_not_3g() {
	setup start_not_3g
	echo 'console=tty0' > "$T/cmdline"
	printf '#!/bin/sh\ntouch "%s/ran"\n' "$T" > "$T/mobile-boot"; chmod +x "$T/mobile-boot"
	start_run || return
	check ! -e "$T/ran"
}
t_start_simple_install() {
	setup start_simple_install
	echo 'console=tty0 e5.mobile=1' > "$T/cmdline"
	start_run || return
	grep -q 'needs the advanced setup' "$T/kmsg" 2>/dev/null || no "no 'needs the advanced setup' message"
}

# ---- runner ----
ALL="${*:-$(sed -n 's/^t_\([a-z0-9_]*\)() {$/\1/p' "$0")}"
for name in $ALL; do
	cur=$name bad=0
	"t_$name"
	# a started e5rt stub: stop it and let the step's watcher finish
	if [ -n "$T" ] && [ -f "$T/run/e5-mobile/e5rt.pid" ]; then
		kill "$(cat "$T/run/e5-mobile/e5rt.pid")" 2>/dev/null
		n=0; while [ "$(cat "$T/run/e5-mobile/state" 2>/dev/null)" != stopped ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
	fi
	T=
	if [ $bad = 0 ]; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); failed="$failed $name"; echo "FAIL $name"; fi
done
echo "$pass passed, $fail failed${failed:+:$failed}"
[ $fail = 0 ]

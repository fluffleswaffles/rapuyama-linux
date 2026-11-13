#!/bin/sh
# tests for the E5 `wayland` command (e5-wayland.sh) and its weston config
# (e5-weston.ini) with stub commands, no weston needed.
# run: sh linux/initramfs/tests/test-e5-wayland.sh [test name...]
# runs the script with dash; busybox ash (the phone's shell) parses it in
# t_syntax_busybox
HERE="$(cd "$(dirname "$0")" && pwd)"
I="$(cd "$HERE/.." && pwd)"
SH="$(command -v "${SH:-dash}")"
TMPBASE="$(mktemp -d)"
trap 'rm -rf "$TMPBASE"' EXIT
pass=0 fail=0 failed=
T=
setup() {
	T="$TMPBASE/$1"
	mkdir -p "$T/bin" "$T/sys/class/input" "$T/run"
	# stubs log to $T/log in call order. weston logs its argv, seat backend
	# and whether the seatd socket is there (fail_backend = a backend name
	# it dies on at once, exit 1); foot only has to exist; seatd makes its
	# socket and waits to be killed (its pid in $T/seatd.pid); udevd and
	# udevadm only log; pidof finds udevd if $T/udevd_running exists
	printf '#!/bin/sh\necho "$*" > "%s/weston.argv"\necho "$LIBSEAT_BACKEND" > "%s/weston.seat"\necho "$LIBSEAT_BACKEND" >> "%s/weston.calls"\n[ -e "$SEATD_SOCKET" ] && echo "weston sock" >> "%s/log" || echo "weston nosock" >> "%s/log"\n[ "$LIBSEAT_BACKEND" = "$(cat "%s/fail_backend" 2>/dev/null)" ] && exit 1\nexit 0\n' "$T" "$T" "$T" "$T" "$T" "$T" > "$T/bin/weston"
	printf '#!/bin/sh\n' > "$T/bin/foot"
	printf '#!/bin/sh\necho "seatd $*" >> "%s/log"\necho $$ > "%s/seatd.pid"\n: > "$SEATD_SOCKET"\nexec sleep 30\n' "$T" "$T" > "$T/bin/seatd"
	printf '#!/bin/sh\necho "udevd $*" >> "%s/log"\n' "$T" > "$T/bin/udevd"
	printf '#!/bin/sh\necho "udevadm $*" >> "%s/log"\n' "$T" > "$T/bin/udevadm"
	printf '#!/bin/sh\n[ "$1" = udevd ] && [ -e "%s/udevd_running" ] && { echo 99; exit 0; }\nexit 1\n' "$T" > "$T/bin/pidof"
	chmod +x "$T/bin/weston" "$T/bin/foot" "$T/bin/seatd" "$T/bin/udevd" "$T/bin/udevadm" "$T/bin/pidof"
}
no() { echo "  $cur: $*"; bad=1; }
check() { [ "$@" ] || no "check failed: [ $* ]"; }
# $1 = PATH for the script, rest = arguments
run() {
	TEMPLATE=$I/e5-weston.ini; export TEMPLATE
	p=$1; shift
	out=$(PATH="$p" SYS="$T/sys" XDG_RUNTIME_DIR="$T/run" SEATD_SOCKET="$T/seatd.sock" $SH "$I/e5-wayland.sh" "$@" 2>&1)
	rc=$?
}
log() { tr '\n' ';' < "$T/log" 2>/dev/null; }
# the seatd stub is gone after the script
seatd_stopped() {
	[ -f "$T/seatd.pid" ] || return 0
	kill -0 "$(cat "$T/seatd.pid")" 2> /dev/null && { no "seatd still running"; kill "$(cat "$T/seatd.pid")"; }
	return 0
}
fake_rgb565() {
	mkdir -p "$T/sys/module/e5_display/parameters"
	echo "$1" > "$T/sys/module/e5_display/parameters/rgb565"
}
APK="apk add weston weston-backend-drm weston-shell-desktop foot font-dejavu seatd eudev"

t_syntax_busybox() {
	command -v busybox > /dev/null || { echo "  (no busybox on this PC, skipped)"; return; }
	busybox sh -n "$I/e5-wayland.sh" || no "e5-wayland.sh does not parse in busybox sh"
}
t_no_weston_hint() {
	setup no_weston_hint
	rm "$T/bin/weston"
	run "$T/bin"
	check $rc = 1
	case "$out" in *"wayland: weston/foot/seatd/udevd not installed: $APK"*) ;; *) no "hint wrong: $out" ;; esac
}
t_no_foot_hint() {
	setup no_foot_hint
	rm "$T/bin/foot"
	run "$T/bin"
	check $rc = 1
	case "$out" in *"$APK"*) ;; *) no "hint wrong: $out" ;; esac
	check ! -f "$T/weston.argv"
}
t_rgb565_on() {
	setup rgb565_on; fake_rgb565 Y
	run "$T/bin:$PATH"
	check $rc = 0
	grep -q '^gbm-format=rgb565$' "$T/run/weston.ini" || no "no gbm-format=rgb565: $(cat "$T/run/weston.ini" 2>/dev/null)"
}
t_rgb565_off() {
	setup rgb565_off; fake_rgb565 N
	run "$T/bin:$PATH"
	check -f "$T/run/weston.ini"
	grep -q 'gbm-format' "$T/run/weston.ini" && no "has gbm-format"
	grep -q '@GBM@' "$T/run/weston.ini" && no "placeholder left"
	return 0
}
t_old_kernel_no_gbm() {
	setup old_kernel_no_gbm
	run "$T/bin:$PATH"
	check -f "$T/run/weston.ini"
	grep -q 'gbm-format' "$T/run/weston.ini" && no "has gbm-format without the param"
	return 0
}
t_weston_args() {
	setup weston_args
	run "$T/bin:$PATH"
	check $rc = 0
	case " $(cat "$T/weston.argv" 2>/dev/null) " in *" --config=$T/run/weston.ini "*) ;; *) no "no --config: $(cat "$T/weston.argv" 2>/dev/null)" ;; esac
	case "$(cat "$T/weston.argv")" in *--backend=drm*) ;; *) no "no drm backend: $(cat "$T/weston.argv")" ;; esac
}
t_no_seatd_hint() {
	setup no_seatd_hint
	rm "$T/bin/seatd"
	run "$T/bin"
	check $rc = 1
	case "$out" in *"wayland: weston/foot/seatd/udevd not installed: $APK"*) ;; *) no "hint wrong: $out" ;; esac
	check ! -f "$T/weston.argv"
}
t_no_udevd_hint() {
	setup no_udevd_hint
	rm "$T/bin/udevd"
	run "$T/bin"
	check $rc = 1
	case "$out" in *"$APK"*) ;; *) no "hint wrong: $out" ;; esac
	check ! -f "$T/weston.argv"
}
# weston gets libseat's seatd backend, with a seatd the script started
# before and stops after it (the vt off the keyboard while weston runs)
t_seatd_started_and_stopped() {
	setup seatd_started_and_stopped
	run "$T/bin:$PATH"
	check $rc = 0
	check "$(cat "$T/weston.seat" 2>/dev/null)" = seatd
	case "$(log)" in *"seatd ;"*"weston sock;"*) ;; *) no "seatd not started before weston: $(log)" ;; esac
	seatd_stopped
}
t_seatd_running_kept() {
	setup seatd_running_kept
	: > "$T/seatd.sock"
	run "$T/bin:$PATH"
	check $rc = 0
	check "$(cat "$T/weston.seat" 2>/dev/null)" = seatd
	case "$(log)" in *seatd*) no "started a second seatd: $(log)" ;; esac
	check -e "$T/seatd.sock"
}
t_seatd_fails() {
	setup seatd_fails
	printf '#!/bin/sh\necho "seatd: no vt" >&2\nexit 1\n' > "$T/bin/seatd"
	run "$T/bin:$PATH"
	check $rc = 1
	check ! -f "$T/weston.argv"
	case "$out" in *"seatd did not start"*) ;; *) no "no seatd message: $out" ;; esac
}
# no noop retry: with noop the vt keeps the keyboard, keys typed in foot
# would also reach the console shell
t_no_noop_retry() {
	setup no_noop_retry
	echo seatd > "$T/fail_backend"
	run "$T/bin:$PATH"
	check $rc = 1
	check "$(cat "$T/weston.calls" 2>/dev/null | tr '\n' ' ')" = "seatd "
	seatd_stopped
}
t_weston_rc() {
	setup weston_rc
	printf '#!/bin/sh\nexit 3\n' > "$T/bin/weston"
	run "$T/bin:$PATH"
	check $rc = 3
	case "$out" in *"weston exited with 3"*) ;; *) no "no exit message: $out" ;; esac
	seatd_stopped
}
t_libseat_explicit_kept() {
	setup libseat_explicit_kept
	out=$(LIBSEAT_BACKEND=noop PATH="$T/bin:$PATH" SYS="$T/sys" TEMPLATE="$I/e5-weston.ini" XDG_RUNTIME_DIR="$T/run" SEATD_SOCKET="$T/seatd.sock" $SH "$I/e5-wayland.sh" 2>&1)
	rc=$?
	check $rc = 0
	check "$(cat "$T/weston.calls" | tr '\n' ' ')" = "noop "
	case "$(log)" in *"seatd"*) no "seatd started for noop: $(log)" ;; esac
}
# libinput only takes devices udev tagged ID_INPUT*: udevd runs for the
# session, the keypad (and drm) get an add event, settled before weston
t_udevd_started_and_stopped() {
	setup udevd_started_and_stopped
	run "$T/bin:$PATH"
	check $rc = 0
	case "$(log)" in *"udevd --daemon;"*"udevadm trigger --action=add"*"--subsystem-match=input"*"udevadm settle"*"weston "*"udevadm control --exit;"*) ;;
	*) no "udevd/trigger/settle/exit order wrong: $(log)" ;; esac
}
t_udevd_running_kept() {
	setup udevd_running_kept
	: > "$T/udevd_running"
	run "$T/bin:$PATH"
	check $rc = 0
	case "$(log)" in *"udevd --daemon"*) no "started a second udevd: $(log)" ;; esac
	case "$(log)" in *"control --exit"*) no "stopped a udevd it did not start: $(log)" ;; esac
	case "$(log)" in *"udevadm trigger"*"weston "*) ;; *) no "no trigger before weston: $(log)" ;; esac
}
t_help() {
	setup help
	run "$T/bin:$PATH" help
	check $rc = 0
	case "$out" in *"apk add"*) ;; *) no "help lacks apk add: $out" ;; esac
	check ! -f "$T/weston.argv"
	rm "$T/bin/weston"
	mkdir "$T/sedbin"; ln -s "$(command -v sed)" "$T/sedbin/sed"
	run "$T/sedbin" help
	check $rc = 0
	case "$out" in *"apk add weston"*) ;; *) no "help without weston lacks apk add: $out" ;; esac
}
t_template() {
	c="$I/e5-weston.ini"
	grep -q '^idle-time=0$' "$c" || no "no idle-time=0"
	grep -q '^require-input=true$' "$c" || no "no require-input=true"
	grep -q '^keymap_layout=e5$' "$c" || no "no keymap_layout=e5"
	grep -q '^path=/usr/bin/foot$' "$c" || no "no foot autolaunch"
	grep -q '^backend=drm$' "$c" || no "no drm"
	grep -q '^renderer=pixman$' "$c" || no "no pixman"
	grep -q '^watch=true$' "$c" || no "no watch=true"
	grep -q 'animation=none' "$c" || no "animations on"
	return 0
}

ALL="${*:-$(sed -n 's/^t_\([a-z0-9_]*\)() {$/\1/p' "$0")}"
for name in $ALL; do
	cur=$name bad=0
	"t_$name"
	if [ $bad = 0 ]; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); failed="$failed $name"; echo "FAIL $name"; fi
done
echo "$pass passed, $fail failed${failed:+:$failed}"
[ $fail = 0 ]

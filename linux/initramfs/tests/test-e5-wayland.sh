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
	# stubs: weston logs its argv and environment (the script execs or runs
	# it), foot only has to exist
	printf '#!/bin/sh\necho "$*" > "%s/weston.argv"\necho "$LIBSEAT_BACKEND" > "%s/weston.seat"\n' "$T" "$T" > "$T/bin/weston"
	printf '#!/bin/sh\n' > "$T/bin/foot"
	chmod +x "$T/bin/weston" "$T/bin/foot"
}
no() { echo "  $cur: $*"; bad=1; }
check() { [ "$@" ] || no "check failed: [ $* ]"; }
# $1 = PATH for the script, rest = arguments
run() {
	TEMPLATE=$I/e5-weston.ini; export TEMPLATE
	p=$1; shift
	out=$(PATH="$p" SYS="$T/sys" BINDIR="$T/bin" XDG_RUNTIME_DIR="$T/run" $SH "$I/e5-wayland.sh" "$@" 2>&1)
	rc=$?
}
fake_rgb565() {
	mkdir -p "$T/sys/module/e5_display/parameters"
	echo "$1" > "$T/sys/module/e5_display/parameters/rgb565"
}
APK="apk add weston weston-backend-drm weston-shell-desktop foot font-dejavu"

t_syntax_busybox() {
	command -v busybox > /dev/null || { echo "  (no busybox on this PC, skipped)"; return; }
	busybox sh -n "$I/e5-wayland.sh" || no "e5-wayland.sh does not parse in busybox sh"
}
t_no_weston_hint() {
	setup no_weston_hint
	rm "$T/bin/weston"
	run "$T/bin"
	check $rc = 1
	case "$out" in *"wayland: weston/foot not installed: $APK"*) ;; *) no "hint wrong: $out" ;; esac
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
t_libseat_builtin() {
	setup libseat_builtin
	run "$T/bin:$PATH"
	check "$(cat "$T/weston.seat" 2>/dev/null)" = builtin
}
t_help() {
	setup help
	run "$T/bin:$PATH" help
	check $rc = 0
	case "$out" in *"apk add"*) ;; *) no "help lacks apk add: $out" ;; esac
	check ! -f "$T/weston.argv"
	rm "$T/bin/weston"
	run "$T/bin" help
	check $rc = 0
}
t_template() {
	c="$I/e5-weston.ini"
	grep -q '^idle-time=0$' "$c" || no "no idle-time=0"
	grep -q '^require-input=false$' "$c" || no "no require-input=false"
	grep -q '^keymap_layout=e5$' "$c" || no "no keymap_layout=e5"
	grep -q '^path=/usr/bin/foot$' "$c" || no "no foot autolaunch"
	grep -q '^backend=drm$' "$c" || no "no drm"
	grep -q '^renderer=pixman$' "$c" || no "no pixman"
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

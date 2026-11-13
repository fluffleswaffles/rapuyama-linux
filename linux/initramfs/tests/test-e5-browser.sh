#!/bin/sh
# tests for the E5 `browser` command (e5-browser.sh) and its X config
# (e5-xorg.conf) with stub commands, no X needed.
# run: sh linux/initramfs/tests/test-e5-browser.sh [test name...]
# runs the script with dash (see test-e5-mobile.sh); busybox ash (the phone's
# shell) parses it in t_syntax_busybox
HERE="$(cd "$(dirname "$0")" && pwd)"
I="$(cd "$HERE/.." && pwd)"
SH="$(command -v "${SH:-dash}")"
TMPBASE="$(mktemp -d)"
trap 'rm -rf "$TMPBASE"' EXIT
pass=0 fail=0 failed=
T=
setup() {
	T="$TMPBASE/$1"
	mkdir -p "$T/bin" "$T/sys/class/input"
	# stubs: xinit logs its argv (the script execs it), dillo only has to exist
	printf '#!/bin/sh\necho "$*" > "%s/xinit.argv"\n' "$T" > "$T/bin/xinit"
	printf '#!/bin/sh\n' > "$T/bin/dillo"
	chmod +x "$T/bin/xinit" "$T/bin/dillo"
}
no() { echo "  $cur: $*"; bad=1; }
check() { [ "$@" ] || no "check failed: [ $* ]"; }
# $1 = PATH for the script
run() {
	out=$(PATH="$1" SYS="$T/sys" BINDIR="$T/bin" LOG="$T/x.log" $SH "$I/e5-browser.sh" 2>&1)
	rc=$?
}
argv_has() { case " $(cat "$T/xinit.argv" 2>/dev/null) " in *" $1 "*) ;; *) no "xinit argv lacks '$1': $(cat "$T/xinit.argv" 2>/dev/null)" ;; esac; }
argv_lacks() { case " $(cat "$T/xinit.argv" 2>/dev/null) " in *" $1 "*) no "xinit argv has '$1'" ;; esac; }

t_syntax_busybox() {
	command -v busybox > /dev/null || { echo "  (no busybox on this PC, skipped)"; return; }
	busybox sh -n "$I/e5-browser.sh" || no "e5-browser.sh does not parse in busybox sh"
}
fake_rgb565() {
	mkdir -p "$T/sys/module/e5_display/parameters"
	echo "$1" > "$T/sys/module/e5_display/parameters/rgb565"
}
t_depth16_rgb565() {
	setup depth16_rgb565; fake_rgb565 Y
	run "$T/bin:$PATH"
	check $rc = 0
	check -f "$T/xinit.argv"
	grep -q -- '-depth 16' "$T/xinit.argv" || no "no -depth 16: $(cat "$T/xinit.argv" 2>/dev/null)"
	grep -q -- '-depth 24' "$T/xinit.argv" && no "has -depth 24"
}
t_depth24_not_rgb565() {
	setup depth24_not_rgb565; fake_rgb565 N
	run "$T/bin:$PATH"
	grep -q -- '-depth 24' "$T/xinit.argv" || no "no -depth 24: $(cat "$T/xinit.argv" 2>/dev/null)"
	grep -q -- '-depth 16' "$T/xinit.argv" && no "has -depth 16"
}
t_depth24_old_kernel() {
	setup depth24_old_kernel
	run "$T/bin:$PATH"
	grep -q -- '-depth 24' "$T/xinit.argv" || no "no -depth 24 without the param: $(cat "$T/xinit.argv" 2>/dev/null)"
}
t_keeps_x_args() {
	setup keeps_x_args; fake_rgb565 Y
	run "$T/bin:$PATH"
	grep -q -- '-- :0 vt2 -nolisten tcp' "$T/xinit.argv" || no "x args changed: $(cat "$T/xinit.argv" 2>/dev/null)"
}
t_no_xinit_hint() {
	setup no_xinit_hint
	rm "$T/bin/xinit"
	run "$T/bin"
	check $rc = 1
	case "$out" in *"apk add xorg-server xf86-input-evdev xinit font-misc-misc"*) ;; *) no "hint wrong: $out" ;; esac
	case "$out" in *xf86-video-fbdev*) no "hint still names fbdev: $out" ;; esac
}
t_xorg_conf_modesetting() {
	c="$I/e5-xorg.conf"
	grep -q 'Driver "modesetting"' "$c" || no "no modesetting driver"
	grep -q 'Option "kmsdev" "/dev/dri/card0"' "$c" || no "no kmsdev"
	grep -q 'Option "ShadowFB" "true"' "$c" || no "no ShadowFB"
	grep -q 'fbdev' "$c" && no "fbdev still mentioned"
	grep -q 'DefaultDepth' "$c" && no "DefaultDepth still set"
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

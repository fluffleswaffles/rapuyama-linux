#!/bin/sh
# tests for the E5 mobile data scripts (init.sh e5_mobile_files,
# e5-mobile-boot.sh, e5-mobile.sh) with stub commands, no modem needed.
# run: sh linux/initramfs/tests/test-e5-mobile.sh [test name...]
# (the phone's shell is busybox ash: SH=busybox sh by default when present)
HERE="$(cd "$(dirname "$0")" && pwd)"
I="$(cd "$HERE/.." && pwd)"
SH="${SH:-$(command -v busybox >/dev/null && echo 'busybox sh' || echo sh)}"
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

# ---- runner ----
ALL="${*:-$(sed -n 's/^t_\([a-z0-9_]*\)() {$/\1/p' "$0")}"
for name in $ALL; do
	cur=$name bad=0
	"t_$name"
	if [ $bad = 0 ]; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); failed="$failed $name"; echo "FAIL $name"; fi
done
echo "$pass passed, $fail failed${failed:+:$failed}"
[ $fail = 0 ]

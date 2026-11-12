#!/bin/sh
# runs the e5rt offline selftests (qemu-arm user) on a build.sh output dir:
# pass = exit 0 and no mismatch/fail line
# usage: ./check-runtime.sh <out dir> [dump dir]
# dump dir = the five ISASW images (default ~/e5/dump-linux-1110);
# QEMU = qemu-arm-static (default: on PATH, else ~/e5/cell/qemu/x/usr/bin)
OUT="${1:?usage: $0 <out dir> [dump dir]}"
DUMP="${2:-$HOME/e5/dump-linux-1110}"
QEMU="${QEMU:-$(command -v qemu-arm-static || echo "$HOME/e5/cell/qemu/x/usr/bin/qemu-arm-static")}"
fail=0
for t in os-timer kasumi sim sim-reset dsp dsp-tx; do
	if timeout 300 "$QEMU" "$OUT/e5rt" "$DUMP" "--$t-selftest" > "$OUT/selftest-$t.log" 2>&1 &&
	   ! grep -qi 'mismatch\|fail' "$OUT/selftest-$t.log"; then
		echo "PASS $t"
	else
		echo "FAIL $t: $(tail -n 2 "$OUT/selftest-$t.log" | tr '\n' ' ')"; fail=1
	fi
done
for p in e5gprs pnping; do [ -x "$OUT/$p" ] || { echo "FAIL $p missing"; fail=1; }; done
[ $fail = 0 ] && echo "ALL PASS"
exit $fail

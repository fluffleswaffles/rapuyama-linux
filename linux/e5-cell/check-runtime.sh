#!/bin/sh
# runs the e5rt offline selftests (qemu-arm user) on a build.sh output dir:
# pass = exit 0 and no mismatch/fail line
# usage: ./check-runtime.sh <out dir> <dump dir>
# dump dir = the five ISASW images of one boot (private, not in the repo);
# QEMU = the qemu-arm user binary (default: qemu-arm-static on PATH)
OUT="${1:?usage: $0 <out dir> <dump dir>}"
DUMP="${2:?usage: $0 <out dir> <dump dir>}"
QEMU="${QEMU:-qemu-arm-static}"
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

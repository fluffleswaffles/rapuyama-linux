#!/bin/sh
# checks that `tools/sdroot.sh <dir> e5` installs the mobile data files
# (no sudo, no swapfile, into a temp dir). usage: tools/check-sdroot-mobile.sh
# needs: linux/kernel/build-e5.sh build, linux/e5-cell/build.sh work/e5-cell linux/kernel/build-e5
TOP="$(cd "$(dirname "$0")/.." && pwd)"
D="$(mktemp -d)"; mkdir "$D/root"
trap 'rm -rf "$D"' EXIT
SDROOT_NOSUDO=1 SWAP_SIZE=0 "$TOP/tools/sdroot.sh" "$D/root" e5 > "$D/log" 2>&1 || { tail -5 "$D/log"; echo FAIL: sdroot.sh; exit 1; }
R="$D/root" ok=1
for f in usr/libexec/e5/e5rt usr/libexec/e5/e5gprs usr/libexec/e5/pnping usr/libexec/e5/mobile-boot usr/bin/mobile; do
	[ -x "$R/$f" ] || { echo "FAIL: /$f missing or not executable"; ok=0; }
done
for f in usr/libexec/e5/e5mem.ko usr/libexec/e5/rapuyama_sim.ko usr/share/doc/e5/mobile.txt; do
	[ -f "$R/$f" ] || { echo "FAIL: /$f missing"; ok=0; }
done
grep -q 'mobile' "$R/etc/motd" 2>/dev/null || { echo "FAIL: motd has no mobile line"; ok=0; }
[ $ok = 1 ] && echo PASS
[ $ok = 1 ]

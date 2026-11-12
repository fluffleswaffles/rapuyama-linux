#!/bin/sh
# checks an e5-release.sh output dir: SHA256SUMS lists both launchers, the
# Image, both dtbs and the advanced tarball, the sums verify, and the
# advanced tarball carries the same five files + install.sh + rootfs
# usage: tools/check-e5-release.sh <out dir>
O="${1:?usage: $0 <out dir>}"
ok=1
bad() { echo "FAIL: $*"; ok=0; }
for f in LinuxE5.sis 3GLinuxE5.sis Image e5.dtb e5-3g.dtb e5-advanced.tar.gz; do
	grep -q "  $f\$" "$O/SHA256SUMS" 2>/dev/null || bad "SHA256SUMS lacks $f"
done
(cd "$O" && sha256sum --quiet -c SHA256SUMS) || bad "sums do not verify"
L=$(tar -tzf "$O/e5-advanced.tar.gz" 2>/dev/null)
for f in install.sh rootfs.tar.gz LinuxE5.sis 3GLinuxE5.sis Image e5.dtb e5-3g.dtb SHA256SUMS; do
	echo "$L" | grep -qx "e5-advanced/$f" || bad "e5-advanced.tar.gz lacks $f"
done
[ $ok = 1 ] && echo "PASS: $O"
[ $ok = 1 ]

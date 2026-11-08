#!/usr/bin/env bash
# packs an exe + ldd into a self signed sis (same layout as wdfind.sis etc):
# approot with the ptdump icon (uid 0xe1000004), signed with sis/cert.pem
# ensymble (my fork) runs in podman with python 2.7
#
# usage: mksis.sh <app.exe> <driver.ldd> <out.sis> [caption]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
[ $# -ge 3 ] || { echo "usage: $0 <app.exe> <driver.ldd> <out.sis> [caption]"; exit 1; }
EXE="$(realpath "$1")"; LDD="$(realpath "$2")"; OUT="$(realpath -m "$3")"
CAP="${4:-$(basename "$OUT" .sis)}"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
cp -r "$HERE/sis/approot" "$W/"
mkdir -p "$W/approot/sys/bin"
cp "$EXE" "$W/approot/sys/bin/ptdump_0xe1000004.exe"
cp "$LDD" "$W/approot/sys/bin/$(basename "$LDD")"
cp "$HERE/sis/cert.pem" "$HERE/sis/key_trad.pem" "$W/"
(cd "$W" && bash "$ROOT/tools/ensymble.sh" simplesis \
	-u 0xe1000004 -c "$CAP" -a cert.pem -k key_trad.pem -p "" approot out.sis)
cp "$W/out.sis" "$OUT"
ls -la "$OUT"
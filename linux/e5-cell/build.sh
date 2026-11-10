#!/usr/bin/env bash
# E5 cellular development tools (see README.md):
#   ./build.sh <out dir> [kernel build dir]
# e5rt + pnping: static armv6 musl, like linux/isi-runtime (clang + the alpine sysroot)
# e5mem.ko: only with a kernel build dir (the one the phone runs, after modules_prepare)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$HERE/../.."
OUT="${1:?usage: build.sh <out dir> [kernel build dir]}"
SYSROOT="$TOP/work/sysroot-alpine"
"$TOP/linux/initramfs/sysroot.sh" "$SYSROOT"
mkdir -p "$OUT"
for p in e5rt pnping; do
	clang --target=armv6-alpine-linux-musleabihf -march=armv6kz -mfloat-abi=hard \
		-mfpu=vfp --sysroot="$SYSROOT" --ld-path=/usr/bin/arm-linux-gnu-ld \
		-static -O2 -Wall -Wextra -o "$OUT/$p" "$HERE/$p.c"
done
if [ -n "${2:-}" ]; then
	KB="$(cd "$2" && pwd)"
	cp "$HERE/e5mem/e5mem.c" "$HERE/e5mem/Kbuild" "$OUT/"
	make -C "$TOP/linux/kernel/linux" O="$KB" ARCH=arm CROSS_COMPILE=arm-linux-gnu- modules_prepare
	make -C "$TOP/linux/kernel/linux" O="$KB" ARCH=arm CROSS_COMPILE=arm-linux-gnu- M="$OUT" KBUILD_MODPOST_WARN=1 modules
fi
ls -la "$OUT"

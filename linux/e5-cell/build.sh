#!/usr/bin/env bash
# E5 cellular development tools (see README.md):
#   ./build.sh <out dir> [kernel build dir]
# e5rt (= the sim runtime, sim/: what registers and carries data on the phone),
# e5gprs, pnping: static armv6 musl, like linux/isi-runtime (clang + the
# alpine sysroot). e5rt-legacy = the old e5rt.c, kept for development
# e5mem.ko: only with a kernel build dir (the one the phone runs, after modules_prepare)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$HERE/../.."
OUT="${1:?usage: build.sh <out dir> [kernel build dir]}"
SYSROOT="$TOP/work/sysroot-alpine"
"$TOP/linux/initramfs/sysroot.sh" "$SYSROOT"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"	# kbuild's M= needs an absolute path
CC=(clang --target=armv6-alpine-linux-musleabihf -march=armv6kz -mfloat-abi=hard
	-mfpu=vfp --sysroot="$SYSROOT" --ld-path=/usr/bin/arm-linux-gnu-ld
	-static -O2 -Wall -Wextra)
for p in pnping simprobe regpeek e5gprs; do
	"${CC[@]}" -o "$OUT/$p" "$HERE/$p.c"
done
"${CC[@]}" -o "$OUT/e5rt-legacy" "$HERE/e5rt.c"
S="$HERE/sim"
"${CC[@]}" -I "$S" -o "$OUT/e5rt" "$S/e5rt-sim.c" "$S/sim_runtime.c" "$S/os_timer_runtime.c" \
	"$S/identity_runtime.c" "$S/kasumi_runtime.c"
if [ -n "${2:-}" ]; then
	KB="$(cd "$2" && pwd)"
	cp "$HERE/e5mem/e5mem.c" "$HERE/e5mem/Kbuild" "$OUT/"
	make -C "$TOP/linux/kernel/linux" O="$KB" ARCH=arm CROSS_COMPILE=arm-linux-gnu- modules_prepare
	make -C "$TOP/linux/kernel/linux" O="$KB" ARCH=arm CROSS_COMPILE=arm-linux-gnu- M="$OUT" KBUILD_MODPOST_WARN=1 modules
fi
ls -la "$OUT"

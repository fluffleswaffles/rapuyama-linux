#!/usr/bin/env bash
# isolated armv6 userspace firmware runtime
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$HERE/../.."
OUT="${1:?usage: build.sh <out directory>}"
SYSROOT="$TOP/work/sysroot-alpine"
"$TOP/linux/initramfs/sysroot.sh" "$SYSROOT"
mkdir -p "$OUT"
clang --target=armv6-alpine-linux-musleabihf -march=armv6kz -mfloat-abi=hard \
	-mfpu=vfp --sysroot="$SYSROOT" --ld-path=/usr/bin/arm-linux-gnu-ld \
	-static -O2 -Wall -Wextra -Werror -o "$OUT/rapuyama-isi-runtime" "$HERE/runtime.c" "$HERE/sim_runtime.c" "$HERE/kasumi_runtime.c" "$HERE/trace_runtime.c" "$HERE/os_timer_runtime.c" "$HERE/identity_runtime.c" "$HERE/devlock_runtime.c" "$HERE/simlock_runtime.c"
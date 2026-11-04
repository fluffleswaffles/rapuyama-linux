#!/bin/sh
# e5-keyd: host tests, then a static armv6 musl binary (the Alpine armhf
# sysroot of linux/initramfs/sysroot.sh, like yamashell). font.h and layer.h
# are generated and committed: tools/psf2h.py (from the pc's console-setup
# fonts) and tools/e5-keyd-layer.py (from tools/e5_keys.py).
# usage: build.sh [out dir]   (default: ./out) -> <out>/e5-keyd
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"

${HOST_CC:-clang} -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
	-o "$OUT/test_core" "$HERE/core.c" "$HERE/test_core.c"
"$OUT/test_core"

SYSROOT="$TOP/work/sysroot-alpine"
"$TOP/linux/initramfs/sysroot.sh" "$SYSROOT" >/dev/null
clang --target=armv6-alpine-linux-musleabihf -march=armv6kz -mfloat-abi=hard -mfpu=vfp \
	--sysroot="$SYSROOT" --ld-path=/usr/bin/arm-linux-gnu-ld -static-libgcc -static -s \
	-std=gnu11 -Os -Wall -Wextra -Werror -o "$OUT/e5-keyd" "$HERE/main.c" "$HERE/core.c"

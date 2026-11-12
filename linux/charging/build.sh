#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$HERE/../.."
OUT="${1:?usage: build.sh output-directory}"
mkdir -p "$OUT"
clang --target=armv6-alpine-linux-musleabihf -march=armv6kz -mfloat-abi=hard -mfpu=vfp \
 --sysroot="$TOP/work/sysroot-alpine" --ld-path=/usr/bin/arm-linux-gnu-ld -static -Os -Wall -Wextra -Werror \
 "$HERE/charge.c" -o "$OUT/rapuyama-charge"
arm-linux-gnu-strip "$OUT/rapuyama-charge"
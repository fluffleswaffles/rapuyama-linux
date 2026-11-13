#!/bin/sh
# e5-display: host tests for the pixel/band helpers (e5-display-core.h).
# usage: run.sh   (HOST_CC overrides clang)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
INC="$TOP/linux/kernel/linux/drivers/soc/rapuyama"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

${HOST_CC:-clang} -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
	-I "$INC" -o "$OUT/test_core" "$HERE/test_core.c"
"$OUT/test_core"

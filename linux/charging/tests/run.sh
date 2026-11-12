#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
clang -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all "$HERE/policy.c" -o "$TEST_DIR/policy"
"$TEST_DIR/policy"
python3 "$HERE/status.py"
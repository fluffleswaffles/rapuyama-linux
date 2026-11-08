#!/usr/bin/env bash
set -e
if [ -n "${ENSYMBLE:-}" ]; then
    exec python2 "$ENSYMBLE" "$@"
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec podman run --rm -v "$ROOT/tools/src/ensymble:/ens:ro,Z" \
    -v "$PWD:/w:Z" -w /w docker.io/library/python:2.7 \
    python /ens/ensymble.py "$@"
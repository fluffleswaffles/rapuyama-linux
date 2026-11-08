#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE="${CONTAINER_ENGINE:-podman}"
USERNS=()
if [ "$(basename "$ENGINE")" = podman ]; then
    USERNS=(--userns=keep-id)
fi
if [ $# -eq 0 ]; then
    set -- bash
fi
exec "$ENGINE" run --rm -i "${USERNS[@]}" --user "$(id -u):$(id -g)" \
    --security-opt label=disable \
    -v "$ROOT:$ROOT" -w "$ROOT" localhost/symbian-build:latest "$@"

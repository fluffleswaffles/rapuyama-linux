#!/usr/bin/env bash
# driver/stage-e5: driver/stage for the E5's Symbian 9.3 kernel. Same tree, but
# kernel/kernel.h loses DObject::RequestUserHandle(..., TUint) and
# DObject::AddToProcess(..., TUint): the 9.3 ekern exports neither (empty
# ordinals 1158/1159) and the two virtuals would shift every DObject vtable.
# usage: STAGE=$PWD/driver/stage-e5 for driver/build.sh and build_app.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/stage"
DST="$HERE/stage-e5"
[ -d "$SRC" ] || "$HERE/mkstage.sh"
rm -rf "$DST"
cp -a "$SRC" "$DST"
H="$DST/kernel/kernel.h"
rm "$H"
grep -v -e 'IMPORT_C virtual TInt RequestUserHandle(DThread\* aThread, TOwnerType aType, TUint aAttr);' \
	-e 'IMPORT_C virtual TInt AddToProcess(DProcess\* aProcess, TUint aAttr);' \
	"$(readlink -f "$SRC/kernel/kernel.h")" > "$H"
[ "$(($(wc -l < "$(readlink -f "$SRC/kernel/kernel.h")") - $(wc -l < "$H")))" = 2 ] || { echo "kernel.h: expected 2 lines removed"; exit 1; }

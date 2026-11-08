#!/usr/bin/env bash
# builds runblob.ldd + runblob.exe with symbian gcce, same as driver/build.sh
# the exe gets timerprobe_app.cpp, the three build_*.sh only differ in that
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
KH="$ROOT/symbian/oss.FCL.sf.os.kernelhwsrv"
EKA="$KH/kernel/eka"
E2E="${E2E:-$ROOT/tools/bin/elf2e32_next}"
GCCE="${GCCE:-$ROOT/tools/gcce/arm-2012.03/bin/arm-none-symbianelf}"
DRV="$ROOT/driver"
STAGE="$DRV/stage"
[ -d "$STAGE" ] || "$DRV/mkstage.sh"
OUT="$HERE/build"
mkdir -p "$OUT"

# rebuild the embedded blobs
make -C "$HERE/.." all >/dev/null

CXX="$GCCE-g++"; LD="$GCCE-ld"
INC=(
  -I"$HERE" -I"$HERE/.." # runblob.h, blob.hdr
  -I"$STAGE" -I"$STAGE/kernel" -I"$STAGE/kernel/arm"
  -I"$STAGE/nkern" -I"$STAGE/nkern/arm"
  -I"$STAGE/memmodel" -I"$STAGE/memmodel/epoc"
  -I"$EKA/compsupp/gcce" -I"$EKA/compsupp/symcpp"
  -I"$KH/userlibandfileserver/fileserver/inc"
)
COMMON=(
  -c -march=armv5t -mthumb-interwork -mapcs -msoft-float
  -nostdinc -fno-exceptions -fno-rtti -fshort-wchar -fno-unit-at-a-time -O2
  -D__SYMBIAN32__ -D__GCCE__ -D__EABI__ -D__MARM__ -D__MARM_ARMV5__ -D__MARM_ARMV6__
  -D__EPOC32__ -D__GCC32__ -D__SUPPORT_CPP_EXCEPTIONS__ -D__GCCV3__ -D__PRODUCT_INCLUDE__='<variant_dummy.h>'
  -include gcce.h -include "$DRV/gcce_post.h"
  "${INC[@]}"
)
echo "" > "$OUT/variant_dummy.h"; INC+=(-I"$OUT"); COMMON+=(-I"$OUT")
LIBGCC="$("$CXX" -march=armv5t -print-libgcc-file-name)"

echo ">> compile LDD"
$CXX "${COMMON[@]}" -D__KERNEL_MODE__ -D__DLL__ -o "$OUT/runblob_ldd.o" "$HERE/runblob_ldd.cpp"
$CXX "${COMMON[@]}" -D__KERNEL_MODE__ -D__DLL__ -x c++ -o "$OUT/d_entry_cia.o" "$EKA/kernel/arm/d_entry.cia"
$CXX "${COMMON[@]}" -D__KERNEL_MODE__ -D__DLL__ -o "$OUT/d_entry.o" "$EKA/kernel/arm/d_entry.cpp"

echo ">> compile APP"
$CXX "${COMMON[@]}" -D_UNICODE -o "$OUT/runblob_app.o" "$HERE/timerprobe_app.cpp"
$CXX "${COMMON[@]}" -D_UNICODE -x c++ -o "$OUT/uc_exe_cia.o" "$EKA/euser/epoc/arm/uc_exe.cia"
$CXX "${COMMON[@]}" -D_UNICODE -o "$OUT/uc_exe.o" "$EKA/euser/epoc/arm/uc_exe.cpp"

LDFLAGS=(--target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 --default-symver)

echo ">> link + convert LDD"
$LD "${LDFLAGS[@]}" --entry _E32Dll -u _E32Dll \
    "$OUT/runblob_ldd.o" "$OUT/d_entry.o" "$OUT/d_entry_cia.o" \
    "$DRV/import/EKERN.dso" "$LIBGCC" -o "$OUT/runblob_ldd.elf"
"$E2E" --elfinput="$OUT/runblob_ldd.elf" --output="$OUT/runblob.ldd" \
    --targettype=LDD --uid1=0x10000079 --uid2=0x100000af --uid3=0xe1000011 \
    --linkas='runblob{000a0000}[e1000011].ldd' --libpath="$DRV/import" \
    --definput="$DRV/ptdump.def" --dlldata --compressionmethod=none --capability=all

echo ">> link + convert APP"
$LD "${LDFLAGS[@]}" --entry _E32Startup -u _E32Startup \
    "$OUT/runblob_app.o" "$OUT/uc_exe.o" "$OUT/uc_exe_cia.o" \
    "$DRV/import/EUSER.dso" "$DRV/import/EFSRV.dso" "$LIBGCC" -o "$OUT/runblob_app.elf"
# uid3 = 0xe1000004 so it launches through the existing approot reg.rsc (ptdump icon)
"$E2E" --elfinput="$OUT/runblob_app.elf" --output="$OUT/runblob.exe" \
    --targettype=EXE --uid1=0x1000007a --uid2=0x100039ce --uid3=0xe1000004 \
    --linkas='ptdump{000a0000}[e1000004].exe' --libpath="$DRV/import" \
    --capability=all --compressionmethod=none

echo ">> DONE"; ls -la "$OUT/runblob.ldd" "$OUT/runblob.exe"
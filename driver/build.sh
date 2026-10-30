#!/usr/bin/env bash
# this builds ptdump ldd+exe with arm-none-symbianelf 4.6.3
# it supports __declspec(dllimport/dllexport) natively so IMPORT_C/ EXPORT_C generate
# symbian's in-code import stubs, which ihs the exact output elf2e32 was written for
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
KH="$ROOT/symbian/oss.FCL.sf.os.kernelhwsrv"
EKA="$KH/kernel/eka"
E2E="$ROOT/tools/bin/elf2e32_next"
GCCE="$ROOT/tools/gcce/arm-2012.03/bin/arm-none-symbianelf"
OUT="${OUT:-$HERE/build}"
STAGE="${STAGE:-$HERE/stage}"
[ -d "$STAGE" ] || "$HERE/mkstage.sh"
mkdir -p "$OUT"

CXX="$GCCE-g++"
LD="$GCCE-ld"
INC=(
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
  -include gcce.h -include "$HERE/gcce_post.h"
  "${INC[@]}"
)
# pulls __PRODUCT_INCLUDE__: empty dummy on the include path
echo "" > "$OUT/variant_dummy.h"; INC+=(-I"$OUT"); COMMON+=(-I"$OUT")

LIBGCC="$("$CXX" -march=armv5t -print-libgcc-file-name)"
LIBGCCDIR="$(dirname "$LIBGCC")"

echo ">> compile LDD (kernel)"
$CXX "${COMMON[@]}" -D__KERNEL_MODE__ -D__DLL__ -o "$OUT/ptdump_ldd.o" "$HERE/ptdump_ldd.cpp"
$CXX "${COMMON[@]}" -D__KERNEL_MODE__ -D__DLL__ -x c++ -o "$OUT/d_entry_cia.o" "$EKA/kernel/arm/d_entry.cia"
$CXX "${COMMON[@]}" -D__KERNEL_MODE__ -D__DLL__ -o "$OUT/d_entry.o" "$EKA/kernel/arm/d_entry.cpp"

echo ">> compile APP (user) + eexe runtime"
$CXX "${COMMON[@]}" -D_UNICODE -o "$OUT/ptdump_app.o" "$HERE/ptdump_app.cpp"
$CXX "${COMMON[@]}" -D_UNICODE -x c++ -o "$OUT/uc_exe_cia.o" "$EKA/euser/epoc/arm/uc_exe.cia"
$CXX "${COMMON[@]}" -D_UNICODE -o "$OUT/uc_exe.o" "$EKA/euser/epoc/arm/uc_exe.cpp"

# link flags separate code (0x8000) and data (0x400000) segments
LDFLAGS=(--target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 --default-symver)

echo ">> link LDD -> ELF"
$LD "${LDFLAGS[@]}" --entry _E32Dll -u _E32Dll "$OUT/ptdump_ldd.o" "$OUT/d_entry.o" "$OUT/d_entry_cia.o" "$HERE/import/EKERN.dso" "$LIBGCC" -o "$OUT/ptdump_ldd.elf"

echo ">> convert LDD -> E32Image"
"$E2E" --elfinput="$OUT/ptdump_ldd.elf" --output="$OUT/ptdump.ldd" --targettype=LDD --uid1=0x10000079 --uid2=0x100000af --uid3=0xe1000001 --linkas='ptdump{000a0000}[e1000001].ldd' --libpath="$HERE/import" --definput="$HERE/ptdump.def" --dlldata --compressionmethod=none --capability=all

echo ">> link APP -> ELF"
$LD "${LDFLAGS[@]}" --entry _E32Startup -u _E32Startup "$OUT/ptdump_app.o" "$OUT/uc_exe.o" "$OUT/uc_exe_cia.o" "$HERE/import/EUSER.dso" "$HERE/import/EFSRV.dso" "$LIBGCC" -o "$OUT/ptdump_app.elf"

echo ">> convert APP -> E32Image"
"$E2E" --elfinput="$OUT/ptdump_app.elf" --output="$OUT/ptdump.exe" --targettype=EXE --uid1=0x1000007a --uid2=0x100039ce --uid3=0xe1000004 --linkas='ptdump{000a0000}[e1000004].exe' --libpath="$HERE/import" --capability=all --compressionmethod=none

echo ">> DONE"; ls -la "$OUT/ptdump.ldd" "$OUT/ptdump.exe"
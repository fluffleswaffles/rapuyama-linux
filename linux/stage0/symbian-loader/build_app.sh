#!/usr/bin/env bash
# builds one runblob app as its OWN sis: own package uid, own exe, own ldd
# file + device name (RB_NAME), own icon caption. so e6boot and e6logs sit on
# the phone side by side instead of the old "everything is ptdump" sis that
# replaced each other on every install.
#
# usage: build_app.sh <name> <app.cpp> <uid>
#   name: exactly 6 chars (the template resources are patched in place)
#   uid:  app uid, the ldd gets uid + 1
#
#   ./build_app.sh e6boot boot_app.cpp    0xe1000020   -> out/e6boot.sis
#   ./build_app.sh e6logs ramread_app.cpp 0xe1000022   -> out/e6logs.sis
#   ./build_app.sh all                                  -> both
#   RB_DEFS=-DRB_IMAGE_BOOT ./build_app.sh e6bfix boot_app.cpp 0xe1000024
#     -> out/e6bfix.sis: boots E:\Image in place (no decompressor)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

if [ "$1" = all ]; then
	"$0" e6boot boot_app.cpp 0xe1000020
	"$0" e6logs ramread_app.cpp 0xe1000022
	exit 0
fi
[ $# -eq 3 ] || { sed -n 2,14p "$0"; exit 1; }
NAME="$1"; SRC="$2"; UID3="$3"
[ ${#NAME} -eq 6 ] || { echo "name must be 6 chars (template patching)"; exit 1; }
LUID=$(printf '0x%08x' $((UID3 + 1)))
UHEX=$(printf '%08x' $((UID3)))
LHEX=$(printf '%08x' $((LUID)))

KH="$ROOT/symbian/oss.FCL.sf.os.kernelhwsrv"
EKA="$KH/kernel/eka"
E2E="${E2E:-$ROOT/tools/bin/elf2e32_next}"
GCCE="${GCCE:-$ROOT/tools/gcce/arm-2012.03/bin/arm-none-symbianelf}"
DRV="$ROOT/driver"
STAGE="$DRV/stage"
[ -d "$STAGE" ] || "$DRV/mkstage.sh"
OUT="${RB_BUILD_ROOT:-$HERE/build}/$NAME"
SIS_OUT="${RB_SIS_OUT:-$ROOT/out}"
mkdir -p "$OUT" "$SIS_OUT"

# rebuild the embedded blobs
make -C "$HERE/.." all >/dev/null

CXX="$GCCE-g++"; LD="$GCCE-ld"
INC=(
  -I"$HERE" -I"$HERE/.."
  -I"$STAGE" -I"$STAGE/kernel" -I"$STAGE/kernel/arm"
  -I"$STAGE/nkern" -I"$STAGE/nkern/arm"
  -I"$STAGE/memmodel" -I"$STAGE/memmodel/epoc"
  -I"$EKA/compsupp/gcce" -I"$EKA/compsupp/symcpp"
  -I"$KH/userlibandfileserver/fileserver/inc"
  -I"$ROOT/symbian/FCL.sf.incubator.modemadaptation/connectivitylayer/isce/iscapi_dll/inc"
  -I"$OUT"
)
echo "" > "$OUT/variant_dummy.h"
COMMON=(
  -c -march=armv5t -mthumb-interwork -mapcs -msoft-float
  -nostdinc -fno-exceptions -fno-rtti -fshort-wchar -fno-unit-at-a-time -O2
  -D__SYMBIAN32__ -D__GCCE__ -D__EABI__ -D__MARM__ -D__MARM_ARMV5__ -D__MARM_ARMV6__
  -D__EPOC32__ -D__GCC32__ -D__SUPPORT_CPP_EXCEPTIONS__ -D__GCCV3__ -D__PRODUCT_INCLUDE__='<variant_dummy.h>'
  -DRB_NAME="\"$NAME\"" ${RB_DEFS:-}
  -include gcce.h -include "$DRV/gcce_post.h"
  "${INC[@]}"
)
LIBGCC="$("$CXX" -march=armv5t -print-libgcc-file-name)"
LDFLAGS=(--target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 --default-symver)

echo ">> $NAME: compile"
$CXX "${COMMON[@]}" -D__KERNEL_MODE__ -D__DLL__ -o "$OUT/ldd.o" "$HERE/runblob_ldd.cpp"
$CXX "${COMMON[@]}" -D__KERNEL_MODE__ -D__DLL__ -x c++ -o "$OUT/d_entry_cia.o" "$EKA/kernel/arm/d_entry.cia"
$CXX "${COMMON[@]}" -D__KERNEL_MODE__ -D__DLL__ -o "$OUT/d_entry.o" "$EKA/kernel/arm/d_entry.cpp"
$CXX "${COMMON[@]}" -D_UNICODE -o "$OUT/app.o" "$HERE/$SRC"
$CXX "${COMMON[@]}" -D_UNICODE -x c++ -o "$OUT/uc_exe_cia.o" "$EKA/euser/epoc/arm/uc_exe.cia"
$CXX "${COMMON[@]}" -D_UNICODE -o "$OUT/uc_exe.o" "$EKA/euser/epoc/arm/uc_exe.cpp"

echo ">> $NAME: link"
$LD "${LDFLAGS[@]}" --entry _E32Dll -u _E32Dll \
    "$OUT/ldd.o" "$OUT/d_entry.o" "$OUT/d_entry_cia.o" \
    "$DRV/import/EKERN.dso" "$LIBGCC" -o "$OUT/ldd.elf"
"$E2E" --elfinput="$OUT/ldd.elf" --output="$OUT/$NAME.ldd" \
    --targettype=LDD --uid1=0x10000079 --uid2=0x100000af --uid3="$LUID" \
    --linkas="$NAME{000a0000}[$LHEX].ldd" --libpath="$DRV/import" \
    --definput="$DRV/ptdump.def" --dlldata --compressionmethod=none --capability=all
$LD "${LDFLAGS[@]}" --entry _E32Startup -u _E32Startup \
    "$OUT/app.o" "$OUT/uc_exe.o" "$OUT/uc_exe_cia.o" \
    "$DRV/import/EUSER.dso" "$DRV/import/EFSRV.dso" "$DRV/import/iscapi.dso" "$LIBGCC" -o "$OUT/app.elf"
"$E2E" --elfinput="$OUT/app.elf" --output="$OUT/${NAME}_0x$UHEX.exe" \
    --targettype=EXE --uid1=0x1000007a --uid2=0x100039ce --uid3="$UID3" \
    --linkas="${NAME}_0x$UHEX{000a0000}[$UHEX].exe" --libpath="$DRV/import" \
    --capability=all --compressionmethod=none

echo ">> $NAME: sis"
W="$OUT/sis"
rm -rf "$W"
mkdir -p "$W/approot/sys/bin" "$W/approot/resource/apps" \
	"$W/approot/private/10003a3f/import/apps"
cp "$OUT/${NAME}_0x$UHEX.exe" "$OUT/$NAME.ldd" "$W/approot/sys/bin/"
# the ptdump reg/loc resources with the names + uid swapped. same string
# lengths, so only the bytes change; the uid checksum in the header is redone
python3 - "$DRV/sis/approot" "$W/approot" "$NAME" "$UID3" <<'EOF'
import struct, sys
src, dst, name, uid = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)

def crc(data):
	c = 0
	for b in data:
		c ^= b << 8
		for _ in range(8):
			c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
			c &= 0xffff
	return c

def u16(s):
	return s.encode("utf-16-le")

def patch(path_in, path_out, pairs):
	d = bytearray(open(path_in, "rb").read())
	orig = bytes(d)
	for a, b in pairs:
		assert len(a) == len(b), a
		d = d.replace(a, b)
	assert bytes(d) != orig, path_in
	u = struct.unpack_from("<III", d, 0)
	if u[2]:
		u = (u[0], u[1], uid)
	b = struct.pack("<III", *u)
	struct.pack_into("<IIII", d, 0, *u, crc(b[1::2]) << 16 | crc(b[0::2]))
	open(path_out, "wb").write(d)

old_exe, new_exe = "ptdump_0xe1000004", "%s_0x%08x" % (name, uid)
pairs = [(u16(old_exe), u16(new_exe)), (u16("ptdump"), u16(name))]
patch(src + "/private/10003a3f/import/apps/%s_reg.rsc" % old_exe,
      dst + "/private/10003a3f/import/apps/%s_reg.rsc" % new_exe, pairs)
patch(src + "/resource/apps/%s_loc.r01" % old_exe,
      dst + "/resource/apps/%s_loc.r01" % new_exe, pairs)
EOF
cp "$DRV/sis/cert.pem" "$DRV/sis/key_trad.pem" "$W/"
(cd "$W" && bash "$ROOT/tools/ensymble.sh" simplesis \
	-u "$UID3" -c "$NAME" -a cert.pem -k key_trad.pem -p "" approot out.sis)
cp "$W/out.sis" "$SIS_OUT/$NAME.sis"
ls -la "$SIS_OUT/$NAME.sis"
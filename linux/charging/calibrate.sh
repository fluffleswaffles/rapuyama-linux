#!/bin/sh
# prints an RCHG1 calibration for rapuyama-charge from what symbian left in the preserved
# modem firmware (phys 0x1f8b0000, c6800000 in the firmwares view), so every e6
# charges with its own factory values
set -eu
shm=${SHM:-/sys/kernel/debug/rapuyama-shm}
test -n "${SHM:-}" || grep -q debugfs /proc/mounts || mount -t debugfs none /sys/kernel/debug
ram=$shm/1f8b0000
test -r "$ram" || { echo 'preserved firmware RAM unavailable' >&2; exit 1; }
u8(){ dd if="$ram" bs=1 skip=$(($1)) count=1 2>/dev/null | od -An -tu1 | tr -d ' '; }
u16(){ dd if="$ram" bs=2 skip=$(($1 / 2)) count=1 2>/dev/null | od -An -tu2 | tr -d ' '; }
id=$(u8 0x12894) type=$(u8 0x12895) gain=$(u16 0x1289c)
usable=$(u8 0x12590) capacity=$(u16 0x12596)
test "$id" = 14 && test "$type" = 6 || { echo "no IBAT calibration entry ($id/$type)" >&2; exit 1; }
test "$gain" -ge 5000 && test "$gain" -le 20000 || { echo "IBAT gain $gain out of range" >&2; exit 1; }
case "$usable" in 1|2) ;; *) echo "battery not identified as usable ($usable)" >&2; exit 1;; esac
test "$capacity" -ge 500 && test "$capacity" -le 2500 || { echo "battery capacity $capacity out of range" >&2; exit 1; }
echo "RCHG1 $gain $capacity 4140"

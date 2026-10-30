#!/usr/bin/env python3
"""Decode the E5 version 2 handoff block (e5bot3 SaveIsah) out of an e5logs klog.bin
(1 MB from phys 0x1f000000; the block is at +0xf1000). usage: parse_isah.py klog.bin [out.bin]"""
import struct, sys
k = open(sys.argv[1], 'rb').read()
b = k[0xf1000:0xf8000]
w = lambda i: struct.unpack_from('<I', b, i * 4)[0]
if w(0) != 0x48415349 or w(1) != 2:
    sys.exit(f"no valid block: magic {w(0):08x} version {w(1)}")
for i, n in enumerate(('ttbr0', 'ttbr1', 'ttbcr', 'dacr', 'contextidr', 'sctlr', 'prrr', 'nmrr')):
    print(f"{n:10} {w(2 + i):08x}")
LINUX = (0x11600000, 0x1fb60000)  # widest Linux RAM (e5.dts memory .. symbian-top); the real start is the loader block
for i in range(8):
    va, d = w(0x20 + 2 * i), w(0x21 + 2 * i)
    if d & 3 == 1:
        pa = d & ~0x3ff
        risk = "IN LINUX RAM RANGE" if LINUX[0] <= pa < LINUX[1] else "outside linux ram"
        l2 = b[0x5000 + i * 0x400:0x5400 + i * 0x400]
        pages = sum(1 for j in range(256) if struct.unpack_from('<I', l2, j * 4)[0] & 2)
        print(f"{va:08x} L1 {d:08x} -> L2 table pa {pa:08x} ({risk}), {pages} small pages mapped")
    else:
        print(f"{va:08x} L1 {d:08x} (type {d & 3}, no L2 table)")
if len(sys.argv) > 2:
    open(sys.argv[2], 'wb').write(b)

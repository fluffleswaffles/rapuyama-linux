#!/usr/bin/env python3
"""Nokia E5 console keymap -> busybox loadkmap binary (linux/initramfs/e5.kmap).

Layers (kernel keymap modifier bits): shift 1, altgr 2 (= Sym), ctrl 4, alt 8,
ctrlr 128 (= the Fn layer: the arrow key, one-shot, layout B in e5_keys.py).
Sym: held or one-shot (SAltGr). Right softkey: one-shot Ctrl. Menu: one-shot
Alt. Mail: Sym lock. Call: Tab. Left softkey: Esc. Keycodes must match the
linux,keymap in linux/boot/e5.dts and stay < 128 (busybox NR_KEYS).
usage: e5-kmap.py <out.kmap>
"""
import os
import struct
import sys

NR_KEYS, MAX_MAPS = 128, 256
SHIFT, ALTGR, CTRL, ALT, FN = 1, 2, 4, 8, 128
HOLE = 0x200


def lat(c): return ord(c)
def let(c): return 0xb00 | ord(c)


sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e5_keys import (KC, COMMA, DOT, SPACE, BKSP, ENTER, AT, QUEST,  # noqa: E402
                     SYM, SYM_KC, BASE_KC, FN_LAYER, KERNEL_SPECIAL)

# keys that are the same in every layer (the Fn layer may override some)
FIXED = {
    97: 0xc07,   # fn-arrow (KEY_RIGHTCTRL): SCtrlR = fn layer for one key
    42: 0x700,   # shift
    100: 0xc01,  # sym (KEY_RIGHTALT): SAltGr, held or one-shot
    29: 0x702,   # ctrl key (dead on the test phone)
    125: 0xc02,  # right softkey (KEY_LEFTMETA): one-shot Control
    56: 0xc03,   # menu (KEY_LEFTALT): one-shot Alt
    58: 0xa01,   # mail (KEY_CAPSLOCK): AltGr_Lock = sym lock
    15: 0x009,   # call (KEY_TAB): Tab
    1: 0x01b,    # left softkey (KEY_ESC): Esc
    ENTER: 0x201,
    103: 0x603, 108: 0x600, 105: 0x601, 106: 0x602,  # up down left right
}


def fn_value(v):
    return lat(v) if len(v) == 1 else KERNEL_SPECIAL[v]


def value(kc, mods):
    if mods & FN and kc in FN_LAYER:
        return fn_value(FN_LAYER[kc])
    if kc in FIXED:
        return FIXED[kc]
    name = next((n for n, k in KC.items() if k == kc), None)
    if name:
        low = name.lower()
        if mods & CTRL:
            return ord(name) & 0x1f
        if mods & ALT:
            return 0x800 | ord(low)
        if mods & ALTGR and name in SYM:
            return lat(SYM[name])
        return let(name) if mods & SHIFT else let(low)
    base = BASE_KC.get(kc)
    if base:
        if mods & CTRL and kc == SPACE:
            return 0
        if mods & ALTGR and kc in SYM_KC:
            return lat(SYM_KC[kc])
        return lat(base)
    if kc == BKSP:
        return 0x7f
    return HOLE


def build():
    flags = bytearray(MAX_MAPS)
    tables = []
    for fn in (0, FN):
        for m in range(16):
            idx = fn | m
            flags[idx] = 1
            tables.append((idx, [value(k, idx) for k in range(NR_KEYS)]))
    tables.sort()
    out = bytearray(b'bkeymap') + flags
    for _, t in tables:
        out += struct.pack('<%dH' % NR_KEYS, *t)
    return bytes(out)


if __name__ == '__main__':
    with open(sys.argv[1], 'wb') as f:
        f.write(build())

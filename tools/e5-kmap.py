#!/usr/bin/env python3
"""Nokia E5 console keymap -> busybox loadkmap binary (linux/initramfs/e5.kmap).

Layers (kernel keymap modifier bits): shift 1, altgr 2 (= Sym), ctrl 4, alt 8,
ctrlr 128 (= the fn-arrow layer, locked/unlocked by the arrow key).
Sym: held or one-shot (SAltGr). Right softkey: one-shot Ctrl. Menu: one-shot
Alt. Mail: Sym lock. Call: Tab. Left softkey: Esc. Keycodes must match the
linux,keymap in linux/boot/e5.dts and stay < 128 (busybox NR_KEYS).
usage: e5-kmap.py <out.kmap>
"""
import struct
import sys

NR_KEYS, MAX_MAPS = 128, 256
SHIFT, ALTGR, CTRL, ALT, FN = 1, 2, 4, 8, 128
HOLE = 0x200


def lat(c): return ord(c)
def let(c): return 0xb00 | ord(c)


FKEY = lambda n: 0x100 + n - 1  # F1..
HOME, END, PGUP, PGDN, DEL = 0x114, 0x117, 0x118, 0x119, 0x116

# linux keycodes (input-event-codes.h) used by e5.dts
KC = dict(Q=16, W=17, E=18, R=19, T=20, Y=21, U=22, I=23, O=24, P=25,
          A=30, S=31, D=32, F=33, G=34, H=35, J=36, K=37, L=38,
          Z=44, X=45, C=46, V=47, B=48, N=49, M=50)
COMMA, DOT, SPACE, BKSP, ENTER = 51, 52, 57, 14, 28
AT, QUEST = 41, 53            # @/ key (KEY_GRAVE), ?! key (KEY_SLASH)

# keys that are the same in every layer
FIXED = {
    97: 0xa07,   # fn-arrow (KEY_RIGHTCTRL): CtrlR_Lock = fn layer on/off
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

# Sym layer = the second symbols printed on the keys
SYM = dict(Q='"', W='(', E=')', R='1', T='2', Y='3', U='*', I='+', O='=',
           F='4', G='5', H='6', J='#', K='-', L="'",
           V='7', B='8', N='9', M='0')
SYM_KC = {COMMA: ';', DOT: ':', AT: '/', QUEST: '!'}

# fn-arrow layer: what the keyboard does not print
FNL = dict(Q='~', W='[', E=']', R='{', T='}', Y='<', U='>', I='^', O='%', P='`',
           A='&', S='$', D='\\', F='|', G='_')
FN_SPECIAL = dict(H=HOME, J=PGDN, K=PGUP, L=END,
                  Z=FKEY(1), X=FKEY(2), C=FKEY(3), V=FKEY(4), B=FKEY(5),
                  N=FKEY(6), M=FKEY(7))


def value(kc, mods):
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
        if mods & FN and name in FNL:
            return lat(FNL[name])
        if mods & FN and name in FN_SPECIAL:
            return FN_SPECIAL[name]
        return let(name) if mods & SHIFT else let(low)
    base = {COMMA: ',', DOT: '.', AT: '@', QUEST: '?', SPACE: ' '}.get(kc)
    if base:
        if mods & CTRL and kc == SPACE:
            return 0
        if mods & ALTGR and kc in SYM_KC:
            return lat(SYM_KC[kc])
        return lat(base)
    if kc == BKSP:
        return DEL if mods & FN else 0x7f
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

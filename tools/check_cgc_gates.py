# SPDX-License-Identifier: GPL-2.0-only
"""check the clk-rapuyama.c gate table against the ROM.

runs power_resources FUN_800c7790 from 0x800c7850 (gate table setup) in
unicorn, records every FUN_800c7534(reg, mask, polarity, gate) call and
compares it with ry_gates[]. usage: python3 tools/check_cgc_gates.py <kern.bin>
"""
import re
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

ROM = 0x80000000
START, END, GATE_FN = 0x800c7850, 0x800c7bd4, 0x800c7534
CGC = 0x20032f00

rom = Path(sys.argv[1]).read_bytes()
u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
u.mem_map(ROM, (len(rom) + 0xfff) & ~0xfff)
u.mem_write(ROM, rom)
u.mem_map(0x10000000, 0x10000)
u.reg_write(UC_ARM_REG_SP, 0x10008000)
calls = []

def code(u, addr, size, data):
	if addr == GATE_FN:
		r = [u.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
		calls.append(r)
		u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))

u.hook_add(UC_HOOK_CODE, code)
u.emu_start(START, END)

src = Path(__file__).resolve().parents[1] / 'linux/kernel/linux/drivers/clk/clk-rapuyama.c'
table = {int(g, 16): (int(r, 16), int(m, 16), int(p)) for g, r, m, p in
	 re.findall(r'\[(0x[0-9a-f]+)\] = \{ (0x[0-9a-f]+), (0x[0-9a-f]+), (\d)', src.read_text())}
assert len(calls) == 34, len(calls)
bad = 0
for reg, mask, pol, gate in calls:
	want = (reg - 0xc6000000 + 0x20000000 - CGC, mask, pol)
	ok = table.get(gate) == want
	bad += not ok
	print(f'gate {gate:#04x} rom reg {reg - 0xc6000000 + 0x20000000:#x} mask {mask:#06x} pol {pol}', 'ok' if ok else f'TABLE {table.get(gate)}')
assert not bad, f'{bad} gates differ'
assert table[0x17] == (0x290, 0x20, 1)
print('all', len(calls), 'gates match')
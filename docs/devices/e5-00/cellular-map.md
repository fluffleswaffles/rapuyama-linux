# E5 cellular: ISASW address map, upstream E6 -> our E6 -> E5 (2026-11-10)

Development notes for the cellular data port (linux/e5-cell). Firmware images and
RAM snapshots are private inputs and are not distributed.

Sources: upstream runtime (origin/main linux/isi-runtime),
our E6 dump (code sha256 fd581084..., NOT upstream's 8b7a1001...),
first E5 dump (code V 0xc6300000, data V 0xc6800000 taken under live Symbian).
Method: thumb bl targets, a masked byte matcher, fuzzy sequence matching (capstone), the task tables.
Thumb functions have bit 0 set.

## Builds
- upstream E6 vs our E6: same data layout (RAM 0x44d000, same struct addresses), code moved
  per region: c621e..c623a +0, c6252 -0xda, c62bb..c62db +0x860, c62ef..c62f3 +0x758, c65a6 -0xce0.
- E5 (ICPR82, older): everything moved; task table and data structs mostly the same shape,
  but task records are 0x14 bytes (E6 0x18), the last task (host queue, current task when idle) is 0x2f (E6 0x30), 48 tasks (E6 49).

## Core
| role | upstream E6 | our E6 | E5 | how E5 was found |
|---|---|---|---|---|
| assert (patched to trap) | c65a7070 ARM entry | c65a6390 ARM -> thumb c62dbace | c63d8865 thumb (handler, no ARM entry) | 495 callers with expr strings; same body as E6 c62dbace |
| reset(reason) (trap) | c62da64c | c62daeac | c63d7be4 | same code (strb reason, svc 0xef print, cmp #3) |
| allocate(size) | c62c8027 | c62c8887 | c63c646d | masked bytes, unique |
| release(p) | c62c8579 | c62c8dd9 | c63c69bf | masked bytes, unique; 3032 callers (E6 3429) |
| send_packet(p) = frame FROM the host (0x15/0x6c/0xd7 special, else -> router task 0x21) | c6252ba7 | c6252af3 | c635b4e3 | CORRECTED 2026-11-10: proven by the version test |
| (to-host: queue frame on last task 0x30/0x2f, NOT upstream send) | - | c6252acd | c635b4bd | |
| receive() | c62be89d | c62bf0fd | c63bef51 | first call of the task 33 loop |
| schedule() | c62bc1c7 | c62bca27 | c63bca37 | push{r1-r7,lr}+scheduler struct+c6f00000 literal |
| MDI rx interrupt | c62cb6db | c62cbf3b | c63c8f6d | masked bytes, unique |
| current task id | (used) | c62bdd01 | c63bdbc9 | masked bytes |
| send to task(id,p) | (used) | c62bdd1d | c63bdbe5 | E5 task 33 code |
| irq_register (prio 2) | c62daea0 | c62db700 | c63d83e8 | 49 callers (E6 46), binds via c63ebf44 with r2=2; MDI regs use it. (c63d835c = prio-6 twin, 3 callers) |
| irq_enable | c62daefa | c62db75a | c63d8442 | CORRECTED: writes INTC +0xc, 47 callers (E6 44); E6 MDI setup calls it after registering |
| irq_disable | c62daf16 | c62db776 | c63d8460 | CORRECTED: writes INTC +0x10, 52 callers (E6 53), same place in the SIM cold reset (c63d8480 = an unrelated 1-caller twin) |
| crit exit hw (-> bx lr) | c62dae84 | c62db6e4 | c63d83cc | masked; E5 writes INTC +0x18? see note |
| crit enter hw (-> bx lr) | c62dae92 | c62db6f2 | c63d83da | masked; order matches irq_register |
| get CPSR (-> mov r0,#0x13) | c65a6de4 | c65a6104 | c6658ce4 | exact bytes (ARM stub block E5 = E6 + 0xb2be0) |
| cache barrier (-> bx lr) | c65a6ea4 | c65a61c4 | c6658da4 | exact bytes |
| veneer (-> bx lr) | c65a6ecc | c65a61ec | c6658dcc | exact bytes, 4th of 4 |
| flash read_sector | c623ac69 | c623ac69 | c6346a91 | masked bytes |
| flash write_sector | c623aca3 | c623aca3 | c6346acb | masked bytes |

## Data
| role | upstream E6 | our E6 | E5 |
|---|---|---|---|
| scheduler struct | (via c62bc81c) | c681b2e0 | c6835c3c |
| task config table | *(sched+0x28) | c65cfbb0 (49 x 16) | c6672a20 (48 x 16, via sched+0x20) |
| task queue array (0x20 each) | base c681bb1c | c681bb1c | c6836258 |
| host queue (last task) | c681c11c (task 0x30) | c681c11c | c6836838 (task 0x2f) |
| stack top constant | c6f00000 | c6f00000 | c6f00000 |

## Task-ownership check (upstream os timer code checks 3 task entries)
- E6 task 27 c62cd141 (2005206a) -> E5 #26 c63ca065 (20052069)
- E6 task 29 c621eadb (00233287) -> E5 #28 c6329389 (00193287)
- E6 task 38 c62bbbd1 (00000101) -> NO E5 task: the E5 build has no such task. Upstream's
  OS-timer handoff must be redone for the E5 table.

## Remaining addresses (2026-11-10)
Region shifts upstream -> our E6 now known for all of the code upstream uses:
c621e..c623a +0, c6252 -0xda, c625a..c6263 +0x85c (identity: upstream's exact 12 bytes found at our
c62630f0), c628..c62db +0x860 (SIM pool + task table confirm), c62ef..c62f3 +0x758, c65a6 -0xce0.
E6 -> E5 by masked bytes, fuzzy sequence match (score), call counts/order, or code shape.

| role (upstream use) | upstream | our E6 | E5 | evidence |
|---|---|---|---|---|
| INTC wrapper 1 (-> bx lr) | c62bb820 | c62bc080 | inlined: NOP the call at c63b9bba | E5 calls helper c63d8420 directly |
| INTC wrapper 2 (-> bx lr) | c62bb828 | c62bc088 | inlined: NOP the call at c63b9cfa | E5 calls helper c63d842e directly |
| c6033340 helper (-> bx lr) | c62bb846 | c62bc0a6 | c63b9d16 | same code |
| timer test callback | c62bb821 | c62bc081 | (selftest only) | |
| OS timer callback (SMS t144) | c62c1199 | c62c19f9 | c63ba1eb | masked bytes |
| elapsed (patched) | c62c13dc | c62c1c3c | c63ba434 | fuzzy 0.93 |
| OS timer table ptr (28 B entries) | *c62c162c | c681fd48 | c6832e8c | callback code |
| start(id,x) | c62c1673 | c62c1ed3 | c63ba68f | masked bytes |
| stop(id) | c62c18b7 | c62c2117 | c63ba94f | first of a sibling pair, asserts like E6 |
| rebase (patched) | c62c2868 | c62c30c8 | c63bb852 | c6033340 access pattern + position |
| timer create/arm (patched) | c62ca416 | c62cac76 | c63bb89c | order + callers 13/11/2 vs 12/10/2 |
| timer cancel | c62ca4e0 | c62cad40 | c63bb954 | same |
| timer remaining | c62ca642 | c62caea2 | c63bba60 | same (fuzzy 0.49) |
| copied timer pool ptr | *c62ca794 | c6821e58 (279 x 20 B, header at -8) | c683496c (240, header differs) | create code |
| next_remaining (patched) | c62ca922 | c62cb182 | c63bbb9e | masked bytes |
| MDI tx-consumed irq 21 | c62cb7cf | c62cc02f | c63c9031 | masked bytes |
| MDI event irq 23 | c62cb561 | c62cbdc1 | c63c8e0d | masked bytes |
| scheduler ptr | *c62bc81c | c681b2e0 | c6835c3c | |
| sim timer create/cancel/remaining | c62d9bd4/9cd8/9da2 | c62da434/da538/da602 | c63d6ec0/c63d6fba/c63d7084 | fuzzy .69/.96/.84, same spacing |
| delay (patched) | c62da5a2 | c62dae02 | c63d7ace | same c6033340 sequence at +4 |
| host IRQ bind (patched) | c62ef96c | c62f00c4 | c63ebf44 | same first bytes f8b5 0446 0d46 |
| bx-lr patches | c62efb88/bba/bf8/c34/c72 | c62f02e0/0312/0350/038c/03ca | c63ec10c/c63ec13e/c63ec17c/c63ec1ba/c63ec1f8 | fuzzy .87-.91 + E5 veneer table order |
| movs r0,#0; bx lr patch | c62efd00 | c62f0458 | c63ec494 | masked bytes |
| secure fault decode site | c62ef9fc | c62f0154 | c63ebfd4 | masked bytes |
| ADC read (patched) / channel map | c62eff18 / *c62effc4 | c62f0670 / c65d50a8 | c63ec534 / c66775d8 | fuzzy .93; map byte-identical to E6 (hw-map) |
| PMIC write / read (patched) | c62f0038 / c62f00a4 | c62f0790 / c62f07fc | c63ec64a / c63ec6b0 | fuzzy .90/.93, same order |
| KASUMI f9 entry | c62f221d | c62f2975 | c66553d5 (different args!) | E5 wrapper uses HW c6024c00 + SW path |
| KASUMI selector d30a -> nop | c62f222e | c62f2986 | c6655426 (also d30a) | same cmp #0x1400 bits |
| KASUMI f8 batch (redirected) | c62f262a | c62f2d82 | c6655aac | only E5 fn using c6024c00 + c6025000 |
| KASUMI cipher / key schedule | c62f28ab / c62f291f | c62f3003 / c62f3077 | c6655c9b / c6655d0f | masked bytes |
| identity reader entry | c6262894 | c62630f0 | c636a06e | exact upstream bytes on E6; E5 same fn (+0xa0 table offset) |
| identity selftest / stack ret addrs | c625a48d, c6261a51, c62628bd | c625ace9, c62622ad, c6263119 | c6362ee1, c63693d5, c636a095 | masked bytes |
| SBIF control ptr | *c623ae8c | c6813f48 | c682baa4 (same struct shape) | |
| SBIF RAM drives table | c680aa7c | c680aa7c (0x90900000..) | c6823f3c (0xcb600000, 0xcb67e000) | |
| SIM ctx ptr | *c628a754 | c681a28c | (E5 pool near c638d4bc, TBD) | |
| SIM initial handler pool (6) | c628a534..548 | c628ad94..ada8 | c638d240..c638d254 | same pool order |
| SIM IRQ callbacks b6/b4/b5 | c60f0419/0127/04a1 | same | c60f03d9/c60f0127/c60f048d | neighbouring pool |
| SIM HAL owner/transaction complete | c628922e / c6289548 | c6289a8e / c6289da8 | c638bfa0 / c638c270 | masked bytes |
| SIM start / wake / cold reset | c6289fe7 / c628a1af / c628a303 | c628a847 / c628aa0f / c628ab63 | c638cd1f / c638cf17 / c638d06b | masked bytes |
| SIM wake MMIO write site | c628a1cc | c628aa2c | c638cf36 | str r0,[r1,#4] |
| IRQ handler table (RAM) | c6826f80 | c6826f80 | c683d280; PMIC id N handler at +8+4N (dispatcher c63ec9d2, mask bits table c6677634): SIM ids 13/14/15 = bits 0x2000/0x1000/0x0800 = c60f08f3/0aed/094b (CORRECTED 2026-11-11, was read one slot off) | |
| task ids checked 27/29 | c62cc8e1/c621eadb | c62cd141/c621eadb | c63ca065/c6329389 | task table |
| task 38 | c62bb371 | c62bbbd1 | NONE on E5 | |
| code end bound | c660bf00/c660c000 | same | c66ae000 | E5 code size 0x3ae000 |
| secure fault title page | c65b8000/c65b8400 | (our E6 secure table c65b8244) | E5 secure table c666c074 | selftest only, TBD |

## E5 needs different runtime LOGIC, not just addresses
1. assert: E5 has no ARM entry; trap the thumb fn c63d8864 (4b00 4718 + handler, like upstream's reset trap).
2. INTC wrappers inlined: NOP two call sites instead of patching wrappers.
3. Task ownership: E5 has 48 tasks, records 0x14 B, host/last task 0x2f; no twin of E6 task 38.
4. Timers: E5 copied-timer pool 240 (E6 279) with a different header; OS timer IDs < 0xed (E6 0x114);
   event timers < 0x14 (E6 0x32), table c6834878 (E6 c6821b78). os_timer_runtime.c constants must change.
5. KASUMI f9: E5 entry signature differs (selftest call must be rewritten); selector patch is the same.
6. SBIF RAM drives at VA 0xcb600000 (E6 0x90d00000): physical backing + DSP windows (E6: 0x101ff000,
   0x11400000, 0x115a0000) must be found for the E5 from its page tables.
7. SIM patch blob differs completely (164/8192 bytes equal); length bound TBD (E6 0xd48; E5 main code
   points up to c60f0ca9, so likely ~0xdb8). Upstream's SIM patch sha check is E6-only.
8. MDI: E5 ring/doorbell addresses and IRQ numbers (E6 19/21/23, mdi@30003ce0) still to confirm; the
   I/O window VAs (c6018000, c6033000, c6038000, c6040000, c6060000) and register layouts match the E6.

## E5 snapshot from Linux (2026-11-10; the dumps themselves are private and not in the repo)
Taken 76 s after boot with module linux/e5-cell/e5mem (read-only debugfs, built against build-e5-pub #14).
Code identical to the Symbian-live dump (942/942 pages). Data 489/640 pages differ from it.
Idle check: E5 sched+2 (current task) = 0x2f = upstream's E6 check runtime[0x1b2e2]==0x30
(sched c681b2e0+2). Host queue c6836838 empty (head == tail 0x7b).
| file | sha256 |
|---|---|
| 10480000.bin code 0x3ae000 | b9069f8f... |
| 100ec000.bin SIM patch 0x2000 | f5d0c823... |
| 1fb60000.bin 0x180000 | 1323841a... |
| 1fd50000.bin data 0x280000 | 631d8642... |
| 1ffe0000.bin secure 0x10000 | 5150dce2... |

## MDI + page tables (2026-11-10, xm2: low memory 0x10000000..0x11600000 from Linux)
- E5 MDI = E6: rings c601bce0/c601bdf0 -> phys 0x30003ce0/0x30003df0 (ptrs in E5 MDI state c683a440+0x33c/0x340),
  doorbells c6040280 -> 0x20040280, IRQs rx 0x13 / tx-consumed 0x15 / event 0x17 (registered at c63c9084..90b2,
  same order as E6). Upstream mdi.c + dts node apply with an E5 compatible.
- Symbian live L1 at 0x10000000 (4055/4096 equal to the isah copy). walk.py translates VAs:
  c6018000->30000000, c6024c00->20024c00 (crypto), c6033000->20033000, c6038000->20038000 (SIM),
  c6040000->20040000, c6060000->20400000 (INTC), c60f0000->100ec000, c6300000->10480000, c6800000->1fd50000,
  c6b00000->1ffe0000, c6e00000->20500000, c6f00000->40100000 (modem FIFO), cb300000/cb400000->10200000/10300000 (DSP, sections).
- SBIF RAM drives (E5 VA cb600000 + cb67e000, 2 x 0x7e000): their L2 table (and cb500000's) was at phys
  0x11609000/0x11609400 = inside the Linux kernel image -> LOST after the handoff. Physical backing unknown.
  Not in 0x11400000..0x115fffff (Symbian paged ROM code) nor proven in 0x1fb70000..0x1fc8ffff (zeros; 0x1fc90000+ = pixels).
  Needs the L2 read while Symbian runs: Symbian probe or loader saving the cb5/cb6 L2 tables (isah words 0xf7..0x3ff free).

## SBIF RAM drives + DSP windows FOUND (2026-11-10, probe e5l2pt, 2 Symbian boots, 2nd with cellular on)
L2 tables identical on both boots (L1 cb5 = 11609001, cb6 = 11609401 both times):
- va cb600000..cb6fbfff -> pa 11200000..112fbfff (contiguous, 0xfc000 = drive 9 @cb600000 + drive 10 @cb67e000)
  = E6 upstream window {0x90d00000, 0x11400000, 0xfc000} -> E5 {0xcb600000, 0x11200000, 0xfc000}.
  Below Linux RAM, survives the handoff: Linux snapshot vs Symbian copy 1900/1920 sectors equal
  (diffs = drive headers + a few log sectors). Headers 3203cdab / 3204cdab.
- va cb500000..cb57ffff -> pa 10140000..101bffff (512 KB, 64 KB pages): DSP shared buffers
  (upstream descriptors carry "AP physical minus 0x10000000", e.g. 0x141000 -> 0x10141000 = E5 va cb501000).
- va cb580000..cb5cffff -> pa 1fc90000..1fcdffff (320 KB): the "pixel-like" data at the top of RAM is a DSP
  buffer (inside the reserved symbian-top area, kept).
- va cb5d0000..cb5effff -> pa 00000000..0001ffff (128 KB, boot ROM/SRAM?), cb5f2000..cb5f9fff -> 12c4a000 (32 KB,
  inside Linux RAM: lost; role unknown, check if the runtime touches it).
Probe: linux/stage0/symbian-loader/e5l2pt_{app,ldd}.cpp.

## E5 runtime step 1 PASSED (2026-11-10, PC, qemu-arm 7.2 user mode)
linux/e5-cell/e5rt.c (linux/e5-cell/build.sh), images from the Linux-side
snapshot (10480000/1fd50000/1ffe0000.bin). E5 patches + thumb traps for assert c63d8864 / reset c63d7be4.
"E5 native ARM firmware: 100 MTC version request/reply/free cycles passed". Reply:
15 6c 00 15 0a 00 a1 1f <tid> f0 13 12 00 -> MTC ISI version 18.0 (upstream E6 build: 18.2). No assert, no reset.
qemu: Debian qemu-user-static 7.2 (qemu-arm-static).

## E5 runtime: synthetic MDI tests PASSED (2026-11-10, qemu-arm)
e5rt --dsp-selftest (100 DSP->AP deliveries through the rx ring + MDI rx IRQ c63c8f6d + schedule) and
--dsp-tx-selftest (100 host->DSP publishes, descriptor type 06 = pa - 0x10000000 -> E5 va = desc + 0xb6ab0000,
first 0fd5184c; acks free the packet). Both pass, version test still passes.
Found on the way: the E5 snapshot has 2 deallocated DSP allocation tags pending (MDI state c683a440 +0x20/+0x24 =
0x3b/0x39; E6 upstream snapshot was empty). The firmware reads each tag from the 4 bytes before its DSP buffer and
panics (hal_mdi_send_deallocated_blocks, c63c9436 -> ring dump c63c9d30 -> reset 0xd) on duplicates. So the DSP
window must hold the REAL contents: the test loads pa 0x10140000 (low dump) + pa 0x1fc90000 (top dump) into
va cb500000/cb580000; live mode maps the real DSP memory. E5 MDI state layout = E6's shifted by 4 bytes.

## SIM runtime (2026-11-10, qemu): upstream SIM self-tests PASS on the E5 firmware
linux/e5-cell/sim: --sim-selftest (STATUS 90 00 + 7-phase T=0 APDU) and --sim-reset-selftest (ATR 3b 05 HELLO,
periodic callbacks, timer cancel) pass against the simulated card. E5 differences found: SIM HAL context layout is
IDENTICAL to the E6 (init c638cfc0 = E6 c628aab8); SIM tx/rx buffers have their data at +4 after the count (E6 +0x20);
irq_enable/irq_disable were swapped in the first map (now c63d8442/c63d8460). E5 SIM IRQ callbacks per number:
b3 c60f07db, b4 c60f0361, b6 c60f03d9 (from the init's irq_register calls).

## Real SIM card (2026-11-10, phone, cellular kernel + rapuyama_sim.ko)
e5rt-sim --sim-reset /dev/rapuyama-sim: the E5 firmware's original cold reset through the bridge returns a valid
USIM ATR (3b 9f 96 80 1f ..., T=0, 22 bytes); SIM receive interrupt 0xb4 (INTC 180) fires, the firmware's PMIC
power-up stays inside the bridge allowlist.
Command traffic too: --sim-probe with SIM_APDU (select MF, select EF ICCID, READ BINARY 10) -> 90 00 / 90 00 /
10 bytes + 90 00 (a valid ICCID), through the E5 firmware's own T=0 driver and the bridge.

## IMSI read (2026-11-10, phone, xn5, same e5rt-sim build as the ICCID read)
EF_DIR 2F00 record 1 (Le 00 -> 6c 24, retried with 24): `61 19 4f 0c <USIM AID a0 00 00 00 87 10 02 ...> 50 ..` (12-byte AID).
Select the ADF by that AID (00 a4 04 0c) -> 90 00, select EF_IMSI 6F07 -> 90 00, READ BINARY 9 -> 08 + 15-digit IMSI
(odd parity nibble 9) + 90 00; EF_AD 6FAD -> 00 00 00 02 (2-digit MNC). The PIN is off, so no VERIFY was needed.
The IMSI stays in the phone log only.

## OS timers (2026-11-10, qemu): upstream timer runtime ported (linux/e5-cell/sim/os_timer_runtime.c)
Same semantics as the E6 (microseconds in/out, 8192 Hz pool ticks, elapsed() in 5 ms units with the same
32.768/5.0 constants), same 28-byte OS timer entries (+0x10 owner task, +0x11 flags, +0x12 state 1 idle/2 running/3 expired).
| role | E6 (ours) | E5 |
|---|---|---|
| OS timer table / ids | c681fd48, < 0x114 | c6832e8c, < 0xed |
| copied pool | ptr c6821e58, header at -8, 279 x 20 B {us, ticks, cb, arg, u16 next} | header c683496c {count, free, head, tail}, 240 x 16 B at +8 {ticks, cb, arg, u16 next} |
| create return | delay<<32 \| handle | handle only |
| Linux-owned energy timers | task 29 (223, 236, 241-244) | task 28 (182, 183, 201, 203, 204) |
| watchdogs stopped at attach | 212 (task 27), 266 (task 38) | 172 (task 26), 219 (task 34 = twin of E6 task 38, see below) |
| selftest timer / energy test timer | 62 (task 2) / 223 | 29 (task 2, same flag pattern 29..56 = E6 62..90) / 183 |
Snapshot: 13 OS timers running, 4 pool entries (3 OS-timer callbacks c63ba1eb, args 0xe8/0/0xac + c63ba575).
qemu: --os-timer-selftest (100 expiries, 100 cancel/rearm, 240 slots, long clock, 4 adopted) and
--os-timer-live-selftest (attach: copied 4, energy 5, watchdog 1; real-time expiry) pass. Not ported: the SMS timeout selftest.

### OS timers live (2026-11-10)
- Phone xo1 (timers without the SIM backend): attach ok, first pings answered, then the firmware spun in the
  hardware semaphore wait c63d851e (c6040200+4) under PMIC read c63ec6b0 -> e5rt alarm. Like upstream, live OS
  timers need the embedded SIM backend (PMIC/ADC/IRQ redirected to the bridge): E5RT_SIM_DEVICE=/dev/rapuyama-sim.
- Task 34 (code c63c8b75) = E6 task 38: c63c8951 starts timer 219 (200 ticks = 1 s) and sends a request 0x12 to
  dev 0x7c (DSP); the 0x12/0xda reply stops 219 (c63c8a3a). Any timer event in task 34 = reset 9 (c63c8b8a).
  Stopped at attach when running; the offline soak (no DSP) stops it every round.
- qemu --os-soak (simulated embedded SIM + live timers + scheduler, no MDI): the 5.2 s housekeeping timer
  c63ba575 feeds the PMIC watchdog (1.17 = 0x20, delegated to Linux), the firmware sends a resource 0x47
  indication to the host, then ~10 s in it faults at the secure fault decode site c63ebfd4 from the identity
  reader (lr c63693d5): the identity (IMEI) runtime is next.

## Identity + secure services (2026-11-10, qemu)
- identity_runtime.c: upstream's IMEI provider (E5RT_IMEI_FILE) on the E5 reader c636a06e (E6 c6262894, same code),
  buffers in the secure pool c6b0002c..c6b08000 (E6 c670202c..c6708000, checker c636b51c). Selftest calls the E5 reader.
- The secure API (helper c63693b0, title table c666c074.., module id at +14) calls through a function table into
  c8008900 (not reachable from e5rt): fault at c63ebfd4. Module 0x1b = identity (redirected); module 0x12 =
  data item size query c636a9e6 (E6 c6263ada; reader c6362f60, caller c6363064), reached ~10 s after start
  (item 00000001): answered with the service's own failure status -1 -> firmware item-unavailable path.
- qemu --os-soak 120 with both: no hang, 30 timer expiries, host indications res 0x47 and 0x48.
- Phone xo2 (fresh boot, live link + SIM backend + timers, no IMEI file): selftests pass on the phone; attach ok
  (5 copied timers), 6 MDI tx descriptors, 15 SBIF sector writes, then reset 0xa at c6354a04: sub-block parser
  c635492c (from the message dispatcher c6354a42 on 16-bit codes), type-1 sub-block with u16 +6 == 0. Not seen
  offline (soak), so the input is live (DSP or real SIM). The reset trap now dumps r4-r7 (r7 = message).
- Phone xo3 (fresh boot, started at 56 s uptime, no IMEI file): 90 s live with SIM backend + OS timers, ISI pings
  answered at 3/45/85 s, 21 timer expiries, task 34's DSP request 0x12/0xda answered by the DSP (mdi tx da007c12 ->
  rx da7c0012, twice), SIM IRQs 180-182 once each, secure item 1 "unavailable", clean exit. xo2 had started at 19 s
  uptime: DSP startup traffic -> 15 SBIF writes + reset 0xa. Start the live runtime >= 60 s after boot.

## Registration attempt (2026-11-10, phone xo4, live + SIM backend + timers + IMEI file)
- Resource 0xe6 (DSP, dev 0x7c) version 8.6 (E6 9.0). Upstream's modem NET resource 0xc8 is "not identified" on
  the E5: the E5 serves the legacy NET resource 0x0a (it sent NET 0x2c, channel info, unsolicited). Legacy ids:
  REG_STATUS_GET 0xe0, RAT 0x36, RSSI_GET 0x0b.
- With the IMEI the firmware starts L1 work (DSP descriptors with buffers, ~135 rx / 47 tx IRQs in ~20 s), queries
  secure item 1 six times, then reset 6 from the task switch c6658af4: a task stack guard (table c6672564, word
  must be 0xaaaaaaaa) was overwritten. Task unknown (the trap now names it). An fprintf inside the secure-item hook
  does not break a guard offline; hooks no longer print on task stacks anyway (deferred to the main loop).
- Phone xo5 (start at 60 s, IMEI): reset 0xa ~5 s in. Dump: DSP frame `12 00 7c 38 ea 00 15 00` (to res 0x38,
  234 B), body tid 00 msg 0x15, 1 sub-block type 1 len 0xe4 with u16 +6 == 0 -> parser c635492c resets after
  handling it. No E6 twin of this parser. Development patch: NOP the reset call at c6354a04 (bytes checked).
- Phone xo6 (NOP patch active): ~5 s in, the bridge's PMIC read 2.03 failed with ETIMEDOUT (cbus busy). The
  firmware takes HW semaphore 0 around every cbus transfer (pa 0x20040204: read >= 1 grants, write ~0 releases;
  c63d8514/c63d852c, used by PMIC read c63ec6b0); Linux's rapuyama_wdt cbus code did not -> collisions with the
  now active modem side. Kernel e5-cell: rapuyama_wdt takes it (optional 4th reg, only in e5-cell.dts), falls
  back + counts on timeout. Image-cell3 64c801e8, e5-cell3.dtb 8e2de1ff (install: smoke/xk4).

## NETWORK REGISTRATION (2026-11-10, phone xo7, kernel e5-cell #5 with the cbus semaphore)
Live runtime (SIM backend, OS timers, IMEI file, DSP patch) ran the full 240 s. CBUS semaphore: 172929 takes,
0 misses. Legacy NET (resource 0x0a) queries every 20 s, all answered:
- NET_REG_STATUS_GET (0xe0) -> 0xe1, sub-block 0x00 reg status 0x00 = HOME, operator name sub-blocks
  (UCS-2), GSM/UMTS reg info sub-block 0x09 (cell/area ids, values stay out of the repo);
- NET_RAT (0x36) -> 0x37, sub-block 0x2c value 0x02 = UMTS;
- NET_RSSI_GET (0x0b 00 02) -> 0x0c, sub-block 0x04: signal 100 %.
The E5 registers by itself once the SIM, timers and IMEI are in place (no request from Linux).

## Packet data, first attempts (2026-11-10, phone xg1..xg2)
- GPDS (legacy resource 0x31) server at 00:35, ISI 17.26; GPDS_STATUS_RESP answered.
- GPDS_ATTACH_REQ made the firmware run UMTS integrity: f9 wrapper c66554c7 took the hardware path (bits <
  0x13be: semaphore 3 + crypto c6024c00) and spun on the semaphore. KASUMI (kasumi_runtime.c): NOP the blo at
  c6655508 (f9) and c6655426 (traced wrapper c66553d5, < 0x1400), redirect the f8 batch c6655aac (E5 layout:
  count byte, 12-byte records {data, bits, COUNT-C} from +4) to C f8 on the firmware's key schedule c6655d0f /
  cipher c6655c9b. qemu: ETSI f8 test set 1 passes through the E5 batch entry.

## PACKET DATA (2026-11-11, phone xg3..xg14, e5rt-os16 + e5gprs4)
Live GPRS/UMTS PS data on the E5's own ISASW: attach, PDP context (IPv4, APN), gprs0 netdev,
DNS answered by the operator resolver over the cellular link, TCP handshake and an HTTP reply.
Fixes on the way (all in linux/e5-cell):
- e5gprs: accept() the pipe end point while PNS_PIPE_CREATE waits (the kernel answers the
  firmware's PNS_PEP_CONNECT_REQ only inside accept()); PNS_PIPE_ENABLE right after the
  activate request (ofono order); E5GPRS_DELAY between attach and context (an activate sent
  40 ms after the attach response, while that radio connection was released, never went out).
- Pipe service: resource 0xd9 in the router task (c63cb5c8 -> c63cabda), host frames with
  res d9 msg 0x20 (pipe data) take the fast path c63cba04 from send_packet. PIPE_CREATED_IND
  negotiates multi-credit flow control both ways.
- SIM (the attach needs a working card: START-HFN writes, AUTHENTICATE, EF_KeysPS update):
  - Gazoo PMIC SIM interrupts: id N -> handler c683d280 + 8 + 4N, mask bit table c6677634
    (dispatcher c63ec9d2), ack c63ec9a2 = write the bit to dev 2 reg 1 (W1C). e5rt read the
    handler one slot off: id 14's handler (c60f0aed) ran for id 13 and aborted transfers.
  - id 14 is re-raised by the PMIC on SIM activity; dispatched by the e5rt poll in the middle
    of a reply it powers the card down. Live mode holds ids 13-15 while the SIM transaction
    state (context +0x30) is 1.
  - SIM start c638cd1f: state 0 -> sets 2 (deferred, resumed by the id 14 handler), else
    state 1 and transmit. Ops table at context +0x98.
- KASUMI: all three paths checked against ETSI TS 135203 in qemu (--kasumi-selftest): f9
  (UIA1) sets 1-3 through c66554c7 (args msg, bits, IK, COUNT-I; stack FRESH, MAC out, dir,
  alg 1), f8 set 1 through the wrapper c66553d5, the C batch (layout matches c66558fe:
  COUNT at engine +4, bearer<<27 | dir<<26 at +8).
- Diagnostics: E5RT_SIM_TRACE (APDU headers, status words, IRQ/HAL-timer/register/PMIC
  events, never data phases), E5RT_MSG_TRACE (inter-task messages via a trampoline on
  send-to-task c63bdbe4; GPDS = task 0f / obj 35, router 21, host queue 2f, DSP link 22,
  SIM server 29, MM 0d).
xg15/xg16 (2026-11-11): e5rt crashed in xg15 with reset 6, task 0x0c (stack 0x468) guard broken,
under data; the software KASUMI paths now run on a private 8 KB stack (veneers in the dead
hardware blocks c6655524/c6655440 + C shims) and xg16 ran clean to the end. Pipe flow control
is fine both ways (multi-credit, grants of 10 then 7). Downlink is only released right after
an uplink packet: pings 0..5 answered together ~15 s after the first, later ones lost; looks
like RLC AM status reports going out only piggybacked on uplink data (no standalone uplink
opportunity). MDI event IRQ is requested but never raised by the DSP.
xg17-xg19 (2026-11-11): on CELL_DCH the DSP sends only d8 (per-slot report) and e8 (transport
block indication, always the same pattern: never a decoded block) to tasks 13/12; data reaches
GPDS only on FACH (task 10, msg 03/05 with inline payload). Kernel 4 (mdi mmap, buffers mode 2)
+ E5RT_DSP_DIRECT mapped the real DSP window: the DSP sent garbage buffer descriptors (tag 0 at
window offset 0) within seconds - the firmware's Symbian-era view of the shared pool overwrote
live DSP state. Do NOT use direct mode; copy mode (1) stays. The 8 x 0xa84 ring at cb531874 holds
power-on noise in the snapshot (never used). So the DCH downlink fails at L1 (DSP never decodes
a block), not in e5rt's buffer handling. Also fixed: sbif_write printed on the SBIF task stack
(reset 6, task 0x23).
Open: downlink latency. User data crosses the pipe both ways, but replies reach the firmware
6-9 s late and in bursts (pings at 1 s spacing answered together; HTTP reply after TCP
retransmits), DNS ~1.3 s. Suspects: RLC/RRC timing under the polling loop and the trace
output (MDI descriptors printed unbuffered).

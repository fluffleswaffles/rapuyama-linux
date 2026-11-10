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
| irq_enable | c62daefa | c62db75a | c63d8461 | masked bytes |
| irq_disable | c62daf16 | c62db776 | c63d8481 | next to enable, same body |
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
| IRQ handler table (RAM) | c6826f80 | c6826f80 | c683d280 (SIM 16-18: c60f08f3/0aed/094b) | |
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

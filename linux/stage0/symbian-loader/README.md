# runblob loader

Kernel-side LDD that loads raw (non-E32) ARM code into executable kernel memory
and runs it. Started as milestone 0 (run `../blob.bin`), now it also holds the
trampoline tests, the fb/watchdog experiments and the zImage booter.

## Files
| File | Role |
|---|---|
| `runblob_ldd.cpp` | the driver, all the controls below |
| `runblob.h` | shared channel interface (`RRunBlob`) |
| `boot_app.cpp` | boot.exe: streams `E:\zImage` + `E:\e6.dtb` and calls `Boot` |
| `runblob_app.cpp` | takeover launcher: calls `Takeover` (one-way) |
| `wdfind_app.cpp` | timer/watchdog finder: `TimedDiff` over `0x30000000` |
| `boot_alloc.h` | e6bfix block policy (which RAM block Linux boots in), no Symbian calls |
| `test_boot_alloc.cpp` / `test_boot_alloc.sh` | host test of that policy with a fake allocator (ASan/UBSan) |
| `build_boot.sh` / `build_runblob.sh` / `build_wdfind.sh` | build ldd + exe, differ only in which `*_app.cpp` goes in |

The asm payloads (`blob.S`, `stub2.S`, `tramp3.S`, `fakekern.S`, `takeover.S`,
`../../boot/tramp_boot.S`) get built by `../Makefile` into `.hdr` byte arrays the
driver includes. The build scripts run `make` themselves.

## Controls
| Control | What | Status |
|---|---|---|
| `RunBlob` | copy `blob.bin` into an exec page, run it, return 32 words (sig + cp15 ids) | works (`tools/parse_sysinfo.py`) |
| `IdentityTest` | build + check a 1:1 section mapping, mmu stays on | works (`parse_idtest.py`) |
| `TrampTest` | jump to `stub2`: mmu off, phys write, mmu back on | works (`parse_tramp.py`) |
| `FullTramp` | `tramp3` hands off to `fakekern` at `+0x8000` with boot regs, comes back | works but ~50% flaky (cache/tlb race), `parse_fulltramp.py` |
| `FbFill` | fill ram through an existing kernel va (framebuffer hunt) | found the fb at `0x10480000` |
| `Takeover` | ONE-WAY: mmu off, loop filling fb candidates | screen doesn't update (command-mode panel) |
| `AllocBoot` / `LoadChunk` / `Boot` | zImage booter, see `../../boot/README.md` | boots; PA_SWD reset fixed on E6 (22.10.2026) |
| `TimedDiff` | read A, spin, read B -> free-running counters | built, not run yet |
| `SecureTrace` | one-way watchdog-fed loop; logs fast-counter gaps to reserved RAM | build/run below |

## DSP snapshot at the handoff
Right before the jump (interrupts off) `Boot` copies the first 8 KB of the DSP SRAM page
(`0x30000000`) to `0x1f0f6000` (ramcon reservation, after the ISAH block, before the flight
recorder): header `'DSPS'`, version, fast counter, bytes, `0x200330a0`, mapping result, then the
data at `+0x20`. Linux reads it with `peek 0x1f0f6000 0x2000 1 0`, `work/tools/dsp-snap.py`
decodes it.

## Build
Standalone apps (own package uid, exe, ldd and icon, they install side by side):
```
./build_app.sh all # out/e6boot.sis (boot_app.cpp) + out/e6logs.sis (ramread_app.cpp)
./build_app.sh <6 char name> <app.cpp> <uid> # any other app the same way
```
Build the TrustZone-adjacent stall probe:
```
./build_app.sh e6tztr securetrace_app.cpp 0xe1000030
```
Install `out/e6tztr.sis` and `out/e6logs.sis`. Run `e6tztr`; the phone enters
a bare-metal loop and will reset if the unexplained reset condition recurs.
Start a stopwatch when launching `e6tztr`. After it returns to Symbian, run
`e6logs` to save the 1 MiB RAM window as `E:\klog.bin`, then decode it on the
host with the measured wall time (seconds):
```
python3 tools/parse_securetrace.py /path/to/klog.bin 123.4
```
The trace is at physical `0x1f080000`, inside the RAM-console reservation.
It records the ARM security-extension feature field (`ID_PFR1`), entry
`CPSR`/`SCTLR`, and a fast-counter sample about every 31 ms while feeding both
known watchdogs. `ID_PFR1` reports that the CPU implements the extension; it
does not reveal the current security world. Large internal gaps or a long
unrecorded tail show that the non-secure loop stopped or was delayed; this is
evidence to correlate with TrustZone, not proof of a secure interrupt or of a
`PA_SWD` timeout. The run is one-way; if it is still running after five
minutes, power-cycle the phone to recover and retrieve the log.
Old way, every app as the same "ptdump" package (replace each other):
```
./build_wdfind.sh # or build_boot.sh / build_runblob.sh
```
Output: `build/runblob.ldd` + `build/runblob.exe`, then package like in `driver/DEPLOY.md`.
Needs `<kernel/cache.h>` in `driver/stage/` (`driver/mkstage.sh` puts it there).

## If it goes wrong
- **Reboot / no file:** check the `.log` on `E:`, it's flushed after every step.
- Get out of a reboot loop by uninstalling the package (or delete `C:\sys\bin\runblob.ldd`).
- One-way controls (`Takeover`, `Boot`) never return, battery pull to recover.
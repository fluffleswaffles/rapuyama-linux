# Nokia E6 display: BCM2727B1 VideoCore III from Linux

## Status

**Linux shows an image on the E6 panel** (14.10.2026). The kernel driver `drivers/soc/rapuyama/videocore.c` does the following:

1. Boots the VideoCore over the ViSSI host bus with Nokia's own firmware.
2. Brings up VCHI.
3. Powers the LCD panel up the way Symbian does.
4. Exposes a 640x480 XRGB8888 KMS device (`/dev/dri/card0`), plus `/dev/fb0` through the drm fbdev emulation.
5. Pushes the frame to the VideoCore whenever it changes.

fbcon runs on it, so the kernel console and the shell are visible on the phone itself. The log of the first run is in [`first-display_SUCCESS.log`](first-display_SUCCESS.log).

## Wayland (runs on the phone since 18.10.2026)

Daily config: `Image` + `e6-vc-565.dtb` from `build-vc.sh` (`videocore.dma=1 videocore.rgb565=1`), booted with `e6bfix.sis` (see [`../linux/boot/README.md`](../linux/boot/README.md)). ~9-16 fps full screen, picture correct.

- the fbdev layer of `videocore.c` is a tiny KMS driver (simple display pipe, gem shmem, shadow plane, `late_initcall` so drm core is up first). a commit copies the damage into `vc->fb` and wakes the push thread. fake vblank events, 60 Hz mode, frames that come faster than the bus are merged
- weston sends a commit with no damage clips before every real one, the helper calls that a full screen update -> 0-clip commits are diffed row by row against `vc->fb`
- userspace = alpine 3.24 armhf (`linux/initramfs/alpine.sh`, pruned by `tools/prune_rootfs.py`): seatd + weston (pixman) + `foot` (redraws only changed cells, weston-terminal redrew the whole window on every key)
- keyboard: `linux/initramfs/xkb-e6`. fn = level 3 (yellow symbols), sym = en/ru, fn+space = tab, fn+backspace = escape
- `fps` / `fps line` in the on-screen terminal: full screen scroll / one line counter for 12 s, then the driver's 10 s frame counts. `N / 10` is the measured update rate; `1000000 / push_us` is only a push-time ceiling, excluding application/compositor/commit costs. The script now reads uptime with the shell builtin, avoiding a fork and `cut` process for every output line

<img src="weston-live.jpg" alt="weston running in live" width="400">
<img src="weston-qemu-preview.png" alt="weston running in qemu" width="400">

## Speed (18.10.2026)

| what | fps on the phone |
|---|---|
| PIO, XRGB8888, full frame | ~5 |
| gfx dma, XRGB8888 | 5-6 (after the sleep fix, 2 before) |
| gfx dma, RGB565 | 9 (scroll), 16 (`fps`) |
| gfx dma, RGB565, partial (broken picture) | 16 / 28 (`fps line`) |

- **historical transport measurement**: ~277 ns per 16-bit word, ~7 MB/s, the same with PIO and dma. dma only frees the cpu. bus timings = Symbian's
- **gfx dma** (`0x20024800`, vcdriver FUN_8032a154 / FUN_80329ec8): divider from the pll (<= 173 MHz) + reset + regs, done in `vc_boot` right after the bus init with the VC held in reset (after "display up" it killed the phone). the Symbian self test (test mode `0x700000` + go) also kills the phone, so there is none. VCHI tx slots go by dma too (Symbian: PIO only <= 48 bytes)
- **random dma timeouts** (22.10.2026): in about half of the boots the first piece (a VCHI tx slot, 16 lines) never completes (irq status 0), then everything went PIO (~4 fps, laggy). same sequence byte for byte as in the working boots, same pll, not the buffer address (low buffers still time out). the self test Symbian runs after the init ends with mode reg `+0x68` = 0 (FUN_80329c8c); we skipped the test and left the init value `0x900000`, so every piece ran in a mode Symbian never uses. `+0x68` = 0 after the init did not fix the timeout on the phone. that older implementation retried a timed out slot with PIOm,, the current driver stops instead, since part of the slot may already have been sent
- the host bus init (FUN_8032fb08 + FUN_8032a368) used to run only when Symbian had not left the bus in data mode, so DMACTL/0x6c/0x90 stayed whatever Symbian had at the handoff. it now always runs, with the VC held in reset first (like Symbian's cold start), and logs what Symbian left (`bus left by symbian: ...`). the bus cycle timings remain fixed for a 597.33 MHz core clock; Symbian computes them from the current bus clock (FUN_8032fa3c). A different inherited clock can therefore make the fixed cycles too short or unnecessarily long
- the phone log then showed Symbian leaves exactly what the init writes (mode `0x100023`, dmactl `0x64`, `0x6c` = 0x10, `0x90` = 0x6d60), so those four inherited values do not distinguish the failure. The timings differ: Symbian left `06008060 01bc0203` (for its current clock), we set `05008050 017c01c3`
- **root cause found** (26.10.2026): the RFBI/ViSSI clock gate was never switched on by Linux. `power_resources FUN_800c7790` builds the gate table by walking one pointer (`add`, `bic`, `eor 0x1e0`, `sub`), and `clk-rapuyama.c` had applied the `eor` to the offset instead of the address, so every gate after it was 0x100 too low: gates `0x17`/`0x18`/`0x19`/`0x2c`/`0x2d` are at `0x20033190` (not `0x20033090`), `0x1e`/`0x1f`/`0x2a` at `0x20033150`, `0x2b` at `0x200330e0`. Resources `0x18` (`EHwClkRFBI`) and `0x19` (`EHwClkViSSI`) map to gate `0x17` = `0x20033190` bit 5. `clocks = <&cgc 0x18>` therefore wrote `0x20` to `0x20033094`, and the real gate stayed however Symbian left it at the handoff: on = DMA works, off = the first piece never starts. In a failing boot `0x20033190` read `0x200` (bit 5 clear); writing `0x20` to `0x20033194` (what `FUN_800c8250` does for the request) and rebinding the driver gave `dma 1st piece: done, irq 00000081` and 46-49 DMA frames per 10 s in the same boot. Toggling only that bit and rebinding is deterministic: off -> first piece fails with IRQ 0, on -> `done, irq 00000081`, two full off/on cycles in one boot, 4 of 4. `tools/check_cgc_gates.py` runs the ROM table setup in Unicorn and checks all 34 gates against the driver. This also explains the non-stall startup pulse ending with IRQ status 0 (no VSYNC either): without that clock the DISPC timing generator does not run, and reading `+0x5c` (line status) stalls the bus
- **60 fps** (26.10.2026, measured with `VCGL_PROFILE=1` in libvcgl): the compositor used two EGL surfaces and re-added the GCE element every frame; the submit after that waits for the next flip (~14 ms) and nothing overlaps it, so `fps line` ran at 28. Changing only the content (ELEMENT_MODIFIED on one surface) is cheap (0.45 ms) but draws into the shown buffer and tears. Now a window has three VC buffers: `eglSwapBuffers` sends the VC swap, op `0x1f` (GCE element advance) shows the newest swapped buffer and frees the old one, the submit is async (op `0x11`, done arrives on UPDH) and only the previous flip is waited for, after this frame has been drawn. yamashell sends frame callbacks before it presents. That gave 45.3 fps for any scene (vctri too): the flip is tied to the panel TE at 60 Hz, and with Symbian's panel write clock (DSPC config `20000000`) a frame does not fit in one period. The VC steps that clock: 20 and 22 MHz give ~43-45 fps, 24 and 26 MHz give 60.0. With `videocore.panel_clk=24000000`: cached motion 59.1 fps, `fps line` 58.4 fps (28 before). Full-window client updates stay bus bound (~90 ms per 565 KB texture upload)
- **L2 cache** (26.10.2026): with the gate fixed (kernel #242 started DMA on the first boot), yamashell on DMA showed broken triangles and ghost frames. The SoC has an ARM L220 outer cache at `0x20500000` (cache id `0x41000486`, AUX `0x02050249` = 8 ways x 32 kB) that Symbian leaves enabled (CTRL = 1); its ekern maintains it by physical address (`+0x7b0` clean, `+0x770` invalidate, `+0x7f0` both, `+0x730` sync). Linux had no node for it, so `dma_sync` cleaned only L1 and the engine read stale RAM; PIO reads through the cache and was never affected. The address comes from the Symbian page tables left in RAM: ekern keeps the controller VA at `0xc8000024` (`0xc6e00000`), the L1 entry is section `0x20500412`. A 20 ms clean-by-way loop on the live phone changed the artifacts (not removed, cleaning cannot close the race). Fix: `arm,l220-cache` node in `e6.dts`, `l2c_aux_val/mask` and a no-op `l2c_write_sec` in `rapuyama.c` (CTRL/AUX are secure registers)
- **the result is decided per boot** (22-25.10.2026): kernel #184 has working and failing boots at different buffer addresses; builds #7 and #8 have opposite results at the same TX address but differ in other code. In the recorded boots, rebinds did not change the result (a working boot kept DMA for 916 s; in a failing boot every rebind, reload and retry fails). In a failing boot the first 16-line piece never completes: gfx IRQ status `0`, ViSSI status `0`, control `0x75`, count `0x800` (= the programmed size). Symbian always leaves the same host bus state (`dmactl 0x64`, `tim 06008060 01bc0203`) and the handoff word `md+70` does not correlate. Root cause unknown
- **ruled out on the phone** (each in a failing boot, the first piece still ended with IRQ status 0): TX slot below `0x18000000`; the ROM geometry for a 4128-byte slot (43 x 48 halfwords); the VGALite clock (resource `0x1c`) on; an END_WIN pulse; a bounded version of the ROM startup pulse (`FUN_8032a154`, `videocore.dma_prime=1`, default off), also with the ROM read/modify/write of `+0x40` (`FUN_80329ec8`); a ViSSI clock off/on cycle after init; gate `0x28` on (`0x200330b4 = 8`, resource `0x22` that `dma.dll` holds while its channels run, it was off: `0x200330b0 = 0x7ad6`); the ROM bus GPIO setup and the ROM pin/reset/init order; GFX `SYSCONFIG = 0x100c`; ViSSI `+0x10 = 0x08` (readback ok); waiting on gfx IRQ 146 (ROM `0x92`) instead of polling (zero interrupts delivered). A first transfer with all three planes disabled (background only, 8 x 1) fails the same way. That background configuration has no positive control on a working DMA boot, so it does not by itself exclude a memory-path failure
- **never read the running counter `+0x5c` right after a reset**: it rebooted the phone
- **receiver snapshot**: the count still equals the programmed size and control bits 0 and 4 stay set. In the related [TI OMAP36xx manual](https://www.ti.com/lit/ug/swpu177aa/swpu177aa.pdf) (7.5.7.2.5-6, tables 7-252/254) bit 4 is a trigger consumed by the hardware and the count drops as pixels are sent, consistent with an unconsumed trigger. These semantics are not independently verified for RapuYama
- **clock state** (read only, identical in a failing and a working boot): `0x20033080 = 0x4b13`, `0x20033090 = 0x32b`, `0x200330b0 = 0x7ad6`, `0x200330d0 = 0xb073`, `0x200330e0 = 0xaf`, `0x20033140 = 0x210`, `0x20033150 = 0x24`, `0x20033180 = 0x1ff`, `0x20033010 = 0x4494`, `0x20033034 = 0x6d00`, `0x20033038 = 0x3d9`. PLLs (`ecust FUN_800bec88`: 0 = `0x20032604`, 1 = `0x20032624`, 2 = `0x20032664` gfx, 3 = `0x20032644` ACCPLL; control at `+8`, lock = bit 4) read `0xf86/0x9898`, `0xb8a/0x9daf`, `0x483/0x9c98`, `0x1e2/0x9c98`. The divider at `0x20033038` selects 2. The ROM maps both `0x18` (`EHwClkRFBI`) and `0x19` (`EHwClkViSSI`) to hardware gate `0x17`. This snapshot read `0x20033090` for that gate, which was wrong: the gate is `0x20033190` bit 5 (see root cause above), never read before 26.10.2026
- **runtime bus clock and timings** (26.10.2026): the earlier matching clock snapshots did not include `0x20033004`. Actual ROM `karap FUN_800b8750/FUN_800b8828` uses bits 7:6 there to select core-clock divisors 1, 3, 4 or 6. A failing boot has `0x2491`, giving a calculated bus clock of 170.057 MHz from the other captured clock registers. Running the ROM timing setter with that state produces normal timings `06008060 / 01bc0203`, matching Symbian's inherited values; Linux still writes `05008050 / 017c01c3`. A temporary driver using only the corrected normal pair booted the VC and panel, read back both values, then still failed the first DMA with IRQ 0 and receiver count `0x800`. This is a timing discrepancy, but correcting that pair alone did not fix DMA. No PLL or clock-divider programming was changed. A matching register snapshot is not a measurement of physical clock activity
- **recent startup controls** (26.10.2026): disconnecting USB and removing the battery before the same kernel/DTB boot did not recover DMA. Neither an enable readback followed by a 100 microsecond delay before the receiver trigger nor the corrected normal timing pair recovered the first transfer. The experimental changes were not promoted into the default driver
- **GPIO**: before bus init the ROM makes GPIOs 106..121 and 124..126 outputs and drives them high (`0x20031a28`, `0x200319c4`, mask `0x73fffc00`); Linux sets the mux only. In a working boot the latch was already high under that mask; the direction is not readable through the GPIO interface. A difference, not a known cause
- **driver policy**: `videocore.dma=1` requires DMA; a PIO or DMA transfer failure latches the error and stops all later sends, a timed out VCHI slot is never replayed because the engine may have sent part of it, and `/dev/vc-khrn` reports the error. `videocore.dma=0` selects PIO. Firmware/display bring-up and short tails always use PIO. ViSSI init stops if its reset does not finish in 10 ms. DMA IRQ status accumulates until the worker reads it, so an error and a completion IRQ cannot overwrite each other. Because DMA still fails in some boots, `e6-vc-565-gpu.dtb` uses `videocore.dma=0`
- **DMA buffer ownership**: the persistent staging mapping is CPU-owned between transfers. Only complete 256-byte DMA lines are synchronized for the device, then returned to CPU ownership after a successful transfer. The short PIO tail stays CPU-owned. Inline bulk messages, framebuffer copies and Khronos uploads can reuse the staging memory without accessing a device-owned range. A failed bulk is not returned for reuse; later framebuffer copies and bulk submissions check the latched transport error first
- **host transport checks**: `python3 tools/vc-dma-test.py` and `python3 tools/vchi-bulk-test.py` extract the actual driver functions and run them with modeled MMIO and DMA ownership under ASan/UBSan. They cover completion/error ordering, timeouts, no replay after PIO/DMA failure, mapping boundaries, inline bulk, multiple pieces and PIO tails. They do not establish physical clock state, successful DMA after reboot, display correctness or frame rate
- **timer**: the timer is periodic and the hardware already makes 1000 interrupts per second; with HZ=100 short reply sleeps waited a whole 10 ms tick, so the kernel now uses HZ=1000. `e6.oneshot=1` is separate and stays off
- **Resources around the transfer** (ROM): Symbian requests resource `0x18` (`EHwClkRFBI`) only during init and shutdown, and resource `4` (EClockSdram) before every transfer (`FUN_8032acdc -> FUN_8032f9a8`), released by the completion DFC (`FUN_80329a48`). It also registers for change notifications of resource `6` (EPerformance) and then recomputes the bus timings. `dma.dll` requests resource `0x22` while any system DMA channel is active and, after an idle timer, idles its controllers (`0x20042900`, `0x20043100`, `0x20043200`) and releases it; Linux never requests it, and it was off in a failing boot (`0x200330b0 = 0x7ad6`), but switching it on did not help
- **DMA divisor** (22.10.2026, did not resolve the timeout): the gfx register layout matches the OMAP DISPC block, including `+0x70` (logic/pixel divisor). Linux used to leave this register at its reset value. Symbian `FUN_8032fa3c -> FUN_8032a6b4` writes it after calculating the bus timings: `0x10000 | max(2, gfx_kHz / (1000000000 / access_ps))`. `FUN_80329698 -> FUN_8032f980` selects 14000 or 17000 ps; with the gfx clock capped at 173 MHz, both give pixel divisor 2. The init now writes `0x00010002` explicitly. The parent divider at `0x20033038` is separate. The phone reported `0x00010002` both before reset and after init, and the first DMA piece still timed out. The log records the inherited divisor before reset and the programmed value after init; this missing initialization step was not the cause of that failure
- **combined hardware result** (22.10.2026): with the HS charger using SYSTEST line IO and its ACCPLL parent enabled, the first 16-line DMA slot at `0x1b5bc000` completed with IRQ `0x81`. Later full-frame transfers remained in DMA mode through uptime 63.95 s; one idle-screen sample took 37.994 ms. The charger simultaneously reported charging with no fault across 45 seconds. This proves both functions ran in the same boot. It does not isolate the DMA fix: the earlier failure was intermittent, and the parent-clock change was made for I2C. Sustained redraw was subsequently exercised as described below; repeat boot remains to be checked. A sparse sample of one frame in 10 s measures idle-screen activity, not maximum FPS
- **extended redraw run** (same boot): DMA remained active through uptime 374.79 s with no timeout or PIO fallback. Five consecutive nominal 10-second intervals contained 57, 59, 58, 58 and 58 full frames (290 total, about 5.8 FPS). Average frame-push times were 40.643..42.133 ms, implying a push-only ceiling of about 24 FPS. That ceiling is not the observed terminal redraw rate. The old shell benchmark launches `cut` in a command substitution for each output line, adding process-creation load; the source script now avoids both. The relative contributions of the producer, terminal/compositor, commit path and scheduling still require measurement; no kernel performance fix is inferred from these counts
- **producer comparison and CPU profile** (same boot): replacing the shell producer with an unbounded `awk` loop gave 48, 52 and 51 frames in three nominal 10-second windows (about 5.0 FPS), while frame updates took 42.425..43.195 ms. DMA remained active through uptime 916.11 s. Removing per-line process creation did not improve this workload. During a subsequent `awk` flood, CPU idle was zero; the terminal used about 43%, the producer 21%, Weston 9%, the VideoCore thread 6%, and one worker about 12%. This is a CPU-saturated terminal workload, not a measurement of the bus's maximum FPS. `push avg` times the whole `vc_display_update()` including upload and VideoCore replies, but excludes application rendering, DRM commits and time waiting for dirtied rows. The profile does not distinguish terminal parsing from glyph rendering; avoid attributing the remaining delay to DMA without further evidence
- **timeout recovery** (22.10.2026): one build stopped immediately after the first timeout message, while evaluating the added gfx register snapshot before its first printk. Those extra reads have been removed: a stalled block must not be inspected as part of error recovery. The IRQ bits already returned by the poll are acknowledged and the ViSSI DMA request is disabled. That older implementation then used PIO; the current driver stops the transport, as described above. No additional gfx status reads or retries follow a timeout. The exact register that stalled the snapshot is unknown
- **host pad initialization** (22.10.2026, ruled out as the timeout cause): the Linux driver never programmed the three mux words that `FUN_80329a10` sets on cold start and when returning to small DMA/PIO mode. It now selects function 4 at `0x200318e0`, `0x200318f0`, `0x20031900`, preserving unrelated bits, with VC RUN low before bus initialization. The phone reported inherited `44444412 44444444 24442244`, already matching function 4 under the masks; the first DMA transfer still timed out. This excludes the omitted mux setup as the cause. Actual ARM execution of the firmware confirmed the masks and values, and its first 16-line DMA transfer has the same address/size/GO/count/request writes as Linux. Step printks between GO and request enable have been removed. The DMA timeout cause remains unconfirmed; the PIO fallback was verified on the phone
- **software idle policy** (ruled out as a fix on the phone): forcing `SYSCONFIG=0x100c` during init and before DMA still produced first-piece timeout with IRQ 0. The driver again uses the firmware value `0x2015` at init. No speculative wake writes or additional gfx reads follow a timeout. Matching register programming in an ARM emulator does not prove clock transitions or DMA completion
- `EClockSdram` (resource 4), requested by Symbian before DMA through `FUN_8032f9a8`, uses the software resource handler `power_resources FUN_800c8e88`: it updates the resource state and optional trace without directly writing a clock gate. It is not equivalent to the hardware clock resource `0x18` (`EHwClkRFBI`)
- weston draws RGB565 itself (`gbm-format=rgb565`, the driver only copies rows instead of converting every pixel) and without a pixman shadow buffer (`pixman-shadow=false`, one 600 KB copy per frame less). the colour bar test pattern is off by default (`videocore.test_pattern=1`), `e6.fbtest` is gone from the daily bootargs

- **RGB565**: vcdispman FUN_80345920/74/c8 map Symbian formats: `4` = res `0x2b` / buf `0x20008` / 32 bpp (default), `0x10` = `5` / `0x20005` / 24, `0x20` = `1` / `0x20004` / 16 (`videocore.rgb565=1`)
- **partial push does not work**: the real VC ignores the BFMX write rects and always writes the data from the top of the resource (tried a 640 x h image + dst rect, and the whole frame + src = dst rect). Symbian never uses them either: it writes whole images and puts the dirty rect only into dispman ElementModified. a working partial push needs separate resources written whole (stripes)

Why Symbian is fast: S^3 renders on the VideoCore (OpenVG / GLES), the ARM sends drawing commands, not pixels.

## Drawing on the VideoCore (works since 19.10.2026)

The VC renders GLES 2 itself and shows the result, no pixels on the bus. `videocore.khrn_draw=1` (`e6-vc-gl.dtb`): a 320x240 magenta square in the middle for 10 s, ~21 s after boot.

The GL server in `vc-main` speaks the raspberry pi khronos rpc (KHRN, KHHN, KHAN, merge buffer with a make current prefix), but it is VC III:

- **ids**: no shared `0x7xxx` GL range (khdispatch `0x3c61e` only knows `0x1xxx` GL11, `0x2xxx` GL20, `0x3xxx` VG, `0x4xxx` EGL, `0x6xxx`). GLES 2 ids come from `khclient_vc3.dll`: glClear `0x202c`, glClearColor `0x2003`, glClearDepthf `0x2004`, glClearStencil `0x2005`. an unknown id is skipped word by word, so its args run as commands
- **arg counts** checked against the VC handlers: `0x4000` create surface 12 `{win, buffers, w, h, color 0xd20d, depth, mask, multi, largest, mipmap, sem, sem}`, `0x4002` 2, `0x4008` 8, `0x4009` flush and wait 2 (int reply), `0x400a` swap 5 `{surface, w, h, window, preserve}` **no reply**, `0x4012` wrap 5, `0x4019` global image 3
- **showing a window** (the Symbian GCE path): the EGL window is only a key in a VC handle table. the swap posts the finished buffer under it (khrn worker), DISP `0x1c` registers the same key as a GCE surface `{0x96000000 | id, key, vc image type 25 TF_RGB565, w, h}` (answer `(n << 28) | size`), DISP `0x1e` GCE element add `{update, surface, 0, key, display, layer, dst x y w h, src 16.16, 0, 0xff, 0}` shows it. window, swap and `0x1c` must use the same key

| step | message |
|---|---|
| context | KHRN `0x4002 {0, 1}` |
| window surface | KHRN `0x4000 {key, 1, 320, 240, 0xd20d, none, none, 0...}` |
| GCE surface | DISP `0x1c {0x96000001, key, 25, 320, 240}` |
| draw | make current + `0x2003` + `0x202c` + `0x4009 {1, 0}` |
| present | KHRN `0x400a {surface, 320, 240, key, 0}` |
| show | DISP update start, `0x1e`, submit |

### GLES 2 for userspace (works on the phone since 19.10.2026)

- `/dev/vc-khrn` (videocore.c): one ioctl moves a whole KHRN or DISP message, optional bulk pieces after it and one reply, served by the driver thread between frames. `VC_KHRN_IOC_DISPLAY` gives the dispman display. layout in `linux/vc-khrn/vc-khrn.h`
- `linux/vc-khrn/`: `libvcgl.so` (EGL 1.4 + GLES 2, `libEGL.so.1` and `libGLESv2.so.2` are links to it), `khrntest` (the magenta square from userspace), `vctri` (a turning triangle). built by `build.sh` with the pc's clang against the alpine sysroot (`linux/initramfs/sysroot.sh`), `MOCK=1` checks every message against the vc-main handler arg counts and runs on the pc under qemu-arm
- VC III GLES 2 rpc (khclient_vc3 + vc-main handlers): the client keeps the attrib state, every draw sends `{0x2076, mode, first, count}` or `{0x2074, mode, count, type, offset, ebo}` + 8 enables + per attrib `{size, type, normalized, stride, offset, buffer}` (enabled) or its current value. buffer data `0x203b {target, size, usage, -1}` + sub data `0x203c {target, offset, size, size}` + bulk, shader source `0x2064 {shader, count, workspace, 0}` + lengths + one bulk per string, uniforms `0x202f.. 0x2036`, vectors / matrices with the data in the message. answers that come as a bulk to the host (info logs, glGenBuffers, glReadPixels) are not done: names are given out by the client. textures: `0x2072` tex image without data + `0x2073` sub image rows + bulk (128 kb pieces), parameters `0x202d` / `0x202e`, bind `0x203a`
- one config: RGB565, no depth / stencil. one buffer per window. the panel is command mode, so every swap ends with dispman ElementModified + submit

Tools: `vc-main` disassembles with the vc4 binutils (itszor/binutils-vc4, `objdump -D -b binary -m vc4`, load address 0, gp `0xde5a0`). the exported symbol table (`{addr, name}` pairs near `0xe8xxx`) names the rtos and libc functions, the dispman command table is at gp+`0xe3e0` (`{handler, min, max args}` for ops `0..0x27`).

Path 2 (VC composition only): dispman `0x17` ElementChangeAttributes (vcdispman FUN_80346198): `{update, element, flags, layer, opacity, mask, transform, [dest rect], [src rect 16.16]}`, flags dest `1<<2`, src `1<<3`. moving the src rect = scrolling with no pixels on the bus (`videocore.disp_probe=1`, `e6-vc-khrn.dtb`).

The qemu VC model (`work/qt/run.py`, `E6_VC_STATS=1`) matches the phone's MMIO counts, but it is my own model: it only renders type `0x2b`, places writes by the dst rect (the phone does not) and says yes to every service. It checks the driver, not the VC.

The wire-level details (host bus registers, boot protocol, VCHI slots, bulk transfers, DSPC/DISP/BFMX message layouts) are in [`videocore-link.md`](videocore-link.md). This page is the overview.

## Stack

```text
Linux KMS (/dev/dri/card0) + /dev/vc-khrn          videocore.c
  |_ BFMX  vc_bufmanx: copy the frame into a VC resource (bulk)
  |_ DISP  dispman: display, resource, element, update/submit
  |_ UPDH  dispman update handler (opened, messages ignored)
  |_ DSPC  dispcommanddrv: panel config, power, DCS batches/reads
       |_ VCHI: 0x1020-byte slots, credits, CTRL service, BULX bulk
            |_ ViSSI host bus 0x20025800, 16-bit PIO, HAT/RUN
                 |_ BCM2727B1, booted with loader A + main image
                      |_ panel 01 8b 79, 640x480 over interface id 4
```

On Symbian the same layers are `vcdriver.dll`, `vchi.dll`, `vc_bufmanx`, `vcdispman*.dll`, `dispcommanddrv.dll` and `display_chipset_drv_ive_bc.dll`. The Linux driver puts all of them into one file and keeps only what the E6 needs.

## Bring-up sequence

The driver repeats what Symbian sends. The sequence was not guessed: it was captured by running Symbian's display stack from `kern.bin` in an emulator (see "How the sequence was obtained" below).

1. **VC boot.** Raise RUN (gpio 92). The VC sends a hello, the host sends `vc-loader-a` and gets an ack. The VC sends a second hello, the host sends `vc-main` and gets the final ack.
2. **VCHI.** Wait for the VC's CTRL connect, answer it, then open the services with the client flags `0x10`.
3. **DSPC install.** Panel config (cmd 3), then ping (cmd 5).
4. **Op 1, init.** Read panel registers `0a`..`0e`. Symbian branches on `0x0a` bit 2 (display on).
   - **Cold** (the driver default): power `0x41`, `0x40`, wait 245 ms; read the panel id (`dc db da`); config; power `0x81`, wait 51 ms; batch `[53 3f, 51 3f, 55 00, 11]` (sleep out), wait 120 ms; config; colour matrix (cmd 7, all zero); batch `[26 01, 36 00, 3a 70]`.
   - **Warm**: config; power `0x81`; backlight; mode; reads; op 8.
5. **Compose.** Run the dispman calls in this order:
   - `DisplayOpen(0)`
   - `ResourceCreate(0x2b, 640, 480)`
   - BFMX write
   - `UpdateStart`
   - `ElementAdd`
   - `SetBackground`
   - `SubmitSync`
6. **Display on.** Send batch `[29]`, read registers `0a`..`0e`, then op 8 three seconds later.
7. **Refresh, op 3.** The first refresh sends batch `[3a 70, 35 00, 34]`, every later one sends `[35 00]`. Each refresh then does the following:
   - BFMX write
   - `UpdateStart`
   - `ElementModified` with the full rectangle
   - `SubmitSync`

The cold path is required because Linux always boots the VC from scratch. After the VC boot the panel still reads `0a = 1c`, so Symbian's own check would pick the warm path. But nothing drives the panel at that point: its booster stays off and the screen stays black. The cold path power-cycles the panel through the VC and takes it out of sleep.

## Using it

1. Build the kernel, initramfs and DTB with [`linux/kernel/build-vc.sh`](../linux/kernel/build-vc.sh):
   ```sh
   KERN_BIN=/path/to/kern.bin linux/kernel/build-vc.sh
   ```
   The script enables `CONFIG_SOC_RAPUYAMA_VIDEOCORE`, builds libvcgl and the alpine/weston userspace (cached in `work/rootfs-alpine`). It also extracts the VideoCore images from your ROM dump into the initramfs (`/lib/firmware/vc/`) with [`tools/vc_extract.py`](../tools/vc_extract.py). The firmware is Nokia's and is not in this repository.
2. Boot `linux/kernel/build-vc/arch/arm/boot/Image` with `e6-vc-565.dtb` through `e6bfix.sis`.
3. Watch the result with `dmesg | grep -i videocore`. The interesting lines are:
   - `panel power-up: cold path`
   - `display up, 640x480`
   - `test pattern: 0`

   The panel first shows 8 colour bars for 3 seconds, then the console.

The kernel driver itself is in the kernel fork, branch `e6`, at `drivers/soc/rapuyama/videocore.c`.

Module parameters (`videocore.<name>=` on the kernel command line):

| parameter | default | meaning |
|---|---|---|
| `cold` | `1` | panel power-up path: `1` cold, `0` warm, `-1` choose by `0x0a` like Symbian |
| `brightness` | `0x3f` | DCS `0x51` value (Symbian uses `0x3f`) |
| `test_pattern` | `1` | colour bars for 3 s after bring-up |
| `matrix` | `0` | send an identity colour matrix instead of Symbian's zero one |
| `loader_b` | `0` | boot with loader B (rejected on the E6, kept for experiments) |
| `dma` | `0` | require gfx DMA after bring-up; `0` explicitly selects PIO |
| `rgb565` | `0` | push RGB565 instead of XRGB8888 |
| `khrn_probe` | `0` | open the khronos services and log the answer |
| `disp_probe` | `0` | move and scroll the screen element through dispman |
| `khrn_draw` | `0` | the VC clears a magenta 320x240 window for 10 s |
| `debug` | `1` | log VCHI traffic: `1` bring-up and first frames, `2` everything |

## How the sequence was obtained

1. Ghidra decompilation of the display modules from `kern.bin` was used for the structure: [`driver-re.md`](driver-re.md) describes the pipeline.
2. Static reading went wrong in places. Ghidra inverted a branch condition, and one state machine depends on what the panel answers. So the real Symbian code was run in a Unicorn harness: [`tools/vcemu/run_panel.py`](../tools/vcemu/run_panel.py).
3. The harness does the following:
   - Loads every module's `.data` from the ROM headers.
   - Runs the static constructors.
   - Stubs the EKA2 kernel: DFCs, timers, physical RAM and the VCHI service open.
   - Answers DSPC requests with `{token, 0}` and DCS reads with the E6 panel's values.
   - Logs every message the stack would send to the VideoCore.
4. `PANEL_0A=<hex>` sets the answer to the `0x0a` read. `00` gives [`symbian-display-sequence-cold.txt`](symbian-display-sequence-cold.txt), `9c` gives [`symbian-display-sequence-warm.txt`](symbian-display-sequence-warm.txt).
5. Timer ticks in those logs are milliseconds.

Lesson learned: the first capture answered `0x0a` with `9c` and therefore recorded the warm path. The resulting driver was "1:1" but still left the screen black. When you port the sequence, feed the emulator the values your real panel returns.

[`linux/initramfs/vcprobe.c`](../linux/initramfs/vcprobe.c) is the userspace `/dev/mem` probe used before the kernel driver existed. It is still useful to poke the bus or single DSPC commands by hand.

## Next steps

1. Blanking and power-off (Symbian op 2: `28 38 13`, `53 00 10`, power `0x80 0x41 0x40`).
2. A oneshot clockevent in timer-rapuyama.
3. Bulk transfers from the VC to the host (info logs, glReadPixels).
4. Find out why loader B is rejected (`1/1`). Symbian uses it at power-up.
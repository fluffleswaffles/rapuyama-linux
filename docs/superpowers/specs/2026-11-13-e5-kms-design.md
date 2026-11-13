# E5 KMS display, weston + foot, 16 bpp: design

Date: 2026-11-13. Status: agreed in conversation with the user, to be approved as written.
Part of the beta (3G + KMS; charging under Linux dropped by the user on 2026-11-13: charge in
Symbian). Order agreed: this design, then the merge plan with the author's main/e6, then the
beta release. The user weighed doing KMS after the merge and chose to do it now (3G is mostly
done), while preparing for the merge.

## Intent

The E5 display is an fbdev driver today (`drivers/video/fbdev/rapuyama-e5-fb.c`: XRGB8888 shadow,
DISPC + RFBI DMA push, panel sleep, brightness, fn-key picture overlay). fbdev is enough for the
console and X, but not for Wayland. The user wants to run **foot** on the phone's own screen, the
way the E6 already runs weston + foot on its KMS driver (`drivers/soc/rapuyama/videocore.c`,
display part). At the same time, issue #5 (JigokuMaster) showed that SDL 1.2 games built for
16 bpp run at half speed because `/dev/fb0` is 32 bpp only (21 vs 46 fps in SuperTux).

## Goals

1. **KMS driver for the E5 panel** (`/dev/dri/card0`), built on the E6 driver's pattern so the later
   merge stays simple: `drm_simple_display_pipe`, one fixed 320x240 mode, GEM shmem with a shadow
   plane, `FB_DAMAGE_CLIPS`, the row comparison against what is on screen (weston sends a full
   commit before each real one), no vblank IRQ (the atomic helpers fake the events), a normal
   backlight device. `/dev/fb0` and fbcon come from DRM's fbdev emulation
   (`drm_fbdev_shmem_setup`). Commits work the E6 way: a commit copies the changed rows into the
   driver's own buffer and wakes a push thread, which merges frames that arrive faster than the
   panel; commits do not wait for the ~18 ms push (so weston and the console never wait on it).
2. **Reuse the existing E5 hardware code**: the DISPC + RFBI DMA push (with the CPU path as fallback),
   panel sleep/wake (DCS 0x28/0x10, 0x11/0x29), the panel DCS brightness (WRDISBV 0x51), the ViSSI
   clock gating, the fn-key picture overlay.
3. **IRQ 146** (DISPC graphics interrupt, FRAMEDONE) instead of polling: today the push
   busy-waits with `readl_poll_timeout` for the whole ~18 ms of a full frame. The interrupt only
   tells the push thread that its push is done, which frees the CPU during pushes. It has never
   been used on the E5: verify it on the phone early and keep polling as a fallback behind a
   module parameter.
4. **16 bpp**: a boot-time option with the E6's name, `rgb565` (E6: `videocore.rgb565=1`). Off by
   default (32 bpp as today). When on: the plane offers RGB565 (XRGB8888 clients are converted, as
   on the E6), the driver's own buffer and the DISPC fetch are 16 bit (GFX format 6 = RGB16, panel
   still receives RGB888 over the 8-bit bus), and the fbdev emulation is created at 16 bpp, so SDL
   1.2 games (fbcon backend only, no KMS backend) get a native 16 bpp `/dev/fb0`.
   **The user's requirement: issue #5 must end up solved, not traded.** Under DRM's fbdev
   emulation, programs that draw into an mmap of `/dev/fb0` (SDL 1.2, X's fbdev driver) are
   flushed by deferred I/O with a fixed 50 ms delay (`drm_fbdev_shmem.c`: `fbdefio.delay = HZ /
   20`), against 10 ms in today's driver (`refresh_ms`, tuned on purpose): about 15-20 instead of
   about 35 screen updates a second. So this design includes a small DRM core change that lets a
   driver set its own deferred-I/O delay; the E5 driver sets 10 ms (the E6 can use it too after
   the merge). Result: 16 bpp SDL games at full speed and today's update rate.
   If the phone test shows the GFX plane cannot fetch format 6, `rgb565` is still built: a 16 bpp
   `/dev/fb0`, with the driver converting the changed rows to XRGB8888 for the DMA; it stays in the
   beta only if a phone measurement shows SuperTux faster than today (builder's call written here).
5. **weston + foot ready to start, not preinstalled** (like `ssh-server` / `tailnet`): a start
   script on the advanced rootfs. When weston or foot is missing it says
   `apk add weston foot` (plus whatever weston needs on Alpine). It starts weston (pixman, the E5
   keyboard layout from the existing `e5.xkb`, 320x240) with foot, and returns to the console when
   weston ends. Advanced setup only, like `browser`.
6. **Shorter welcome screen** (`linux/initramfs/e5-motd`, 18 lines today on a 40x30 console): the
   essentials plus one compact list of the services, e.g.

   ```
    Linux on the Nokia E5 (beta)

    wifi 'name' 'password'    connect
    apk add <package>         install
    brightness 50% | up | down
    poweroff | reboot         off | Symbian

    more: ssh-server  tailnet  mobile
          wayland  browser  font  blank
    (run one alone to see what it does)
   ```

   Commands that start something (`browser`, the weston start script) keep starting it when run
   alone; every listed command answers `<name> help` with a short explanation (some, like `blank`
   and `font`, need that added), and the hint line says so ("`<name> help` explains each"). The simple-mode screen (`e5-welcome`) gets the same
   treatment with only what simple mode has.

## Non-goals (this version)

- Tear-free updates (panel TE signal, DCS 0x35): a full-frame push (~18 ms over the 8-bit bus) is
  slower than the panel's 16.7 ms refresh, so it would only help partial updates; foot mostly
  changes a few lines. After the beta.
- The DISPC video overlay planes (VID1/VID2, YUV, scaler; found in `d_display.ldd`). After the beta.
- Charging under Linux (dropped from the beta by the user).
- A GPU: the E5 has none; weston uses pixman.

## Anti-goals (failure even if it "works")

- **Any regression of what works today**: the console (fbcon, fonts, `font`), `brightness`
  (`/sys/class/backlight/panel`), `blank` and screen-off on console blank with any key waking it,
  the end key toggle (e5-keyd `FBIOBLANK` on `/dev/fb0`), the fn-key picture
  (`/sys/class/graphics/fb0/device/overlay` + `overlay_on`, written by e5-keyd), X for `browser`,
  simple mode (whole system in RAM, console only).
- **Slower pushes or console**: a full frame stays about 18 ms; the extra copy KMS adds must not
  make console scrolling slower.
- **Background load**: no polling or work when nothing changes. The 3G runtime (e5rt) is
  sensitive to CPU latency (reset 9 investigation), so idle display = zero CPU.
- **Shipping weston/foot in the image**: they stay an `apk add` away.
- **Diverging from the E6 patterns without a reason**: names, helpers and structure follow
  `videocore.c` wherever the hardware does not force a difference.

## Constraints and known facts

- Hardware (hw-map.md, D4 notes): DISPC at `P:0x20024800`, RFBI at `P:0x20025800`, IRQ 146 = native
  display graphics (`0x80179054` binds it). Symbian's DISPC setup is kept (stall mode, GFX format 8,
  24 data lines). The ViSSI clock (power resource 0x18 = cgc gate 0x17) is on only during a push.
- Symbian's `d_display.ldd` selects RGB16 (format 6) on the video planes (`0x8017adf4`); that the
  **GFX** plane fetches format 6 correctly is still to be confirmed on the phone (first build
  step, a `devmem` test or a test build, before the 16 bpp code).
- Kernel config: `build-e5.sh` disables DRM today; this turns it back on. Measured cost of the DRM
  core on the cellular config: +418..463 KB kernel RAM, plus 0.2-0.6 MB of buffers (half with
  `rgb565`). The E5 has about 207 MB for Linux.
- Kernel 6.12 (`drm_simple_display_pipe`, `drm_fbdev_shmem_setup` as used by the E6 driver).
- Users of today's fbdev details (must keep working): e5-keyd (overlay sysfs path,
  `FBIOBLANK`), `e5-brightness.sh`, `e5-blank.sh` (consoleblank), `e5-xorg.conf` (X `fbdev`
  driver on `/dev/fb0`), `e5-font.sh` (console fonts).
- The beta kernel is the cellular build (VMSPLIT_3G25); KMS is built on that config.

## Left to the builder

- Whether the KMS driver replaces `rapuyama-e5-fb.c` in place or is a new file next to it (with
  the fbdev driver kept for one release as a fallback): pick what keeps the merge simple and the
  rollback cheap.
- Where the overlay attribute lives under DRM (the fbdev emulation's `fb0/device` should still
  point at the platform device, so the attribute can stay there; otherwise move it and update
  e5-keyd in the same change).
- How the end key / console blank behaves while weston holds the display (fbdev emulation does
  not blank while a DRM master is active): acceptable if it only works on the console, but it
  must not break or hang.
- X: switch `browser` to X's `modesetting` driver (damage rectangles via DIRTYFB, no deferred-I/O
  timer, one extra ~300 KB shadow copy) unless that costs more RAM or regresses something; with
  the 10 ms delay, `xf86-video-fbdev` on the emulated `/dev/fb0` stays a working fallback. Mind
  `DefaultDepth 24` in `e5-xorg.conf` when `rgb565` is on.
- The start script's name (`wayland` in the mockup), weston.ini details, foot font size, and
  how weston gets the E5 keymap.
- The exact form of the DRM deferred-I/O delay change (a field a driver sets before
  `drm_fbdev_shmem_setup`, or similar), kept small and upstream-shaped.
- The DMA source: GEM shmem buffers are not physically contiguous, so the push keeps the driver's
  own contiguous buffer, as today.
- Exact `rgb565` parameter plumbing (module parameter of the driver, set from the dtb bootargs or
  the kernel command line), and how the 16 bpp row conversion is written.
- Test approach: PC-side build checks; phone checks in one boot per batch (the user's rule), with
  scripts served from the PC.

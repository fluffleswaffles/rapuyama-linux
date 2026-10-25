
# Linux on Nokia E6

This directory contains the Linux kernel, board description, boot trampoline and Symbian-side loader for the Nokia E6 (RapuYama, ARM1176JZF-S / ARMv6K).

**Current milestone: no more 240-second reset (22.10.2026).** The `rapuyama-swd` bridge services the secure PA_SWD watchdog (`e6.swd=1`, boot with e6bfix), the phone ran 648 s with four successful calls (`../docs/modem-reset-findings.md`). The SD card and internal eMMC enumerate on the two MMC controllers.

**Wayland and GLES 2 (18-19.10.2026).** The VideoCore driver is a small KMS device: weston + foot run on the phone, with the TC3589x keypad and the maXTouch touchscreen. GLES 2 from userspace is rendered by the VideoCore's own GL server through `vc-khrn/libvcgl.so` and `/dev/vc-khrn` (details: `../docs/videocore-display.md`). The kernel is booted in place by the `e6bfix` / `e6calb` loader (`boot/README.md`).

**Display (14.10.2026).** The kernel boots the BCM2727B1 VideoCore with Nokia's firmware, powers the LCD panel up and shows fbcon on `/dev/fb0` (log: `../docs/first-display_SUCCESS.log`).

**First milestone: the phone boots into Linux userspace.** The successful boot log in `../docs/first-boot_SUCCESS.log` shows the timer interrupt being handled, the RapuYama clocksource becoming active, and BusyBox `/init` starting.
The USB gadget configuration exposes an interactive userspace shell over USB serial; the project owner has confirmed terminal input/output with picocom.

Start here:

- [`kernel/README.md`](kernel/README.md) - toolchain and kernel build, including the display build `build-vc.sh`.
- [`boot/README.md`](boot/README.md) - DTBs, boot files, loaders, and USB shell.
- `initramfs/` - BusyBox init, alpine userspace with weston (`alpine.sh`), xkb layout, sysroot for `vc-khrn/build.sh`.
- `yamashell/` - Wayland compositor with VideoCore composition; see [GPU Wayland](../docs/wayland-gpu.md).
- `vc-khrn/` - `libvcgl` (EGL + GLES 2) and the test programs `khrntest`, `vctri`.
- [`../docs/driver-re.md`](../docs/driver-re.md) - hardware and driver findings.
- [`../docs/videocore-display.md`](../docs/videocore-display.md) - the VideoCore display driver.
- [`../docs/porting-guide.md`](../docs/porting-guide.md) - porting all this to another phone.
- [`../README.md`](../README.md) - project overview and top-level instructions.

The port is at an early bring-up stage: shell, weston, GLES 2, keypad and touchscreen work, but there is no blanking or backlight control, storage is read-only for now, and radio, audio and power management are not supported.

# rapuyama-linux

<img src="docs/rapuyama-linux.png" alt="logo" width="200">

**A Linux bring-up for the RapuYama hardware platform replacing Symbian at runtime.**

This project takes over the phone from a running Symbian installation, hands control to an ARM Linux kernel, and brings up the RapuYama application SoC.
It combines Symbian-side loaders and hardware probes, reverse engineering of Nokia's firmware, and experimental Linux drivers.

> **Current status: Linux runs Wayland and GLES 2 on the phone.** The kernel boots the Broadcom VideoCore itself, powers the LCD panel up and drives it as a small KMS device: weston + foot run on the phone's own display, typed on the hardware keypad, with a working touchscreen. Userspace GLES 2 is rendered by the VideoCore's own GL server through `libvcgl` (a turning triangle at 170-250 fps, textures work). An interactive shell also runs over USB serial.

The former 240-second reset is **fixed on the Nokia E6**: Linux now services the secure PA_SWD watchdog. Kernel #196 completed four successful calls through 648 seconds on hardware ([console log](docs/secure-watchdog_SUCCESS.txt), [fix and boot instructions](docs/modem-reset-findings.md#use-the-confirmed-fix)). Enable it with `e6.swd=1`; it is currently opt-in.

## Milestones

| Date | Milestone |
| --- | --- |
| 13.10.2026 | Kernel boots through to BusyBox `/init` ([log](docs/first-boot_SUCCESS.log)) |
| 13.10.2026 | Interactive shell over the USB gadget serial port |
| 14.10.2026 | First image on the E6 panel from Linux: VideoCore booted by the kernel, `/dev/fb0` + fbcon ([log](docs/first-display_SUCCESS.log), [notes](docs/videocore-display.md)) |
| 18.10.2026 | `e6bfix` loader boots the uncompressed `Image` in place, big kernels boot every time ([notes](linux/boot/README.md#e6bfix-boot-the-image-in-place-18102026)) |
| 18.10.2026 | Wayland on the phone: weston + foot on the VideoCore KMS driver, gfx DMA + RGB565 (~9-16 fps) ([notes](docs/videocore-display.md#wayland-runs-on-the-phone-since-18102026)) |
| 18.10.2026 | Hardware keypad: TC3589x on I2C, own polled driver + E6 xkb layout |
| 19.10.2026 | First frame rendered by the VideoCore GL server, then GLES 2 from userspace through `libvcgl` + `/dev/vc-khrn` ([notes](docs/videocore-display.md#drawing-on-the-videocore-works-since-19102026)) |
| 19.10.2026 | Touchscreen: Atmel maXTouch on I2C, multitouch |
| 22.10.2026 | SD card and internal eMMC work under Linux, btrfs rootfs on the card ([notes](docs/driver-re.md#sd--mmc-epbusmvdll)) |
| 22.10.2026 | 240-second reset fixed: PA_SWD secure watchdog serviced from Linux, four successful calls through 648 s ([log](docs/secure-watchdog_SUCCESS.txt)) |
| 01.11.2026 | First sound during a real outgoing modem call, with the sandboxed ISASW runtime |

## The hardware

| Component | What we know |
| --- | --- |
| Application CPU | ARM1176JZF-S, ARMv6K |
| SoC | Nokia RapuYama |
| RAM | 256 MiB at physical `0x10000000` |
| Display | Broadcom BCM2727B1 / VideoCore III, 640x480 panel (id `01 8b 79`), driven over the ViSSI host bus |
| Input | Toshiba TC3589x keypad (I2C `0x44`), Atmel maXTouch touchscreen (I2C `0x4a`) |
| Storage | removable microSD + internal HYNIX eMMC on two TI OMAP style MMC controllers |
| Boot environment | Symbian^3 / EKA2, with a one-way handoff to Linux |
| Kernel | Linux 6.12, built for ARMv6 |

This is a device-specific port, not a generic EKA2 compatibility layer. Some work may carry over to other phones using the same SoC, but each model still needs its own board description, memory and boot validation, and peripheral bring-up.

## What is here

- **Symbian takeover and probes** - inspect memory mappings and peripheral registers, run small ARM payloads, and transfer control to Linux.
- **Linux kernel port** - an ARMv6 kernel configuration plus experimental RapuYama support for interrupt handling, timers, GPIO, clocks, watchdog, UART, and other peripherals.
- **VideoCore display driver** - boots the BCM2727B1 with Nokia's firmware (extracted from your own ROM dump), speaks VCHI to it, powers the panel up like Symbian and exposes a KMS device (`/dev/dri/card0`, `/dev/fb0` with fbcon) for weston.
- **GLES 2 on the VideoCore** - `/dev/vc-khrn` in the display driver passes KHRN/DISP messages to the VideoCore's own GL server, `linux/vc-khrn/libvcgl.so` is the EGL 1.4 + GLES 2 client library for it.
- **Input drivers** - polled TC3589x keypad and maXTouch touchscreen drivers, keypad layout in `linux/initramfs/xkb-e6`.
- **Secure watchdog bridge** - services the PA_SWD watchdog from Linux so the phone no longer resets after 240 seconds.
- **Symbian display-stack emulator** - runs Nokia's display drivers from the ROM in Unicorn and records every message they send to the VideoCore; the Linux driver replays those sequences.
- **RAM-backed early logging** - preserve early kernel output in reserved RAM so it can be retrieved after a failed boot, even without a serial console.
- **Hardware notes** - reverse-engineering results from firmware analysis and live probes.

The basic boot, timer/IRQ path, initramfs userspace, USB serial shell, the VideoCore display with weston and GLES 2, keypad, touchscreen, MMC enumeration and the secure watchdog bridge have been exercised on the phone. Other devices and drivers remain experimental, their presence in the tree does not imply that they have been validated on hardware.

Ideas for future development, including running Symbian, Java ME and older Android applications on Linux, are in [future ideas](docs/future-ideas.md).

## Build the Symbian loaders and probes

Use the Symbian build container for GCCE,
elf2e32_next and SIS packaging without installing the legacy tools on the host.

## Build the kernel

On a Linux host, install an ARM GNU EABI cross-toolchain (`arm-linux-gnu-gcc` and binutils), then initialize the kernel submodule and build:

```sh
git submodule update --init --depth 1 linux/kernel/linux
cd linux/kernel
./build-kernel.sh e6config
./build-kernel.sh build
```

The kernel image is written to:

```text
linux/kernel/build/arch/arm/boot/zImage
```

The build script uses `ARCH=arm`, `CROSS_COMPILE=arm-linux-gnu-`, and all available CPU cores by default. `e6config` installs the checked-in E6 configuration, `build` builds the kernel image and the kernel tree's DTBs.

## Build the Nokia E6 device tree

The board DTB used by the E6 loader comes from `linux/boot/e6.dts`. It is separate from the DTBs built by the kernel target above. After building the kernel (which also builds the in-tree `dtc` compiler), run this from the repository root:

```sh
linux/kernel/build/scripts/dtc/dtc -I dts -O dtb -o linux/boot/e6.dtb linux/boot/e6.dts
```

The resulting `linux/boot/e6.dtb` is the board description to pair with the kernel image. If you change the DTS, rebuild this file before deploying it.

## Boot flow

```text
Symbian
  |_ e6bfix.sis / e6calb.sis
       |_ allocates the biggest free physical block
       |_ loads E:\Image (uncompressed) at block + 0x8000, E:\e6.dtb at block + 0x1f00000
       |_ prepares the handoff (MMU and caches off)
       |_ jumps to the Linux ARM kernel
            |_ early log is captured in reserved RAM
```

The kernel runs in place where it was loaded; with `e6.lowmem=1` the RAM below it is used too ([details](linux/boot/README.md#ram-below-the-kernel-22102026)). Use `e6bfix.sis`: `e6calb.sis` (the same loader plus a wait for Symbian's PA_SWD phase, up to ~3.5 min) is not needed any more and makes Linux slower, because Symbian drops its clock / performance level while it waits and Linux keeps that. The older `e6boot.sis` (zImage, fixed decompression address) still exists but big kernels die at random boots with it.

The phone-side package and boot sequence are documented in [`linux/boot/README.md`](linux/boot/README.md). That guide includes the expected files, recovery/diagnostic notes, and the distinction between normal and probe images. Do not mistake a diagnostic probe image for the regular kernel.

For the interactive USB shell, use the USB-enabled DTB as `E:\e6.dtb`, connect the phone to the host over USB, and start the boot package. On a Linux host, connect to the enumerated gadget serial port, typically:

```sh
picocom -b 115200 /dev/ttyACM0
```

The shell runs on the phone's `ttyGS0`. `picocom` provides the host-side terminal. The USB-enabled DTB opts into this shell, while the base DTB uses the non-interactive init path.

## Build with the display driver

`build-kernel.sh` builds the plain USB-shell kernel. For the display, use `build-vc.sh`. It needs a dump of the phone's kernel ROM (`kern.bin`, see [ROM analysis](docs/rom-analysis.md)), because the VideoCore firmware is extracted from it at build time and is not part of this repository:

```sh
KERN_BIN=/path/to/kern.bin linux/kernel/build-vc.sh
```

Daily set: `linux/kernel/build-vc/arch/arm/boot/Image` + `linux/kernel/build-vc/e6-vc-565.dtb` (gfx DMA + RGB565) as `E:\Image` / `E:\e6.dtb`, booted with `e6bfix.sis`. The script also builds `e6-vc.dtb` (XRGB8888), `e6-vc-khrn.dtb` and `e6-vc-gl.dtb` (VideoCore GL checks). Driver parameters, weston, GLES 2, the bring-up sequence and known issues are in [`docs/videocore-display.md`](docs/videocore-display.md).

GLES 2 userspace (`libvcgl`, `khrntest`, `vctri`) is built by `linux/vc-khrn/build.sh` against the alpine sysroot from `linux/initramfs/sysroot.sh`.

## Repository map

| Path | Contents |
| --- | --- |
| [`linux/kernel/`](linux/kernel/) | Kernel configuration, build script, and kernel source submodule |
| [`linux/boot/`](linux/boot/) | E6 DTS, trampoline, and boot documentation |
| [`linux/initramfs/`](linux/initramfs/) | Initramfs: BusyBox init, alpine armhf userspace with weston, xkb layout, sysroot script |
| [`linux/vc-khrn/`](linux/vc-khrn/) | `libvcgl` (EGL + GLES 2 over `/dev/vc-khrn`) and its test programs |
| [`linux/stage0/`](linux/stage0/) | Symbian-side loader and low-level experiments |
| [`driver/`](driver/) | Symbian kernel drivers, SIS packaging, and deployment notes |
| [`tools/`](tools/) | Log parsers and reverse-engineering utilities |
| [`docs/`](docs/) | Hardware maps, driver notes, investigation reports, and the porting guide |

## Notes and references

- [Linux bring-up notes](linux/README.md)
- [Kernel build guide](linux/kernel/README.md)
- [Boot pipeline and phone-side instructions](linux/boot/README.md)
- [RapuYama driver reverse engineering](docs/driver-re.md)
- [Hardware memory map](docs/hw-memory-map.md)
- [ROM analysis notes](docs/rom-analysis.md)
- [VideoCore display: overview and driver](docs/videocore-display.md)
- [VideoCore link: wire-level notes](docs/videocore-link.md)
- [Porting guide for other RapuYama / VideoCore phones](docs/porting-guide.md)

## Current boundaries

- This is an early userspace bring-up, not yet a complete everyday phone OS.
- Pushing pixels is slow: the ViSSI bus does ~7 MB/s, so weston (pixman) gets ~9-16 fps in RGB565 and the VideoCore ignores partial write rects. Fast UI has to be rendered on the VideoCore (GLES 2). `libvcgl` has one config (RGB565, no depth/stencil), one buffer per window and no VC-to-host bulk transfers yet (no glReadPixels, info logs). Blanking/backlight control are not implemented. The display comes up a few seconds into boot, so use the USB shell or the RAM log for early messages.
- Storage: the SD card and the internal eMMC work. The card can hold a btrfs rootfs (`tools/sdroot.sh`, `e6.root=`, see [the boot guide](linux/boot/README.md#root-filesystem-on-the-sd-card-22102026)), with zram + a swapfile. The internal eMMC is Symbian's `E:` drive - never write to it.
- RAM: Linux gets ~175 MB of the 256 MB (`e6.lowmem=1`, the rest is Symbian's kernel, the modem and reserved areas), see [the boot guide](linux/boot/README.md#ram-below-the-kernel-22102026).
- The PA_SWD bridge is opt-in (`e6.swd=1`) and validated only with the E6 firmware context (e6bfix / e6calb loaders). The normal build does not enable modem/radio/audio support; the experimental ISI runtime has separate requirements and limitations ([current status](linux/isi-runtime/README.md)).
- Treat register addresses and undocumented peripheral behavior as hypotheses unless the notes explicitly say they were checked on live hardware.
- A successful boot does not imply that every peripheral or power-management path is implemented.
- The boot handoff is one-way. A failed run may require the phone's watchdog reset to recover.
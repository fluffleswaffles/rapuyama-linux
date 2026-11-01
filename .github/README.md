# Nokia E5-00 (RM-632) port

This is the `e5-port` branch. The original E6 project README is [README.md](../README.md).

This is a port of rapuyama-linux to the Nokia E5-00, firmware 101.003 (Symbian 9.3). The E5 has the same RapuYama SoC as the E6. It differs in the timer, display, keyboard and secure-watchdog addresses, so it gets its own loader build, device tree and drivers. The E6 files and build defaults are left unchanged.

The hardware facts and on-device measurements are in [docs/devices/e5-00/hw-map.md](../docs/devices/e5-00/hw-map.md).

## Status

Tested on the phone:

| Part | State |
| --- | --- |
| Boot | `e5bot3` loader hands off to Linux. The RAM log at `0x1f000000` survives a warm reset |
| Shell | BusyBox over USB serial (`ttyGS0`), and on the screen with the keypad (`tty1`) |
| System tick | IRQ 35, 1 kHz (`timer-rapuyama-e5.c`) |
| Watchdogs | SoC/PMIC watchdog with the E6 driver. PA_SWD secure watchdog via the bridge's `nokia,e5` entry (`e6.swd=1`). Linux stays up past the old 64-150 s reset |
| Memory | `e6.lowmem=1` gives back the RAM below the loader block (about 178 MB total) |
| Display | 320x240 fbdev + fbcon over RFBI, CPU path (`rapuyama-e5-fb.c`) |
| Keyboard | Polled 8x8 matrix (`rapuyama-e5-keypad.c`). Console layers for sym, fn and sticky ctrl/alt (`tools/e5-kmap.py` -> `linux/initramfs/e5.kmap`) |

Not done yet: microSD (one MMC controller at `0x20032800`, IRQ 173), charging, audio, WLAN, modem. Ctrl doesn't work on the test phone because its key contact is dead.

## Files

| Path | What |
| --- | --- |
| `linux/boot/e5.dts` | Device tree, self-contained (does not include `e6.dts`) |
| `linux/kernel/build-e5.sh` | Kernel + dtb build into `linux/kernel/build-e5/` |
| `linux/stage0/symbian-loader/e5boot_ldd.cpp` | Loader LDD for the 9.3 kernel (`AllocBoot`, `LoadChunk`, `Boot`, `ReadPhys`, `SaveIsah`) |
| `linux/stage0/symbian-loader/boot_app.cpp` | Loader app, E5 parts under `RB_E5` / `RB_E5_ISAH` |
| `linux/stage0/symbian-loader/e5*_app.cpp`, `e5*_ldd.cpp` | Read-only probes used for the bring-up (ids, RAM map, ROM, timers) |
| `driver/mkstage-e5.sh` | Header tree for the 9.3 kernel (`driver/stage-e5/`) |
| `tools/e5-kmap.py`, `tools/parse_isah.py`, `tools/dram_dump_isasw.py` | Keymap generator, handoff block decoder, ISASW dump helper |
| Kernel (`linux/kernel/linux`, branch `e5`) | `timer-rapuyama-e5.c`, `rapuyama-e5-fb.c`, `rapuyama-e5-keypad.c`, E5 entry in `mach-rapuyama/secure_wdt.c` |

## Build

Set up the toolchain, submodules and `work/dl/busybox-armv5l` first, as for the E6 (see the [main README](../README.md)).

The 9.3 kernel lacks two `DObject` virtuals that the ^3 headers declare, so the E5 builds use their own header tree:

```sh
driver/mkstage-e5.sh
```

Loader (`e5bot3`, uid `0xe1000062`). The phone only accepts a SHA-1 signing certificate, so pass yours with `CERT=`:

```sh
cd linux/stage0/symbian-loader
STAGE=$PWD/../../../driver/stage-e5 LDD_SRC=e5boot_ldd.cpp NO_ISC=1 NO_BLOBS=1 \
CERT=/path/to/cert_sha1.pem RB_DEFS="-DRB_IMAGE_BOOT -DRB_E5 -DRB_E5_ISAH" \
./build_app.sh e5bot3 boot_app.cpp 0xe1000062
```

RAM log reader (`e5logs`, uid `0xe1000056`, writes `E:\klog.bin`): same variables, but `RB_DEFS="-DRB_E5"` and `./build_app.sh e5logs ramread_app.cpp 0xe1000056`.

Kernel and dtb:

```sh
linux/kernel/build-e5.sh          # -> linux/kernel/build-e5/arch/arm/boot/Image, build-e5/e5.dtb
```

## Boot

1. Copy `Image` and `e5.dtb` to the memory card as `E:\Image` and `E:\e5.dtb`.
2. Install `out/e5bot3.sis`, plug in the USB cable, so Symbian leaves the USB block powered.
3. Start the loader. It waits 5 s, then boots without asking.
4. The shell appears on the screen and on the PC as a USB serial port (e.g. `picocom /dev/ttyACM0`).

After a reset, run `e5logs` before taking the battery out, then read `E:\klog.bin` (`tools/parse_klog.py`).

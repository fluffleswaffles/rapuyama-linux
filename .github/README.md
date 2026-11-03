# Nokia E5-00 (RM-632) port

This is the `e5-port` branch. The original E6 project README is [README.md](../README.md).

This is a port of rapuyama-linux to the Nokia E5-00, firmware 101.003 (Symbian 9.3). The E5 has the same RapuYama SoC as the E6. It differs in the timer, display, keyboard, WLAN and secure-watchdog details, so it gets its own loader build, device tree and drivers. The E6 files and build defaults are left unchanged.

The hardware facts and on-device measurements are in [docs/devices/e5-00/hw-map.md](../docs/devices/e5-00/hw-map.md).

## Status: alpha

The alpha is a usable console phone: the phone's own screen and keyboard, USB and WLAN networking, Alpine's `apk`, the battery level and screen power management. Linux runs from RAM after a handoff from Symbian. The internal flash is never written. A reboot returns to Symbian.

To try it, see [Install (simple mode)](#install-simple-mode): three files on the memory card, nothing to format.

Tested on the phone:

| Part | State |
| --- | --- |
| Boot | `e5bot3` loader hands off to Linux. The RAM log at `0x1f000000` survives a warm reset |
| Shell | On the screen with the keypad (`tty1`) and over USB serial (`ttyGS0`). Ctrl-C works on both |
| System tick | IRQ 35, 1 kHz (`timer-rapuyama-e5.c`) |
| CPU idle | `e6.idle=1`: Symbian's shallow WFI between interrupts (`mach-rapuyama/idle.c`, the E5 `power.dll` has the same sequence) instead of `nohlt` |
| Watchdogs | SoC/PMIC watchdog with the E6 driver. PA_SWD secure watchdog via the bridge's `nokia,e5` entry (`e6.swd=1`). Linux stays up past the old 64-150 s reset |
| Memory | `e6.lowmem=1` gives back the RAM below the loader block (about 178 MB total) |
| Display | 320x240 fbdev + fbcon over RFBI (`rapuyama-e5-fb.c`). DISPC + RFBI DMA push, as Symbian does it: a full frame takes about 20 ms with the CPU free, instead of 36-57 ms of CPU byte writes, and console scrolling is twice as fast. `rapuyama_e5_fb.dma=0` on the kernel command line, or `echo 0 > /sys/module/rapuyama_e5_fb/parameters/dma`, goes back to the CPU path. The v0.01-e5 release still uses the CPU path |
| Brightness | Panel DCS backlight, `/sys/class/backlight/panel` and the `brightness` command |
| Screen off | The console blanks after 60 s without a key press (`consoleblank=60`): panel sleep, screen and keyboard light off. Any key wakes it |
| Keyboard | Polled 8x8 matrix (`rapuyama-e5-keypad.c`). Console layers for sym, fn and sticky ctrl/alt (`tools/e5-kmap.py` -> `linux/initramfs/e5.kmap`) |
| Keyboard light | Gazoo PMIC PWM 0, `/sys/class/backlight/gazoo-pwm0` (0..127) |
| microSD | MMC controller `0x20032800`, IRQ 173, clock gates through the E6 `cgc` driver (the E5 gate table is the same). PIO, about 5 MB/s |
| Rootfs | Alpine armhf on the card's second partition (btrfs, `e6.root=/dev/mmcblk0p2`): apk, nano, tmux, htop, python3, git, ssh, 1 GiB swapfile. Without it (simple mode) Linux runs from the initramfs in RAM, with WLAN and `apk` |
| Fonts | `font list / set / next` switches the console font (kbd + Terminus, e.g. `ter-112n` = 53x20). Built-in kernel fonts for `fbcon=font:` in the dtb |
| USB ethernet | `g_cdc`: the serial shell plus CDC ECM `usb0`, DHCP from the PC |
| WLAN | TI WL1251 on SPI 2 (`spi-rapuyama.c`). The firmware and the phone's own MAC address are copied out of its ROM by LinuxE5 to `E:\linux\`, the Image contains no Nokia files. WPA2, DHCP and NTP, power save off |
| Battery | Voltage, temperature and capacity from the Gazoo ADC with the E5's own calibration (`rapuyama-battery.c`). The capacity follows Symbian's battery table. Clean power-off when the average voltage stays below 3.3 V (Symbian's limit) |

Not done:

- Charging under Linux. Linux does not program the bq2415x charger. Charge in Symbian, then start Linux. With the cable in, the battery reads "Not charging" and keeps draining.
- Cellular modem, calls, SMS (planned for v2), audio, camera, Bluetooth, suspend to RAM, the power key (it belongs to the modem side).
- Ctrl doesn't work on the test phone because its key contact is dead (the sticky ctrl layer covers it).
- The advanced install (Alpine on the card's own partition, changes are kept) has no installer yet. Developers can make the card by hand, see [SD card rootfs](#sd-card-rootfs).

## Install (simple mode)

Simple mode runs Linux entirely in RAM. It doesn't format or change the memory card, and nothing is saved: every boot starts fresh.

You need:

- A Nokia E5-00 with firmware 101.003 and a memory card in it (E:).
- A hacked phone: RomPatcher+ with the Install Server patch applied (auto-apply on). LinuxE5 is self-signed but needs all capabilities (it loads its own kernel driver and reads the ROM), and an unpatched Symbian 9.3 refuses to install it. After a battery pull, check in RomPatcher+ that the patch is active again before installing. The hack tools are third-party software, so use a phone without personal data on it.

1. Charge the phone in Symbian. Linux doesn't charge the battery.
2. Copy `LinuxE5.sis`, `Image` and `e5.dtb` from the release to the root of the memory card (E:\), with a card reader or USB mass storage.
3. Install `LinuxE5.sis` on the phone (open it from the card in the file manager). When the installer asks where, choose the phone memory (C:), not the memory card.
4. Start LinuxE5 from the menu. The first start copies the WLAN firmware and the phone's MAC address out of its own ROM into `E:\linux\`. LinuxE5 waits 5 s, then Linux boots.
5. The screen shows a welcome with the commands below, then a shell on the phone's keyboard.

```sh
wifi 'network name' 'password'   # connect (WPA/WPA2) and set the clock
wifi                             # status
apk add fastfetch                # install a package (into RAM)
```

The network and installed packages are gone after a reboot, so run `wifi` again each time. `apk` needs the clock, which `wifi` sets over NTP.

`reboot` goes back to Symbian, `poweroff` turns the phone off. To remove it, uninstall LinuxE5 in Symbian and delete `Image`, `e5.dtb` and `E:\linux\`.

## Files

| Path | What |
| --- | --- |
| `linux/boot/e5.dts` | Device tree, self-contained (does not include `e6.dts`). The bootargs are documented in it |
| `linux/kernel/build-e5.sh` | Kernel + dtb build into `linux/kernel/build-e5/`, writes the E5 initramfs list |
| `linux/initramfs/init.sh` | Shared with the E6; the E5 parts are keyed on `nokia,e5` in the dtb (keymap, font, brightness, USB/WLAN networking, login shells) |
| `linux/initramfs/e5-brightness.sh`, `e5-font.sh`, `e5-blank.sh`, `e5-wifi.sh`, `e5-dhcp.sh`, `e5.kmap` | `brightness`, `font`, `blank` and `wifi` commands, the DHCP script (`usb0`, and `wlan0` in simple mode), the console keymap |
| `linux/initramfs/e5-welcome`, `e5-motd` | Screen welcome in simple mode, login message on the card rootfs |
| `linux/initramfs/e5-browser.sh`, `e5-xorg.conf` | `browser` command and the X config for the optional [graphical browser](#graphical-browser) |
| `tools/sdroot.sh` | Memory card with the Alpine rootfs (`... e5`) |
| `linux/stage0/symbian-loader/e5boot_ldd.cpp` | Loader LDD for the 9.3 kernel (`AllocBoot`, `LoadChunk`, `Boot`, `ReadPhys`, `SaveIsah`) |
| `linux/stage0/symbian-loader/boot_app.cpp` | Loader app, E5 parts under `RB_E5` / `RB_E5_ISAH` |
| `linux/stage0/symbian-loader/e5*_app.cpp`, `e5*_ldd.cpp` | Read-only probes used for the bring-up (ids, RAM map, ROM, timers) |
| `driver/mkstage-e5.sh` | Header tree for the 9.3 kernel (`driver/stage-e5/`) |
| `tools/e5-kmap.py`, `tools/parse_isah.py`, `tools/dram_dump_isasw.py` | Keymap generator, handoff block decoder, ISASW dump helper |

Kernel (`linux/kernel/linux`, branch `e5`):

| File | What |
| --- | --- |
| `drivers/clocksource/timer-rapuyama-e5.c` | IRQ 35 system tick |
| `drivers/video/fbdev/rapuyama-e5-fb.c` | RFBI framebuffer (DISPC DMA push, CPU fallback), panel brightness, panel sleep on blank |
| `drivers/input/keyboard/rapuyama-e5-keypad.c` | Keyboard matrix |
| `drivers/spi/spi-rapuyama.c` | SPI 2 controller for the WL1251 |
| `drivers/video/backlight/rapuyama-e5-bl.c` | Gazoo light PWM (keyboard light) |
| `drivers/power/supply/rapuyama-battery.c` | E5 entry: calibration, capacity table, low-battery power-off |
| `arch/arm/mach-rapuyama/secure_wdt.c` | PA_SWD bridge, E5 entry |
| `arch/arm/mach-rapuyama/idle.c` | Shallow WFI idle, enabled for the E5 too |
| `drivers/gpio/gpio-rapuyama.c`, `drivers/net/wireless/ti/wl1251/spi.c` | GPIO IRQ polarity fix, WL1251 IRQ edge from the DT |
| `drivers/watchdog/rapuyama_wdt.c` | `rapuyama_pmic_update()` for the light PWM |

## Build

Set up the toolchain, submodules and `work/dl/busybox-armv5l` first, as for the E6 (see the [main README](../README.md)).

The 9.3 kernel lacks two `DObject` virtuals that the ^3 headers declare, so the E5 builds use their own header tree:

```sh
driver/mkstage-e5.sh
```

Loader (LinuxE5, uid `0xe1000064`, own menu name and icon). The phone only accepts a SHA-1 signing certificate, so pass yours with `CERT=`:

```sh
cd linux/stage0/symbian-loader
STAGE=$PWD/../../../driver/stage-e5 LDD_SRC=e5boot_ldd.cpp NO_BLOBS=1 \
CERT=/path/to/cert_sha1.pem RB_DEFS="-DRB_IMAGE_BOOT -DRB_E5 -DRB_E5_ISAH" \
CAPTION="LinuxE5" ICON=$PWD/../../../driver/sis/linuxe5.svg \
./build_app.sh linuxb boot_app.cpp 0xe1000064
```

RAM log reader (`e5logs`, uid `0xe1000056`, writes `E:\klog.bin`): same variables, but `RB_DEFS="-DRB_E5"` and `./build_app.sh e5logs ramread_app.cpp 0xe1000056`.

WLAN firmware. It is not in the repo. Cut it from a dump of your E5 ROM (`Sys\Bin\wlanhwinit.dll`, firmware 101.003; the offsets are file offsets in a dump that starts at ROM address `0x80000000`):

```sh
mkdir -p fw/ti-connectivity
dd if=e5rom.dmp of=fw/ti-connectivity/wl1251-fw.bin bs=1 skip=$((0x92eb73)) count=200460
{ dd if=e5rom.dmp bs=1 skip=$((0x92e6fe)) count=729; head -c 3 /dev/zero; } > fw/ti-connectivity/wl1251-nvs.bin
```

The firmware is `Rev 4.0.4.12.12` (200460 bytes) and the nvs is 732 bytes. This is only for a development Image with both built in. LinuxE5 cuts the same bytes from the phone's own ROM to `E:\linux\`, which is what a release Image uses.

Kernel and dtb:

```sh
E5_WLAN_FW=/path/to/fw linux/kernel/build-e5.sh   # -> build-e5/arch/arm/boot/Image, build-e5/e5.dtb
```

Release Image, without Nokia files (WLAN firmware from `E:\linux\` at boot):

```sh
cd linux/kernel
KOUT=$PWD/build-e5-pub E5_WLAN_FW=/none ./build-e5.sh config
KOUT=$PWD/build-e5-pub ./build-e5.sh build
```

The initramfs gets `wpa_supplicant`, `iw` and `apk` from Alpine (cached in `work/rootfs-e5-simple`), so simple mode has WLAN and packages without a rootfs.

Release files: `tools/e5-release.sh out/linuxb.sis` builds this Image (with a neutral `user@host` in the version string), checks that no firmware, saved network, MAC address or path of the build PC is in it, and writes `LinuxE5.sis`, `Image`, `e5.dtb` and `SHA256SUMS` to `out/e5-release/`.

## SD card rootfs

`tools/sdroot.sh /dev/sdX e5` **erases the whole card** (back up E: first). It makes p1 FAT32 2 GiB (Symbian's E:, still mounted by Symbian) and p2 btrfs with Alpine plus the E5 initramfs files. It needs a finished `linux/kernel/build-e5.sh` (for `build-e5/initramfs.list`). Then copy `Image` and `e5.dtb` to p1.

On a card made before an update, copy the changed scripts by hand: `linux/initramfs/init.sh` to p2 as `/init`, `e6sh.sh` as `/bin/e6sh`, `e5-brightness.sh` as `/bin/brightness`, `e5-font.sh` as `/usr/bin/font` (all mode 755).

## Boot

1. Charge the phone in Symbian.
2. Copy `Image` and `e5.dtb` to the memory card as `E:\Image` and `E:\e5.dtb`.
3. Install `out/linuxb.sis` (LinuxE5). For the USB serial shell, plug in the USB cable first, so Symbian leaves the USB block powered.
4. Start the loader. It waits 5 s, then boots without asking.
5. The shell appears on the screen and on the PC as a USB serial port (e.g. `picocom /dev/ttyACM0`, then press Enter).

`reboot` goes back to Symbian, `poweroff` turns the phone off. After a crash, run `e5logs` before taking the battery out, then read `E:\klog.bin` (`tools/parse_klog.py`).

## Using it

| Command | What |
| --- | --- |
| `wifi 'name' 'password'`, `wifi`, `wifi off` | WLAN, see [WLAN](#wlan) |
| `apk add <package>` | Install from Alpine (in simple mode into RAM, until the next boot) |
| `blank 5`, `blank off` | Screen-off delay in minutes |
| `brightness`, `brightness 40%`, `brightness up` | Screen brightness, kept in `/etc/e5-brightness` |
| `font list`, `font set ter-112n` | Console font, kept in `/etc/e5-font` |
| `echo 0 > /sys/class/backlight/gazoo-pwm0/brightness` | Keyboard light off (127 = full) |
| `printf '\033[9;5]' > /dev/tty1` | Screen-off delay for this boot, in minutes (0 = never). The default is in `consoleblank=` in the dtb (seconds) |
| `cat /sys/class/power_supply/battery/{capacity,voltage_now,temp,status}` | Battery |
| `browser`, `browser links`, `browser dillo <url>` | Graphical browser in X, see [Graphical browser](#graphical-browser) |

### USB network

The phone asks for an address on `usb0` by DHCP. On the PC, make the new `enx02e500000001` interface a NetworkManager shared connection (DHCP, DNS and NAT for the phone):

```sh
nmcli connection add type ethernet ifname enx02e500000001 con-name e5-usb ipv4.method shared ipv6.method ignore
```

The phone then gets an address in `10.42.0.0/24`, and the PC is `10.42.0.1`.

### Graphical browser

Optional, on the card rootfs (X and the browsers need about 240 MB on the card, mostly Mesa libraries that X depends on but does not load here). With WLAN up:

```sh
apk add xorg-server xf86-video-fbdev xf86-input-evdev xinit font-misc-misc dillo font-dejavu links-graphics
```

`browser` starts X on the framebuffer (about 30 s) with Dillo, `browser links` with `links -g`; a URL can follow. Quitting the browser returns to the console. X uses the keypad directly (`/etc/X11/xorg.conf.d/10-e5.conf`, no udev on the phone), with OpenGL and video overlays switched off (there is no GPU). In use: about 25 MB RAM in total.

| Key | In X |
| --- | --- |
| Right soft key | Pointer mode on/off |
| Navi key | Arrows; in pointer mode it moves the pointer (`E5_PTR_STEP`, default 2 pixels per step, accelerating while held) |
| Enter, navi centre | Enter; in pointer mode a left click |
| fn (bottom left), held | Ctrl: fn+L address bar, fn+Q quit (Dillo) |

`links -g` works from the keyboard alone. Dillo needs pointer mode to follow links. Alpine's NetSurf framebuffer build and `links` without X can't draw on the E5 screen (they need SDL with a display backend the E5 doesn't have, or X).

### WLAN

`wifi 'name' 'password'` saves the network in `/etc/wpa_supplicant/wpa_supplicant.conf` and connects: `wpa_supplicant`, DHCP, then NTP. On the card rootfs the saved network connects again at every boot; in simple mode `/etc` is in RAM, so it is gone after a reboot. `wifi off` disconnects, `wifi on` connects again, `wifi forget` deletes the network. There is no RTC, so the clock is wrong until NTP sets it, and TLS (`apk`) fails until then.

WLAN needs LinuxE5 to have run once with the memory card in: it copies the firmware and the phone's MAC address (`E:\linux\e5-wlan-mac`) from the ROM, and `init.sh` loads them at boot. An address in `/etc/e5-wlan-mac` on the rootfs takes priority.

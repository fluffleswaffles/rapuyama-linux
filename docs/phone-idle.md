# Phone idle

Idle on the E6 = screen off from YamaShell + the CPU sleeping in WFI between interrupts, like Symbian's shallow idle. The kernel, USB shell, input, watchdog feeding and timers keep running, so the phone wakes instantly on a key or touch. This is not suspend to RAM. Panel sleep-in and lower VideoCore power are still todo.

## Backlights

The display backlight is `/sys/class/backlight/rapuyama-vc/brightness`, with a range of 0..255. The normal firmware value is 63. The VideoCore worker sends panel DCS command `0x51`; zero turns the light off while retaining the displayed image. This does not send the panel's sleep-in command.

The E6 keyboard lights use an LP5523 at address `0x33` on RapuYama HS I2C port 3, shared with the charger. `leds-e6-keyboard.c` exposes `/sys/class/leds/e6:white:kbd_backlight/brightness`. It adopts the firmware configuration and controls PWM channels 0, 1, 2, 4 and 5 together. It leaves the other channels, current limits, clocks, charge-pump configuration and program memory unchanged. It refuses to take over running engines or channels assigned to a master fader. The firmware value observed on these channels was 229; zero PWM readback has been checked on the phone.

## Session policy

| Time without input | Action |
| --- | --- |
| 5 seconds without a key | Keyboard light off |
| 30 seconds | Display brightness reduced to one eighth |
| 60 seconds | Display light off; compositor repaint and frame callbacks paused |

The first key press or touch wakes the screen and is consumed through its release, so it does not type into an application or activate a control. Later input reaches applications normally. Keyboard input also restores the keyboard light. Power, sleep and screen-lock keycodes toggle the screen state; the E6 lock slider is `KEY_SCREENLOCK` (gpio 79 through `gpio-keys-polled`). The power key belongs to the modem side (ISASW MTC `KBD_PWR_KEY_INFO_IND`) and is not available under Linux yet. This is screen idle, without a security lock or authentication screen.

Notifications and application commits do not wake the display. Pending frame callbacks resume on wake. The compositor restores display brightness on a normal exit. Starting the compositor with brightness already zero still allows input to wake it.

`YAMASHELL_KEYLIGHT_SECONDS`, `YAMASHELL_DIM_SECONDS` and `YAMASHELL_SLEEP_SECONDS` override the timeouts in seconds; zero disables the corresponding timeout. `YAMASHELL_BACKLIGHT` and `YAMASHELL_KEYLIGHT` override the brightness file paths, including for host tests. Headless tests do not touch backlights unless an explicit display brightness path is supplied.

## Verification

The keyboard LED driver and a compositor sleep/wake cycle have also been exercised live on the E6 without a reboot. These checks establish backlight and compositor behavior; CPU sleep is covered below, panel sleep-in is not done yet.

## CPU idle

`e6.idle=1` (in `e6-vc-565-gpu.dts` and `e6-vc-565-gpu-dma.dts`) replaces `nohlt`. With `nohlt` the CPU spins at full speed even when Linux counts the time as idle, which made the phone hot. A plain Linux WFI hung the phone at ~5 s (16.10.2026). The likely reason: the clock controller set register 0x20033140 has bit 3 clear after Symbian, so WFI lets the hardware stop system clocks. Symbian's shallow idle (power.dll FUN_8014738c -> FUN_80146f74 -> FUN_80146e64) always writes 8 to 0x20033144 first.

The hook in `arch/arm/mach-rapuyama/idle.c` + `idle_wfi.S` follows that sequence: 0x20033144 = 8, a fully blocking scratch-page store, L220 cache sync, branch prediction off while the idle/wake code is prefetched into the icache, WFI, wait for 0x20033130 bit 10 and 0x20033120 bits 15:14 clear, restore the control register. Before its mappings are ready, or if preparation fails (no L220), it keeps polling. `/sys/kernel/debug/rapuyama-idle` shows readiness, entries, skipped attempts and the time spent in WFI.

`e6-vc-565-gpu-dma-nohlt.dts` is the same configuration with the old polling idle, as a fallback.

| Check | Result |
| --- | --- |
| ROM vs own assembly, 18 emulated cases | same |
| live on a running `nohlt` kernel, same code as a module, 5 s | 96% of the time in WFI |
| live, 150 s with a VC GLES Wayland client, gfx DMA and CPU load | 61 fps, 92% in WFI, no skipped entries |
| live soak, phone idle | ~97% in WFI, no warnings |

The tick is still periodic at 1000 Hz, so the CPU wakes every millisecond. A oneshot timer (`e6.oneshot=1`) would cut that but has not been tested on hardware.
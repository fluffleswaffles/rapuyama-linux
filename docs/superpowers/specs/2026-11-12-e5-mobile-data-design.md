# E5 mobile data service: design

Date: 2026-11-12. Status: approved by the user (advanced setup only).

## Intent

Cellular data on the Nokia E5 works today only through the phone smoke scripts
(`~/e5/newcard/smoke/xg*`, PC-served): they download the runtime, load modules,
start `e5rt`, register, activate a PDP context and set routes, all by hand, on a
fresh boot. Proven on the phone (xg44/xg45): HSDPA downloads at 1.3-1.6 Mbit/s,
pings, FACH/DCH switching done by the network, no crash past 4.5 min.

The goal is to make mobile data an ordinary feature of the beta: the user starts
Linux from Symbian with a "3G" launcher and has a working internet connection on
`gprs0` without typing anything, plus a `mobile` command to check and control it.

## Goals

- A second Symbian launcher, **3GLinuxE5**, next to LinuxE5. Starting it means
  "Linux with mobile data". LinuxE5 keeps booting Wi-Fi only, exactly as today.
- **IMEI and APN are set up by the user, by hand** (user decision 2026-11-12):
  whoever wants 3G writes `E:\linux\e5-imei` (the phone's IMEI, `*#06#` in
  Symbian) and `E:\linux\e5-apn` (their operator's APN). Nothing reads them
  automatically (unlike the Wi-Fi MAC). This is said clearly in the login
  message, in a short guide on the phone, in `mobile`'s messages and in the
  3GLinuxE5 launcher when the files are missing.
- On a 3G boot, Linux starts the modem stack early (the runtime must start within
  about 2 minutes of boot) and brings data up automatically.
- `mobile` command, in the style of `ssh-server` / `tailnet`:
  - `mobile`: status (modem running/stopped, registered, data up/down, IP,
    bytes, "reboot needed" when the modem died)
  - `mobile on`: activate data (context, default route, DNS)
  - `mobile off`: deactivate data, restore routes and `resolv.conf`; the modem
    stays running
- **APN set by hand**: a one-line file on the card; no default (operator
  specific).
- Wi-Fi keeps priority when both are up.

## Non-goals (this version)

- Restarting the modem stack within a boot (e5rt can run once per boot; after a
  modem death the answer is "reboot").
- Reading the IMEI or the APN from Symbian (telephony API, CommsDat): the user
  writes both files.
- SMS, voice calls, signal bars in a GUI, data-usage limits, roaming checks.
- Changing the network mode (2G/3G/HSDPA) from Linux. That stays a Symbian
  setting (see anti-goals).

## Anti-goals (failure even if it "works")

- Any GSS / RAT (network mode) write from Linux: it persists in Symbian and broke
  later sessions (HANDOFF 2026-11-11, xg21-xg23).
- Debug traces on in normal use (`E5RT_FW_TRACE`, `E5RT_INV_LOG`, message traces
  beyond the counters): they stall the DSP link (reset 9 in 4 of 7 traced runs).
- Mobile data used without the user choosing it: a LinuxE5 (non-3G) boot must
  never start the modem stack.
- The IMEI or other identities in logs, release files or commits.

## Constraints (found on the phone)

- `e5rt` must start within ~2 min of boot (later: "mdi dealloc with an empty
  tag blocked") and only once per boot.
- It needs the ISASW images of the same boot, read through `e5mem` (debugfs
  `e5mem/<pa>`: 10480000, 1fd50000, 1ffe0000, 1fb60000, 100ec000).
- Kernel: the E5_CELL build with RAPUYAMA_MDI_DSP_PUT/GET (kernel e5-cell
  c8785a5f1, phone `Image-cell6`). The beta kernel becomes this build; the
  modem driver is idle unless e5rt opens it, so Wi-Fi-only boots are unchanged.
- Runtime: `e5rt` from e5-cell cfebc78 (`e5rt-os38`), plus `e5gprs` (PDP context
  + pipe) and `pnping` (registration check), `e5mem.ko`, `rapuyama_sim.ko`.
- Data path after the context: `ip addr add <IP>/32 dev gprs0`, `ip link set
  gprs0 up`, routes. Today's scripts use host routes only.
- e5rt costs ~8 MB RAM and 5-45 % CPU (idle vs. HSDPA download).

## Design (my calls, to be confirmed)

### 1. 3GLinuxE5 launcher

- Same `boot_app.cpp`, second build variant (like the existing `build_*.sh`
  variants), own UID and name "3GLinuxE5", own icon tint.
- Loads the same `E:\Image`; device tree `E:\e5-3g.dtb` = `e5.dtb` with
  `e5.mobile=1` added to `bootargs` (built by the kernel build next to `e5.dtb`).
  One kernel file on the card.
- Before handing over: if `E:\linux\e5-imei` or `E:\linux\e5-apn` is
  missing, a dialog "3G needs E:\linux\e5-imei and E:\linux\e5-apn (see
  E:\linux\3G-README.txt). Boot anyway?" with Boot/Cancel. The launcher writes
  `E:\linux\3G-README.txt` (the short setup guide) if it is missing, so the
  instructions are on the card where the files go.
- `/etc/e5-apn` in the root overrides the card file, like `/etc/e5-wlan-mac`.

### 2. Boot step `e5-mobile` (Linux, sd rootfs)

Runs from `linux/initramfs/init.sh` in the root that boots (the sd rootfs runs
the same `/init` after switch_root), next to `e5_wlan_files`, which already
mounts the card's p1 read-only at `/tmp/card` and copies `E:\linux\*` files:
the same step copies `e5-imei` (mode 600) and `e5-apn` to `/run/e5-mobile/`.
Only when `/proc/cmdline` has `e5.mobile=1`; started in the background so boot
is not delayed, early enough to start e5rt within ~60-90 s of boot:

1. mount debugfs, `insmod e5mem.ko`, copy the five ISASW images to `/run/e5img`
   (tmpfs), keep e5mem loaded (needed by nothing else; may be unloaded).
2. `insmod rapuyama_sim.ko`, `ip link set phonet0 up`.
3. start `e5rt` in the background, lean environment only
   (`E5RT_QUIET=1 E5RT_IMEI_FILE=... E5RT_SIM_DEVICE=/dev/rapuyama-sim
   E5RT_OS_CLOCK=1`), log to `/run/e5-mobile/e5rt.log` (size-capped), pid file.
   IMEI from `/run/e5-mobile/e5-imei` (copied from the card above).
4. wait for registration (`pnping`), then `mobile on` (data up automatically on
   a 3G boot).
Never runs on a LinuxE5 boot.

### 3. `mobile` command (`linux/initramfs/e5-mobile.sh` -> `/usr/bin/mobile`)

- `mobile on`: needs the modem running and registered; runs `e5gprs <APN>` (APN
  `/etc/e5-apn`, else `/run/e5-mobile/e5-apn`; none: "no APN (write it to
  E:\linux\e5-apn)"), applies IP,
  adds a default route via `gprs0` with metric 600 (`e5-dhcp` uses 0 for wlan0,
  500 for usb0, so Wi-Fi and the PC link win while up), writes the
  context's DNS to `resolv.conf` only when Wi-Fi has no default route; saves the
  previous `resolv.conf`.
- `mobile off`: stops `e5gprs` (context deactivation), removes routes, restores
  `resolv.conf`.
- `mobile` (status): modem pid alive?, registered?, data up?, IP, `gprs0`
  rx/tx bytes, APN; if e5rt exited: "modem stopped, reboot to use mobile data"
  and the last 5 log lines.
- Root only, like `ssh-server`.

### 4. Packaging

- Login message (`e5-motd`): `mobile | on | off   3G data (3GLinuxE5; needs
  E:\linux\e5-imei + e5-apn, see /usr/share/doc/e5/mobile.txt)`.
- `/usr/share/doc/e5/mobile.txt` = the same guide as `3G-README.txt`: what the
  two files are, how to find the IMEI (`*#06#`), APN examples are the user's to
  look up, one line each, no newline needed.

Advanced setup (sd rootfs) only, like `tailnet`: the files are ~1.5 MB and an
initramfs is RAM for its whole life, so the simple install stays without mobile
data (user decision 2026-11-12). A 3GLinuxE5 boot without the files: the boot step
logs "mobile data needs the advanced setup" and does nothing.

- `tools/sdroot.sh e5`: installs `e5rt`, `e5gprs`, `pnping` (`/usr/libexec/e5/`),
  `e5mem.ko` + `rapuyama_sim.ko` (`/lib/modules/<ver>/extra/` or next to the
  binaries, insmod by path), `/usr/bin/mobile`, the boot step.
- Release (`tools/e5-release.sh`, `tools/e5-install.sh`): add `3GLinuxE5.sis`
  and `e5-3g.dtb`; `Image` = E5_CELL build.

## Testing

- PC: `mobile` script logic with fakes (no modem) where practical; e5rt
  selftests as today.
- Phone, one boot session per batch: (a) LinuxE5 boot: no modem, Wi-Fi as
  before; (b) 3GLinuxE5 boot: data up automatically, ping + 1 MB download,
  `mobile off` / `mobile on`, Wi-Fi on/off priority; (c) xg46-style stability
  (3 rounds); (d) modem death message (kill e5rt by hand).

## Left to the builder

- Exact init hook point and timing inside `init.sh` (must keep e5rt start
  < ~90 s after boot).
- Whether e5mem is unloaded after the image copy.
- Icon tint / launcher caption details; UID choice within the project's range.
- Log size cap and rotation.

## Neighbouring beta work (separate projects)

- KMS (DRM display driver): measured +418..463 KB kernel RAM for the DRM core
  (cellular config + DRM/KMS helpers/fbdev emulation), plus 0.2-0.6 MB of
  buffers. Builds on the same kernel config this design makes the beta kernel.
- Charging under Linux (bq2415x, not programmed by Linux): a 3G session drains
  the battery faster (e5rt 5-45 % CPU + radio), so long mobile sessions depend
  on it. Static RE first, battery safety review before anything runs.
- Audio: NOT part of the beta (user decision 2026-11-12). Upstream E6 audio goes through the same DSP/ISASW runtime,
  so it would need a 3G-style boot too.

## Open before release (not this design)

- xg46 lean stability result; longer sessions; upload (HSUPA) rate.
- Push of e5-cell / kernel e5-cell after a full diff review.

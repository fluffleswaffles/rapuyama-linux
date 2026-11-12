# E5 Mobile Data Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Starting "3GLinuxE5" from Symbian boots Linux with mobile data up on `gprs0`, controlled by a `mobile` command; "LinuxE5" stays Wi-Fi only.

**Architecture:** One kernel (the E5_CELL build) and two device trees (`e5.dtb`, `e5-3g.dtb` = same + `e5.mobile=1`). A second launcher build loads `e5-3g.dtb`; the user writes the IMEI and APN files on the card by hand. On a 3G boot, `init.sh` starts a background boot step that runs the e5rt runtime (lean) and then `mobile on`, which runs `e5gprs` and sets route/DNS.

**Tech Stack:** POSIX sh (busybox ash) for scripts, C (static armv6 musl, clang) for the runtime, Symbian GCCE C++ for the launcher, dtc/fdtput, bash build scripts.

**Spec:** `docs/superpowers/specs/2026-11-12-e5-mobile-data-design.md`

## Global Constraints

- Never send GSS / RAT (network mode) writes from Linux.
- Normal use runs e5rt lean: `E5RT_QUIET=1 E5RT_IMEI_FILE=<file> E5RT_SIM_DEVICE=/dev/rapuyama-sim E5RT_OS_CLOCK=1` only (no `E5RT_FW_TRACE`, `E5RT_INV_LOG`, `E5RT_MSG_TRACE`, `E5RT_HS_TRACE`, `E5RT_TX_DUMP`).
- A boot without `e5.mobile=1` on `/proc/cmdline` never starts the modem stack.
- The IMEI is never printed, logged or committed; IMEI files mode 600.
- e5rt starts at most once per boot and within 90 s of boot (`/proc/uptime`); otherwise refuse with a message.
- Advanced setup (sd rootfs) only; files under `/usr/libexec/e5/`.
- IMEI and APN are written by the user (`E:\linux\e5-imei`, `E:\linux\e5-apn`); nothing reads them automatically; no default APN; `/etc/e5-apn` overrides the card file.
- Every place that needs them says so with the exact paths: login message, `/usr/share/doc/e5/mobile.txt`, `E:\linux\3G-README.txt`, `mobile` messages, the 3GLinuxE5 dialog.
- Mobile default route metric 600 (`e5-dhcp`: wlan0 0, usb0 500).
- Runtime = e5-cell `linux/e5-cell/sim/` (e5rt-os38 behaviour, commit cfebc78); kernel = e5-cell c8785a5f1 E5_CELL config.
- Commits as `git -c user.name=Kapina-Dev`, trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; no push.
- Phone tests: one boot per batch, scripts served from the PC (`wget -qO- URL | sh`).

## Review Focus

1. 3G boot where registration never succeeds (no SIM, no coverage): boot finishes normally, `mobile` says "not registered", no data attempt loops forever. -> Task 4 test `boot_not_registered`.
2. `e5-apn` edited on Symbian/Windows with CRLF or a trailing newline: the APN is used without `\r`/`\n`. -> Task 5 test `apn_crlf`.
3. `mobile on` twice / `mobile off` twice: no duplicate routes, no error exit, second call reports the state. -> Task 5 tests `on_twice`, `off_twice`.
4. Wi-Fi default route present when data comes up: `resolv.conf` and the wlan route are untouched. -> Task 5 test `wifi_keeps_dns`.
5. User has not written the IMEI or APN file: the boot step does not start e5rt without the IMEI (`mobile`: "no IMEI (write it to E:\linux\e5-imei)"), `mobile on` without an APN says "no APN (write it to E:\linux\e5-apn)". -> Task 4 test `boot_no_imei`, Task 5 test `no_apn_message`.

## Neighbouring beta projects (not in this plan, but this plan touches them)

- **KMS:** Task 1 makes the E5_CELL config the beta kernel; KMS work must branch from that config. Measured cost of the DRM core on it: +418..463 KB kernel RAM, + 0.2-0.6 MB buffers.
- **Charging under Linux:** not done (bq2415x not programmed by Linux). 3G sessions drain faster (e5rt 5-45 % CPU). `mobile` status prints the battery line from the existing read-only battery driver so the user sees it (Task 5). Charging is the next separate project.
- **Audio:** NOT part of the beta (user decision 2026-11-12). It would ride on the same e5rt/DSP runtime (shared serving thread, DSP load), so any later audio work must be switchable off and re-test 3G stability.

## File Structure

| File | Responsibility |
|---|---|
| `linux/kernel/build-e5.sh` (modify) | beta kernel = cell config; emit `e5.dtb` + `e5-3g.dtb`; build `rapuyama_sim.ko`, `e5mem.ko` |
| `linux/e5-cell/build.sh` (modify) | build the sim runtime as `e5rt` + `e5gprs` + `pnping` |
| `linux/initramfs/e5-mobile-boot.sh` (create) | 3G boot step: images, modules, e5rt, registration, `mobile on` |
| `linux/initramfs/e5-mobile.sh` (create) | `mobile` / `mobile on` / `mobile off` |
| `linux/initramfs/init.sh` (modify) | copy `e5-imei`/`e5-apn` from the card; start the boot step on `e5.mobile=1` |
| `linux/initramfs/tests/test-e5-mobile.sh` (create) | sh tests for both scripts with stub commands |
| `tools/sdroot.sh` (modify) | install runtime, modules, scripts on the sd rootfs |
| `driver/mksis.sh` (modify) | UID + exe name parameters |
| `linux/stage0/symbian-loader/boot_app.cpp`, `build_boot.sh` (modify) | `RB_3G` variant: `E:\e5-3g.dtb`, caption, UID, missing-files dialog, `3G-README.txt` |
| `linux/initramfs/e5-mobile.txt` (create) | the 3G setup guide (installed as `/usr/share/doc/e5/mobile.txt`, embedded in the launcher as `3G-README.txt`) |
| `linux/initramfs/e5-motd` (modify) | `mobile` line |
| `tools/e5-release.sh`, `tools/e5-install.sh` (modify) | ship `3GLinuxE5.sis` + `e5-3g.dtb` |

---

### Task 1: Beta kernel = cellular build, two device trees

**Files:**
- Modify: `linux/kernel/build-e5.sh` (config: cell options for the default build; build: dtbs + modules)

**Interfaces:**
- Produces: `$KOUT/arch/arm/boot/Image`, `$KOUT/e5.dtb` (from `e5-cell.dts`, no `e5.mobile`), `$KOUT/e5-3g.dtb` (bootargs + ` e5.mobile=1`), `$KOUT/drivers/soc/rapuyama/rapuyama_sim.ko`.

- [ ] **Step 1: Write the check** `linux/kernel/check-e5-dtbs.sh <KOUT>`: fails unless `fdtget -t s $KOUT/e5-3g.dtb /chosen bootargs` ends with ` e5.mobile=1`, the same for `e5.dtb` does not contain `e5.mobile`, both have the `nokia,e5-mdi` node (`fdtget -l ... /soc` lists `mdi@30003ce0`), and `rapuyama_sim.ko` exists.
- [ ] **Step 2: Run it on the current build-e5 output** — expected FAIL (no `e5-3g.dtb`).
- [ ] **Step 3: Change `build-e5.sh`**: the default build applies today's `E5_CELL` options (VMSPLIT_3G25, PHONET, RAPUYAMA_ISI, SOC_RAPUYAMA_MDI, RAPUYAMA_SIM_BRIDGE=m) and uses `e5-cell.dts`; `E5_NOCELL=1` restores the old wifi-only kernel. After `dtc`, `cp e5.dtb e5-3g.dtb && fdtput -t s e5-3g.dtb /chosen bootargs "$(fdtget -t s e5.dtb /chosen bootargs) e5.mobile=1"`; add `make modules` for `drivers/soc/rapuyama/`.
- [ ] **Step 4: Build and run the check** — `./build-e5.sh build && ./check-e5-dtbs.sh build-e5` -> PASS. Record `size vmlinux` delta vs today's build-e5 in the commit message (VMSPLIT_3G25 for everyone is new: note it).
- [ ] **Step 5: Commit** `e5: beta kernel = cellular build, e5-3g.dtb with e5.mobile=1`.

### Task 2: Runtime build from the sim sources

**Files:**
- Modify: `linux/e5-cell/build.sh`

**Interfaces:**
- Produces: `$OUT/e5rt` (from `sim/e5rt-sim.c sim/sim_runtime.c sim/os_timer_runtime.c sim/identity_runtime.c sim/kasumi_runtime.c`, same clang flags as the loop + `-I sim`), `$OUT/e5gprs`, `$OUT/pnping`, `$OUT/e5mem.ko` (with a kernel dir). The old `e5rt.c` binary is renamed `e5rt-legacy` (kept for development).

- [ ] **Step 1: Check** — `qemu-arm-static $OUT/e5rt <dump dir> --dsp-selftest` and `--os-timer-selftest` (dump dir `~/e5/dump-linux-1110`) must print "passed"; today `$OUT/e5rt` is the legacy binary without these options -> FAIL.
- [ ] **Step 2: Change the build loop**; keep `-Wall -Wextra` clean.
- [ ] **Step 3: Run the selftests** (all six from HANDOFF: os-timer, kasumi, sim, sim-reset, dsp, dsp-tx) -> PASS; `sha256sum` matches a fresh build of e5rt-os38 sources.
- [ ] **Step 4: Commit** `e5-cell: build.sh builds the sim runtime as e5rt`.

### Task 3: Card files copied at boot

**Files:**
- Modify: `linux/initramfs/init.sh` (next to `e5_wlan_files`, line ~185)
- Test: `linux/initramfs/tests/test-e5-mobile.sh`

**Interfaces:**
- Produces: shell function `e5_mobile_files <card mount dir>`: copies `<dir>/linux/e5-imei` to `/run/e5-mobile/e5-imei` (mode 600) and `<dir>/linux/e5-apn` to `/run/e5-mobile/e5-apn`; `RUN=/run/e5-mobile` overridable for tests. Called inside `e5_wlan_files` while the card is mounted, only when `e5.mobile=1` is on the cmdline (`CMDLINE=/proc/cmdline` overridable).

- [ ] **Step 1: Tests** `files_copied` (fake card dir -> both files present, imei mode 600), `files_skip_nocell` (cmdline without the flag -> nothing created).
- [ ] **Step 2: Run** `sh linux/initramfs/tests/test-e5-mobile.sh` -> FAIL.
- [ ] **Step 3: Implement** the function (sourcing-safe: the test sources `init.sh` functions via `E5_INIT_TEST=1`, which makes `init.sh` define functions and return before doing anything).
- [ ] **Step 4: Run** -> PASS.
- [ ] **Step 5: Commit** `e5: copy e5-imei / e5-apn from the card on a 3G boot`.

### Task 4: 3G boot step

**Files:**
- Create: `linux/initramfs/e5-mobile-boot.sh` (installed as `/usr/libexec/e5/mobile-boot`)
- Modify: `linux/initramfs/init.sh` (start it in the background after the card copy: `[ -x /usr/libexec/e5/mobile-boot ] && /usr/libexec/e5/mobile-boot &`)
- Test: `linux/initramfs/tests/test-e5-mobile.sh`

**Interfaces:**
- Consumes: `/run/e5-mobile/e5-imei`, `/run/e5-mobile/e5-apn` (Task 3); binaries in `LIBEXEC=/usr/libexec/e5` (`e5rt`, `pnping`, `e5mem.ko`, `rapuyama_sim.ko`).
- Produces: `/run/e5-mobile/e5rt.pid`, `/run/e5-mobile/e5rt.log` (capped 256 KB by the step: truncated to the last 64 KB when larger, checked every 30 s), `/run/e5-mobile/state` (one word: `starting`, `registered`, `not-registered`, `no-imei`, `too-late`, `stopped`), `/run/e5-mobile/started` (once-per-boot marker); calls `mobile on` when registered.
- Sequence (from the xg46 script, without downloads/hashes): `mount -t debugfs`, `insmod e5mem.ko`, copy `10480000 1fd50000 1ffe0000 1fb60000 100ec000` to `/run/e5img/<name>.bin`, `insmod rapuyama_sim.ko`, wait until uptime >= 60 s (as today), `ip link set phonet0 up`, start e5rt with the lean environment, registration loop `pnping --send 0a e000` up to 20 x 5 s matching `42 e1 00 .. 00 24 00|01`, then `pnping --send 0a 3600`, then `mobile on`.

- [ ] **Step 1: Tests** with stub commands on `PATH` (stubs log their argv to a file): `boot_happy` (stub pnping answers registered -> state `registered`, `mobile on` called once, e5rt argv/env contains no `E5RT_FW_TRACE`/`E5RT_MSG_TRACE`), `boot_no_imei` (state `no-imei`, e5rt never started), `boot_too_late` (fake uptime 120 -> state `too-late`), `boot_not_registered` (stub never registers -> state `not-registered` after the loop, `mobile on` not called, script exits 0), `boot_once` (marker present -> exits without starting e5rt). Uptime/sleep come from `UPTIME=/proc/uptime` and `SLEEP=sleep` overrides.
- [ ] **Step 2: Run** -> FAIL.
- [ ] **Step 3: Implement** `e5-mobile-boot.sh`.
- [ ] **Step 4: Run** -> PASS.
- [ ] **Step 5: Commit** `e5: 3G boot step (lean e5rt, registration, data up)`.

### Task 5: `mobile` command

**Files:**
- Create: `linux/initramfs/e5-mobile.sh` (installed as `/usr/bin/mobile`)
- Test: `linux/initramfs/tests/test-e5-mobile.sh`

**Interfaces:**
- Consumes: `/run/e5-mobile/state`, `e5rt.pid`, `e5-apn`; `$LIBEXEC/e5gprs <apn>` output lines `IFNAME <if>`, `IP <a.b.c.d>`, `DNS1 <a.b.c.d>`, `CONTEXT ACTIVE on <if>` (e5gprs stays running while data is up; its pid in `/run/e5-mobile/e5gprs.pid`).
- Produces: `mobile` (exit 0, status lines: `modem: running|stopped (reboot to use mobile data)|off (not a 3G boot)`, `network: registered|not registered`, `data: on <ip>|off`, `apn: <apn>`, `gprs0: rx <n> tx <n> bytes`, `battery: <capacity>%` from `/sys/class/power_supply/*/capacity` when present, last 5 e5rt log lines when stopped), `mobile on` (exit 0 when data is up, 1 with a reason), `mobile off`.
- Routes: `ip addr add <IP>/32 dev <if>`, `ip link set <if> up`, `ip route add default dev <if> metric 600`; DNS: if `ip route show default` has no `dev wlan0`, save `/etc/resolv.conf` to `/run/e5-mobile/resolv.conf.saved` and write `nameserver <DNS1>`; `off` restores it only if it saved it.

- [ ] **Step 1: Tests** (stub `ip`, stub e5gprs printing the four lines then sleeping): `on_happy`, `on_twice` (second `on`: "data already on", one default route), `off_twice` (second `off`: "data already off", exit 0), `apn_crlf` (`e5-apn` = `internet.a1.bg\r\n` -> e5gprs argv is exactly `internet.a1.bg`), `apn_override` (`/etc/e5-apn` wins; `ETC=` override), `no_apn_message` (no APN file -> exit 1, "no APN (write it to E:\linux\e5-apn)", e5gprs not run), `wifi_keeps_dns` (stub `ip route show default` prints `default via 192.168.0.1 dev wlan0` -> resolv.conf unchanged), `status_stopped` (pid not alive -> "stopped (reboot to use mobile data)"), `status_not_3g` (no state file -> "off (not a 3G boot)"), `no_imei_message` (state `no-imei` -> "no IMEI (write it to E:\linux\e5-imei)"), `root_only` (non-root -> exit 1).
- [ ] **Step 2: Run** -> FAIL.
- [ ] **Step 3: Implement** `e5-mobile.sh` (style of `e5-tailnet.sh`: `case "$1"`, root check, short messages).
- [ ] **Step 4: Run** -> PASS.
- [ ] **Step 5: Guide + login line** — write `linux/initramfs/e5-mobile.txt` (≤ 25 lines: start 3GLinuxE5; create `E:\linux\e5-imei` = the 15 digits from `*#06#`; create `E:\linux\e5-apn` = your operator's APN, one line, or `/etc/e5-apn` on Linux; `mobile`, `mobile on`, `mobile off`; data comes up by itself on a 3G boot; Wi-Fi wins when both are up; "modem stopped" = reboot; HSDPA must be allowed in Symbian's network settings for fast data); add the motd line from the spec to `e5-motd`.
- [ ] **Step 6: Commit** `e5: mobile command (on/off/status), 3G guide, motd line`.

### Task 6: sd rootfs packaging

**Files:**
- Modify: `tools/sdroot.sh` (the `e5` block next to `ssh-server`/`tailnet`)

**Interfaces:**
- Consumes: Task 1 outputs (`KOUT`), Task 2 `$OUT` (env `E5_CELL_OUT`, default `$TOP/work/e5-cell`).
- Produces on the rootfs: `/usr/libexec/e5/{e5rt,e5gprs,pnping,e5mem.ko,rapuyama_sim.ko,mobile-boot}`, `/usr/bin/mobile`, `/usr/share/doc/e5/mobile.txt`.

- [ ] **Step 1: Check** — `SDROOT_NOSUDO=1 tools/sdroot.sh <tmpdir> e5` then `test -x` for each path -> FAIL before the change.
- [ ] **Step 2: Implement**; missing `E5_CELL_OUT` files = warning, rootfs still built (mobile then reports itself unavailable).
- [ ] **Step 3: Run the check** -> PASS.
- [ ] **Step 4: Commit** `e5: sdroot installs the mobile data files`.

### Task 7: 3GLinuxE5 launcher

**Files:**
- Modify: `driver/mksis.sh` (args: `[caption] [uid] [icon.svg]`, default uid `0xe1000004`, default icon `driver/sis/linuxe5.svg`; exe/rsc/mif names derived from the uid via `driver/sis/mkappres.py`)
- Create: `driver/sis/linuxe5-3g.svg` (the LinuxE5 penguin + a small antenna mast with two signal arcs to its right, same canvas and palette; SVG-Tiny features only, like `linuxe5.svg`, so `miffile` converts it)
- Modify: `linux/stage0/symbian-loader/build_boot.sh` (variant `RB_3G` -> `-DRB_3G`, linkas uid `e1000005`, output `3GLinuxE5.sis`)
- Modify: `linux/stage0/symbian-loader/boot_app.cpp` (`#ifdef RB_3G`: `KDtb` = `E:\e5-3g.dtb`; error dialogs name `e5-3g.dtb`; before streaming: write `E:\linux\3G-README.txt` from the embedded guide if missing; if `E:\linux\e5-imei` or `E:\linux\e5-apn` is missing, `Ask()` "3G needs E:\linux\e5-imei and E:\linux\e5-apn (see E:\linux\3G-README.txt). Boot anyway?" Boot/Cancel; nothing is read from Symbian)

**Interfaces:**
- Produces: `3GLinuxE5.sis` installable next to `LinuxE5.sis` (different UID 0xe1000005, caption "3GLinuxE5", antenna icon).

- [ ] **Step 1: Icon** — draw `linuxe5-3g.svg`; render both SVGs to PNG on the PC (`rsvg-convert -w 88`) and show them to the user side by side before packaging; the user approves the look.
- [ ] **Step 2: Build both** variants; the ensymble listing shows distinct exe names/UIDs and a `_aif.mif` for each; `strings` of the 3G exe contains `e5-3g.dtb` and not `\e5.dtb`.
- [ ] **Step 3: Phone check** (one Symbian session): install both, both icons appear (3GLinuxE5 with the antenna, readable at menu size), 3GLinuxE5 with a missing `e5-3g.dtb` shows the "missing" dialog naming `e5-3g.dtb` (do not boot yet).
- [ ] **Step 4: Phone check, files missing/present:** without the two files the dialog appears and Cancel returns to the menu; `3G-README.txt` is on the card; with both files no dialog.
- [ ] **Step 5: Commit** `e5: 3GLinuxE5 launcher variant (e5-3g.dtb, setup dialog + 3G-README, antenna icon)`.

### Task 8: Release files

**Files:**
- Modify: `tools/e5-release.sh` (add `3GLinuxE5.sis`, `e5-3g.dtb` to the copied files and `SHA256SUMS`)
- Modify: `tools/e5-install.sh` (copy and verify them to p1; final message names both launchers)

- [ ] **Step 1: Check** — run `e5-release.sh` into a temp dir; `SHA256SUMS` lists `LinuxE5.sis 3GLinuxE5.sis Image e5.dtb e5-3g.dtb`.
- [ ] **Step 2: Implement; re-run** -> PASS.
- [ ] **Step 3: Commit** `e5: release ships 3GLinuxE5 and e5-3g.dtb`.

### Task 9: Phone acceptance (one batch per boot)

**Files:** PC-served test scripts in `~/e5/newcard/smoke/` (not committed).

- [ ] **Step 1: Card** — install the Task 1 Image, both dtbs, both sis files, the Task 6 rootfs files.
- [ ] **Step 2: LinuxE5 boot:** no `e5rt` process, `mobile` says "off (not a 3G boot)", Wi-Fi works as before, `free` vs the previous beta (expect ~equal: the modem driver is idle).
- [ ] **Step 3: 3GLinuxE5 boot** (IMEI + APN files written by hand on the card first), no typing: within ~2 min `mobile` shows `data: on`, `ping 1.1.1.1` and a 1 MB download work (expect >= 1 Mbit/s on HSDPA), DNS resolves.
- [ ] **Step 4: Same boot:** `mobile off` (route/DNS gone), `mobile on` (back), Wi-Fi on (wlan route wins, data keeps working through Wi-Fi), Wi-Fi off (mobile route used again).
- [ ] **Step 5: xg46-style stability** — 3 x (1 MB + 60 s idle + 5 pings), 0 resets.
- [ ] **Step 6: Modem death message** — `kill` the e5rt pid: `mobile` says "stopped (reboot to use mobile data)" with the last log lines.
- [ ] **Step 7:** record results in `~/e5/HANDOFF.md`; commit nothing from the smoke dir.

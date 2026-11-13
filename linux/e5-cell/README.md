# E5 cellular (development)

Work towards mobile data on the Nokia E5-00, ported from the E6's userspace
ISASW runtime (`linux/isi-runtime` on the main branch). Nothing here is part of a
release. Addresses and findings: `docs/devices/e5-00/cellular-map.md`.

| | |
|---|---|
| `sim/` | the runtime that registers and carries data (built as `e5rt`, see `sim/README.md`) |
| `e5rt.c` | the first E5 runtime (built as `e5rt-legacy`): self-tests, live MDI link, Phonet backend, SBIF consumer |
| `pnping.c` | Linux side: phonet0 address 0x6c + up, ISI version requests over an AF_PHONET socket |
| `e5mem/` | read-only debugfs view of the preserved ISASW / DSP memory, for same-boot images |
| `build.sh` | `./build.sh <out> [kernel build dir]` |
| `check-runtime.sh` | `./check-runtime.sh <out> <dump dir>`: the six e5rt selftests under qemu-arm |

Kernel: `linux/kernel/build-e5.sh` (the default E5 kernel since 2026-11-12; `E5_CELL=1` = same
config in `build-e5-cell`): VMSPLIT_3G25 so the
firmware addresses are user addresses, Phonet + `rapuyama_isi` + the MDI driver
(`nokia,e5-mdi`), dtb from `linux/boot/e5-cell.dts`.

Status (2026-11-13, EXPERIMENTAL, not in a release): the runtime in `sim/` registers on the
network (UMTS) with the real SIM, attaches, activates a PDP context (`e5gprs`, gprs0 with IPv4 +
DNS) and carries data on FACH, CELL_DCH and HSDPA: pings, DNS, 1 MB downloads at 1.2-2.2 Mbit/s,
a 3 x (1 MB + 60 s idle + pings) stability run without a reset. The DCH/HSDPA fix was two
copy-mode gaps: uplink buffers named by address in DSP messages (RAPUYAMA_MDI_DSP_PUT) and HS-DSCH
PDUs read after the firmware's DSP-buffer invalidate (RAPUYAMA_MDI_DSP_GET, kernel e5-cell).

Mobile data service (3GLinuxE5 launcher, e5-3g.dtb, boot step `mobile-boot`, `mobile` command
with `mobile setup` for the IMEI + APN): built and phone-tested, works when the modem survives
the boot (data comes up by itself, off/on, Wi-Fi keeps priority), but NOT release-ready:

- firmware reset 9 at boot (r4 0x19b = the 1 s supervision of a DSP request: the DSP did not
  answer) is intermittent: e5rt started by the boot step failed in 3 of 5 runs (with and without
  the message trace; `setsid` + `nice -n -10` did not remove it), started by hand from a login
  shell 0 of 2. Debug traces on (E5RT_FW_TRACE, E5RT_INV_LOG) or a start later than ~2 min after
  boot also end in reset 9. Cause unknown.
- one run stalled after ~6 min of data (1 MB took 120 s, pings lost) with e5rt alive and no reset.
- a context activation started while the previous one is still going down never comes up:
  `mobile off`/`on` wait for GPDS status "no context" (pnping 31 19).

Details and ruled-out paths: `docs/devices/e5-00/cellular-map.md`, the design
`docs/superpowers/specs/2026-11-12-e5-mobile-data-design.md` and its plan.

| | |
|---|---|
| `e5gprs.c` | ofono-style GPRS: attach, Phonet pipe end point (gprsN), context with APN, activate |
| `sim/` | the live runtime (SIM, timers, identity, KASUMI, traces), see sim/README.md |

On the phone (one runtime start per boot):

```
insmod e5mem.ko; for f in 10480000 1fd50000 1ffe0000 1fb60000; do cat /sys/kernel/debug/e5mem/$f > img/$f.bin; done; rmmod e5mem
e5rt img --mdi /dev/rapuyama-mdi --backend /dev/rapuyama-isi --seconds 0 &
pnping 00:15
```

Firmware images and RAM snapshots are private and stay out of the repo.

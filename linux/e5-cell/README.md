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
| `check-runtime.sh` | `./check-runtime.sh <out>`: the six e5rt selftests under qemu-arm |

Kernel: `linux/kernel/build-e5.sh` (the default E5 kernel since 2026-11-12; `E5_CELL=1` = same
config in `build-e5-cell`): VMSPLIT_3G25 so the
firmware addresses are user addresses, Phonet + `rapuyama_isi` + the MDI driver
(`nokia,e5-mdi`), dtb from `linux/boot/e5-cell.dts`.

Status (2026-11-11, PAUSED): the runtime in `sim/` registers on the network (UMTS) with the real
SIM, attaches to GPRS, activates a PDP context (`e5gprs`, gprs0 with IPv4 + DNS) and carries data
on the shared channel (FACH): operator DNS answers, TCP handshake, HTTP response. Open: once the
network moves the phone to a dedicated channel (CELL_DCH) the DSP never decodes a downlink block,
so traffic stalls until the fallback to FACH (replies 6-15 s late, most pings lost). Not caused
by ciphering (ETSI-verified), pipe flow control or e5rt's buffer copies; forcing 2G kills the DSP
link. Details and ruled-out paths: `docs/devices/e5-00/cellular-map.md` (PACKET DATA).

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

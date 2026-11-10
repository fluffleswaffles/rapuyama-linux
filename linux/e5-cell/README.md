# E5 cellular (development)

Work towards mobile data on the Nokia E5-00, ported from the E6's userspace
ISASW runtime (`linux/isi-runtime` on the main branch). Nothing here is part of a
release. Addresses and findings: `docs/devices/e5-00/cellular-map.md`.

| | |
|---|---|
| `e5rt.c` | the E5's own modem firmware (fw 101.003) as a userspace process: self-tests, live MDI link, Phonet backend, SBIF consumer |
| `pnping.c` | Linux side: phonet0 address 0x6c + up, ISI version requests over an AF_PHONET socket |
| `e5mem/` | read-only debugfs view of the preserved ISASW / DSP memory, for same-boot images |
| `build.sh` | `./build.sh <out> [kernel build dir]` |

Kernel: `E5_CELL=1 linux/kernel/build-e5.sh` (own dir `build-e5-cell`): VMSPLIT_3G25 so the
firmware addresses are user addresses, Phonet + `rapuyama_isi` + the MDI driver
(`nokia,e5-mdi`), dtb from `linux/boot/e5-cell.dts`.

Status (2026-11-10): self-tests pass in qemu-arm; on the phone the runtime
attaches to the real MDI rings, the DSP answers, and a Linux Phonet socket gets
the firmware's ISI version 18.0 through phonet0. Not done: SIM, OS timers,
identity, registration, packet data.

On the phone (one runtime start per boot):

```
insmod e5mem.ko; for f in 10480000 1fd50000 1ffe0000 1fb60000; do cat /sys/kernel/debug/e5mem/$f > img/$f.bin; done; rmmod e5mem
e5rt img --mdi /dev/rapuyama-mdi --backend /dev/rapuyama-isi --seconds 0 &
pnping 00:15
```

Firmware images and RAM snapshots are private and stay out of the repo.

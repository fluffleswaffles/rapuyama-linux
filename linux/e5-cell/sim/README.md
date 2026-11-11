# E5 live runtime: SIM, OS timers, identity, KASUMI (cellular data development, paused 2026-11-11)

`sim_runtime.c` is upstream's `linux/isi-runtime/sim_runtime.c` with every E6 ISASW
address replaced by its E5 fw 101.003 twin (`docs/devices/e5-00/cellular-map.md`),
plus the E5 bytes of the host IRQ wrapper check. The headers are upstream's.
`e5rt-sim.c` is `../e5rt.c` with the SIM modes added (`--sim-selftest`,
`--sim-reset-selftest`, `--sim-probe <dev>`, `--sim-reset <dev>`): SIM patch window
c60f0000 from the snapshot, SIM register page trapped and forwarded to
/dev/rapuyama-sim (drivers/misc/rapuyama_sim.c).

Build: `clang ... -I. -o e5rt e5rt-sim.c sim_runtime.c` (flags as in ../build.sh).

State (qemu-arm, simulated card): BOTH upstream SIM self-tests PASS (2026-11-10). Earlier:
`--sim-reset-selftest` reads a wrong ATR and `--sim-selftest` stalls. Likely cause:
upstream reads the SIM HAL context at E6 offsets (+0x30 state, +0xb8 voltage setter,
rx/tx buffers, +0x50/+0x74/+0x78); the E5 context init (c638cfc0) has its own layout.
OS timers: `os_timer_runtime.c` (upstream port, E5 table/pool/owners), modes `--os-timer-selftest`,
`--os-timer-live-selftest`, and `E5RT_OS_CLOCK=1` with `--mdi` for live timers.
Identity: `identity_runtime.c` (E5RT_IMEI_FILE, `--identity-selftest`; secure module 0x12 answered "unavailable").
Offline soak: `--os-soak <s>`. Live: `E5RT_SIM_DEVICE=/dev/rapuyama-sim E5RT_OS_CLOCK=1 [E5RT_IMEI_FILE=..] e5rt <dir> --mdi ...`.
Build: `clang ... -I. -o e5rt e5rt-sim.c sim_runtime.c os_timer_runtime.c identity_runtime.c kasumi_runtime.c`
(flags as in ../build.sh). This is the runtime that registers and carries packet data on the phone.

KASUMI: `kasumi_runtime.c`, software f8/f9 instead of the crypto engine; `--kasumi-selftest` runs ETSI
TS 135203 f8 set 1 (batch + wrapper) and f9 sets 1-3 through the firmware entries and prints each
path's stack depth. The software paths run on a private 8 KB stack.

Live environment (all optional except the first three for registration):
| variable | |
|---|---|
| `E5RT_SIM_DEVICE=/dev/rapuyama-sim` | embedded SIM backend (PMIC/ADC/IRQs via the bridge) |
| `E5RT_OS_CLOCK=1` | firmware OS timers on the Linux clock (needs the SIM device) |
| `E5RT_IMEI_FILE=<file>` | 15-digit IMEI for the identity reader (never printed) |
| `E5RT_QUIET=1` | no per-frame MDI / host frame lines |
| `E5RT_SIM_TRACE=1` | APDU headers, status words, IRQ/HAL timer/register/PMIC events (never data phases) |
| `E5RT_MSG_TRACE=1` / `=data` | inter-task messages from the first GPDS request / first uplink packet (`=all`: offline) |
| `E5RT_OS_TRACE=1` | OS timer fires |
| `E5RT_POOL_DUMP=<file>` | on SIGTERM, e5rt's copy of the second DSP pool |
| `E5RT_DSP_DIRECT=1` | map the real DSP window (kernel 4, mdi buffers mode 2). BREAKS the DSP link: do not use |

Packet data: `../e5gprs.c` (`E5GPRS_DELAY=20 e5gprs <apn>`), status in ../README.md.

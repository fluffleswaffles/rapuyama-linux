# E5 SIM runtime port (WORK IN PROGRESS, does not pass yet)

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
Build: `clang ... -I. -o e5rt e5rt-sim.c sim_runtime.c os_timer_runtime.c`.

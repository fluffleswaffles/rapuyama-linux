# RapuYama ISI runtime

Development runtime for the E6's preserved ISASW firmware, with a Phonet packet endpoint in `drivers/net/rapuyama_isi.c`. The firmware runs in a separate ARM userspace process. It owns private copies of code and RAM, its peripheral register address ranges are ordinary anonymous memory. Live MDI mode additionally maps three reserved DSP memory windows read-only before dropping privileges. It drops privileges, installs a seccomp syscall filter and limits memory and request execution time.

The Phonet transport and firmware packet allocation, service dispatch, response dequeue and freeing have been exercised together in QEMU. On hardware, live MDI and SIM backends, linux-backed OS timers, the optional factory identity provider, the ACodec clock through Linux CCF and the original audio-server output policy produced confirmed audible UMTS/3G calls in both directions. With the volatile private SBIF consumer, two consecutive outgoing GSM/2G calls and one incoming GSM call reached ACTIVE with sound, then NO_CALL on the same live runtime with DSP9.0 responsive. The consumer preserves original queue/free/semaphore handling and never writes physical calibration storage. This remains a development runtime: long endurance, controlled recovery, SMS and data need separate end-to-end verification, and the earlier UMTS incoming secure-path fault remains unresolved.

## Build

```sh
./linux/isi-runtime/build.sh /path/to/build
python3 linux/isi-runtime/prepare.py --code /path/to/isasw.bin --runtime /path/to/1f8b0000.bin --secure /path/to/1ffe0000.bin --out /path/to/private-images
```

The preparation tool accepts one verified E6 firmware image and complete preserved RAM snapshots. Firmware and snapshots are private inputs and are not distributed. It preserves every RAM byte and applies only explicit interrupt, time accounting and cache-helper replacements to the private code copy.

## Kernel

The original firmware addresses start at `0xc6200000`. Select the optional RapuYama `VMSPLIT_3G25` configuration so these addresses are in userspace, together with `CONFIG_PHONET=y` and `CONFIG_RAPUYAMA_ISI=y`. Other memory splits, including the normal E6 build, cannot map the original addresses. The default configuration is unchanged.

A separate configuration can be supplied to the canonical kernel build with `KCONFIG_FRAGMENT=/path/to/config.fragment`; `INITRAMFS_EXTRA=/path/to/initramfs.list` adds an optional userspace file list. Use a separate `KOUT` for experiments.

The packet endpoint `/dev/rapuyama-isi` allows one privileged backend opener. One read returns one outgoing ISI frame; one write submits one incoming frame. The media byte is `0x15`; the two-byte length at offset 4 is little endian and counts bytes after the first six. The network interface uses standard big endian Phonet headers. Short reads and failed copies into caller memory retain the outgoing packet. Carrier follows backend ownership.

## Run

```sh
rapuyama-isi-runtime /path/to/private-images
rapuyama-isi-runtime /path/to/private-images --backend /dev/rapuyama-isi
rapuyama-isi-runtime /path/to/private-images --mdi /dev/rapuyama-mdi --backend /dev/rapuyama-isi
```

The first command tests 100 MTC version transactions entirely inside the process. `--dsp-selftest` and `--dsp-tx-selftest` run 100 simulated DSP deliveries or 100 publishes with DSP acknowledgements against anonymous ring memory; they need the 3G25 kernel too. The second serves Phonet packets from the kernel endpoint. The interface needs a host Phonet address, normally `0x6c`. A firmware fault or unsupported privileged operation terminates the process and disconnects the backend; it does not execute firmware in kernel mode.

## DSP transport development

`CONFIG_SOC_RAPUYAMA_MDI` provides the optional MCU/DSP descriptor controller and `/dev/rapuyama-mdi`. No device node is enabled in the normal device tree. The driver adopts existing ring headers, handles RX/TX interrupts and transfers one 16-byte little endian descriptor per read or write. It does not boot or reset the DSP. The runtime attaches with `--mdi`, maintains private shadow rings and replays pending descriptors from the hardware consumer position.

The status ioctl reports both headers and interrupt counters; `POLLPRI` indicates an interrupt since the last status read. The optional copied-buffer mode validates long-packet descriptor bounds and tags, copies outgoing payloads into reserved AP RAM before publishing, and copies incoming DSP buffers into the opener's private address space before consuming. Raw descriptor mode does not synchronize payload buffers. The transport has passed repeated version transactions on hardware. This does not validate radio activation or recovery from a DSP fault.

`rapuyama-isi-runtime /path/to/private-images --dsp-selftest` injects 100 synthetic DSP replies with distinct allocation tags into anonymous memory and exercises the original firmware RX interrupt handler, scheduler, host routing, ring index wrapping and packet freeing. It has no hardware access and does not establish radio operation.

Live mode needs `/dev/mem` read access during setup for the reserved DSP windows at physical `0x101ff000`, `0x11400000` and `0x115a0000`; it closes the descriptor before installing the sandbox. These mappings are read-only, while incoming packet buffers are copied by the MDI driver. Firmware assertions and reset requests terminate the process with diagnostics. Use one long-lived runtime so outstanding DSP allocation tags can be returned in their original groups of three.

## Standalone SIM probe

`--sim-selftest` exercises the original SIM HAL and patch code with a simulated STATUS reply and seven consecutive T=0 transfer phases, including an INS acknowledgement before sending command data. `--sim-reset-selftest` runs the original cold reset, periodic HAL callbacks, ATR receive handler and completion with a simulated card. `--sim-probe /dev/rapuyama-sim` uses the optional exclusive SIM register/IRQ bridge. `--sim-reset /dev/rapuyama-sim` resets the card through the original HAL and reports its ATR. These modes run separately from the MDI backend and require a validated SIM patch image in addition to the three normal images; prepare it with `--sim-patches`.

The bridge exposes bounded register transfers and six one-shot IRQ events, with a narrow PMIC write allowlist and configuration restoration on close. It is currently an experimental module tied to one verified kernel build. The runtime opens its descriptor before dropping privileges, traps supported Thumb MMIO instructions on a protected private page, and invokes the original HAL callbacks in userspace. Private firmware counters follow Linux monotonic time; the original hardware timer is not mapped. Standalone probes record upper completion events without reentering the copied transaction state machine. Their HAL timers use Linux monotonic time and original callbacks without claiming the original timer IRQ. The optional `SIM_APDU` environment variable supplies space-separated hexadecimal short APDUs for transport development. Outgoing command data waits for the card's INS acknowledgement; `6Cxx` and `61xx` request a bounded retry or GET RESPONSE. Unsupported lengths and procedure bytes stop the probe. The event loop also checks RX FIFO indices after a poll timeout and dispatches the original receive callback only when its complete requested batch is present, handling a threshold edge missed during trapped MMIO. Real card tests have selected the USIM application and read ICCID and IMSI. Card transport does not establish network registration, calls or SMS.

## UMTS integrity

The private runtime selects the firmware's existing software KASUMI f9 path for short messages as well as long ones. This computes the original integrity MAC without accessing the unmapped hardware accelerator or its semaphore. The original wrapper ABI is retained. The software path matches the first three integrity vectors in [ETSI TS135203](https://www.etsi.org/deliver/etsi_TS/135200_135299/135203/19.00.00_60/ts_135203v190000p.pdf); the embedded backend selftest also checks the 189-bit vector in the native sandbox. Other hardware accelerator paths remain unadapted. This does not establish network or call operation.

UMTS f8 scatter/gather requests use the original software KASUMI key expansion and block cipher with the UMTS nonce format. The adapter bounds packet and segment counts and preserves the stream across segments. The native selftest checks the public 120-bit ETSI vector, fragmented input and output bounds.

Set `ISI_TRACE` to a new private file to retain complete MDI descriptors and copied long-message payloads, plus host requests and initial ring headers. The runtime opens the file before dropping privileges, records monotonic timestamps and stops tracing after 8MiB or an I/O error while leaving transport running. Existing files are never overwritten. Each little endian record has nine 32-bit words: magic `0x54495349`, kind (1 RX, 2 TX, 3 host request, 4 initial status), payload length, four descriptor words and the low/high halves of monotonic microseconds, followed by the payload. The trace may contain subscriber and message data; keep it private.

Raw per-descriptor text logging is disabled by default; set `ISI_MDI_VERBOSE=1` for short diagnostic sessions. Fatal diagnostics and DSP panic descriptors remain visible. The binary trace is independent of this switch. Avoid external truncation loops, which can erase the final failure report.

Optional factory identity provider: `ISI_DEVICE_IMEI_FILE` names a private file containing this E6's original 15-digit IMEI, optionally followed by one newline. The runtime validates its checksum and loads it before sandboxing; it never prints the value. This narrowly replaces the firmware identity-read operation, preserving the original allocation, event, padding and IMEI/IMEISV conversion paths. It does not emulate arbitrary secure calls, translate private buffers into physical addresses, or generate an identity. Without this opt-in file the firmware identity path is unchanged and may still enter an unsupported Symbian service. Keep the file private and do not provision another device's identifier. `--identity-selftest` exercises the original reader in an isolated process without MDI or SIM hardware; fictional vectors belong only in this test, never in live mode. The working call configuration combined this provider with the OS timer adapter; the relative contribution of each was not isolated.

Experimental OS timer adapter: `--os-timer-selftest` redirects the original low-level timer entry points only in a separate test process. It exercises original OS timer expiry/cancellation callbacks, the EABI r0/r1 return pair, all 279 handles and a clock longer than 32-bit microseconds. Live mode enables the adapter with `ISI_OS_TIMERS=1`. The adapter validates and adopts the captured queue; coupled SIM HAL and general OS timer operation need separate verification. The SMS timeout self-test demonstrates that the original SMS server arms timer 144 for 60 seconds and uses its expiry to retry network waiting. A successful emulator test is not proof of SMS delivery.

`--sms-timeout-selftest` runs the original SMS server with a fictional destination and anonymous MDI mappings. It isolates SMS timer 144, advances exactly 60 seconds, and requires a client status indication plus a rearmed network wait. The test has no radio peer and sends no physical SMS.

`--os-timer-live-selftest` validates the task ownership table, adopts the copied timers and tests an original OS expiry with `CLOCK_MONOTONIC`. It cancels the captured Energy Management timers because Linux owns the battery/charger, and timer 212 because Linux already services PA_SWD. Initial ASIC watchdog timer 266 is also handed over. These cancellations use the original OS API and modify private RAM only. `test_timer_ownership.py` reproduces the I2C and secure-monitor conflicts without this handoff and advances 600 seconds of the remaining captured callbacks with it. The replay uses mock PMIC values and no radio peer; it does not establish real service stability. Live mode uses the attach and service functions through `ISI_OS_TIMERS=1`; the separate selftest validates them in isolation.

# RapuYama ISI runtime

Development runtime for the E6's preserved ISASW firmware, with a Phonet packet endpoint in `drivers/net/rapuyama_isi.c`. The firmware runs in a separate ARM userspace process. It owns private copies of code and RAM, its peripheral register address ranges are ordinary anonymous memory. Live MDI mode additionally maps three reserved DSP memory windows read-only before dropping privileges. It drops privileges, installs a seccomp syscall filter and limits memory and request execution time.

The Phonet transport and firmware packet allocation, service dispatch, response dequeue and freeing have been exercised together in QEMU. On hardware, live MDI and SIM backends, linux-backed OS timers, the optional factory identity provider, the ACodec clock through Linux CCF and the original audio-server output policy produced confirmed audible UMTS/3G calls in both directions. With the volatile private SBIF consumer, two consecutive outgoing GSM/2G calls and one incoming GSM call reached ACTIVE with sound, then NO_CALL on the same live runtime with DSP9.0 responsive. The consumer preserves original queue/free/semaphore handling and never writes physical calibration storage. Later media-to-call tests exposed a silent microphone uplink despite audible downlink; that remains unresolved. This is a development runtime: long endurance, registration reliability, controlled recovery and data need separate verification.

## Build

```sh
./linux/isi-runtime/build.sh /path/to/build
python3 linux/isi-runtime/prepare.py --code /path/to/isasw.bin --runtime /path/to/1f8b0000.bin --secure /path/to/1ffe0000.bin --out /path/to/private-images
```

The preparation tool accepts one verified E6 firmware image and complete preserved RAM snapshots. Firmware and snapshots are private inputs and are not distributed. It preserves every RAM byte and applies explicit interrupt and cache-helper replacements to the private code copy. Original millisecond and 5 ms time accounting remains intact; the private counter starts at the captured accounting sample.

With `e6.swd=1`, the kernel also exposes the original read-only `PA_DEVLOCK` status query through `/dev/rapuyama-devlock` (root only). The runtime opens it before dropping privileges and redirects only the verified `c6263982` wrapper. Each query executes the original secure service in its preserved address space, serialized with watchdog servicing. Both locked and unlocked results are passed through. invalid requests and unavailable or unsuccessful secure replies stop the runtime. The real query and subsequent watchdog servicing have passed on hardware, and a subsequent incoming call survived this specific former secure fault. This does not implement other device-lock operations or establish general fault recovery.

The same serialized secure context now has a root-only `/dev/rapuyama-simlock` interface for the original `PA_SIMLOC30` check at `c6263fc8`. It copies the 456-byte request and its bounded indirect buffer into preserved memory, runs the original wrapper, and returns the genuine 28-byte response. The runtime validates private buffer bounds and redirects only that verified wrapper. Production MDI mode with the embedded SIM backend requires the identity provider and both secure bridges at startup; missing support is rejected before firmware service. The serialized context checks preserved heap capacity before allocating watchdog, DEVLOCK or SIM-lock buffers because the original allocator asserts on exhaustion. Original ARM contract tests, context-switch tests and adapter error checks pass; physical verification of this new path is pending. This does not change SIM-lock policy or provide general secure-service emulation.

## Linux speaker playback

The ALSA frontend uses the standard loopback card and default plug/dmix configuration. The PCM backend converts 48 kHz stereo S16 samples into the original 240-sample Q23 channel planes and uses the original fixed-depth native mode0: ten native frames queued, publication field5, no ramp or `51 enable`. The original disable/close sequence remains mandatory; an unacknowledged close retains the endpoint and forbids reuse.

On E6, `snd-aloop` must be loaded with `coherent_buffers=1`. The opt-in buffer allocator uses the platform device's coherent DMA allocation and mmap APIs; unrelated platforms retain the normal vmalloc default. Without coherent mappings, an E6 mmap client reproducibly read thousands of stale samples after a kernel silence reset. The generic dmix mixer interpreted stale nonzero destinations as active sums and mixed old audio into new data. Coherent allocation made all four silence-reset checks read zero, restored bit-exact full-source transfer through plug/dmix and passed a two-client mixing test. No global cache disable, guessed physical address or modem register change is involved. The lifecycle helper refuses a loopback missing this requirement rather than silently starting a noisy backend.

The silent microphone after media playback may be related to audio-server item `01`: `audio_start` sets it to `0x0d` with operation `06` (resource `0x0b`, message `0x14`), but cleanup does not restore it and `modem-call`/`modem-answer` do not set it. The cause is still unconfirmed.

Clean full-phrase loudspeaker playback through the ordinary ALSA default device has been confirmed by the user. Five consecutive production-backend streams and a 16-second OGG ringtone completed with acknowledged cleanup on the same modem owner. These checks do not establish arbitrary-duration playback, microphone capture or media/call handoff reliability.

## Kernel

The original firmware addresses start at `0xc6200000`. Select the optional RapuYama `VMSPLIT_3G25` configuration so these addresses are in userspace, together with `CONFIG_PHONET=y` and `CONFIG_RAPUYAMA_ISI=y`. Other memory splits, including the normal E6 build, cannot map the original addresses. The default configuration is unchanged.

A separate configuration can be supplied to the canonical kernel build with `KCONFIG_FRAGMENT=/path/to/config.fragment`; `INITRAMFS_EXTRA=/path/to/initramfs.list` adds an optional userspace file list. Use a separate `KOUT` for experiments.

The packet endpoint `/dev/rapuyama-isi` allows one privileged backend opener. One read returns one outgoing ISI frame; one write submits one incoming frame. The media byte is `0x15`; the two-byte length at offset 4 is little endian and counts bytes after the first six. The network interface uses standard big endian Phonet headers. Short reads and failed copies into caller memory retain the outgoing packet. Carrier follows backend ownership.

The runtime reserves `0xc6000000-0xc7200000` before allocating other memory so the host heap cannot occupy firmware addresses. Fixed mappings replace only their own part of that reservation. The SIM adapter also checks kernel-address signatures and must be rebound when the kernel layout changes; matching module vermagic is not enough.

## Run

```sh
rapuyama-isi-runtime /path/to/private-images
rapuyama-isi-runtime /path/to/private-images --backend /dev/rapuyama-isi
rapuyama-isi-runtime /path/to/private-images --mdi /dev/rapuyama-mdi --backend /dev/rapuyama-isi
```

The first command tests 100 MTC version transactions entirely inside the process. `--dsp-selftest` and `--dsp-tx-selftest` run 100 simulated DSP deliveries or 100 publishes with DSP acknowledgements against anonymous ring memory; they need the 3G25 kernel too. The second serves Phonet packets from the kernel endpoint. The interface needs a host Phonet address, normally `0x6c`. A firmware fault or unsupported privileged operation terminates the process and disconnects the backend; it does not execute firmware in kernel mode.

On the phone, `init.sh` calls `modem-boot`, which runs `modem-start` once per boot. The MDI doorbell register `0x20040280` must read 4 after the Symbian handoff. A value of 0 means the DSP is no longer ready; starting against it can panic the runtime and reset the phone. `modem-start` uses the `doorbells` helper to wait up to 20 s, then exits with status 3. `modem-boot` records `failed`/`dsp`, shown in Settings as "Not started: DSP not ready, restart the phone". An unreadable register does not block startup.

Once the modem is ready, `modem-boot` starts `modem-data`. With `mobile_data=1` in Yamashell, it waits for network registration and keeps one `modem-data-connect` session running. The `apn` setting selects an explicit profile; an empty setting uses automatic APN selection. Failed sessions reconnect after 10 s, doubling up to 1 min. The service sets the clock once through NTP for HTTPS certificate validation and ends the session within 2 s of disabling mobile data. `/run/rapuyama-modem/data` publishes `off`, `waiting`, `connecting`, `connected ADDRESS`, `retry SECONDS` or `failed` for the status bar and Settings.

## Packet data

The build also produces `modem-gprs`. `--probe` reads the GPDS version and status; `--data APN` attaches, creates a pipe and activates a PDP context. Attach requests use `FOLLOW_ON` and proceed directly to context setup. An attach can take up to 120 s. The client retries `ACCESS_BARRED` (`cause fd`) up to three attaches in total, 10 s apart; `modem-data-connect` allows up to 400 s for setup. A client timeout does not cancel a pending firmware radio operation.

`modem-data-connect [PROFILE]` runs one session. Without a profile, it reads the registered network's MCC/MNC and selects its internet APN from `/usr/share/rapuyama-modem/apns.tsv`. `apn-table.py` generates this table from GNOME's public-domain `mobile-broadband-provider-info`. Automatic selection refuses roaming and unknown networks; supply an explicit profile in those cases. The session installs a default route through `gprs0` (metric 100), a policy rule using table 182 and the network's DNS in `/etc/resolv.conf`. Cleanup removes the session's routes and rule and restores the previous resolver configuration.

GSM data works, including DNS, TCP/HTTP and HTTPS with certificate validation using an automatic Beeline profile. UMTS DNS, HTTP and 1000-byte ICMP packets also work after the CELL_DCH downlink-buffer fix. UMTS still has intermittent attach refusals and multi-minute outages after RRC reconfiguration; long sessions are not reliable yet. The failed attach traces show unanswered RRC connection requests despite a normal DSP signal report. The cause is unresolved. GSM is not forced automatically.

### Connection profiles

`modem-gprs --symbian-data APN` uses the context layout from `CMmPacketContextMessHandler::GpdsContextConfigureReq`: a DNS request, explicit compression defaults (`ff/ff`), APN and radio-activity notification. `--data APN` keeps the existing wire format.

`modem-gprs --profile FILE` reads an owner-only regular file. Symlinks, group/other access, duplicate keys and unknown keys are rejected; values are not expanded by a shell. An unauthenticated IPv4 profile looks like this (replace `internet` with your operator's APN):

```ini
apn=internet
auth=none
data_compression=default
header_compression=default
```

PAP requires `auth=pap`, `username=` and `password=`. CHAP requires `auth=chap`, `username=`, `challenge_hex=`, a 16-byte `response_hex=` and decimal `identifier=`. The client expects CHAP values from the caller; it does not negotiate the challenge or generate a response. Authentication is sent after configuration acceptance and before activation, and requires a matching response. PAP/CHAP with real credentials has not been verified on hardware.

Credentials are refused unless the running modem process can be inspected and has no nonempty `ISI_TRACE`, since full packet traces can expose them. The client does not restart the modem to disable tracing. It wipes secret file contents and outgoing request buffers after use and does not print them in errors.

`qos=r99-subscribed` adds a separate R99 QoS configure exchange before activation, after any authentication. It sends requested/minimum QoS subblocks `0x06`/`0x07` (20 bytes each), a DNS request and radio-activity notification. Classified fields use `SUBSCRIBED` (zero), with all numeric requests and minima zero. This requests subscribed defaults rather than importing the active Symbian QoS policy. `qos=none`, the default, omits this exchange. Setup stops on rejection, a short reply or a mismatched context ID.

IPv6 context encoding exists, but Linux IPv6 address and interface setup are not implemented. Operator-profile import and recovery across radio-mode changes are also unfinished.

## DSP transport development

`CONFIG_SOC_RAPUYAMA_MDI` provides the optional MCU/DSP descriptor controller and `/dev/rapuyama-mdi`. No device node is enabled in the normal device tree. The driver adopts existing ring headers, handles RX/TX interrupts and transfers one 16-byte little endian descriptor per read or write. It does not boot or reset the DSP. The runtime attaches with `--mdi`, maintains private shadow rings and replays pending descriptors from the hardware consumer position.

The status ioctl reports both headers and interrupt counters; `POLLPRI` indicates an interrupt since the last status read. The optional copied-buffer mode validates long-packet descriptor bounds and tags, copies outgoing payloads into reserved AP RAM before publishing, and copies incoming DSP buffers into the opener's private address space before consuming. Raw descriptor mode does not synchronize payload buffers. The transport has passed repeated version transactions on hardware. This does not validate radio activation or recovery from a DSP fault.

`rapuyama-isi-runtime /path/to/private-images --dsp-selftest` injects 100 synthetic DSP replies with distinct allocation tags into anonymous memory and exercises the original firmware RX interrupt handler, scheduler, host routing, ring index wrapping and packet freeing. It has no hardware access and does not establish radio operation.

Live mode needs `/dev/mem` read access during setup for the reserved DSP windows at physical `0x101ff000`, `0x11400000` and `0x115a0000`; it closes the descriptor before installing the sandbox. These mappings are read-only, while incoming packet buffers are copied by the MDI driver. Firmware assertions and reset requests terminate the process with diagnostics. Use one long-lived runtime so outstanding DSP allocation tags can be returned in their original groups of three.

HSUPA MAC-e responses (`PN_WCDMA`, object `0x3b`, message `0x61`) reference payloads outside the message envelope. Before publishing a nonempty response, the runtime validates the DSP address and copies the word-rounded payload out of its private shadow with `RAPUYAMA_MDI_DSP_PUT`. Copies are limited to the DSP buffer window and 4096 bytes. The updated MDI driver is required; an unsupported ioctl stops publication.

CELL_DCH downlink messages (object `0x3c`, message `0x00`) likewise reference transport blocks in DSP memory rather than carrying them inline. A big-endian 16-bit count at offset 16 precedes entries containing a 16-bit ID and 32-bit DSP address. Before the firmware RX interrupt, `isi_dsp_dl_buffers` fetches each block with `RAPUYAMA_MDI_DSP_GET`, at most 2 KiB per entry and below `0x1b0000`. Without this copy the firmware reads stale shadow memory, loses downlink STATUS/RESET ACK messages and repeatedly re-establishes the bearer. CELL_FACH blocks arrive inline in `0x3b`/`0x1e` and do not need this fetch.

There is also an unresolved SI copy-length discrepancy: the original packer can append 18 SI bits to a 690-bit PDU, while the current decoder copies 88 bytes (704 bits), leaving four bits outside the copy. Hardware occurrence is unconfirmed. The `harq_copy_span.h` planner calculates a guarded full span but does not change runtime copy sizes.

## Standalone SIM probe

`--sim-selftest` exercises the original SIM HAL and patch code with a simulated STATUS reply and seven consecutive T=0 transfer phases, including an INS acknowledgement before sending command data. `--sim-reset-selftest` runs the original cold reset, periodic HAL callbacks, ATR receive handler and completion with a simulated card. `--sim-probe /dev/rapuyama-sim` uses the optional exclusive SIM register/IRQ bridge. `--sim-reset /dev/rapuyama-sim` resets the card through the original HAL and reports its ATR. These modes run separately from the MDI backend and require a validated SIM patch image in addition to the three normal images; prepare it with `--sim-patches`.

The bridge exposes bounded register transfers and six one-shot IRQ events, with a narrow PMIC write allowlist and configuration restoration on close. It is currently an experimental module tied to one verified kernel build. The runtime opens its descriptor before dropping privileges, traps supported Thumb MMIO instructions on a protected private page, and invokes the original HAL callbacks in userspace. Private firmware counters follow Linux monotonic time; the original hardware timer is not mapped. Standalone probes record upper completion events without reentering the copied transaction state machine. Their HAL timers use Linux monotonic time and original callbacks without claiming the original timer IRQ. The optional `SIM_APDU` environment variable supplies space-separated hexadecimal short APDUs for transport development. Outgoing command data waits for the card's INS acknowledgement; `6Cxx` and `61xx` request a bounded retry or GET RESPONSE. Unsupported lengths and procedure bytes stop the probe. The event loop also checks RX FIFO indices after a poll timeout and dispatches the original receive callback only when its complete requested batch is present, handling a threshold edge missed during trapped MMIO. Real card tests have selected the USIM application and read ICCID and IMSI. Card transport does not establish network registration, calls or SMS.

## UMTS integrity

The private runtime selects the firmware's existing software KASUMI f9 path for short messages as well as long ones. This computes the original integrity MAC without accessing the unmapped hardware accelerator or its semaphore. The original wrapper ABI is retained. The software path matches the first three integrity vectors in [ETSI TS135203](https://www.etsi.org/deliver/etsi_TS/135200_135299/135203/19.00.00_60/ts_135203v190000p.pdf); the embedded backend selftest also checks the 189-bit vector in the native sandbox. Other hardware accelerator paths remain unadapted. This does not establish network or call operation.

GPRS LLC GEA3 uses the firmware's software path at `c62f2ba6` for all lengths, with the same key schedule and block functions as f8. This avoids the unmapped cipher engine at `c6024c00`, which the original wrapper at `c62f2274` used for requests below 5120 bits. GEA3 has not been verified on hardware because the observed network has not negotiated it.

UMTS f8 scatter/gather requests use the original software KASUMI key expansion and block cipher with the UMTS nonce format. The adapter bounds packet and segment counts and preserves the stream across segments. The native selftest checks the public 120-bit ETSI vector, fragmented input and output bounds.

Set `ISI_TRACE` to a new private file to retain complete MDI descriptors and copied long-message payloads, plus host requests and initial ring headers. The runtime opens the file before dropping privileges, records monotonic timestamps and stops tracing after 8MiB or an I/O error while leaving transport running. Existing files are never overwritten. Each little endian record has nine 32-bit words: magic `0x54495349`, kind (1 RX, 2 TX, 3 host request, 4 initial status), payload length, four descriptor words and the low/high halves of monotonic microseconds, followed by the payload. The trace may contain subscriber and message data; keep it private.

Raw per-descriptor text logging is disabled by default; set `ISI_MDI_VERBOSE=1` for short diagnostic sessions. Fatal diagnostics and DSP panic descriptors remain visible. The binary trace is independent of this switch. Avoid external truncation loops, which can erase the final failure report.

`ISI_GPRS_DIAG=1` adds GPDS and pipe replies as trace kind 5 and up to 4096 f8 metadata records as kind 6. The latter contain nine little-endian 32-bit words: sequence, batch count, item index, bearer, direction, segment count, total bit length, COUNT-C and the two bytes before the first ciphered segment (bit 16 marks readable bytes). Older records have only the first seven words. Metadata is also printed to stderr as `F8` lines, without keys, addresses or ciphered packet contents.

Optional HARQ diagnostics are disabled by default:

- `ISI_HARQ_VERIFY=1` reads back up to 64 payloads after `DSP_PUT`, before publication. It requires `DSP_GET`; a read failure or mismatch stops the runtime. Records go to stderr and, with tracing, kind 7 (ten little-endian 32-bit words, last reserved). They contain correlation, tags, lengths, difference counts and errors, not payloads or addresses. Equal readback confirms the copy, not radio delivery.
- `ISI_HARQ_STATE=1` requires `ISI_HARQ_VERIFY=1` and pairs readbacks with read-only HARQ process and flow state, including E-TFC indices and saved/table values.
- `ISI_HARQ_PDU=1` requires `ISI_HARQ_STATE=1` and compares response byte 21 and bit length with the selected process and native PDU table entry.
- `ISI_HARQ_PACK=1` requires `ISI_HARQ_PDU=1` and captures metadata at native packer call sites, limited to 64 packing captures and eight allocation records. Generation, buffer, tags, index, length and attempt correlate publications with snapshots. Buffer steps come from the native initializer, not independent DSP heap inspection; allocation bounds remain unresolved.

These options only observe state; they do not repair descriptors, expand copies or restart the DSP.

Optional factory identity provider: `ISI_DEVICE_IMEI_FILE` names a private file containing this E6's original 15-digit IMEI, optionally followed by one newline. The runtime validates its checksum and loads it before sandboxing; it never prints the value. This narrowly replaces the firmware identity-read operation, preserving the original allocation, event, padding and IMEI/IMEISV conversion paths. It does not emulate arbitrary secure calls, translate private buffers into physical addresses, or generate an identity. Without this opt-in file the firmware identity path is unchanged and may still enter an unsupported Symbian service. Keep the file private and do not provision another device's identifier. `--identity-selftest` exercises the original reader in an isolated process without MDI or SIM hardware; fictional vectors belong only in this test, never in live mode. The working call configuration combined this provider with the OS timer adapter; the relative contribution of each was not isolated.

Experimental OS timer adapter: `--os-timer-selftest` redirects the original low-level timer entry points only in a separate test process. It exercises original OS timer expiry/cancellation callbacks, the EABI r0/r1 return pair, all 279 handles and a clock longer than 32-bit microseconds. Live mode enables the adapter with `ISI_OS_TIMERS=1`. The adapter validates and adopts the captured queue; coupled SIM HAL and general OS timer operation need separate verification. The SMS timeout self-test demonstrates that the original SMS server arms timer 144 for 60 seconds and uses its expiry to retry network waiting. A successful emulator test is not proof of SMS delivery.

`--sms-timeout-selftest` runs the original SMS server with a fictional destination and anonymous MDI mappings. It isolates SMS timer 144, advances exactly 60 seconds, and requires a client status indication plus a rearmed network wait. The test has no radio peer and sends no physical SMS.

`--os-timer-live-selftest` validates the task ownership table, adopts the copied timers and tests an original OS expiry with `CLOCK_MONOTONIC`. It cancels the captured Energy Management timers because Linux owns the battery/charger, and timer 212 because Linux already services PA_SWD. Initial ASIC watchdog timer 266 is also handed over. These cancellations use the original OS API and modify private RAM only. `test_timer_ownership.py` reproduces the I2C and secure-monitor conflicts without this handoff and advances 600 seconds of the remaining captured callbacks with it. The replay uses mock PMIC values and no radio peer; it does not establish real service stability. Live mode uses the attach and service functions through `ISI_OS_TIMERS=1`; the separate selftest validates them in isolation.

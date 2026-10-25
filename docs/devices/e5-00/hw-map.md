# Nokia E5-00 hardware map

Target: **RM-632, firmware 101.003**, core image `rm632_101.003_prd.core.fpsx`

This is a map of the hardware described and used by this firmware, with ROM addresses for checking each finding in a disassembler.

## Differences from the E6

| Part | E5-00 finding | Porting consequence |
| --- | --- | --- |
| SoC family | RAPU baseport: `karap.dll`, `egpio.dll`, `base_rapu` paths and apertures matching the E6 rapuyama port | Candidate for reusing SoC support; exact ASIC/revision still comes from step 0 |
| System tick | IRQ 35, handler `0x8009d778`, register `0x200401c4` | **DO NOT** copy the E6 IRQ 242 / `0x200337a0` clockevent configuration |
| Display | `d_display.ldd` directly drives on-chip graphics | The E6 VideoCore boot/VCHI/DISP path does not describe this firmware's display path |
| Keyboard | ROM default is an **8 x 8 GPIO matrix** in `ekeyb.dll` | The E6 TC3589x expander driver and its I2C address do not apply |
| Mass storage | `mednand.pdd` and one MMC controller in `EPBUSMV.DLL` | Do not assume the E6 internal eMMC / second MMC controller arrangement |
| Secure watchdog | Very much different to E6's. Do not assume it works the same, better double check yourself. | Do not enable the E6 PA_SWD bridge with E6 addresses |

The firmware itself names `Rapu_Display` (`0x8018379c`), `Rapu_GDMASS` (`0x800b3ac8`), `DUsbRapuIf` and `base_rapu\assp\rapu` paths. 

The ROM contains no `unsigned_2727b1.dll`, `vcdriver.dll`, `vchi.dll` or `vcdispman*.dll` directory entries. Together with the native display driver's actual register accesses, this establishes a different software display architecture. Probably a new GPU driver needed for the lack of Broadcom

## Address conventions

- `P:` means a physical bus address, suitable for a device tree after validation
- `V:` means a smybian kernel virtual address
- `ROM:` is a virtual instruction/data address in this exact firmware. File offsets below refer to the address-indexed **plain FPSX partition** (`marker 0x54`, type `0x17`, partition `0x01`), not to offsets in the original FPSX container

For unpaged ROM bytes, `file_offset = 0x00e41000 + (ROM_address - 0x80000000)`. Beyond the unpaged boundary, use the ROM page index, not this addition. All ranges below are half-open unless explicitly described as inclusive.

## ROM layout and reproducibility

| Item | Value | Evidence |
| --- | --- | --- |
| Plain flash partition size | `0x07978800` | 419 FPSX data blocks |
| APE ROM base in plain partition | `0x00e41000` | Bootstrap branch words and consistent `TRomHeader` |
| Textual image marker | `EPOCARM4ROM` at file `0x00e40f00` | Marker is 0x100 bytes before the executable ROM header |
| ROM virtual base | `0x80000000` | Header `+0x8c` |
| Logical ROM size | `0x03200000` (50 MiB) | Header `+0x90` |
| Root directory list | `0x800451c8` | Header `+0x94`, 2618 walked file entries |
| Kernel data virtual base | `0xc8000000` | Header `+0x98` |
| Kernel virtual limit | `0xc9100000` | Header `+0x9c`. not a physical ram size declaration |
| Supervisor data size | `0x0009bc60` | Header `+0xc8` |
| Kernel configuration flags | `0x7e` | Header `+0xb8` |
| ROM header size | `0x200` | Header `+0xc0` |
| Unpaged ROM boundary | `0x007da000` | Header `+0x148`; page 0x7da has compression=1, paging attribute=1 |
| Pageable size | `0x029ac618` | Header `+0x14c` |
| Page index offset | `0x2b00`, plain file `0x00e43b00` | Header `+0x150`, `SRomPageInfo` entries |
| Core executable set | `ekern.exe` + RapuYama kernel extensions, LDDs/PDDs | `TRomImageHeader`, module table below |

Extract and list with the repository tools:

```sh
python3 tools/fpsx_extract.py /path/to/rm632_101.003_prd.core.fpsx e5-analysis/flash
python3 tools/rom_walk.py e5-analysis/flash/m54_t17_p01.bin e41000 list
```

`tools/rom_unpage.py` currently defaults to E6 offsets. For this E5 image its module variables must be `BASE_F=0xe41000`, `TBL=0xe43b00`, `ROM_V=0x80000000`. The index has `{u32 data_start, u16 data_size, u8 compression, u8 attributes}` per logical 4 KiB page. Data starts are relative to `BASE_F`. Pages with `attributes & 1` and compression 1 use byte-pair decompression. the unpaged pages have compression 1 too, but attribute 0 and are stored raw. Reconstructing all 0x3200 pages produces exactly `0x03200000` bytes.

Ghidra analysis uses ARM little endian, ARM:LE:32:v6, raw binary at `0x80000000`, the module text intervals from each valid `TRomImageHeader`, and separate uninitialized peripheral/kernel-data blocks. This language choice describes the analyzed application code,. Step-0 MIDR result remains the authority for the exact CPU revision. Export addresses come from this ROM's `ekern.exe` export directory (`0x80097b64`, 1162 entries). Names can be matched by ordinal to `kernel/eka/eabi/ekernu.def`. Confirm important calls by their instructions as well as names. Whole-range disassembly also decodes some literal pools as instructions, so decompiler warnings and raw constant scans are not accepted as hardware evidence by themselves.


The saved Ghidra pass exports 78 kernel/driver C listing files from this region. Discovery uses module entries, ARM prologues, export targets and explicitly checked leaf functions, with disassembly restricted to module text and global/incremental auto-analysis disabled. The first unrestricted pass was discarded after it treated the ROM data gap as code and exhausted an ARM analyzer's heap. The focused pass completed and saved successfully. Export targets can also denote data, and successful pseudocode output does not certify a function's correctness: the register meanings and critical routines in this map were checked against instructions and ROM table bytes. This is not a decompilation of all 2556 executable images whose headers can be read after unpaging the full ROM.

## Bootstrap physical mappings

The mapping selector at `ROM:0x80001e64` reads the variant value at superpage-extension `+0x738`: value `0x210` selects table `0x80001e90`, other values select `0x80001f04`. The tables have the same mappings except that `P:0x30000000` covers 32 KiB in the first and 16 KiB in the second.

The descriptor format is independently documented in [`bootcpu.inc`](../../../symbian/oss.FCL.sf.os.kernelhwsrv/kernel/eka/include/kernel/arm/bootcpu.inc) and [`bootmain.s`](../../../symbian/oss.FCL.sf.os.kernelhwsrv/kernel/eka/kernel/arm/bootmain.s): low byte is page count, `0x400` means 64 KiB units, `0x800` means 1 MiB units, `0x100` supplies an explicit virtual address, `0x200` supplies an extra permissions descriptor. For example, `0x20020503` means physical `0x20020000`, three 64 KiB units, not address `0x20020503`.

| Physical base | Symbian virtual base | Size | Descriptor at ROM (first table) | Interpretation |
| --- | --- | --- | --- | --- |
| `0x30000000` | `0xc6018000` | `0x8000` / `0x4000` | `0x80001e90` | Shared system/configuration area, attribute descriptor `0x80003010` |
| `0x08000000` | `0xc6011000` | `0x1000` | `0x80001e9c` | External-memory bank controller. Display/camera use `+0x180..0x1f8` |
| `0x09000000` | `0xc6010000` | `0x1000` | `0x80001ea4` | External-memory bank controller, power code uses `+0x70` |
| `0x20010000` | `0xc6012000` | `0x1000` | `0x80001eac` | Mapped peripheral, role unknoewn |
| `0x20020000` | `0xc6020000` | `0x30000` | `0x80001eb4` | Main SoC peripheral window |
| `0x20400000` | `0xc6060000` | `0x4000` | `0x80001ebc` | Interrupt controller |
| `0x20500000` | `0xc6e00000` | `0x100000` | `0x80001ec4` | Mapped aperture, role unknown |
| `0x40000000` | `0xc6050000` | `0x1000` | `0x80001ecc` | Firmware trace/control aperture, accesses in ecust/power. Not established as a UART |
| `0x20600000` | `0xc60a0000` | `0x1000` | `0x80001ed4` | Mapped peripheral, role unknown |
| `0x20680000` | `0xc60af000` | `0x1000` | `0x80001edc` | Mapped periphera,, role unknown |
| `0xfffd0000` | `0xc6070000` | `0x10000` | `0x80001ee4` | Firmware vector/handler aperture, attribute descriptor `0x80003190` |
| `0x40100000` | `0xc6f00000` | `0x100000` | `0x80001ef0` | XTI trace aperture (`xtitx`, `xtiwriter`) |
| `0x20700000` | `0xc60a1000` | `0x1000` | `0x80001ef8` | Mapped peripheral, role unknown |

Within the main SoC window, `physical = virtual - 0xa6000000`. Within the INTC window, `physical = virtual - 0xa5c60000`. These translations apply only to the listed ranges. In particular, `0xc6200000`, `0xc6800000`, `0xc6b00000` and `0xc60f0000` are not automatically MMIO.Bootstrap code uses them for additional firmware/shared-memory mappings. Do not apply the peripheral subtraction to arbitrary `0xc6xxxxxx` values.

Bootstrap literal `ROM:0x80001e40` contains physical `0x10001f00`, and `0x80001df0..0x80001e38` constructs a bank list in `0x30003820` from loader-supplied records. This is evidence for the `0x10000000` RAM region, but does not establish which banks Linux owns, their size on the tested unit, or the physical locations of the modem and shared-memory reservations.

## Periperal blocks

These are E5 driver references translated using the E5 bootstrap table above. Some blocks have multiple users; the inventory records firmware use rather than Linux support.

| Physical base / range | Symbian virtual | Firmware owner | Evidence / role |
| --- | --- | --- | --- |
| `0x20020200` | `0xc6020200` | `bc02_uart1.PDD` | Bluetooth UART side configuration, literal at `0x80137210` |
| `0x20020400` | `0xc6020400` | `spi1.dll` | SPI 1, offsets `+0x04..+0x64` |
| `0x20020600` | `0xc6020600` | `spi2.dll` | SPI 2, same register layout |
| `0x20020804`, `0x20020820` | `0xc6020804`, `0xc6020820` | `power_resources`, SPI drivers | Shared SPI configuration |
| `0x20020a00` | `0xc6020a00` | `spi3.dll` | SPI 3 |
| `0x20024000` | `0xc6024000` | `dma.dll`, `SecLdd.LDD` | DMA/security registers. do not treat the whole page as one channel |
| `0x20024680`, `0x20024780` | `0xc6024680`, `0xc6024780` | `dma.dll` | Per-channel peripheral routing table, eight entries at +4 stride |
| `0x20024800` | `0xc6024800` | `d_display.ldd` | Native graphics controller and plane registers |
| `0x20025800` | `0xc6025800` | `d_display.ldd` | Direct panel MeSSI/RFBI command/data interface |
| `0x20025a00` | `0xc6025a00` | `bc02_uart1.PDD` | Bluetooth UART, probably debug connector |
| `0x20030000`, `0x20030400` | `0xc6030000`, `0xc6030400` | SPI / DMA | SPI transfer/DMA-side registers |
| `0x20030700` | `0xc6030700` | `karap.dll` | CBUS master for PMIC register transactions |
| `0x20031868`, `0x20031870`, `0x2003187c` | `0xc6031868`, `0xc6031870`, `0xc603187c` | Bluetooth UART | UART pin configuration |
| `0x20031900..0x20031b00` | `0xc6031900..0xc6031b00` | `egpio.dll`, USB/camera/custom | GPIO direction/data/mux/pull configuration |
| `0x20032200` | `0xc6032200` | `i2c0.dll` | I2C controller, IRQ 150 |
| `0x20032604`, `0x20032624`, `0x20032644`, `0x20032664` | `0xc6032604` etc. | `ecust.dll`, `karap.dll` | PLL configuration words, table at `0x800a3bb4` |
| `0x20032800` | `0xc6032800` | `EPBUSMV.DLL` | MMC controller |
| `0x20033000` | `0xc6033000` | karap/ecust/power resources | Clock selectors, gates, wakeup banks, counters |
| `0x20033350`, `0x20033354` | `0xc6033350`, `0xc6033354` | `ekern`, monitor drivers | 32.768 kHz fast counter: latch then read |
| `0x20033400..0x20033540` | `0xc6033400..0xc6033540` | `egpio.dll`, `karap.dll` | GPIO interrupt/wakeup banks, 0x40 stride |
| `0x20033600` | `0xc6033600` | `karap.dll` | System counter/control block initialized at `0x8009d7cc` |
| `0x200336c4` | `0xc60336c4` | `power.dll`, `sisa_wrapper.dll` | SoC watchdog reload |
| `0x20034000..0x20035500` | `0xc6034000..0xc6035500` | `campdd.pdd` | Camera processing / transfer blocks |
| `0x20038000` | `0xc6038000` | `4550.ldd`, `4550_jpegkernel.dll` | On-chip multimedia/JPEG hardware path |
| `0x20038214` | `0xc6038214` | `mednand.pdd` | NAND-side control reference |
| `0x20038800` | `0xc6038800` | `usb.dll` | MUSB USB core |
| `0x20038c00` | `0xc6038c00` | `usb.dll` | USB wrapper/PHY-side registers |
| `0x20040000` | `0xc6040000` | karap/ecust/power/USB | System control and interprocessor signals |
| **`0x200401c4`** | **`0xc60401c4`** | **`karap.dll`** | **E5 system-tick handler register** |
| `0x20040280..0x2004028c` | `0xc6040280..0xc604028c` | `karap.dll` | Interprocessor status / set / clear |
| `0x20040384`, `0x20040388` | `0xc6040384`, `0xc6040388` | `usb.dll` | USB-side system control |
| `0x20042800` | `0xc6042800` | `dma.dll` | DMA control block |
| `0x20043000`, `0x20043100`, `0x20043200` | `0xc6043000` etc. | `dma.dll`, `ecust.dll` | DMA global/channel-group registers |
| `0x20400000` | `0xc6060000` | `karap.dll` | Primary interrupt controller |

## Interrupt controller and IRQ numbers

`karap` IRQ entry is `ROM:0x8009c2d8`, FIQ entry `0x8009c490`, bind routine `0x8009d260`, initialization `0x8009d59c`. The primary handler table is `V:0xc8001f50`, eight bytes per entry. Initialization iterates inclusive `0..0x1a4` (421 entries), and bind rejects a number above 420.

| INTC offset from `P:0x20400000` | Firmware behavior |
| --- | --- |
| `+0x04` | Global enable, init writes 1 |
| `+0x08` | Read pending IRQ number, `0xffffffff` means none |
| `+0x0c` | Write IRQ number to enable |
| `+0x10` | Write IRQ number to disable, init writes -1 |
| `+0x1c` | Init writes zero |
| `+0x20` | Select IRQ number for configuration |
| `+0x24` | Selected IRQ configuration |
| `+0x48` | FIQ pending-number read |
| `+0x50` | FIQ disable |
| `+0x80` | Read at completion of IRQ dispatch (EOI) |
| `+0x84` | Init writes 5 |
| `+0x90`, `+0x9c` | IRQ/FIQ nesting control |

At `0x8009c304..0x8009c328` the dispatcher repeatedly reads pending numbers, calls the corresponding table entry and finally reads EOI. At `0x8009d4ec..0x8009d598` it drains the controller and five secondary banks at `P:0x20033414 + 0x40*k`, each bank is disabled through that address and drained at `+0x28` relative to it.

The following numbers are traced through calls to this firmware's `Interrupt::Bind`, including imported `ldr pc` thunks. Numbers in a device tree must use the controller's numbering convention, not Linux-assigned virqs.

| IRQ (decimal / hex) | User | E5 ROM bind call |
| --- | --- | --- |
| 30 / `0x1e` | ISASW wrapper | `0x8010873c` |
| **35 / `0x23`** | **System tick, handler `0x8009d778`** | **`0x8009d91c`** |
| 99, 100, 101 / `0x63..0x65` | Bluetooth UART | `0x801368cc`, `0x801368ec`, `0x801368ac` |
| 128, 129, 130 / `0x80..0x82` | SPI 3 | `0x80161078`, `0x80161060`, `0x80160fa4` |
| 146 / `0x92` | Native display graphics | `0x80179054` |
| 150 / `0x96` | I2C 0 | `0x800b52e4`, retry `0x800b5304` |
| 183, 215 / `0xb7`, `0xd7` | USB interrupt binding paths | `0x80126fd8`, `0x80126ff0` |
| 184, 185, 186 / `0xb8..0xba` | SPI 1 | `0x8015ca84`, `0x8015ca6c`, `0x8015c9b0` |
| 191, 192, 193 / `0xbf..0xc1` | SPI 2 | `0x8015ed74`, `0x8015ed5c`, `0x8015eca0` |
| 201 / `0xc9` | Camera | `0x8019a21c` |
| 202 / `0xca` | 4550 multimedia path | `0x802944b0` |
| Table-driven (see GPIO section) | GPIO per-pin IRQs | Table `0x800a6830`, 137 records of 12 bytes, `egpio` binds through `0x800a6148` |
| 417, 418, 419, 420 / `0x1a1..0x1a4` | DMA interrupt paths | `0x800b0b0c`, `0x800b0b58`, `0x800b0af0`, `0x800b0b28` |

MMC interrupt binding at `0x8013e990` takes a configuration-supplied number.  An immediate constant at a nearby instruction would not establish its IRQ. INTENTIONAL!!

## Timers and clocks

`NKern::FastCounterFrequency` at `ROM:0x8006b1dc` returns the immediate 32768. `NKern::FastCounter` at `0x80065874` uses the latch/read pair `V:0xc6033354` / `V:0xc6033350`, giving physical `0x20033354` / `0x20033350`. 

**The E5 millisecond tick is a different path from E6.** `0x8009d914..0x8009d91c` binds IRQ 35 to `0x8009d778` with the timer queue pointer. That handler reads `V:0xc60401c4` until zero, writes **`0x1e00`**, then tail-calls `NTimerQ::Tick` through thunk `0x800a08c0` -> kernel `0x8006525c`. Do not infer a 38.4 MHz timer clock from the reload value: the source clock/divider and initial arming path need to be followed before implementing an E5 clockevent. The helper at `0x8009d198` returning `0x9600` is not sufficient evidence to use the E6 timer.

PLL bases are a four-word table at `0x800a3bb4`: `V:0xc6032604`, `0xc6032624`, `0xc6032664`, `0xc6032644` in that order. Clock-source decoding is in `karap:0x8009cf8c`; board PLL programming is in `ecust`, with gates/selectors in `power_resources.dll`. A matching register address does not establish a matching clock rate or a valid enable sequence. Preserve the handed-off clock state until the relevant E5 sequence is understood.

## Watchdogs and PMIC

`power.dll:0x80116790` is the periodic watchdog service. At `0x801167a8..0x801167c4` it does:

```text
cbus_write(device=1, logical_register=0x13, mask=0xffff, value=0x30)
*(volatile u32 *)V:0xc60336c4 = 0x3fffffff
```

It schedules another call with `0xbb8` (3000) through the timer helper at `0x801167d4`. The CBUS imported call is `0x8011874c`; its implementation and logical-register descriptor table are in `karap`, not an I2C device. In `karap:0x8009cc14..0x8009cc54`, CBUS uses physical `0x20030704` as the lock/control location, `+0x38` as control, `+0x28` for width-minus-one, `+0x1c` for write data, and `+0x18` for the encoded command, all relative to `0x20030704`. It encodes the descriptor's device in bits starting at 6 and the hardware register from descriptor byte 8.

The descriptor table is at `ROM:0x800a0cec`. Entry `0x13` at **`0x800a0e1c`** contains `{1, 0, 0x17, 16}`: device 1, hardware register **`0x17`**, width **16 bits**. The encoded write command is therefore **`0x57`**, with data **`0x30`**. This is independently verified from the E5 table, rather than assumed from the matching E6 watchdog. E6's charging-controller identities, temperature conversion, CBUS calibration and PA_SWD addresses are not established for E5 by this watchdog routine. Charging and thermal control require their own board configuration and EM/ISASW analysis.

## Display

The native stack is `EDISP.DLL` / `r_display_kernel.dll` / **`d_display.ldd`**. The latter contains `DisplayControllerHw`, `DisplayHandlerMessi`, `InterfaceMeSSIRfbi`, `InterfaceMeSSICommands`, `DmaCopier`, and `TDisplayModule` RTTI. These are actual strings in the E5 module at `0x80184088..0x80184148`, not names imported from E6.

| Physical register | Evidence in `d_display.ldd` | Interpretation |
| --- | --- | --- |
| `0x20024800` | Literal `0x801799dc` and many plane-register users | Graphics-controller base |
| `0x20024810` | Literal `0x801799e0` | Graphics control |
| `0x20024818`, `0x2002481c` | Multiple write/poll paths | Graphics IRQ status/control. exact bits must follow the caller |
| `0x20024840` | `0x80176548`, `0x801826b0` and later | Graphics output/control path |
| `0x20024880`, `0x200248a0`, `0x200248bc` | Plane configuration code and literals | Framebuffer/plane register set, no fixed framebuffer address implied |
| `0x20025814` | `0x80181d5c..0x80181d6c` | Wait while bit `0x100` is set before bus operations |
| `0x20025844` | `0x80181d80..0x80181d88` | Bus transfer control write |
| `0x2002584c` | `0x80181bd8..0x80181bec` | One command/data register write path |
| `0x20025850` | `0x80181bbc..0x80181bd0` | A second command/data register write path |
| `0x20025858` | `0x80181c40..0x80181d04` | Panel read path: write zero, wait, read byte, shift into result |
| `0x20025890` | Literal `0x80180aec` | Bus timing/configuration path |
| `0x08000180..0x080001f8` | Tables at `0x8017c800`, `0x80182614`, `0x80182cd8` | External-bank configuration used by display |

`0x80179054` binds graphics IRQ 146. A framebuffer is allocated dynamically: the diagnostic at `0x80166050` is "Cannot allocate physical RAM for DMA and front buffer". There is no statically established physical framebuffer address to put into `simplefb`. Copying E6 framebuffer guesses, its 640x480 geometry, or its VideoCore firmware blobs would be wrong.

The display ROM defaults include **320 x 240** at `ROM:0x80182d54` / `0x80182d58` code at `0x8016a13c..0x8016a15c` copies those values into `V:0xc8009ee8` / `+4`, with additional users throughout the driver. The panel-specific initialization, selected geometry/orientation, reset pin and power sequencing remain tied to display configuration records and driver branches. The register map alone is not a complete panel initialization sequence. Preserve the original display during early bring-up
A a native graphics/RFBI driver needs the selected E5 configuration and complete command sequence.

## Keyboard and GPIO

The ROM defaults in `ekeyb.dll` describe an **8 x 8 direct GPIO matrix**. At `0x80157c10` the driver obtains/configures GPIO handles, reads pin state through imported `egpio` routines, and uses a software scan. Configuration lives at `ROM:0x801595d4`; its `+0x40` and `+0x44` words both contain 8.

| Array | ROM address | GPIO ids (decimal, in stored order) |
| --- | --- | --- |
| First group | `0x801595d4` | 114, 115, 116, 117, 118, 119, 121, 2 |
| Second group | `0x801595f4` | 0, 3, 13, 33, 36, 38, 45, 55 |

At `0x80157edc..0x80157ef4` the driver drives the first group's handles low; at `0x80157f50..0x80157f70` it drives them high and changes their state through `egpio`. At `0x80157e5c..0x80157e78` it samples the first group and sets a result bit when the pin reads zero. Trace the complete scan before assigning Linux row/column direction, the two arrays' existence does not by itself establish active polarity, pull configuration or safe drive overlap.

The ROM default scan-code source begins at `0x8015961c` (configuration `+0x48`) and is copied bytewise from 32-bit words into kernel RAM by the constructor starting at `0x801581d8`. Matrix lookup at `0x801550fc` is `row + column*8` before the byte load. These are **Symbian scan codes**, not Linux `KEY_*` values. The default table is a starting point for decoding the scan algorithm, not a proven E5 QWERTY layout. Pphysical keys and any configuration overrides still need to be resolved. Runtime configuration handling must be checked before treating defaults as the selected board state.

The first 64 ROM default scan codes, written as two-digit hex and arranged by the lookup `row + 8*column`:

| Row / column | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | `19` | `5a` | `41` | `51` | `7e` | `7a` | `4c` | `4f` |
| 1 | `12` | `58` | `53` | `57` | `17` | `03` | `01` | `50` |
| 2 | `18` | `43` | `44` | `45` | `0e` | `a4` | `00` | `00` |
| 3 | `05` | `37` | `34` | `31` | `a7` | `10` | `00` | `00` |
| 4 | `a2` | `38` | `35` | `32` | `0f` | `a5` | `00` | `00` |
| 5 | `a3` | `39` | `36` | `33` | `11` | `e4` | `00` | `00` |
| 6 | `05` | `30` | `7f` | `2a` | `b4` | `c4` | `00` | `00` |
| 7 | `7d` | `79` | `4b` | `49` | `00` | `00` | `00` | `00` |

`egpio.dll` register descriptors at `0x800a5d80..0x800a5e6c` contain five banks. For bank index `k=0..4`, the main groups include `P:0x200319f0 + 0x10*k`, `0x200319f4 + 0x10*k`, `0x20031994 + 0x10*k`, `0x20031a58 + 0x10*k`, and IRQ/wakeup descriptors at `0x20033400 + 0x40*k` through `+0x38` in 8-byte steps. Pin functions go through masks and bank tables.


GPIO IRQs are not universally `316 + gpio`. `0x800a6118` checks GPIO ids below 137 and indexes `ROM:0x800a6830 + 12*gpio`; each record supplies the IRQ plus two routing fields. For example, GPIO 0/32/64 map to IRQ 316/348/380, but GPIO 100 has IRQ `0xffffffff` (no entry), while GPIO 114..119 map to IRQ 298..303, GPIO 121 to IRQ 305, and GPIO 136 to IRQ 287. The keyboard's first group therefore has IRQs **298, 299, 300, 301, 302, 303, 305, 318** in its stored GPIO order. The platform WLAN module's GPIO 73 maps to IRQ **389**, while GPIO 100 has no per-pin IRQ. These are values of this ROM's routing table; GPIO availability/muxing on the board still needs checking. The complete IRQ column can be expressed as: GPIO `0..87` -> `316 + gpio`; `88..105` -> no IRQ; `106..127` -> `184 + gpio`; `128..136` -> `151 + gpio`. These are inclusive ranges, verified against all 137 records.

## USB, I2C, SPI, Bluetooth and storage

USB uses the MUSB register layout at `P:0x20038800`: the firmware references byte registers `+0x00`, `+0x01`, `+0x02`, `+0x04`, `+0x06`, `+0x08`, `+0x0a`, `+0x0b`, `+0x0c`, `+0x0e`, `+0x0f`, endpoint index at `+0x0e`, FIFOs at `+0x20`, and `DEVCTL` at `+0x60`. Wrapper literals include `P:0x20038c04/08/0c/14`. The firmware has both `DUsbChargerDetectorIsp1702a` and `DUsbCableDetectorISAGazooHack` implementations; their presence does not prove which detector branch this board selects. Leave charging behavior separate from gadget-serial bring-up.

I2C 0 is at `P:0x20032200`, with register literals at offsets `00, 04, 08, 10, 14, 18, 1c, 24, 28, 2c, 30, 38`. IRQ 150 binds handler `0x800b497c`. This is the controller address,

SPI 1/2/3 have bases `0x20020400`, `0x20020600`, `0x20020a00`, with shared register `0x20020820` and the corresponding IRQ triplets listed above.

The WLAN stack includes the TI TNETW / 1251 family over WSPI, using SPI 2. This is supported by calls from `wlanpdd` at `0x801d95cc/9624/9674/9734` into `spi2.dll:0x8015df70/df60/df80`; `pltwlanpdd:0x8024c738` also calls `0x8015df80`. Thus the host controller is `P:0x20020600`. The driver contains `TIWlanSpiaClient`, `WlanSpia`, WSPI state-machine strings, and chip-ID branches at `0x80203b50..0x80203b84` for `0x07020101` (message "1251 PG 1.1") and `0x07030101` ("1251 PG 1.2"), with an earlier PG 1.0 branch. These are supported revisions, not a live identification of the fitted chip.

`pltwlanpdd` has two GPIO defaults at `ROM:0x80255070`: **100, 73**. The first is consumed by `0x8024ca08` and the second by `0x8024caa8`, which constructs and binds the GPIO event object. Keep the values marked as this module's defaults until its selected instance and initialization are verified. Full WSPI transfer setup, power/reset sequencing and firmware download remain outside a ready-to-run Linux driver.

Bluetooth uses `bc02_uart1.PDD` + `BC02.LDD`. The UART at `P:0x20025a00` has offset literals `04, 08, 14, 18, 1c, 28, 2c, 30, 34, 38, 3c, 64, 84, 9c, a0, a4, b4`; IRQs 99/100/101 are bound by its constructor. The pin-control literals are `P:0x20031868/870/87c`. This establishes the Bluetooth transport, not the location of accessible UART pads or an early console baud rate.

Removable-card support is `MEDSD.PDD` -> `EPBUSM.DLL` -> `EPBUSMV.DLL`. The latter uses the MMC register block **`P:0x20032800`**, including offsets `00, 04, 0c, 10, 14, 18, 1c, 24, 28, 2c, 34, 40..64`. Card-detect, supply and interrupt configuration flow through platform configuration.

Internal flash is handled by `mednand.pdd`; the module includes `TNandInterrupt`, `DMediaDriverNand`, `DMediaNandPowerHandler`, `SOS+CORE`, `SOS+ROFS`, `SOS-USER`, and a reference to `P:0x20038214`. NAND geometry, partition addressing and erase/write protocol are not inferred from the filename.

## Camera, multimedia, modem and unresolved board data

`campdd.pdd` directly accesses `P:0x20034000..0x200354ff`, uses IRQ 201, and also uses external-bank registers at `0x08000180..0x08000198`. `4550.ldd` / `4550_jpegkernel.dll` access `P:0x20038000` and `0x20030844`, with IRQ 202 bound by the former. These identify register users, not a complete camera sensor/pipeline driver. Sensor model, I2C address, power rails and reset pins remain unresolved.

The ROM contains `ISA_KERNEL_IF.dll`, `isaaccessldd.ldd`, `isaaccessextension.dll`, `sisa_wrapper.dll`, `empmIsaIf.dll`, security modules and the Rapu NIPS module. Bootstrap code contains aliases `0xc6200000`, `0xc6800000`, `0xc6b00000`, `0xc60f0000` at `0x80002430..0x8000243c`, physical backing is supplied through loader hardware records.

`ecust.dll` contains the **`PMM_CONF`** name (`0x800a560a`) and handles runtime hardware records. `GAPE.DLL` provides named extension registration, with identifiers such as Keypad, DisplayDriver, CameraDriver, PowerController, Accelerometer and KeypadExpander in several modules. Such strings are the registry's available interfaces, not proof that every named device is fitted to E5. In particular, Digitiser/TouchIC2 strings do not prove an E5 touchscreen.

Remaining work is specific rather than a reason to rescan all words:

- reconcile the step 0 MIDR/RAM map with bootstrap bank records, reserve the actual modem/shared memory and choose an E5 ram console location
- follow initial arming and source/divider selection for the IRQ-35 tick
- resolve the selected display configuration, panel identity/geometry, pins, regulators, command sequence and framebuffer format
- confirm the keyboard matrix direction/polarity, selected configuration and Linux keycode translation
- follow MMC configuration for card-detect GPIO, IRQ, voltage resources and clock parent
- verify the WLAN 1251 revision and module selection, SPI-2 setup and GPIO power/IRQ sequence
- analyze the E5 EM/ISASW charging, temperature, audio and secure-watchdog paths independently

## Porting order

After the step 0, start from the E5 RAM/IRQ/timer/watchdog findings above, then bring up a RAM log and USB gadget serial with display, charger, camera, WLAN and experimental secure calls disabled. SoC address matches are useful for reusing code, but the timer, keyboard and native display require E5-specific configuration or drivers. Keep internal flash read-only. Do not enable an unknown MMIO block in a daily device tree: an unpowered access can abort the CPU before a log is available.

## Module address index

Addresses are from each module's `TRomImageHeader` text end is exclusive. Entry stubs often load a second entry address and branch to it. These are firmware-version-specific disassembly coordinates, not physical register addresses. The index covers the bootstrap and the kernel/driver region used for the analysis, not every userspace application in the ROM.

| Module | Text start | Text end | Entry |
| --- | --- | --- | --- |
| `bootstrap` | `0x80000000` | `0x80004000` | `0x80001900` |
| `ekern.exe` | `0x80064000` | `0x8009bfb8` | `0x80064110` |
| `karap.dll` | `0x8009c228` | `0x800a12ec` | `0x8009c228` |
| `GAPE.DLL` | `0x800a1388` | `0x800a19b0` | `0x800a1388` |
| `ecust.dll` | `0x800a1a38` | `0x800a5640` | `0x800a1a38` |
| `egpio.dll` | `0x800a56f8` | `0x800a7080` | `0x800a56f8` |
| `PowerResourceManager.dll` | `0x800a7108` | `0x800aa7f0` | `0x800a7108` |
| `power_resources.dll` | `0x800aa878` | `0x800ae954` | `0x800aa878` |
| `REBOOTDRIVER.LDD` | `0x800aea08` | `0x800af864` | `0x800aea08` |
| `dma.dll` | `0x800af8e8` | `0x800b3da0` | `0x800af8e8` |
| `medint.pdd` | `0x800b3e78` | `0x800b4708` | `0x800b3e78` |
| `i2c0.dll` | `0x800b4788` | `0x800b5ac0` | `0x800b4788` |
| `LightPdd.Pdd` | `0x800b5b48` | `0x800b812c` | `0x800b5b48` |
| `LightLdd.Ldd` | `0x800b81b8` | `0x800b9d14` | `0x800b81b8` |
| `mednand.pdd` | `0x800b9d98` | `0x800dfad4` | `0x800b9d98` |
| `exstart.dll` | `0x800e0078` | `0x800e02f4` | `0x800e0078` |
| `elocd.ldd` | `0x800e0378` | `0x800e3e38` | `0x800e0378` |
| `ecomm.ldd` | `0x800e3ed8` | `0x800e6a0c` | `0x800e3ed8` |
| `eusbc.ldd` | `0x800e6a98` | `0x800eacdc` | `0x800e6a98` |
| `evfp.dll` | `0x800ead68` | `0x800ef20c` | `0x800ead68` |
| `ISA_KERNEL_IF.dll` | `0x800ef298` | `0x800f14c0` | `0x800ef298` |
| `isaaccessldd.ldd` | `0x800f1548` | `0x800f55cc` | `0x800f1548` |
| `isaaccessextension.dll` | `0x800f5658` | `0x80103d58` | `0x800f5658` |
| `empmIsaIf.dll` | `0x80103de8` | `0x80106a24` | `0x80103de8` |
| `sisa_wrapper.dll` | `0x80106aa8` | `0x80109bf0` | `0x80106aa8` |
| `SecLdd.LDD` | `0x80109d08` | `0x80113704` | `0x80109d08` |
| `securityKext.dll` | `0x801137a8` | `0x801160e0` | `0x801137a8` |
| `power.dll` | `0x80116188` | `0x80118bc4` | `0x80116188` |
| `dmamonitor.dll` | `0x80118c78` | `0x80119338` | `0x80118c78` |
| `cpumonitor.dll` | `0x801193c8` | `0x80119a48` | `0x801193c8` |
| `dvfs_hal.dll` | `0x80119b08` | `0x8011a9cc` | `0x80119b08` |
| `dvfs_engine.dll` | `0x8011aa58` | `0x8011d5c0` | `0x8011aa58` |
| `dvfs_interface.ldd` | `0x8011d658` | `0x8011df14` | `0x8011d658` |
| `usb.dll` | `0x8011df98` | `0x8012c684` | `0x8011df98` |
| `usbcc.dll` | `0x8012ca08` | `0x80136298` | `0x8012ca08` |
| `bc02_uart1.PDD` | `0x80136328` | `0x80138dac` | `0x80136328` |
| `BC02.LDD` | `0x80138e38` | `0x8013b774` | `0x80138e38` |
| `thermalsensor.dll` | `0x8013b7f8` | `0x8013cfdc` | `0x8013b7f8` |
| `thermalmanager.dll` | `0x8013d078` | `0x8013e6f4` | `0x8013d078` |
| `EPBUSMV.DLL` | `0x8013e788` | `0x80145424` | `0x8013e788` |
| `EPBUSM.DLL` | `0x801454a8` | `0x80151cc8` | `0x801454a8` |
| `MEDSD.PDD` | `0x80151d48` | `0x80154fb8` | `0x80151d48` |
| `ekeyb.dll` | `0x80155038` | `0x80159e04` | `0x80155038` |
| `isimessagekernel.dll` | `0x80159e98` | `0x8015bba4` | `0x80159e98` |
| `spi1.dll` | `0x8015bc28` | `0x8015de88` | `0x8015bc28` |
| `spi2.dll` | `0x8015df18` | `0x80160178` | `0x8015df18` |
| `spi3.dll` | `0x80160208` | `0x8016247c` | `0x80160208` |
| `display_tools.dll` | `0x80162508` | `0x801653d4` | `0x80162508` |
| `d_display.ldd` | `0x80165478` | `0x80184958` | `0x80165478` |
| `r_display_kernel.dll` | `0x80185498` | `0x80186180` | `0x80185498` |
| `camldd.ldd` | `0x80186208` | `0x8018ad88` | `0x80186208` |
| `campdd.pdd` | `0x8018ae18` | `0x801d3160` | `0x8018ae18` |
| `wlanpdd.pdd` | `0x801d8d98` | `0x80218148` | `0x801d8d98` |
| `wlanhpa.dll` | `0x80218d68` | `0x80219420` | `0x80218d68` |
| `pltwlanpdd.pdd` | `0x802194a8` | `0x802555c0` | `0x802194a8` |
| `xtitx.dll` | `0x802561e8` | `0x802566f8` | `0x802561e8` |
| `TraceCoreOstLdd.ldd` | `0x80256788` | `0x8025773c` | `0x80256788` |
| `tracecore.dll` | `0x802577e8` | `0x80261714` | `0x802577e8` |
| `xtiwriter.dll` | `0x802617c8` | `0x80261aa4` | `0x802617c8` |
| `ostmemorywriter.dll` | `0x80261b38` | `0x80262498` | `0x80261b38` |
| `tcldd.ldd` | `0x80262528` | `0x80263710` | `0x80262528` |
| `mobilecrashdriver.ldd` | `0x80263798` | `0x80264660` | `0x80263798` |
| `exmoncommon.dll` | `0x802646e8` | `0x80268110` | `0x802646e8` |
| `exmondebug.dll` | `0x80268198` | `0x80287114` | `0x80268198` |
| `CUSTOM.LDD` | `0x802875b8` | `0x8028cdb8` | `0x802875b8` |
| `ssc.ext` | `0x8028ce48` | `0x8028f13c` | `0x8028ce48` |
| `VariationMgrKernel.DLL` | `0x8028f1c8` | `0x8028fbf8` | `0x8028f1c8` |
| `aamdaifdriver.ldd` | `0x8028fc78` | `0x80290ac8` | `0x8028fc78` |
| `usbaudiodriver.ldd` | `0x80290b48` | `0x80293500` | `0x80290b48` |
| `4550.ldd` | `0x80293588` | `0x802948b8` | `0x80293588` |
| `4550_jpegkernel.dll` | `0x80294938` | `0x802a1268` | `0x80294938` |
| `bufferhandler.ldd` | `0x802a1308` | `0x802a1af8` | `0x802a1308` |
| `4550_colorconv.ldd` | `0x802a1b78` | `0x802a233c` | `0x802a1b78` |
| `rapu_nips_kernel_eka2.dll` | `0x802a23c8` | `0x802c8090` | `0x802a23c8` |
| `wlanldd.ldd` | `0x802c9088` | `0x802ef308` | `0x802c9088` |
| `EDISP.DLL` | `0x802ef388` | `0x802f0be0` | `0x802ef388` |
| `ecomp.fsy` | `0x802f0c58` | `0x802f21f8` | `0x802f0c58` |
| `erofs.fsy` | `0x802f2278` | `0x802f446c` | `0x802f2278` |
| `DRTAEABI.dll` | `0x802f44f8` | `0x802f7dc0` | `0x802f44f8` |
| `DFPAEABI.dll` | `0x802f7e38` | `0x802f90dc` | `0x802f7e38` |
| `DRTRVCT2_1.dll` | `0x802f9158` | `0x802fad5c` | `0x802f9158` |
| `DRTRVCT2_2.dll` | `0x802f9158` | `0x802fad5c` | `0x802f9158` |
| `DFPRVCT2_1.dll` | `0x802fadd8` | `0x802fcaa4` | `0x802fadd8` |
| `DFPRVCT2_2.dll` | `0x802fadd8` | `0x802fcaa4` | `0x802fadd8` |
| `dfprvct2_1-thunk.dll` | `0x802fcb28` | `0x802fcc70` | `0x802fcb28` |
| `dfprvct2_2-thunk.dll` | `0x802fcb28` | `0x802fcc70` | `0x802fcb28` |
| `SCPPNWDL.dll` | `0x802fcce8` | `0x802fce10` | `0x802fcce8` |
| `EUser.dll` | `0x802fce88` | `0x80337990` | `0x802fce88` |

## Peripheral literal-load index

This index is a lookup aid for further RE. It includes A32 PC-relative literal loads in the kernel/driver text ranges whose loaded word translates into the E5 mapped peripheral windows. It omits addresses constructed arithmetically and tables reached indirectly. A load of a pointer is not proof that every offset is a register or that it is safe to read.Use the instruction and caller before assigning semantics. Compiler/runtime and userspace ranges are excluded. The detailed block descriptions above take precedence over this mechanical inventory.

| Physical 4 KiB page | Module | Loaded offsets (hex) |
| --- | --- | --- |
| `0x08000000` | `campdd.pdd` | `180`, `184`, `188`, `190`, `194`, `198` |
| `0x08000000` | `d_display.ldd` | `180`, `184`, `188`, `190`, `194`, `198`, `1a0`, `1a4`, `1a8`, `1b0`, `1b4`, `1b8`, `1c0`, `1c4`, `1c8`, `1d0`, `1d4`, `1d8`, `1e0`, `1e4`, `1e8`, `1f0`, `1f4`, `1f8` |
| `0x20020000` | `bc02_uart1.PDD` | `200` |
| `0x20020000` | `power_resources.dll` | `804` |
| `0x20020000` | `spi1.dll` | `404`, `40c`, `410`, `418`, `41c`, `428`, `42c`, `434`, `438`, `43c`, `440`, `444`, `448`, `44c`, `458`, `45c`, `460`, `464`, `820` |
| `0x20020000` | `spi2.dll` | `604`, `60c`, `610`, `618`, `61c`, `628`, `62c`, `634`, `638`, `63c`, `640`, `644`, `648`, `64c`, `658`, `65c`, `660`, `664`, `820` |
| `0x20020000` | `spi3.dll` | `820`, `a04`, `a0c`, `a10`, `a18`, `a1c`, `a28`, `a2c`, `a34`, `a38`, `a3c`, `a40`, `a44`, `a48`, `a4c`, `a58`, `a5c`, `a60`, `a64` |
| `0x20024000` | `SecLdd.LDD` | `008`, `018`, `01c`, `020`, `030`, `300`, `30c`, `320`, `324`, `328`, `32c`, `330`, `334`, `348`, `400` |
| `0x20024000` | `d_display.ldd` | `800`, `810`, `818`, `81c`, `840`, `844`, `84c`, `854`, `85c`, `864`, `86c`, `870`, `87c`, `880`, `888`, `88c`, `8a0`, `8a4`, `8a8`, `8ac`, `8b0`, `8b4`, `8bc`, `8c4`, `8c8`, `8cc`, `8d0`, `8d8`, `8dc`, `8e4`, `8f0`, `930`, `94c`, `950`, `954`, `958`, `95c`, `960`, `968`, `96c`, `974`, `980` |
| `0x20025000` | `bc02_uart1.PDD` | `a04`, `a08`, `a14`, `a18`, `a1c`, `a28`, `a2c`, `a30`, `a34`, `a38`, `a3c`, `a64`, `a84`, `a9c`, `aa0`, `aa4`, `ab4` |
| `0x20025000` | `d_display.ldd` | `800`, `814`, `840`, `844`, `84c`, `850`, `858`, `890` |
| `0x20030000` | `4550.ldd` | `844` |
| `0x20030000` | `4550_jpegkernel.dll` | `844` |
| `0x20030000` | `SecLdd.LDD` | `300` |
| `0x20030000` | `karap.dll` | `704`, `708` |
| `0x20030000` | `spi1.dll` | `000` |
| `0x20030000` | `spi2.dll` | `000` |
| `0x20030000` | `spi3.dll` | `000` |
| `0x20031000` | `CUSTOM.LDD` | `990` |
| `0x20031000` | `bc02_uart1.PDD` | `868`, `870`, `87c` |
| `0x20031000` | `campdd.pdd` | `8b4`, `8b8`, `9e4`, `9e8` |
| `0x20031000` | `egpio.dll` | `994`, `9a4`, `9b4`, `9c4`, `9d4`, `9f0`, `9f4`, `a00`, `a04`, `a10`, `a14`, `a20`, `a24`, `a30`, `a34`, `a58`, `a68`, `a78`, `a88`, `a98` |
| `0x20031000` | `usb.dll` | `a94`, `a98` |
| `0x20032000` | `EPBUSMV.DLL` | `800`, `804`, `80c`, `810`, `814`, `818`, `81c`, `824`, `828`, `82c`, `834`, `840`, `844`, `848`, `84c`, `850`, `854`, `858`, `85c`, `864` |
| `0x20032000` | `d_display.ldd` | `664` |
| `0x20032000` | `ecust.dll` | `604`, `624`, `644`, `664` |
| `0x20032000` | `i2c0.dll` | `200`, `204`, `208`, `210`, `214`, `218`, `21c`, `224`, `228`, `22c`, `230`, `238` |
| `0x20032000` | `karap.dll` | `604`, `644` |
| `0x20032000` | `power_resources.dll` | `604`, `60c` |
| `0x20033000` | `REBOOTDRIVER.LDD` | `114` |
| `0x20033000` | `campdd.pdd` | `038` |
| `0x20033000` | `cpumonitor.dll` | `350`, `354` |
| `0x20033000` | `d_display.ldd` | `038` |
| `0x20033000` | `dmamonitor.dll` | `350`, `354` |
| `0x20033000` | `dvfs_hal.dll` | `350`, `354` |
| `0x20033000` | `ecust.dll` | `004`, `0b8`, `0d8`, `198` |
| `0x20033000` | `egpio.dll` | `400`, `408`, `410`, `418`, `420`, `428`, `430`, `438`, `440`, `448`, `450`, `458`, `460`, `468`, `470`, `478`, `480`, `488`, `490`, `498`, `4a0`, `4a8`, `4b0`, `4b8`, `4c0`, `4c8`, `4d0`, `4d8`, `4e0`, `4e8`, `4f0`, `4f8`, `500`, `508`, `510`, `518`, `520`, `528`, `530`, `538` |
| `0x20033000` | `ekern.exe` | `350`, `354` |
| `0x20033000` | `karap.dll` | `004`, `090`, `0b0`, `120`, `130`, `414`, `454`, `494`, `4d4`, `514`, `600` |
| `0x20033000` | `power.dll` | `044`, `120`, `130`, `4e0`, `6c4` |
| `0x20033000` | `power_resources.dll` | `064`, `080`, `084`, `088`, `094`, `098`, `0a8`, `0b0`, `0b4`, `0b8`, `0d0`, `0e0`, `104`, `140`, `150`, `180`, `190` |
| `0x20033000` | `sisa_wrapper.dll` | `6c4` |
| `0x20033000` | `usb.dll` | `4c8` |
| `0x20034000` | `campdd.pdd` | `000`, `004`, `008`, `00c`, `010`, `034`, `040`, `050`, `05c`, `064`, `118`, `11c`, `120`, `130`, `140`, `150`, `160`, `200`, `210`, `250`, `430`, `450`, `604`, `608`, `614`, `62c`, `634`, `638`, `63c`, `648`, `64c`, `654`, `658`, `65c`, `660`, `698`, `69c`, `6a0`, `6a4`, `a04`, `a08`, `a0c`, `a30`, `a34`, `a38`, `a40`, `c04`, `c08`, `c18`, `c1c`, `c20`, `c24`, `c28`, `c2c`, `c30`, `c34`, `c38`, `c3c`, `c40`, `c44`, `c48`, `c4c`, `c50`, `c54`, `c58`, `c5c`, `e04`, `e08`, `e0c`, `e10`, `e14`, `e20`, `e24`, `e28`, `e30`, `e34`, `e40`, `e44`, `e48`, `e4c`, `e50`, `e54`, `e5c`, `e80`, `e84` |
| `0x20035000` | `campdd.pdd` | `004`, `00c`, `010`, `014`, `018`, `01c`, `020`, `0a8`, `204`, `2f8` |
| `0x20038000` | `4550.ldd` | `000` |
| `0x20038000` | `4550_jpegkernel.dll` | `000` |
| `0x20038000` | `mednand.pdd` | `214` |
| `0x20038000` | `usb.dll` | `800`, `801`, `802`, `804`, `806`, `808`, `80a`, `80b`, `80c`, `80e`, `80f`, `812`, `820`, `860`, `862`, `863`, `86c`, `870`, `871`, `872`, `873`, `874`, `875`, `876`, `877`, `878`, `879`, `87a`, `87b`, `87c`, `87d`, `87e`, `886`, `902`, `908`, `90a`, `c00`, `c04`, `c08`, `c0c`, `c14` |
| `0x20040000` | `REBOOTDRIVER.LDD` | `400` |
| `0x20040000` | `dmamonitor.dll` | `304` |
| `0x20040000` | `ecust.dll` | `780` |
| `0x20040000` | `karap.dll` | `000`, `180`, `1c4`, `280`, `284`, `288` |
| `0x20040000` | `power_resources.dll` | `584` |
| `0x20040000` | `usb.dll` | `384`, `388` |
| `0x20042000` | `dma.dll` | `800` |
| `0x20043000` | `dma.dll` | `000`, `100`, `200` |
| `0x20043000` | `ecust.dll` | `104` |
| `0x20400000` | `SecLdd.LDD` | `040` |
| `0x20400000` | `karap.dll` | `004`, `008`, `00c`, `010`, `020`, `024`, `040`, `048`, `050`, `080`, `090`, `09c` |
| `0x20400000` | `power.dll` | `080` |
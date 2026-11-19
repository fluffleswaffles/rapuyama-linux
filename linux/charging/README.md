# Charging

`rapuyama-charge CALIBRATION` is the E6 charge controller. It takes the bq2415x lease (`/dev/rapuyama-energy`, enabled by `nokia,em-control` in the DTB) and publishes its state in `/run/rapuyama-charge/state` for Yamashell. The calibration is one line, `RCHG1 GAIN CAPACITY TARGET`:

- `GAIN`: the factory scaler of the battery current measurement, ×10000 (5000..20000).
- `CAPACITY`: nominal battery capacity in mAh (500..2500).
- `TARGET`: charge voltage in mV, a multiple of 20 (3900..4200). Below about 14 °C the controller uses at most 4100.

## Calibration from Symbian

`calibrate.sh` (installed as `rapuyama-charge-calibrate`) prints this line for the phone it runs on. It reads only what Symbian left in the preserved modem firmware RAM (physical `0x1f8b0000`, through `/sys/kernel/debug/rapuyama-shm/1f8b0000`):

- the gain from the firmware's ADC calibration table (`c68127a4`), the entry of measurement 14 (IBAT, type 6, gain at +8). Uncalibrated entries hold 10000.
- the capacity from the energy management's battery state (`c6812590`).
- the target is fixed at 4140 mV. The firmware's two active profiles (`c65b1f14`, `c65b1f94`) charge to 4180 or 4150 mV at normal temperature; which one applies is energy-management state that is not followed here, so the lower one in 20 mV steps.

Like the original energy management, it refuses unless the battery-usable byte is 1 or 2, and it refuses implausible values. `init.sh` uses `/etc/rapuyama-charge.cal` if the image has one; otherwise it generates `/run/rapuyama-charge.cal`. Without either, the controller does not start. On the E6 used for development it printed `RCHG1 9989 1521 4140`. The gain and capacity match the hand-made file of that phone.

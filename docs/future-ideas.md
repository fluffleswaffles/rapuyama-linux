# Future ideas

## Symbian and Java ME applications on Linux

Explore using EKA2L1 as a backend for installing and running Symbian apps and games from the Linux interface. Check whether it builds and runs on the E6's ARMv6 CPU, fits within 256 MB RAM, and can use our display, keyboard, touchscreen, audio and GLES stack. Application compatibility and speed will need testing on hardware. EKA2L1 has limited J2ME support; also investigate a separate lightweight Java ME runtime.

## Older Android applications on Linux

Investigate running selected old Android apps and games, starting with the Android 2.x era. Possible approaches include an older Android userspace in a container or a smaller compatibility environment. ARMv6 can run suitable old `armeabi` native code, but Android's ABI is distinct from Linux armhf, and `armeabi-v7a` binaries require ARMv7. Android services, Dalvik, graphics, input, sound, compatibility with our kernel, memory usage and performance all need investigation. Modern Waydroid is not a ready-made solution for E6.

## Minimal Symbian firmware for booting Linux

Build a stripped-down custom Symbian firmware with no graphical interface or unnecessary applications and services. Keep only what is needed to initialize the hardware, modem and secure services, then launch Linux automatically from a known, repeatable handoff state. The goal is to use Symbian as a small bootstrap stage rather than boot a full phone OS before Linux.

Work out which components must remain, when the modem and DSP are ready, and what state Linux needs to inherit. Integrate the Linux loader into the boot sequence so the handoff happens at that point, without manual startup or unrelated services changing the hardware state.

## Browser-based installer

Build a browser-based rapuyama-linux installer, similar to Google's Android Flash Tool for Pixels communicating with the phone through WebUSB. Eventually it could install the minimal Symbian bootstrap firmware and Linux together, without requiring a separate desktop flashing application.

Investigate Nokia's USB flashing protocols and firmware formats, starting with reverse-engineering Phoenix. Implement and validate a standalone command-line flasher first, then explore a WebUSB frontend. Browser access to the required USB interfaces, device reconnection between boot modes, host drivers and firmware signature restrictions all need investigation.

The installer needs model and image checks, a safe write sequence and a documented recovery path for interrupted flashing. A first version could install Linux files and the loader through a running Symbian system without rewriting system partitions.
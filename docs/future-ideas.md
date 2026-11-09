# Future ideas

## Symbian and Java ME applications on Linux

Explore using EKA2L1 as a backend for installing and running Symbian apps and games from the Linux interface. Check whether it builds and runs on the E6's ARMv6 CPU, fits within 256 MB RAM, and can use our display, keyboard, touchscreen, audio and GLES stack. Application compatibility and speed will need testing on hardware. EKA2L1 has limited J2ME support; also investigate a separate lightweight Java ME runtime.

## Older Android applications on Linux

Investigate running selected old Android apps and games, starting with the Android 2.x era. Possible approaches include an older Android userspace in a container or a smaller compatibility environment. ARMv6 can run suitable old `armeabi` native code, but Android's ABI is distinct from Linux armhf, and `armeabi-v7a` binaries require ARMv7. Android services, Dalvik, graphics, input, sound, compatibility with our kernel, memory usage and performance all need investigation. Modern Waydroid is not a ready-made solution for E6.
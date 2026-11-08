# GCCE (Symbian toolchain)

Needs `arm-2012.03` - Sourcery CodeBench Lite 2012.03-42, gcc 4.6.3,
target `arm-none-symbianelf`. The driver builds (`driver/build*.sh`,
`linux/stage0/symbian-loader/build_*.sh`) look for it in
`tools/gcce/arm-2012.03/bin/`.

## Where to get it
Internet Archive, "Symbian and Nokia Development Tools" collection:
<https://archive.org/details/symbian_dev> -> `Compilers.zip` (has GCCE 4.6.3 inside).

The local archive everything was built with:
```
gcce-linux.tar.bz2  27961234 bytes
sha256 b341215df8732fc0d39054b6946efb30ba1eea806adfe8fe299290d6d7c0ac74
```

## Install

The container build downloads and verifies GCCE
automatically. The steps below are only needed for builds on the host.

```
cd tools/gcce
tar -xjf gcce-linux.tar.bz2 # -> arm-2012.03/
arm-2012.03/bin/arm-none-symbianelf-gcc --version
```
`bootstrap.sh` does the unpacking too if the tarball is here.
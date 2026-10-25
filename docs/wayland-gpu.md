# Wayland composition on VideoCore

[yamashell](../linux/yamashell) is a small Wayland compositor and shell using `libvcgl` and `/dev/vc-khrn`. Applications submit shared-memory buffers; the compositor keeps a texture for each surface, uploads only the damage and draws the scene on the VideoCore. The output is two fullscreen RGB565 EGL surfaces presented in turn through GCE elements. The ARM never blends or uploads a composed framebuffer.

This accelerates composition. Applications such as foot still rasterize their own shared-memory buffers on the CPU. Wayland EGL clients and dmabuf imports are not implemented.

## Build

Use the normal kernel build, with the E6 kernel ROM dump described in [porting-guide.md](porting-guide.md):

```sh
KERN_BIN=/path/to/kern.bin ./linux/kernel/build-vc.sh
```

The host needs `wayland-scanner`, Clang and the ARM binutils, in addition to the normal build dependencies. `sysroot.sh` installs the ARM Wayland, xkbcommon, FreeType, libpng and libjpeg development packages in the cross-compilation sysroot. The runtime libraries already belong to the Weston rootfs.

The build puts `yamashell`, `yamashell-session`, the yamashell assets and `libvcgl.so` into the initramfs and also produces `e6-vc-565-gpu.dtb`. The normal `e6-vc-565.dtb` continues to start Weston. For development on a PC, yamashell itself also builds against Mesa, see its README.

## Boot

Use the uncompressed `Image` with `e6bfix`. Copy `e6-vc-565-gpu.dtb` as `E:\e6.dtb`. This DTB adds `e6.gui=yamashell` and uses the initramfs, so the installed SD rootfs does not need updating. It keeps the E6 hardware configuration, lowmem and secure watchdog settings, and selects PIO for the display (`videocore.dma=0`), see [the DMA notes](videocore-display.md).

`yamashell-session` waits for `/dev/vc-khrn`, starts the compositor on `wayland-vc` and opens one terminal; other apps start from the shell's menu. If the compositor exits, init restores the console and starts Weston.

The compositor reads evdev devices directly and owns tty1 while running. It uses the E6 English/Russian XKB layout, including Fn and Sym. Alt+Tab switches between top-level windows. More clients can connect with:

```sh
XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-vc foot
```

For an SD installation, rebuild the SD rootfs with `tools/sdroot.sh`, then add `e6.gui=yamashell` to the SD boot configuration. The compositor and its matching `libvcgl.so` must both come from the same build.

## Rendering

- XRGB8888 buffers, and ARGB8888 buffers with a full-surface opaque-region hint, are packed on the CPU into RGB565 textures: half the payload, same precision as the panel. Other ARGB8888 buffers stay RGBA and a shader swaps blue/red. Foot's main surface sends such a hint, so it takes the RGB565 path
- surface and buffer damage are clipped; rectangles merge only when their union is exactly a rectangle, so pixels between disjoint damage are never re-uploaded. A new or resized texture gets the whole buffer
- a buffer is released after its upload, frame callbacks are sent after the scene is presented
- frames are drawn only on change; a callback-only commit or an idle compositor submits nothing
- the compositor draws into the hidden EGL surface; `vcglPresent` waits for the GPU, then removes the visible GCE element and adds the new one in one display update. Each server buffer is posted to the VC swap queue only once (a 1-buffer window never gives its queue slot back)
- window quads use one static vertex buffer, gesture geometry one interleaved buffer
- textures and buffers are freed with the VC III RPCs `0x203e` / `0x203d` (`{count, bytes}` + a bulk list of names), checked against the Symbian client and the VC handlers

## Performance

`yamashell-session` writes cumulative counters to `/run/user/0/yamashell.stats`: PID, monotonic ms, submitted frames, uploaded bytes, upload calls, upload ms, draw/present ms, updated once a second. In a terminal on `wayland-vc`, `fps 10` and `fps line 10` report the compositor submission rate and upload/present time from these counters, save the result to `/run/user/0/fps-last.txt` and write it to dmesg with the `e6-fps` tag. These are submissions, not measured panel scanout.

Measured on the E6 (24.10.2026, PIO):

| Workload | FPS |
| --- | --- |
| moving cached subsurfaces | 45.3 |
| one-line terminal scroll | 21.5 |
| full terminal scroll | 2.6 |

Full scroll is dominated by texture upload over the ViSSI bus.

Tried and dropped: presenting through the native GCE swap queue (op `0x1f`, ~45 FPS but heavy artifacts while moving), asynchronous display submission with three buffers (no gain) and keeping both GCE elements and switching opacity (the second element add hung the VC).

## Limits

The output is fixed at 640x480. Top-level windows get a maximized configure; popups and synchronized subsurfaces have basic support. Buffer transforms other than normal are rejected. Input regions, popup constraint adjustment, drag and drop, IME, primary selection and Xwayland are not implemented. The clipboard supports regular Wayland selection transfers.
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
- a buffer is released after its upload, frame callbacks are sent before the scene is presented, so clients draw their next frame while the VC flips this one
- frames are drawn only on change; a callback-only commit or an idle compositor submits nothing
- the compositor draws into one EGL window with three VC buffers. `eglSwapBuffers` sends the VC swap, DISP op `0x1f` makes the GCE element show the newest swapped buffer at the next flip and frees the old one, and the display update is submitted asynchronously (op `0x11`, completion on UPDH). Only the previous flip is waited for, after the current frame has been drawn, so drawing and flipping overlap and the shown buffer is never drawn into
- window quads use one static vertex buffer, gesture geometry one interleaved buffer
- textures and buffers are freed with the VC III RPCs `0x203e` / `0x203d` (`{count, bytes}` + a bulk list of names), checked against the Symbian client and the VC handlers

## Performance

`yamashell-session` writes cumulative counters to `/run/user/0/yamashell.stats`: PID, monotonic ms, submitted frames, uploaded bytes, upload calls, upload ms, draw/present ms, updated once a second. In a terminal on `wayland-vc`, `fps 10` and `fps line 10` report the compositor submission rate and upload/present time from these counters, save the result to `/run/user/0/fps-last.txt` and write it to dmesg with the `e6-fps` tag. These are submissions, not measured panel scanout.

Measured on the E6:

| Workload | 24.10.2026 (PIO) | 26.10.2026 (DMA, 3-buffer swap, `videocore.panel_clk=24000000`) |
| --- | --- | --- |
| moving cached subsurfaces | 45.3 | 59.1 |
| one-line terminal scroll | 21.5 | 58.4 |
| full terminal scroll | 2.6 | ~4 |

Flips follow the panel's 60 Hz tearing effect signal. With Symbian's 20 MHz panel write clock a frame does not fit into one period and the rate settles at ~45 FPS; the VC steps that clock, 24 MHz is the first step that reaches 60. Full scroll is dominated by texture upload over the ViSSI bus (~90 ms per full RGB565 window). Drawing into the shown buffer (ELEMENT_MODIFIED on one surface) is fast but tears; re-adding the GCE element each frame costs a whole flip without overlap. Keeping both GCE elements and switching opacity hung the VC (only one GCE element exists).

## Client rendering on the VC

Wayland clients can draw on the VC (26.10.2026): `vcwl`, a plain xdg-shell client using `wl_egl_window` and libvcgl, runs full window at 60 FPS through yamashell with no pixels crossing the bus (`wl_shm` clients need ~90 ms per full window upload).

- protocol `e6_vc_image` (`linux/vc-khrn/protocol/e6-vc-image.xml`): `create_buffer(wl_buffer, id0, id1, width, height, format)` wraps a VC global image
- libvcgl on a `wl_display` (`eglGetDisplay`, `eglGetPlatformDisplay(EXT)`, `eglCreatePlatformWindowSurface(EXT)`): a window is three global images (RGB565, GLES2 render + texture usage), each with a wrapped surface. `eglSwapBuffers` flushes without waiting (`0x400c`), paces itself with the previous frame callback, makes the next free image current on the VC, then attaches and commits. The VC server handles one message after the other and spins inside a flush until the frame is drawn, so the compositor's later draws always see the finished frame. Its events use a private queue
- yamashell binds a texture to each image when the buffer is created and draws the shown buffer's texture, y-flipped (GL writes the bottom row first). The previous buffer is released after the next present
- the KHRN client id in every message is the process id, the VC keeps per-client state under it

Rules found on the phone: `0x2082` refuses (`0x502`) an image that has been drawn into, or that another context has current as its draw surface; a texture bound before the first draw keeps seeing every later frame. The client therefore waits for the compositor (roundtrip) after creating its images. Global images and GCE elements of a process that dies without cleaning up stay on the VC until the driver is rebound.

### alacritty and the VC III GL

alacritty 0.17 (Alpine package, `renderer = "gles2"`, `/etc/alacritty/alacritty.toml`) runs on the VC. What it needed from libvcgl, all of it generic:

- **attribute locations**: the VC numbers vertex attributes from the last declared one down, Mesa from the first one up, and alacritty never binds or asks for them. A program that binds none itself gets its attributes bound in declaration order before the link (names parsed from the vertex shader source). Without this nothing was drawn
- **one uniform int, one program per value**: the VC III runs every branch of a shader. alacritty's text shader switches its four passes on `uniform int renderingPass`; the background pass alone took 21 ms per frame, the same pass as its own shader 3.9 ms. libvcgl compiles a variant with the uniform as a constant for every value it is set to and replays the other uniforms into it (`VCGL_NOSPEC=1` turns this off)
- **draws that change nothing are dropped**: every surface is RGB565 without depth, so blending with source factor 0 or `ONE_MINUS_DST_ALPHA` and destination `ONE` keeps the destination (alacritty's second text pass)
- shader strings with explicit lengths get a NUL appended (the VC drops the last byte of each string), `GL_SRC1_*` blend factors are ignored, VAOs are emulated, `eglGetProcAddress` looks in libvcgl itself (alacritty loads libEGL `RTLD_LOCAL`)

The swap ends on a thread of its own (the flush wait, the dispman calls of the compositor's window), the application continues until it needs the VC again. Per frame with a full screen of changing text: ~70 ms before, ~29 ms (~34 FPS) after. What is left is VC work (~15 ms for the client and the compositor frame together, one after the other) and the compositor's swap (dispman update, ~5 ms). `VCGL_PROFILE=1` prints the swap stages and the variants made; `VCGL_SYNC=1` (the swap in the caller), `VCGL_FLUSH_WAIT=1` (clients wait for their frame) and `VCGL_DISP_REPLY=1` (dispman answers) bring the old behaviour back for comparison.

The underlying proof was `vcimg`: a VC global image drawn into by one GL context and sampled as a texture by another (61.6 FPS with a redraw every frame).

| Message | Arguments | Reply |
| --- | --- | --- |
| `0x4019` create global image | `{w, h, format}` | `{len, id0, id1, ..}` |
| `0x4012` wrapped surface | `{id0, id1, depth, mask, multisample}` | `{len, surface, ..}` |
| `0x401a` fill from the host | `{id0, id1, y, rows, stride, format}` + bulk | none |
| `0x401c` destroy | `{id0, id1}` | none |
| `0x2082` image as GLES2 texture | `{GL_TEXTURE_2D, id0, id1}` | none |

The format's low bits select the pixels (0 ARGB premultiplied, 1 ARGB, 2 XRGB, 3 RGB565, 4 A8), `0x20` allows GLES2 rendering and `0x200` GLES2 texturing. The texture references the image, later rendering shows up without rebinding; texture parameters are set after `0x2082`. The VC's own error state can be read with the GL 1.1 `glGetError` (`0x106d`, `VCGL_CHECK=1|2`), but the answer is not reliable: it reports `0x1` on calls that demonstrably work.

## Limits

The output is fixed at 640x480. Top-level windows get a maximized configure; popups and synchronized subsurfaces have basic support. Buffer transforms other than normal are rejected. Input regions, popup constraint adjustment, drag and drop, IME, primary selection and Xwayland are not implemented. The clipboard supports regular Wayland selection transfers.
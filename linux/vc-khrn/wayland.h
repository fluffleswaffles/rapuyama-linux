// SPDX-License-Identifier: GPL-2.0-only
/* EGL windows of Wayland clients, drawn into VC global images (wayland.c) */
#ifndef VCWL_H
#define VCWL_H

#include <stdint.h>

struct vcwl;

/* native is a struct wl_display */
int vcwl_display(void *native);
int vcwl_init(void *native);
/* native is a struct wl_egl_window */
struct vcwl *vcwl_create(void *native);
void vcwl_destroy(struct vcwl *s);
/* the wrapped surface to draw into now */
uint32_t vcwl_draw(const struct vcwl *s);
int vcwl_width(const struct vcwl *s);
int vcwl_height(const struct vcwl *s);
/* ctx = the current GL context handle drawing into s, or 0 */
int vcwl_swap(struct vcwl *s, uint32_t ctx);

#endif
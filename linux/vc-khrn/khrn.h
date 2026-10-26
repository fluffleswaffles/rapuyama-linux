// SPDX-License-Identifier: GPL-2.0-only
/* khronos rpc to the E6's VideoCore III over /dev/vc-khrn */
#ifndef KHRN_H
#define KHRN_H

#include <stdint.h>

/* VC III ids, each one checked against its vc-main handler (arg count) */
#define KHRN_EGL_CREATE_SURFACE 0x4000	/* 12, reply {w, h, buffer, handle} */
#define KHRN_EGL_CREATE_GLES20 0x4002	/* 2 {share, 1}, reply ctx */
#define KHRN_EGL_DESTROY_SURFACE 0x4004	/* 1, no reply */
#define KHRN_EGL_DESTROY_GL 0x4005	/* 1 */
#define KHRN_EGL_MAKE_CURRENT 0x4008	/* 8 {pid lo, pid hi, type, ctx, draw, read, vg, vg} */
#define KHRN_EGL_FLUSH_WAIT 0x4009	/* 2 {1, 0}, int reply */
#define KHRN_EGL_FLUSH 0x400c		/* 2 {1, 0}, no reply: 0x4009 without the answer (vc-main 0x4637c) */
#define KHRN_EGL_SWAP 0x400a		/* 5 {surface, w, h, window, preserve}, no reply */
#define KHRN_EGL_WRAP_IMAGE 0x4012	/* 5 {id0, id1, depth, mask, multisample}, res reply {surface, ..} */
#define KHRN_EGL_CREATE_IMAGE 0x4019	/* 3 {w, h, format}, res reply {id0, id1, ..}: a global image */
#define KHRN_EGL_DESTROY_IMAGE 0x401c	/* 2 {id0, id1} */
#define KHRN_GL_TEXTURE_IMAGE 0x2082	/* 3 {GL_TEXTURE_2D, id0, id1}: the bound texture samples the image */
#define KHRN_GL_CLEAR_COLOR 0x2003	/* 4 floats */
#define KHRN_GL_CLEAR 0x202c		/* 1 mask */

#define KHRN_GL20 2			/* make current type */
#define KHRN_RGB_565 0xd20d		/* khclient config table: color, no depth, no mask */
#define KHRN_FMT_NONE 0xffffffff

/* dispman */
#define DISP_REPLY 0x80000000
#define DISP_UPDATE_START 0x10
#define DISP_SUBMIT_ASYNC 0x11		/* {update, 1}, done = the update handle on UPDH */
#define DISP_SUBMIT_SYNC 0x12
#define DISP_ELEMENT_MODIFIED 0x14	/* {update, element, x, y, w, h}, no reply */
#define DISP_ELEMENT_REMOVE 0x15
#define DISP_GCE_REGISTER 0x1c		/* {type | id, key, vc image type, w, h} */
#define DISP_GCE_ELEMENT_ADD 0x1e	/* 17 args, see khrntest.c */
#define DISP_GCE_ELEMENT_ADVANCE 0x1f	/* {update, element, 0, 0}: show the newest swapped buffer */
#define VC_IMG_TF_RGB565 25

int khrn_init(void);
uint32_t khrn_display(void);
/* the make current prefix every KHRN message starts with */
void khrn_current(uint32_t type, uint32_t ctx, uint32_t draw, uint32_t read);
/* one KHRN message + nbulk bulk pieces, returns the reply bytes or -errno */
int khrn_call(const uint32_t *w, uint32_t n, const void *bulk,
	      const uint32_t *sizes, uint32_t nbulk, void *reply, uint32_t reply_max);
/* queue n words of calls without reply or bulk, they go out with the next
 * message (or khrn_flush) */
int khrn_push(const uint32_t *w, uint32_t n);
int khrn_flush(void);
/* send the make current prefix now, with whatever is queued */
int khrn_send_current(void);
/* word i of a "res" reply, which carries its length in front */
uint32_t khrn_res_word(const uint32_t *r, int len, int i);
/* one word reply, -errno if none */
int khrn_call_int(const uint32_t *w, uint32_t n, uint32_t *out);
/* dispman command, with reply: *out = the answer */
int disp_call(uint32_t cmd, const uint32_t *a, uint32_t n, int reply, uint32_t *out);
/* wait until the VC reports an async submitted update as done */
int khrn_wait_update(uint32_t update);
/* the async swap (wayland.c) owns the VC while the gate is closed: every
 * khrn call of another thread waits in khrn_gate until it opens again */
void khrn_gate(void);
void khrn_gate_close(void);
void khrn_gate_open(void);
void khrn_gate_owner(void);
extern uint64_t khrn_gate_wait_us;
/* fn(arg) on the swap thread, the gate closed until it returns. returns -1
 * when the previous one failed. VCGL_SYNC=1: fn runs here, its result back */
int khrn_async(int (*fn)(void *), void *arg);

#endif
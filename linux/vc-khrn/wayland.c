// SPDX-License-Identifier: GPL-2.0-only
/*
 * EGL windows of Wayland clients: three VC global images, each drawn through
 * a wrapped surface and handed to the compositor as an e6_vc_image wl_buffer.
 * The pixels never leave the VC. Events go to a queue of our own so the
 * application's dispatching never sees them
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl-backend.h>

#include "e6-vc-image-client.h"
#include "khrn.h"
#include "wayland.h"

#define NBUF 3			/* shown, waiting to be released, being drawn */
#define FORMAT (3 | 0x20 | 0x200)	/* RGB565, GLES2 draws it, GLES2 textures it */

struct buf{
	uint32_t id0, id1, surface;
	struct wl_buffer *buffer;
	int busy;
};

struct vcwl{
	struct wl_egl_window *win;
	struct wl_surface *surface;	/* wrapper on our queue */
	struct wl_callback *frame;
	struct buf b[NBUF];
	int w, h, back;
	uint32_t ctx;	/* for the swap job */
};

static struct wl_display *display;
static struct wl_event_queue *queue;
static struct e6_vc_image *images;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version){
	(void)data;
	(void)version;
	if(!strcmp(interface, e6_vc_image_interface.name)){images=wl_registry_bind(r, name, &e6_vc_image_interface, 1);}
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name){(void)data; (void)r; (void)name;}
static const struct wl_registry_listener registry_listener={global, global_remove};

int vcwl_display(void *native){
	return native && *(const struct wl_interface **)native==&wl_display_interface;
}

int vcwl_init(void *native){
	struct wl_display *wrapper;
	struct wl_registry *r;

	if(images){return 0;}
	display=native;
	queue=wl_display_create_queue(display);
	wrapper=queue ? wl_proxy_create_wrapper(display) : NULL;
	if(!wrapper){return -1;}
	wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
	r=wl_display_get_registry(wrapper);
	wl_registry_add_listener(r, &registry_listener, NULL);
	wl_display_roundtrip_queue(display, queue);
	wl_registry_destroy(r);
	wl_proxy_wrapper_destroy(wrapper);
	return images ? 0 : -1;
}

static void released(void *data, struct wl_buffer *buffer){
	(void)buffer;
	((struct buf *)data)->busy=0;
}
static const struct wl_buffer_listener buffer_listener={released};

static void buf_destroy(struct buf *b){
	if(b->buffer){wl_buffer_destroy(b->buffer);}
	if(b->surface){
		uint32_t w[2]={KHRN_EGL_DESTROY_SURFACE, b->surface};

		khrn_push(w, 2);
	}
	if(b->id0 || b->id1){
		uint32_t w[3]={KHRN_EGL_DESTROY_IMAGE, b->id0, b->id1};

		khrn_push(w, 3);
	}
	khrn_flush();
	memset(b, 0, sizeof(*b));
}

static int buf_create(struct buf *b, int w, int h){
	uint32_t r[8], c[6]={KHRN_EGL_CREATE_IMAGE, w, h, FORMAT};
	int len;

	memset(b, 0, sizeof(*b));
	len=khrn_call(c, 4, 0, 0, 0, r, sizeof(r));
	b->id0=khrn_res_word(r, len, 0);
	b->id1=khrn_res_word(r, len, 1);
	if(len<8 || (!b->id0 && !b->id1)){return -1;}
	/* VC III wrapped surface {id0, id1, depth, mask, multisample} */
	c[0]=KHRN_EGL_WRAP_IMAGE;
	c[1]=b->id0;
	c[2]=b->id1;
	c[3]=KHRN_FMT_NONE;
	c[4]=KHRN_FMT_NONE;
	c[5]=0;
	len=khrn_call(c, 6, 0, 0, 0, r, sizeof(r));
	b->surface=khrn_res_word(r, len, 0);
	b->buffer=b->surface ? e6_vc_image_create_buffer(images, b->id0, b->id1, w, h, FORMAT) : NULL;
	if(!b->buffer){buf_destroy(b); return -1;}
	wl_buffer_add_listener(b->buffer, &buffer_listener, b);
	return 0;
}

static void bufs_destroy(struct vcwl *s){
	for(int i=0; i<NBUF; i++){buf_destroy(&s->b[i]);}
}

/* the compositor binds its textures when it gets the buffers, which only
 * works before anything is drawn into the images: wait for that */
static int bufs_create(struct vcwl *s, int w, int h){
	if(w<1 || h<1 || w>2048 || h>2048){return -1;}
	for(int i=0; i<NBUF; i++){
		if(buf_create(&s->b[i], w, h)){bufs_destroy(s); return -1;}
	}
	if(wl_display_roundtrip_queue(display, queue)<0){bufs_destroy(s); return -1;}
	s->w=w;
	s->h=h;
	s->back=0;
	return 0;
}

static void window_gone(void *data){((struct vcwl *)data)->win=NULL;}

/* khrn_current must be (0, 0, 0, 0) while this creates VC objects */
struct vcwl *vcwl_create(void *native){
	struct wl_egl_window *win=native;
	struct vcwl *s=calloc(1, sizeof(*s));

	if(!s){return NULL;}
	s->win=win;
	s->surface=wl_proxy_create_wrapper(win->surface);
	if(!s->surface || bufs_create(s, win->width, win->height)){
		if(s->surface){wl_proxy_wrapper_destroy(s->surface);}
		free(s);
		return NULL;
	}
	wl_proxy_set_queue((struct wl_proxy *)s->surface, queue);
	win->driver_private=s;
	win->destroy_window_callback=window_gone;
	return s;
}

void vcwl_destroy(struct vcwl *s){
	khrn_gate();
	if(s->frame){wl_callback_destroy(s->frame);}
	bufs_destroy(s);
	if(s->win){
		s->win->driver_private=NULL;
		s->win->destroy_window_callback=NULL;
	}
	wl_proxy_wrapper_destroy(s->surface);
	wl_display_flush(display);
	free(s);
}

uint32_t vcwl_draw(const struct vcwl *s){
	khrn_gate();
	return s->b[s->back].surface;
}
/* shortcut: no gate, a resize in a running swap shows up one swap later */
int vcwl_width(const struct vcwl *s){return s->w;}
int vcwl_height(const struct vcwl *s){return s->h;}

/* VCGL_PROFILE=1: where a client frame goes, on stderr every 128 swaps:
 * the app (between swaps), the flush wait, the frame callback wait, the
 * buffer release wait, making the next image current + commit */
static int profile=-1;
static uint64_t prof_t, prof_us[5], prof_n, commit_t, done_t, cb_us, draw_us;

static uint64_t now_us(void){
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec*1000000+ts.tv_nsec/1000;
}

static void prof(int stage){
	uint64_t t;

	if(profile<0){profile=getenv("VCGL_PROFILE")!=NULL;}
	if(!profile){return;}
	t=now_us();
	/* the swap starts: the app drew since our frame callback came */
	if(stage==0 && done_t){draw_us+=t-done_t; done_t=0;}
	if(stage==4){commit_t=t;}
	if(prof_t){prof_us[stage]+=t-prof_t;}
	prof_t=t;
	if(stage==4 && ++prof_n==128){
		fprintf(stderr, "vcgl: 128 swaps, us per frame: app %llu, flush %llu, frame cb %llu, release %llu, current+commit %llu, app at the gate %llu\n",
			(unsigned long long)prof_us[0]/128, (unsigned long long)prof_us[1]/128, (unsigned long long)prof_us[2]/128,
			(unsigned long long)prof_us[3]/128, (unsigned long long)prof_us[4]/128, (unsigned long long)khrn_gate_wait_us/128);
		fprintf(stderr, "vcgl: commit -> frame callback %llu us, frame callback -> next swap %llu us\n",
			(unsigned long long)cb_us/128, (unsigned long long)draw_us/128);
		khrn_gate_wait_us=cb_us=draw_us=0;
		memset(prof_us, 0, sizeof(prof_us));
		prof_n=0;
	}
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time){
	(void)time;
	if(profile>0 && commit_t){
		done_t=now_us();
		cb_us+=done_t-commit_t;
		commit_t=0;
	}
	wl_callback_destroy(cb);
	((struct vcwl *)data)->frame=NULL;
}
static const struct wl_callback_listener frame_listener={frame_done};

/* the next image nobody holds, waiting for the compositor to release one */
static int next_free(struct vcwl *s){
	for(;;){
		wl_display_dispatch_queue_pending(display, queue);
		for(int i=0; i<NBUF; i++){
			if(!s->b[i].busy){return i;}
		}
		if(wl_display_dispatch_queue(display, queue)<0){return -1;}
	}
}

/*
 * the frame must be in the image before the compositor samples it: the VC
 * server answers one message after the other and spins in the flush until
 * the frame is drawn, so whatever the compositor sends after our commit runs
 * after it. no need to wait for the flush here (VCGL_FLUSH_WAIT=1 does).
 * the image must not be the context's draw surface any more when
 * it does: the VC refuses a texture from an image another context renders
 * into (0x502 in vc-main 0x5ee94, the window stays black). so the next free
 * image becomes current on the VC before the commit. the previous frame
 * callback paces us to the compositor; a resized window gets new images
 */
static int swap_tail(struct vcwl *s, uint32_t ctx){
	static int wait=-1;
	uint32_t fl[3]={KHRN_EGL_FLUSH_WAIT, 1, 0}, done;
	struct buf *b=&s->b[s->back];
	int next;

	if(!s->win){return -1;}
	prof(0);
	if(wait<0){wait=getenv("VCGL_FLUSH_WAIT")!=NULL;}
	if(wait && khrn_call_int(fl, 3, &done)){return -1;}
	/* with the current draw surface, before the next image becomes it */
	fl[0]=KHRN_EGL_FLUSH;
	if(!wait && (khrn_push(fl, 3)<0 || khrn_flush()<0)){return -1;}
	prof(1);
	while(s->frame){
		if(wl_display_dispatch_queue(display, queue)<0){return -1;}
	}
	prof(2);
	b->busy=1;
	next=next_free(s);
	if(next<0){return -1;}
	prof(3);
	khrn_current(ctx ? KHRN_GL20 : 0, ctx, ctx ? s->b[next].surface : 0, ctx ? s->b[next].surface : 0);
	if(khrn_send_current()<0){return -1;}
	s->frame=wl_surface_frame(s->surface);
	wl_callback_add_listener(s->frame, &frame_listener, s);
	wl_surface_attach(s->surface, b->buffer, 0, 0);
	wl_surface_damage(s->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(s->surface);
	s->back=next;
	s->win->attached_width=s->w;
	s->win->attached_height=s->h;
	if(wl_display_flush(display)<0){return -1;}
	prof(4);
	if(s->win->width!=s->w || s->win->height!=s->h){
		khrn_current(0, 0, 0, 0);
		bufs_destroy(s);
		return bufs_create(s, s->win->width, s->win->height);
	}
	return 0;
}

static int swap_job(void *arg){
	struct vcwl *s=arg;

	return swap_tail(s, s->ctx);
}

int vcwl_swap(struct vcwl *s, uint32_t ctx){
	khrn_gate();
	if(!s->win){return -1;}
	s->ctx=ctx;
	return khrn_async(swap_job, s);
}
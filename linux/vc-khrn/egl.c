// SPDX-License-Identifier: GPL-2.0-only
/*
 * EGL 1.4 for the VideoCore III: one config (RGB565, no depth), GLES 2
 * contexts, window surfaces with three VC buffers shown as GCE elements.
 * On a wl_display the windows are Wayland ones instead (wayland.c)
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gl.h"
#include "khrn.h"
#include "wayland.h"

#define SCREEN_W 640
#define SCREEN_H 480
#define MAX_SURFACES 4
#define KEY_BASE 0xe6100000	/* window keys, the driver's own test uses 0xe600000x */
#define GCE_BASE 0x96000010	/* one-handle GCE surfaces */
#define LAYER 10		/* the console is -400 */
#define BUFFERS 3		/* shown, waiting for its flip, being drawn */

struct surface{
	int used;
	struct vcwl *wl;
	uint32_t serverbuffer, element;
	int x, y, w, h, idx;
};

struct context{
	int used;
	uint32_t handle;
};

static struct surface surfaces[MAX_SURFACES];
static struct context contexts[MAX_SURFACES];
static EGLint error=EGL_SUCCESS;
static int initialized;
static struct context *current;
static struct surface *current_draw;
static void *native_display;	/* a wl_display, or none */
static uint32_t pending;	/* async update whose flip is not done yet */

static int submit_update(uint32_t update){
	uint32_t status=0;

	return disp_call(DISP_SUBMIT_SYNC, &update, 1, 1, &status) || (int32_t)status<0 ? -1 : 0;
}

/* VCGL_PROFILE=1: per stage time of present() on stderr every 128 frames */
static int profile=-1;
static uint64_t prof_t, prof_us[4], prof_n;

static uint64_t now_us(void){
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}

static void prof(int stage){
	uint64_t t;

	if(profile<0){profile=getenv("VCGL_PROFILE")!=NULL;}
	if(!profile){return;}
	t=now_us();
	if(stage>=0){prof_us[stage]+=t-prof_t;}
	prof_t=t;
	if(stage==3 && ++prof_n==128){
		fprintf(stderr, "vcgl: 128 presents, us per frame: flush wait %llu, update start %llu, element %llu, submit %llu\n",
			(unsigned long long)prof_us[0]/128, (unsigned long long)prof_us[1]/128,
			(unsigned long long)prof_us[2]/128, (unsigned long long)prof_us[3]/128);
		memset(prof_us, 0, sizeof(prof_us));
		prof_n=0;
	}
}

#define DPY ((EGLDisplay)1)
#define CONFIG ((EGLConfig)1)

static EGLBoolean fail(EGLint e){
	error=e;
	return EGL_FALSE;
}

EGLAPI EGLint EGLAPIENTRY eglGetError(void){
	EGLint e=error;

	error=EGL_SUCCESS;
	return e;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType display_id){
	if(vcwl_display((void *)display_id)){native_display=(void *)display_id;}
	return DPY;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attribs){
	(void)attribs;
	if(platform!=EGL_PLATFORM_WAYLAND_KHR || !vcwl_display(native)){error=EGL_BAD_PARAMETER; return EGL_NO_DISPLAY;}
	native_display=native;
	return DPY;
}
EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native, const EGLint *attribs){
	(void)attribs;
	return eglGetPlatformDisplay(platform, native, NULL);
}

EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor){
	if(dpy!=DPY){return fail(EGL_BAD_DISPLAY);}
	if(!initialized && khrn_init()){return fail(EGL_NOT_INITIALIZED);}
	if(native_display && vcwl_init(native_display)){return fail(EGL_NOT_INITIALIZED);}
	initialized=1;
	if(major){*major=1;}
	if(minor){*minor=4;}
	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay dpy){
	(void)dpy;
	return EGL_TRUE;
}

EGLAPI const char *EGLAPIENTRY eglQueryString(EGLDisplay dpy, EGLint name){
	if(dpy==EGL_NO_DISPLAY && name==EGL_EXTENSIONS){return "EGL_EXT_platform_base EGL_KHR_platform_wayland EGL_EXT_platform_wayland";}
	switch(name){
	case EGL_VENDOR: return "Broadcom (E6 vc-khrn)";
	case EGL_VERSION: return "1.4";
	case EGL_EXTENSIONS: return "";
	case EGL_CLIENT_APIS: return "OpenGL_ES";
	}
	error=EGL_BAD_PARAMETER;
	return 0;
}

static const EGLint config_attribs[][2]={
	{EGL_BUFFER_SIZE, 16}, {EGL_RED_SIZE, 5}, {EGL_GREEN_SIZE, 6},
	{EGL_BLUE_SIZE, 5}, {EGL_ALPHA_SIZE, 0}, {EGL_DEPTH_SIZE, 0},
	{EGL_STENCIL_SIZE, 0}, {EGL_CONFIG_ID, 1}, {EGL_LEVEL, 0},
	{EGL_SAMPLES, 0}, {EGL_SAMPLE_BUFFERS, 0},
	{EGL_SURFACE_TYPE, EGL_WINDOW_BIT}, {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT},
	{EGL_CONFORMANT, EGL_OPENGL_ES2_BIT}, {EGL_NATIVE_RENDERABLE, EGL_FALSE},
	{EGL_NATIVE_VISUAL_ID, 0}, {EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER},
	{EGL_CONFIG_CAVEAT, EGL_NONE}, {EGL_TRANSPARENT_TYPE, EGL_NONE},
	{EGL_MAX_PBUFFER_WIDTH, 0}, {EGL_MAX_PBUFFER_HEIGHT, 0}, {EGL_MAX_PBUFFER_PIXELS, 0},
	{EGL_MIN_SWAP_INTERVAL, 0}, {EGL_MAX_SWAP_INTERVAL, 1},
	{EGL_ALPHA_MASK_SIZE, 0}, {EGL_LUMINANCE_SIZE, 0}, {EGL_BIND_TO_TEXTURE_RGB, EGL_FALSE},
	{EGL_BIND_TO_TEXTURE_RGBA, EGL_FALSE}, {EGL_NATIVE_VISUAL_TYPE, EGL_NONE},
	{EGL_TRANSPARENT_RED_VALUE, 0}, {EGL_TRANSPARENT_GREEN_VALUE, 0}, {EGL_TRANSPARENT_BLUE_VALUE, 0},
};

EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint size, EGLint *num){
	(void)dpy;
	if(!num){return fail(EGL_BAD_PARAMETER);}
	*num=1;
	if(configs && size>0){configs[0]=CONFIG;}
	return EGL_TRUE;
}

/* shortcut: there is one config, it is returned whatever was asked (depth asks get none) */
EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs, EGLint size, EGLint *num){
	(void)attribs;
	return eglGetConfigs(dpy, configs, size, num);
}

EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value){
	(void)dpy;
	if(config!=CONFIG){return fail(EGL_BAD_CONFIG);}
	for(size_t i=0; i<sizeof(config_attribs)/sizeof(config_attribs[0]); i++){
		if(config_attribs[i][0]==attribute){*value=config_attribs[i][1]; return EGL_TRUE;}
	}
	return fail(EGL_BAD_ATTRIBUTE);
}

EGLAPI EGLBoolean EGLAPIENTRY eglBindAPI(EGLenum api){
	return api==EGL_OPENGL_ES_API ? EGL_TRUE : fail(EGL_BAD_PARAMETER);
}
EGLAPI EGLenum EGLAPIENTRY eglQueryAPI(void){return EGL_OPENGL_ES_API;}

EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint *attribs){
	uint32_t w[3]={KHRN_EGL_CREATE_GLES20, 0, 1}, h=0;
	int i;

	(void)dpy;
	(void)config;
	(void)share;
	for(; attribs && attribs[0]!=EGL_NONE; attribs+=2){
		if(attribs[0]==EGL_CONTEXT_CLIENT_VERSION && attribs[1]!=2){error=EGL_BAD_MATCH; return EGL_NO_CONTEXT;}
	}
	for(i=0; i<MAX_SURFACES && contexts[i].used; i++){}
	if(i==MAX_SURFACES){error=EGL_BAD_ALLOC; return EGL_NO_CONTEXT;}
	khrn_current(0, 0, 0, 0);
	if(khrn_call_int(w, 3, &h) || !h){error=EGL_BAD_ALLOC; return EGL_NO_CONTEXT;}
	contexts[i].used=1;
	contexts[i].handle=h;
	return &contexts[i];
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay dpy, EGLContext ctx){
	struct context *c=ctx;
	uint32_t w[2]={KHRN_EGL_DESTROY_GL, 0};

	(void)dpy;
	if(!c || !c->used){return fail(EGL_BAD_CONTEXT);}
	if(c==current){
		khrn_flush();
		current=0;
	}
	khrn_current(0, 0, 0, 0);
	w[1]=c->handle;
	khrn_push(w, 2);
	khrn_flush();
	c->used=0;
	return EGL_TRUE;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attribs){
	const struct vc_window *vw=(const struct vc_window *)win;
	struct surface *s;
	uint32_t r[8], reg=0;
	int i, len;

	(void)dpy;
	(void)config;
	(void)attribs;
	for(i=0; i<MAX_SURFACES && surfaces[i].used; i++){}
	if(i==MAX_SURFACES){error=EGL_BAD_ALLOC; return EGL_NO_SURFACE;}
	s=&surfaces[i];
	memset(s, 0, sizeof(*s));
	s->idx=i;
	if(native_display){
		/* a wl_egl_window: its pixels go to the compositor as VC images */
		if(!win){error=EGL_BAD_NATIVE_WINDOW; return EGL_NO_SURFACE;}
		khrn_current(0, 0, 0, 0);
		s->wl=vcwl_create((void *)win);
		if(!s->wl){error=EGL_BAD_ALLOC; return EGL_NO_SURFACE;}
		s->w=vcwl_width(s->wl);
		s->h=vcwl_height(s->wl);
		s->used=1;
		return s;
	}
	s->x=vw ? vw->x : 0;
	s->y=vw ? vw->y : 0;
	s->w=vw && vw->width>0 ? vw->width : SCREEN_W;
	s->h=vw && vw->height>0 ? vw->height : SCREEN_H;
	{
		/* VC III layout (khclient_vc3 FUN_000096d4), 12 args */
		uint32_t c[13]={KHRN_EGL_CREATE_SURFACE, KEY_BASE+i, BUFFERS, s->w, s->h, KHRN_RGB_565,
				KHRN_FMT_NONE, KHRN_FMT_NONE, 0, 0, 0, 0, 0};

		khrn_current(0, 0, 0, 0);
		len=khrn_call(c, 13, 0, 0, 0, r, sizeof(r));
		s->serverbuffer=khrn_res_word(r, len, 2);
	}
	if(!s->serverbuffer){error=EGL_BAD_ALLOC; return EGL_NO_SURFACE;}
	{
		/* one-handle GCE surface under the window key, vc image TF_RGB565 */
		uint32_t a[5]={GCE_BASE+i, KEY_BASE+i, VC_IMG_TF_RGB565, s->w, s->h};

		if(disp_call(DISP_GCE_REGISTER, a, 5, 1, &reg) || (int32_t)reg<0){
			uint32_t d[2]={KHRN_EGL_DESTROY_SURFACE, s->serverbuffer};

			khrn_push(d, 2);
			khrn_flush();
			error=EGL_BAD_ALLOC;
			return EGL_NO_SURFACE;
		}
	}
	s->used=1;
	return s;
}

static int wait_pending(void){
	uint32_t update=pending;

	pending=0;
	return update ? khrn_wait_update(update) : 0;
}

static void element_remove(struct surface *s){
	uint32_t a[2], update;

	khrn_gate();	/* a swap job may still own the element */
	if(!s->element){return;}
	wait_pending();
	a[0]=1;
	if(!disp_call(DISP_UPDATE_START, a, 1, 1, &update)){
		a[0]=update;
		a[1]=s->element;
		disp_call(DISP_ELEMENT_REMOVE, a, 2, 0, 0);
		a[0]=update;
		disp_call(DISP_SUBMIT_SYNC, a, 1, 1, 0);
	}
	s->element=0;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay dpy, EGLSurface surface){
	struct surface *s=surface;
	uint32_t gce=0, d[2]={KHRN_EGL_DESTROY_SURFACE, 0};

	(void)dpy;
	if(!s || !s->used){return fail(EGL_BAD_SURFACE);}
	if(current_draw==s){current_draw=NULL;}
	if(s->wl){
		khrn_flush();
		khrn_current(0, 0, 0, 0);
		vcwl_destroy(s->wl);
		s->wl=NULL;
		s->used=0;
		return EGL_TRUE;
	}
	khrn_flush();
	element_remove(s);
	gce=GCE_BASE+s->idx;
	disp_call(0x1d, &gce, 1, 0, 0);	/* GCE surface destroy {type | id} */
	khrn_current(0, 0, 0, 0);
	d[1]=s->serverbuffer;
	/* no reply on VC III (khclient FUN_0001ed98) */
	khrn_push(d, 2);
	khrn_flush();
	s->used=0;
	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx){
	struct surface *d=draw, *r=read;
	struct context *c=ctx;

	(void)dpy;
	if(!c){
		khrn_flush();
		khrn_current(0, 0, 0, 0);
		current=0;
		current_draw=NULL;
		return EGL_TRUE;
	}
	if(!c->used || !d || !d->used || !r || !r->used){return fail(EGL_BAD_MATCH);}
	if(c!=current){glgl_reset();}
	khrn_current(KHRN_GL20, c->handle, d->wl ? vcwl_draw(d->wl) : d->serverbuffer,
		     r->wl ? vcwl_draw(r->wl) : r->serverbuffer);
	current=c;
	current_draw=d;
	return EGL_TRUE;
}

EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext(void){return current ? current : EGL_NO_CONTEXT;}
EGLAPI EGLSurface EGLAPIENTRY eglGetCurrentSurface(EGLint readdraw){(void)readdraw; return current && current_draw ? current_draw : EGL_NO_SURFACE;}

EGLAPI EGLBoolean EGLAPIENTRY eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value){
	struct context *c=ctx;

	(void)dpy;
	if(!c || !c->used){return fail(EGL_BAD_CONTEXT);}
	switch(attribute){
	case EGL_CONFIG_ID: *value=1; return EGL_TRUE;
	case EGL_CONTEXT_CLIENT_TYPE: *value=EGL_OPENGL_ES_API; return EGL_TRUE;
	case EGL_CONTEXT_CLIENT_VERSION: *value=2; return EGL_TRUE;
	case EGL_RENDER_BUFFER: *value=EGL_BACK_BUFFER; return EGL_TRUE;
	}
	return fail(EGL_BAD_ATTRIBUTE);
}

/* swap behaviour and the like: nothing to change */
EGLAPI EGLBoolean EGLAPIENTRY eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value){
	struct surface *s=surface;

	(void)dpy;
	(void)attribute;
	(void)value;
	return s && s->used ? EGL_TRUE : fail(EGL_BAD_SURFACE);
}
EGLAPI EGLDisplay EGLAPIENTRY eglGetCurrentDisplay(void){return current ? DPY : EGL_NO_DISPLAY;}

/* the GCE element add (vc-main 0x43278): 17 args */
static int element_add(struct surface *s){
	uint32_t a[17], update;

	a[0]=1;
	if(disp_call(DISP_UPDATE_START, a, 1, 1, &update)){return -1;}
	prof(1);
	a[0]=update;
	a[1]=GCE_BASE+s->idx;
	a[2]=0;
	a[3]=KEY_BASE+s->idx;
	a[4]=khrn_display();
	a[5]=LAYER;
	a[6]=s->x;
	a[7]=s->y;
	a[8]=s->w;
	a[9]=s->h;
	a[10]=0;
	a[11]=0;
	a[12]=s->w<<16;
	a[13]=s->h<<16;
	a[14]=0;	/* bit 0 = an extra slot check i did not follow */
	a[15]=0xff;
	a[16]=0;
	if(disp_call(DISP_GCE_ELEMENT_ADD, a, 17, 1, &s->element) || s->element==0xffffffff){s->element=0;}
	prof(2);
	if(submit_update(update) || !s->element){return -1;}
	prof(3);
	return 0;
}

/*
 * the swap posts the drawn buffer under the window key (the khrn worker does
 * it after the render), op 0x1f makes the element show it at the next flip
 * (~13 ms) and frees the old one. the flip runs while the next frame draws
 * into the third buffer, only the previous flip is waited for. re-adding the
 * element per frame costs the same flip without the overlap, and drawing into
 * the shown buffer (MODIFIED only) tears
 */
static int window_tail(void *arg);

EGLAPI EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface){
	struct surface *s=surface;
	uint32_t w[6];

	(void)dpy;
	if(!s || !s->used){return fail(EGL_BAD_SURFACE);}
	if(s->wl){
		if(vcwl_swap(s->wl, current && current_draw==s ? current->handle : 0)){return fail(EGL_BAD_SURFACE);}
		/* vcwl_swap makes the next image current */
		s->w=vcwl_width(s->wl);
		s->h=vcwl_height(s->wl);
		return EGL_TRUE;
	}
	/* VC III swap {serverbuffer, w, h, window, preserve}, no reply */
	w[0]=KHRN_EGL_SWAP;
	w[1]=s->serverbuffer;
	w[2]=s->w;
	w[3]=s->h;
	w[4]=KEY_BASE+s->idx;
	w[5]=0;
	khrn_push(w, 6);
	return khrn_async(window_tail, s) ? fail(EGL_CONTEXT_LOST) : EGL_TRUE;
}

/* the rest of the swap, on the swap thread: the compositor's event loop
 * takes commits meanwhile */
static int window_tail(void *arg){
	struct surface *s=arg;
	static int reply=-1;
	uint32_t fl[3]={KHRN_EGL_FLUSH_WAIT, 1, 0}, done, a[4], update, status=0;

	prof(-1);
	if(khrn_call_int(fl, 3, &done) || wait_pending()){return -1;}
	prof(0);
	if(!s->element){
		/* the first buffer has to be posted under the key before the add takes it */
		usleep(500000);
		return element_add(s);
	}
	a[0]=1;
	if(disp_call(DISP_UPDATE_START, a, 1, 1, &update)){return -1;}
	prof(1);
	a[0]=update;
	a[1]=s->element;
	a[2]=0;
	a[3]=0;
	/* no answers: a failed update shows up in the UPDH wait. VCGL_DISP_REPLY=1: with */
	if(reply<0){reply=getenv("VCGL_DISP_REPLY")!=NULL;}
	if(disp_call(DISP_GCE_ELEMENT_ADVANCE, a, 4, reply, &status) || (reply && (int32_t)status<0)){return -1;}
	prof(2);
	a[0]=update;
	a[1]=1;
	if(disp_call(DISP_SUBMIT_ASYNC, a, 2, reply, &status) || (reply && (int32_t)status<0)){return -1;}
	pending=update;
	prof(3);
	return 0;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void *win, const EGLAttrib *attribs){
	(void)attribs;
	return eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)win, NULL);
}
EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void *win, const EGLint *attribs){
	(void)attribs;
	return eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)win, NULL);
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapInterval(EGLDisplay dpy, EGLint interval){
	(void)dpy;
	(void)interval;
	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value){
	struct surface *s=surface;

	(void)dpy;
	if(!s || !s->used){return fail(EGL_BAD_SURFACE);}
	switch(attribute){
	case EGL_WIDTH: *value=s->w; return EGL_TRUE;
	case EGL_HEIGHT: *value=s->h; return EGL_TRUE;
	case EGL_CONFIG_ID: *value=1; return EGL_TRUE;
	case EGL_RENDER_BUFFER: *value=EGL_BACK_BUFFER; return EGL_TRUE;
	}
	return fail(EGL_BAD_ATTRIBUTE);
}

EGLAPI EGLBoolean EGLAPIENTRY eglWaitClient(void){
	uint32_t w[3]={KHRN_EGL_FLUSH_WAIT, 1, 0}, done;

	return khrn_call_int(w, 3, &done) ? EGL_FALSE : EGL_TRUE;
}
EGLAPI EGLBoolean EGLAPIENTRY eglWaitGL(void){return eglWaitClient();}
EGLAPI EGLBoolean EGLAPIENTRY eglWaitNative(EGLint engine){(void)engine; return EGL_TRUE;}
EGLAPI EGLBoolean EGLAPIENTRY eglReleaseThread(void){return eglMakeCurrent(DPY, 0, 0, 0);}

EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char *name){
	static void *self;
	void *f;

	/* clients often dlopen libEGL RTLD_LOCAL: look in this library itself */
	if(!self){
		Dl_info info;

		if(dladdr((void *)eglGetProcAddress, &info)){self=dlopen(info.dli_fname, RTLD_LAZY|RTLD_NOLOAD);}
	}
	f=self ? dlsym(self, name) : NULL;
	if(!f){f=dlsym(RTLD_DEFAULT, name);}

	/* VCGL_MISSING=1: name what a client asks for and libvcgl does not have */
	if(!f && getenv("VCGL_MISSING")){fprintf(stderr, "vcgl: no %s\n", name);}
	return (__eglMustCastToProperFunctionPointerType)f;
}
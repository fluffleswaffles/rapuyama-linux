// SPDX-License-Identifier: GPL-2.0-only
/*
 * EGL 1.4 for the VideoCore III: one config (RGB565, no depth), GLES 2
 * contexts, one buffer window surfaces shown as GCE elements
 */
#include <EGL/egl.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "gl.h"
#include "khrn.h"

#define SCREEN_W 640
#define SCREEN_H 480
#define MAX_SURFACES 4
#define KEY_BASE 0xe6100000	/* window keys, the driver's own test uses 0xe600000x */
#define GCE_BASE 0x96000010	/* one-handle GCE surfaces */
#define LAYER 10		/* the console is -400 */

struct surface{
	int used, posted;
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

static int submit_update(uint32_t update){
	uint32_t status=0;

	return disp_call(DISP_SUBMIT_SYNC, &update, 1, 1, &status) || (int32_t)status<0 ? -1 : 0;
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
	(void)display_id;
	return DPY;
}

EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor){
	if(dpy!=DPY){return fail(EGL_BAD_DISPLAY);}
	if(!initialized && khrn_init()){return fail(EGL_NOT_INITIALIZED);}
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
	(void)dpy;
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
	{EGL_MIN_SWAP_INTERVAL, 1}, {EGL_MAX_SWAP_INTERVAL, 1},
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

/* "res" replies carry their length in front */
static uint32_t word(const uint32_t *r, int len, int i){
	if(len>=4 && r[0]==(uint32_t)len-4){i++;}
	return len>=4*(i+1) ? r[i] : 0;
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
	s->x=vw ? vw->x : 0;
	s->y=vw ? vw->y : 0;
	s->w=vw && vw->width>0 ? vw->width : SCREEN_W;
	s->h=vw && vw->height>0 ? vw->height : SCREEN_H;
	{
		/* VC III layout (khclient_vc3 FUN_000096d4), 12 args */
		uint32_t c[13]={KHRN_EGL_CREATE_SURFACE, KEY_BASE+i, 1, s->w, s->h, KHRN_RGB_565,
				KHRN_FMT_NONE, KHRN_FMT_NONE, 0, 0, 0, 0, 0};

		khrn_current(0, 0, 0, 0);
		len=khrn_call(c, 13, 0, 0, 0, r, sizeof(r));
		s->serverbuffer=word(r, len, 2);
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

static void element_remove(struct surface *s){
	uint32_t a[2], update;

	if(!s->element){return;}
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
		return EGL_TRUE;
	}
	if(!c->used || !d || !d->used || !r || !r->used){return fail(EGL_BAD_MATCH);}
	if(c!=current){glgl_reset();}
	khrn_current(KHRN_GL20, c->handle, d->serverbuffer, r->serverbuffer);
	current=c;
	return EGL_TRUE;
}

EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext(void){return current ? current : EGL_NO_CONTEXT;}
EGLAPI EGLDisplay EGLAPIENTRY eglGetCurrentDisplay(void){return current ? DPY : EGL_NO_DISPLAY;}

/* the GCE element add (vc-main 0x43278): 17 args, removes previous in the same update */
static int element_add(struct surface *s, struct surface *previous){
	uint32_t a[17], update;

	a[0]=1;
	if(disp_call(DISP_UPDATE_START, a, 1, 1, &update)){return -1;}
	if(previous && previous->element){
		uint32_t remove[2]={update, previous->element};

		if(disp_call(DISP_ELEMENT_REMOVE, remove, 2, 0, 0)){return -1;}
	}
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
	a[0]=update;
	if(submit_update(update) || !s->element){return -1;}
	if(previous){previous->element=0;}
	return 0;
}

/*
 * one buffer never gives its display slot back (40 swaps and the VC stops),
 * so swap once to post it, later frames only wait and recompose
 */
static EGLBoolean present(EGLDisplay dpy, EGLSurface surface, struct surface *previous){
	struct surface *s=surface;
	uint32_t w[6], fl[3]={KHRN_EGL_FLUSH_WAIT, 1, 0}, done, a[6], update;

	(void)dpy;
	if(!s || !s->used){return fail(EGL_BAD_SURFACE);}
	if(!s->posted){
		/* VC III swap {serverbuffer, w, h, window, preserve}, no reply */
		w[0]=KHRN_EGL_SWAP;
		w[1]=s->serverbuffer;
		w[2]=s->w;
		w[3]=s->h;
		w[4]=KEY_BASE+s->idx;
		w[5]=0;
		khrn_push(w, 6);
	}
	if(khrn_call_int(fl, 3, &done)){return fail(EGL_CONTEXT_LOST);}
	if(!s->posted){
		/* the khrn worker posts the buffer under the key, then the element takes it */
		usleep(500000);
		s->posted=1;
	}
	if(!s->element){return element_add(s, previous) ? fail(EGL_BAD_SURFACE) : EGL_TRUE;}
	a[0]=1;
	if(disp_call(DISP_UPDATE_START, a, 1, 1, &update)){return fail(EGL_BAD_SURFACE);}
	a[0]=update;
	a[1]=s->element;
	a[2]=0;
	a[3]=0;
	a[4]=s->w;
	a[5]=s->h;
	if(disp_call(DISP_ELEMENT_MODIFIED, a, 6, 0, 0) || submit_update(update)){return fail(EGL_BAD_SURFACE);}
	return EGL_TRUE;
}

/* compositor double buffering: surface goes on screen, previous comes off in the same update */
EGLBoolean vcglPresent(EGLDisplay dpy, EGLSurface surface, EGLSurface previous){
	struct surface *old=previous;

	if(!old || !old->used || old==surface){return fail(EGL_BAD_SURFACE);}
	return present(dpy, surface, old);
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface){return present(dpy, surface, NULL);}

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
	return (__eglMustCastToProperFunctionPointerType)dlsym(RTLD_DEFAULT, name);
}
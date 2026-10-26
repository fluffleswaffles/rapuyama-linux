// SPDX-License-Identifier: GPL-2.0-only
/*
 * a VC global image drawn into through a wrapped surface (what a client would
 * do) and shown as a texture of the window (what the compositor would do), no
 * pixels over the bus. usage: vcimg [seconds] [rebind|fill|fillloop|fork|forkfill]
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "khrn.h"

#define IMG_W 256
#define IMG_H 256
/* low bits = pixel format (3 = RGB565), 0x20 = GLES2 renders it, 0x200 = GLES2 textures it */
#define IMG_FMT (3 | 0x20 | 0x200)

static const char *vs=
	"attribute vec2 pos;\n"
	"attribute vec2 uv;\n"
	"varying vec2 t;\n"
	"void main(){gl_Position=vec4(pos, 0.0, 1.0); t=uv;}\n";
static const char *fs=
	"precision mediump float;\n"
	"uniform sampler2D tex;\n"
	"varying vec2 t;\n"
	"void main(){gl_FragColor=texture2D(tex, t);}\n";

/* right: the global image, left: a plain glTexImage2D texture as the control */
static const GLfloat quad[]={
	0.1f, -0.6f, 0, 0,
	0.9f, -0.6f, 1, 0,
	0.1f, 0.6f, 0, 1,
	0.9f, 0.6f, 1, 1,
};
static const GLfloat ctl_quad[]={
	-0.9f, -0.6f, 0, 0,
	-0.1f, -0.6f, 1, 0,
	-0.9f, 0.6f, 0, 1,
	-0.1f, 0.6f, 1, 1,
};

static GLuint shader(GLenum type, const char *src){
	GLuint s=glCreateShader(type);

	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	return s;
}

/* the VC's glGetError: GL11 0x106d (vc-main 0x65e40 -> 0x6124e) reads the current
 * context's error, GL20 has none of its own. libvcgl's glGetError is local */
static uint32_t vc_error(void){
	uint32_t c[1]={0x106d}, e=0xffffffff;

	khrn_call_int(c, 1, &e);
	return e;
}

static double now(void){
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec+t.tv_nsec/1e9;
}

static uint32_t fbits(float f){
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

/* "res" replies carry their length in front */
static uint32_t word(const uint32_t *r, int len, int i){
	if(len>=4 && r[0]==(uint32_t)len-4){i++;}
	return len>=4*(i+1) ? r[i] : 0;
}

int main(int argc, char **argv){
	double secs=argc>1 ? atof(argv[1]) : 10, t0, t;
	EGLint n=0;
	EGLConfig cfg;
	EGLDisplay dpy=eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint ctx_attr[]={EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	uint32_t r[8], id0=0, id1=0, wsurf=0, ctxh;
	int frames=0, len, rebind=argc>2 && !strcmp(argv[2], "rebind"), fill=argc>2 && !strcmp(argv[2], "fill"),
	    fillloop=argc>2 && !strcmp(argv[2], "fillloop"), forked=argc>2 && !strncmp(argv[2], "fork", 4),
	    forkfill=argc>2 && !strcmp(argv[2], "forkfill");
	GLuint prog, tex, ctl;

	if(!eglInitialize(dpy, 0, 0)){fprintf(stderr, "eglInitialize failed: %#x\n", eglGetError()); return 1;}
	eglChooseConfig(dpy, 0, &cfg, 1, &n);
	if(forked){
		/* fork: this process makes and fills the image, a child with its own pid samples it */
		uint32_t c[4]={0x4019, IMG_W, IMG_H, IMG_FMT};
		static uint16_t px[IMG_W*IMG_H];
		uint32_t bytes=sizeof(px), f[7];
		pid_t child;

		khrn_current(0, 0, 0, 0);
		len=khrn_call(c, 4, 0, 0, 0, r, sizeof(r));
		id0=word(r, len, 0);
		id1=word(r, len, 1);
		printf("pid %d: global image %#x %#x\n", getpid(), id0, id1);
		for(int i=0; i<IMG_W*IMG_H; i++){px[i]=(((i%IMG_W)/32+(i/IMG_W/32))&1) ? 0xf800 : 0x07e0;}
		f[0]=0x401a; f[1]=id0; f[2]=id1; f[3]=0; f[4]=IMG_H; f[5]=IMG_W*2; f[6]=IMG_FMT;
		khrn_call(f, 7, px, &bytes, 1, 0, 0);
		fflush(stdout);
		child=fork();
		if(child>0 && forkfill){
			waitpid(child, 0, 0);
			return 0;
		}
		if(child>0){
			/* the parent keeps drawing into the image through its own context */
			EGLContext pc=eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
			uint32_t w6[6]={0x4012, id0, id1, KHRN_FMT_NONE, KHRN_FMT_NONE, 0}, ps, frames2=0;
			double p0=now();

			khrn_current(0, 0, 0, 0);
			len=khrn_call(w6, 6, 0, 0, 0, r, sizeof(r));
			ps=word(r, len, 0);
			printf("parent pid %d draws into surface %#x\n", getpid(), ps);
			while(waitpid(child, 0, WNOHANG)==0){
				float pt=(float)(now()-p0);
				uint32_t c7[7]={KHRN_GL_CLEAR_COLOR, fbits(0.5f+0.5f*sinf(pt*3)), fbits(0.5f+0.5f*sinf(pt*2+2)), fbits(0.5f), fbits(1),
						KHRN_GL_CLEAR, 0x4000}, done=0, fl[3]={KHRN_EGL_FLUSH_WAIT, 1, 0};

				khrn_current(KHRN_GL20, ((const uint32_t *)pc)[1], ps, ps);
				khrn_push(c7, 7);
				khrn_call_int(fl, 3, &done);
				frames2++;
				usleep(10000);
			}
			printf("parent: %u clears, vc error %#x\n", frames2, vc_error());
			return 0;
		}
		fill=1;
		printf("child pid %d samples it\n", getpid());
	}
	eglChooseConfig(dpy, 0, &cfg, 1, &n);
	EGLContext ctx=eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	EGLSurface surf=eglCreateWindowSurface(dpy, cfg, 0, 0);
	if(ctx==EGL_NO_CONTEXT || surf==EGL_NO_SURFACE){fprintf(stderr, "no context / surface: %#x\n", eglGetError()); return 1;}
	/* libvcgl's context is {used, handle} */
	ctxh=((const uint32_t *)ctx)[1];

	if(!forked){
		uint32_t c[4]={0x4019, IMG_W, IMG_H, IMG_FMT};

		khrn_current(0, 0, 0, 0);
		len=khrn_call(c, 4, 0, 0, 0, r, sizeof(r));
		id0=word(r, len, 0);
		id1=word(r, len, 1);
		printf("global image: reply %d bytes, id %#x %#x\n", len, id0, id1);
		if(len<8 || (!id0 && !id1)){return 1;}
	}
	if(fill && !forked){
		/* host data into the image (khclient FUN_0000b214): {id0, id1, y, rows, stride, format} + bulk */
		static uint16_t px[IMG_W*IMG_H];
		uint32_t bytes=sizeof(px), c[7]={0x401a, id0, id1, 0, IMG_H, IMG_W*2, IMG_FMT};

		for(int i=0; i<IMG_W*IMG_H; i++){px[i]=(((i%IMG_W)/32+(i/IMG_W/32))&1) ? 0xf800 : 0x07e0;}
		khrn_current(0, 0, 0, 0);
		printf("fill: %d\n", khrn_call(c, 7, px, &bytes, 1, 0, 0));
	}
	{
		/* VC III wrapped surface {id0, id1, depth, mask, multi}, a "res" reply
		 * {len, surface, ..} like create surface (khclient FUN_000096d4) */
		uint32_t c[6]={0x4012, id0, id1, KHRN_FMT_NONE, KHRN_FMT_NONE, 0};

		len=khrn_call(c, 6, 0, 0, 0, r, sizeof(r));
		wsurf=word(r, len, 0);
		printf("wrapped surface: reply %d bytes, surface %#x %#x\n", len, wsurf, word(r, len, 1));
		if(!wsurf){return 1;}
	}

	eglMakeCurrent(dpy, surf, surf, ctx);
	prog=glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "uv");
	glLinkProgram(prog);
	glUseProgram(prog);
	glActiveTexture(GL_TEXTURE0);
	{
		static uint16_t px[64*64];

		for(int i=0; i<64*64; i++){px[i]=(((i%64)/8+(i/64/8))&1) ? 0xffe0 : 0x001f;}
		glGenTextures(1, &ctl);
		glBindTexture(GL_TEXTURE_2D, ctl);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
	}
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	{
		/* glEGLImageTargetTexture2DOES for a global image (khclient 0xa964, vc-main 0x5ee94) */
		uint32_t c[4]={0x2082, GL_TEXTURE_2D, id0, id1};

		khrn_push(c, 4);
	}
	printf("vc gl error after the bind: %#x\n", vc_error());
	{
		/* control: an id that does not exist must give GL_INVALID_VALUE */
		uint32_t c[4]={0x2082, GL_TEXTURE_2D, 0xdead, 0};

		khrn_push(c, 4);
		printf("vc gl error after a bad id (want 0x501): %#x\n", vc_error());
		c[2]=id0;
		c[3]=id1;
		khrn_push(c, 4);
	}
	/* after the image: no mipmaps, so the default min filter would leave it incomplete */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glUniform1i(glGetUniformLocation(prog, "tex"), 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, quad+2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glViewport(0, 0, 640, 480);

	t0=now();
	do{
		t=now()-t0;
		if(fillloop){
			/* host data after the bind, a new solid colour every frame */
			static uint16_t px[IMG_W*IMG_H];
			uint32_t bytes=sizeof(px), c[7]={0x401a, id0, id1, 0, IMG_H, IMG_W*2, IMG_FMT};
			uint16_t v=(uint16_t)((frames*0x0841)&0xffff);

			for(int i=0; i<IMG_W*IMG_H; i++){px[i]=v;}
			khrn_current(0, 0, 0, 0);
			khrn_call(c, 7, px, &bytes, 1, 0, 0);
		}else if(!fill){
			/* the "client": clear the image to a colour that moves with time */
			uint32_t c[9]={KHRN_GL_CLEAR_COLOR, fbits(0.5f+0.5f*sinf(t*3)), fbits(0.5f+0.5f*sinf(t*2+2)), fbits(0.5f), fbits(1),
				       KHRN_GL_CLEAR, 0x4000, KHRN_EGL_FLUSH_WAIT, 1};
			uint32_t done=0, f[3]={KHRN_EGL_FLUSH_WAIT, 1, 0};

			khrn_current(KHRN_GL20, ctxh, wsurf, wsurf);
			khrn_push(c, 7);
			khrn_call_int(f, 3, &done);
		}
		eglMakeCurrent(dpy, surf, surf, ctx);
		if(rebind){
			uint32_t c[4]={0x2082, GL_TEXTURE_2D, id0, id1};

			khrn_push(c, 4);
			if(frames<3){printf("vc gl error after rebind %d: %#x\n", frames, vc_error());}
		}
		glClearColor(0.1f, 0.1f, 0.4f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindTexture(GL_TEXTURE_2D, ctl);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, ctl_quad);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, ctl_quad+2);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindTexture(GL_TEXTURE_2D, tex);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, quad+2);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		if(!frames){printf("vc gl error after the first draw: %#x\n", vc_error());}
		eglSwapBuffers(dpy, surf);
		frames++;
	}while(t<secs);
	printf("%d frames in %.1f s = %.1f fps, gl error %#x\n", frames, t, frames/t, glGetError());

	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	{
		uint32_t c[2]={KHRN_EGL_DESTROY_SURFACE, wsurf};

		khrn_push(c, 2);
		khrn_flush();
	}
	eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	return 0;
}
// SPDX-License-Identifier: GPL-2.0-only
/*
 * a Wayland GLES 2 client drawn on the VC: a colour that moves with time and
 * a white triangle in the top left corner (shows the orientation).
 * usage: vcwl [seconds] [feat]
 * feat: one GL feature alacritty's text needs per quarter of the window, each
 * green when it works: top left GL_SHORT positions from a VBO drawn through an
 * EBO, top right a GL_UNSIGNED_BYTE colour attrib, bottom left a glyph put with
 * glTexSubImage2D (alignment 1, odd width) into a 1024x1024 RGBA atlas,
 * bottom right a branch on a uniform int
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "xdg-shell-client.h"

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm;
static int width=320, height=240, configured, closed;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version){
	(void)data;
	(void)version;
	if(!strcmp(interface, wl_compositor_interface.name)){compositor=wl_registry_bind(r, name, &wl_compositor_interface, 4);}
	if(!strcmp(interface, xdg_wm_base_interface.name)){wm=wl_registry_bind(r, name, &xdg_wm_base_interface, 1);}
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name){(void)data; (void)r; (void)name;}
static const struct wl_registry_listener registry_listener={global, global_remove};

static void ping(void *data, struct xdg_wm_base *b, uint32_t serial){(void)data; xdg_wm_base_pong(b, serial);}
static const struct xdg_wm_base_listener wm_listener={ping};

static void surface_configure(void *data, struct xdg_surface *s, uint32_t serial){
	(void)data;
	xdg_surface_ack_configure(s, serial);
	configured=1;
}
static const struct xdg_surface_listener surface_listener={surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states){
	(void)data;
	(void)t;
	(void)states;
	if(w>0 && h>0){width=w; height=h;}
}
static void toplevel_close(void *data, struct xdg_toplevel *t){(void)data; (void)t; closed=1;}
/* xdg_wm_base is bound at version 1, these two never come */
static void toplevel_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h){(void)data; (void)t; (void)w; (void)h;}
static void toplevel_caps(void *data, struct xdg_toplevel *t, struct wl_array *caps){(void)data; (void)t; (void)caps;}
static const struct xdg_toplevel_listener toplevel_listener={toplevel_configure, toplevel_close, toplevel_bounds, toplevel_caps};

static const char *vs=
	"attribute vec2 pos;\n"
	"void main(){gl_Position=vec4(pos, 0.0, 1.0);}\n";
static const char *fs=
	"precision mediump float;\n"
	"uniform vec4 colour;\n"
	"void main(){gl_FragColor=colour;}\n";

static GLuint shader(GLenum type, const char *src){
	GLuint s=glCreateShader(type);

	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	return s;
}

static const char *feat_vs=
	"attribute vec2 pos;\n"
	"attribute vec4 col;\n"
	"attribute vec2 uv;\n"
	"uniform vec4 projection;\n"
	"varying vec4 c;\n"
	"varying vec2 t;\n"
	"void main(){\n"
	"	c=col/255.;\n"
	"	t=uv;\n"
	"	gl_Position=vec4(projection.xy+pos*projection.zw, 0., 1.);\n"
	"}\n";
static const char *feat_fs=
	"precision mediump float;\n"
	"uniform highp int mode;\n"
	"uniform highp int pass;\n"
	"uniform sampler2D mask;\n"
	"varying vec4 c;\n"
	"varying vec2 t;\n"
	"void main(){\n"
	"	if(mode==0){gl_FragColor=vec4(0., 1., 0., 1.);}\n"
	"	else if(mode==1){gl_FragColor=c;}\n"
	"	else if(mode==2){gl_FragColor=vec4(0., texture2D(mask, t).g, 0., 1.);}\n"
	"	else if(pass==1){gl_FragColor=vec4(0., 1., 0., 1.);}\n"
	"	else{gl_FragColor=vec4(1., 0., 0., 1.);}\n"
	"}\n";

/* alacritty's vertex: short positions, float uv, byte colour */
struct fv{
	GLshort x, y;
	GLfloat u, v;
	GLubyte r, g, b, a;
};

static GLuint feat_prog, feat_vbo, feat_ebo, feat_tex;
static GLint feat_proj, feat_mode, feat_pass;

static void feat_init(void){
	static const GLushort idx[]={0, 1, 3, 1, 2, 3};
	GLubyte glyph[7*9*4];

	feat_prog=glCreateProgram();
	glAttachShader(feat_prog, shader(GL_VERTEX_SHADER, feat_vs));
	glAttachShader(feat_prog, shader(GL_FRAGMENT_SHADER, feat_fs));
	glBindAttribLocation(feat_prog, 0, "pos");
	glBindAttribLocation(feat_prog, 1, "uv");
	glBindAttribLocation(feat_prog, 2, "col");
	glLinkProgram(feat_prog);
	feat_proj=glGetUniformLocation(feat_prog, "projection");
	feat_mode=glGetUniformLocation(feat_prog, "mode");
	feat_pass=glGetUniformLocation(feat_prog, "pass");
	printf("feat uniforms proj %d mode %d pass %d\n", feat_proj, feat_mode, feat_pass);

	glGenBuffers(1, &feat_ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, feat_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
	glGenBuffers(1, &feat_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, feat_vbo);
	glBufferData(GL_ARRAY_BUFFER, 4*4*sizeof(struct fv), NULL, GL_STREAM_DRAW);

	/* an all-green 7x9 glyph at (5, 3) of an empty atlas */
	for(unsigned i=0; i<sizeof(glyph); i+=4){glyph[i]=0; glyph[i+1]=255; glyph[i+2]=0; glyph[i+3]=255;}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glGenTextures(1, &feat_tex);
	glBindTexture(GL_TEXTURE_2D, feat_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 5, 3, 7, 9, GL_RGBA, GL_UNSIGNED_BYTE, glyph);
}

/* four quads in pixels, y down like alacritty's projection */
static void feat_draw(int w, int h){
	struct fv v[16];
	float u0=6/1024.f, v0=4/1024.f, u1=11/1024.f, v1=11/1024.f;

	for(int q=0; q<4; q++){
		GLshort x0=(q&1) ? w/2+8 : 8, y0=(q&2) ? h/2+8 : 8, x1=x0+w/2-16, y1=y0+h/2-16;
		struct fv *p=v+q*4;

		p[0]=(struct fv){x0, y0, u0, v0, 0, 255, 0, 255};
		p[1]=(struct fv){x1, y0, u1, v0, 0, 255, 0, 255};
		p[2]=(struct fv){x1, y1, u1, v1, 0, 255, 0, 255};
		p[3]=(struct fv){x0, y1, u0, v1, 0, 255, 0, 255};
	}
	glUseProgram(feat_prog);
	glUniform4f(feat_proj, -1, 1, 2.f/w, -2.f/h);
	glUniform1i(feat_pass, 1);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, feat_tex);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, feat_ebo);
	glBindBuffer(GL_ARRAY_BUFFER, feat_vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
	glVertexAttribPointer(0, 2, GL_SHORT, GL_FALSE, sizeof(struct fv), (void *)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct fv), (void *)4);
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(struct fv), (void *)12);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	for(int q=0; q<4; q++){
		glUniform1i(feat_mode, q);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)0);
		/* the next quad: the same indices on the next 4 vertices */
		glVertexAttribPointer(0, 2, GL_SHORT, GL_FALSE, sizeof(struct fv), (void *)((q+1)*4*sizeof(struct fv)));
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct fv), (void *)((q+1)*4*sizeof(struct fv)+4));
		glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(struct fv), (void *)((q+1)*4*sizeof(struct fv)+12));
	}
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static double now(void){
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec+t.tv_nsec/1e9;
}

int main(int argc, char **argv){
	double secs=argc>1 ? atof(argv[1]) : 10, t0, t;
	int feat=argc>2 && !strcmp(argv[2], "feat");
	struct wl_display *dpy=wl_display_connect(NULL);
	EGLint n=0, ctx_attr[]={EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLConfig cfg;
	/* top left corner of the window, in clip space */
	static const GLfloat tri[]={-1, 1, -0.6f, 1, -1, 0.6f};
	int frames=0;
	GLuint prog;
	GLint colour;

	if(!dpy){fprintf(stderr, "no wayland display\n"); return 1;}
	wl_registry_add_listener(wl_display_get_registry(dpy), &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if(!compositor || !wm){fprintf(stderr, "no compositor / xdg_wm_base\n"); return 1;}
	xdg_wm_base_add_listener(wm, &wm_listener, NULL);
	struct wl_surface *surface=wl_compositor_create_surface(compositor);
	struct xdg_surface *xs=xdg_wm_base_get_xdg_surface(wm, surface);
	struct xdg_toplevel *top=xdg_surface_get_toplevel(xs);
	xdg_surface_add_listener(xs, &surface_listener, NULL);
	xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
	xdg_toplevel_set_title(top, "vcwl");
	wl_surface_commit(surface);
	while(!configured && wl_display_dispatch(dpy)>=0){}

	EGLDisplay edpy=eglGetDisplay((EGLNativeDisplayType)dpy);
	if(!eglInitialize(edpy, 0, 0)){fprintf(stderr, "eglInitialize failed: %#x\n", eglGetError()); return 1;}
	eglChooseConfig(edpy, 0, &cfg, 1, &n);
	struct wl_egl_window *win=wl_egl_window_create(surface, width, height);
	EGLContext ctx=eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	EGLSurface es=eglCreateWindowSurface(edpy, cfg, (EGLNativeWindowType)win, 0);
	if(ctx==EGL_NO_CONTEXT || es==EGL_NO_SURFACE){fprintf(stderr, "no context / surface: %#x\n", eglGetError()); return 1;}
	eglMakeCurrent(edpy, es, es, ctx);
	printf("window %dx%d\n", width, height);

	prog=glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "pos");
	glLinkProgram(prog);
	glUseProgram(prog);
	colour=glGetUniformLocation(prog, "colour");
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glEnableVertexAttribArray(0);
	if(feat){feat_init();}

	t0=now();
	do{
		t=now()-t0;
		wl_display_dispatch_pending(dpy);
		if(win && (width!=0) && (height!=0)){wl_egl_window_resize(win, width, height, 0, 0);}
		glViewport(0, 0, width, height);
		glClearColor(0.5f+0.5f*sinf(t*3), 0.5f+0.5f*sinf(t*2+2), 0.4f, 1.0f);
		if(feat){glClearColor(0.15f, 0.15f, 0.15f, 1);}
		glClear(GL_COLOR_BUFFER_BIT);
		if(feat){
			feat_draw(width, height);
			glUseProgram(prog);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
		}
		glUniform4f(colour, 1, 1, 1, 1);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		if(!eglSwapBuffers(edpy, es)){fprintf(stderr, "swap failed: %#x\n", eglGetError()); break;}
		frames++;
	}while(t<secs && !closed);
	printf("%d frames in %.1f s = %.1f fps\n", frames, t, frames/t);

	eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(edpy, es);
	eglDestroyContext(edpy, ctx);
	wl_egl_window_destroy(win);
	xdg_toplevel_destroy(top);
	xdg_surface_destroy(xs);
	wl_surface_destroy(surface);
	wl_display_disconnect(dpy);
	return 0;
}
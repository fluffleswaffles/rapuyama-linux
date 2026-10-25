// SPDX-License-Identifier: GPL-2.0-only
/*
 * GLES 2 for the VideoCore III, ids and layouts from khclient_vc3.dll. not
 * done: read back (info logs, glReadPixels), mipmaps
 */
#include <GLES2/gl2.h>
#include <string.h>

#include "gl.h"
#include "khrn.h"

#define MAX_ATTRIBS 8
#define TMP_BUFFER 0x7fff0000	/* our VBOs for client arrays, + attrib, +8 = indices */
#define INLINE_MAX 900		/* uniform data in the message, the client sends more as bulk */

struct attrib{
	GLint size;
	GLenum type;
	GLboolean norm;
	GLsizei stride;
	const void *ptr;
	GLuint buffer;
	GLboolean enabled;
	float cur[4];
};

static struct{
	struct attrib a[MAX_ATTRIBS];
	GLuint array, element, names;
	GLint unpack;
	GLenum error;
}gl={.names=1, .unpack=4};

static uint32_t fbits(float f){
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

static void push(const uint32_t *w, uint32_t n){
	if(khrn_push(w, n)<0 && !gl.error){gl.error=GL_OUT_OF_MEMORY;}
}

static void call1(uint32_t id, uint32_t a){
	uint32_t w[2]={id, a};

	push(w, 2);
}

static void call2(uint32_t id, uint32_t a, uint32_t b){
	uint32_t w[3]={id, a, b};

	push(w, 3);
}

static void call4(uint32_t id, uint32_t a, uint32_t b, uint32_t c, uint32_t d){
	uint32_t w[5]={id, a, b, c, d};

	push(w, 5);
}

static uint32_t call_int(const uint32_t *w, uint32_t n){
	uint32_t r=0;

	if(khrn_call_int(w, n, &r) && !gl.error){gl.error=GL_OUT_OF_MEMORY;}
	return r;
}

/* "res" replies carry their length in front */
static int res_word(const uint32_t *w, uint32_t n, uint32_t *out){
	uint32_t r[8];
	int len=khrn_call(w, n, 0, 0, 0, r, sizeof(r));

	if(len<8){return -1;}
	*out=r[0]==(uint32_t)len-4 ? r[1] : r[0];
	return 0;
}

void glgl_reset(void){
	memset(gl.a, 0, sizeof(gl.a));
	gl.array=gl.element=0;
	for(int i=0; i<MAX_ATTRIBS; i++){gl.a[i].cur[3]=1.0f;}
}

GL_APICALL void GL_APIENTRY glActiveTexture(GLenum texture){call1(0x2052, texture);}
GL_APICALL void GL_APIENTRY glAttachShader(GLuint program, GLuint shader){call2(0x2037, program, shader);}

GL_APICALL void GL_APIENTRY glBindAttribLocation(GLuint program, GLuint index, const GLchar *name){
	uint32_t len=strlen(name)+1, w[4]={0x2038, program, index, len};

	khrn_call(w, 4, name, &len, 1, 0, 0);
}

GL_APICALL void GL_APIENTRY glBindBuffer(GLenum target, GLuint buffer){
	if(target==GL_ARRAY_BUFFER){gl.array=buffer;}
	else if(target==GL_ELEMENT_ARRAY_BUFFER){gl.element=buffer;}
	call2(0x2039, target, buffer);
}

GL_APICALL void GL_APIENTRY glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a){
	call4(0x2000, fbits(r), fbits(g), fbits(b), fbits(a));
}
GL_APICALL void GL_APIENTRY glBlendEquation(GLenum mode){call2(0x2001, mode, mode);}
GL_APICALL void GL_APIENTRY glBlendEquationSeparate(GLenum rgb, GLenum alpha){call2(0x2001, rgb, alpha);}
GL_APICALL void GL_APIENTRY glBlendFunc(GLenum s, GLenum d){call4(0x2002, s, d, s, d);}
GL_APICALL void GL_APIENTRY glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da){call4(0x2002, sr, dr, sa, da);}

/* khclient FUN_00014124: chunks of 128 kb, each {target, offset, size, size} + bulk */
GL_APICALL void GL_APIENTRY glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data){
	GLsizeiptr off=0;

	if(!data){return;}
	while(off<size){
		uint32_t n=size-off>0x20000 ? 0x20000 : size-off;
		uint32_t w[5]={0x203c, target, offset+off, n, n};

		khrn_call(w, 5, (const char *)data+off, &n, 1, 0, 0);
		off+=n;
	}
}

/* khclient FUN_00014020: {target, size, usage, -1}, the data as sub data */
GL_APICALL void GL_APIENTRY glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage){
	call4(0x203b, target, size, usage, 0xffffffff);
	glBufferSubData(target, 0, size, data);
}

GL_APICALL void GL_APIENTRY glClear(GLbitfield mask){call1(0x202c, mask);}
GL_APICALL void GL_APIENTRY glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a){
	call4(0x2003, fbits(r), fbits(g), fbits(b), fbits(a));
}
GL_APICALL void GL_APIENTRY glClearDepthf(GLfloat d){call1(0x2004, fbits(d));}
GL_APICALL void GL_APIENTRY glClearStencil(GLint s){call1(0x2005, s);}
GL_APICALL void GL_APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a){call4(0x2006, r, g, b, a);}
GL_APICALL void GL_APIENTRY glCompileShader(GLuint shader){call1(0x2057, shader);}

GL_APICALL GLuint GL_APIENTRY glCreateProgram(void){
	uint32_t w[1]={0x201c};

	return call_int(w, 1);
}

GL_APICALL GLuint GL_APIENTRY glCreateShader(GLenum type){
	uint32_t w[2]={0x201d, type};

	return call_int(w, 2);
}

GL_APICALL void GL_APIENTRY glCullFace(GLenum mode){call1(0x2007, mode);}
GL_APICALL void GL_APIENTRY glDeleteProgram(GLuint program){call1(0x201e, program);}
GL_APICALL void GL_APIENTRY glDeleteShader(GLuint shader){call1(0x201f, shader);}
/* khclient 0x151xx / 0x1523c, vc-main 0x66e54 / 0x66ef0: count, bytes, bulk */
static void delete_names(uint32_t id, GLsizei n, const GLuint *names){
	if(n<0){gl.error=GL_INVALID_VALUE; return;}
	while(n>0){
		uint32_t count=n>0x8000 ? 0x8000 : (uint32_t)n, bytes=count*4;
		uint32_t w[3]={id, count, bytes};
		if(khrn_call(w, 3, names, &bytes, 1, NULL, 0)<0){gl.error=GL_OUT_OF_MEMORY; return;}
		n-=count; names+=count;
	}
}
GL_APICALL void GL_APIENTRY glDeleteTextures(GLsizei n, const GLuint *textures){delete_names(0x203e, n, textures);}
GL_APICALL void GL_APIENTRY glDeleteBuffers(GLsizei n, const GLuint *buffers){
	if(n<0){gl.error=GL_INVALID_VALUE; return;}
	for(GLsizei j=0; j<n; j++){
		if(gl.array==buffers[j]){gl.array=0;}
		if(gl.element==buffers[j]){gl.element=0;}
		for(int i=0; i<MAX_ATTRIBS; i++){if(gl.a[i].buffer==buffers[j]){gl.a[i].buffer=0;}}
	}
	delete_names(0x203d, n, buffers);
}
GL_APICALL void GL_APIENTRY glDepthFunc(GLenum func){call1(0x2008, func);}
GL_APICALL void GL_APIENTRY glDepthMask(GLboolean flag){call1(0x2009, flag);}
GL_APICALL void GL_APIENTRY glDepthRangef(GLfloat n, GLfloat f){call2(0x200a, fbits(n), fbits(f));}
GL_APICALL void GL_APIENTRY glDetachShader(GLuint program, GLuint shader){call2(0x2020, program, shader);}
GL_APICALL void GL_APIENTRY glDisable(GLenum cap){call1(0x200b, cap);}
GL_APICALL void GL_APIENTRY glEnable(GLenum cap){call1(0x200c, cap);}

GL_APICALL void GL_APIENTRY glDisableVertexAttribArray(GLuint index){
	if(index<MAX_ATTRIBS){gl.a[index].enabled=GL_FALSE;}
}
GL_APICALL void GL_APIENTRY glEnableVertexAttribArray(GLuint index){
	if(index<MAX_ATTRIBS){gl.a[index].enabled=GL_TRUE;}
}

static uint32_t type_size(GLenum type){
	switch(type){
	case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
	case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
	default: return 4;
	}
}

/* the attrib block of a draw: 8 enables, then per attrib {size, type,
 * normalized, stride, offset, buffer} or its current value */
static uint32_t attribs(uint32_t *w, uint32_t max_index){
	uint32_t i, n=8;

	for(i=0; i<MAX_ATTRIBS; i++){w[i]=gl.a[i].enabled;}
	for(i=0; i<MAX_ATTRIBS; i++){
		struct attrib *a=&gl.a[i];

		if(!a->enabled){
			memcpy(w+n, a->cur, 16);
			n+=4;
			continue;
		}
		uint32_t buffer=a->buffer, off=(uintptr_t)a->ptr;

		if(!buffer){
			uint32_t elem=a->size*type_size(a->type);
			uint32_t stride=a->stride ? a->stride : elem;

			buffer=TMP_BUFFER+i;
			call2(0x2039, GL_ARRAY_BUFFER, buffer);
			glBufferData(GL_ARRAY_BUFFER, max_index*stride+elem, a->ptr, GL_STREAM_DRAW);
			call1(0x2078, i);
			off=0;
		}
		w[n++]=a->size;
		w[n++]=a->type;
		w[n++]=a->norm;
		w[n++]=a->stride;
		w[n++]=off;
		w[n++]=buffer;
	}
	call2(0x2039, GL_ARRAY_BUFFER, gl.array);
	return n;
}

GL_APICALL void GL_APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count){
	uint32_t w[4+8+6*MAX_ATTRIBS]={0x2076, mode, first, count};

	if(count<=0){return;}
	push(w, 4+attribs(w+4, first+count-1));
}

GL_APICALL void GL_APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices){
	uint32_t w[6+8+6*MAX_ATTRIBS]={0x2074, mode, count, type, (uintptr_t)indices, gl.element};
	uint32_t max=0, i, size=type_size(type);

	if(count<=0){return;}
	if(!gl.element){
		/* client indices: their max sizes the client arrays, then into our EBO */
		for(i=0; i<(uint32_t)count; i++){
			uint32_t v=size==1 ? ((const uint8_t *)indices)[i] : ((const uint16_t *)indices)[i];

			if(v>max){max=v;}
		}
		call2(0x2039, GL_ELEMENT_ARRAY_BUFFER, TMP_BUFFER+MAX_ATTRIBS);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, count*size, indices, GL_STREAM_DRAW);
		w[4]=0;
		w[5]=TMP_BUFFER+MAX_ATTRIBS;
	}else{
		/* shortcut: client arrays with an index buffer need the VC III findmax rpc, not done: refused */
		for(i=0; i<MAX_ATTRIBS; i++){
			if(gl.a[i].enabled && !gl.a[i].buffer){gl.error=GL_INVALID_OPERATION; return;}
		}
	}
	push(w, 6+attribs(w+6, max));
	if(!gl.element){call2(0x2039, GL_ELEMENT_ARRAY_BUFFER, 0);}
}

GL_APICALL void GL_APIENTRY glFinish(void){
	uint32_t w[3]={KHRN_EGL_FLUSH_WAIT, 1, 0}, r;

	khrn_call_int(w, 3, &r);
}
GL_APICALL void GL_APIENTRY glFlush(void){khrn_flush();}
GL_APICALL void GL_APIENTRY glFrontFace(GLenum mode){call1(0x200d, mode);}

/* names are ours: in GLES 2 binding an unused name creates the object */
GL_APICALL void GL_APIENTRY glGenBuffers(GLsizei n, GLuint *buffers){
	for(GLsizei i=0; i<n; i++){buffers[i]=gl.names++;}
}
GL_APICALL void GL_APIENTRY glGenTextures(GLsizei n, GLuint *textures){glGenBuffers(n, textures);}

static GLint location(uint32_t id, GLuint program, const GLchar *name){
	uint32_t len=strlen(name)+1, w[3]={id, program, len}, r[4];
	int ret=khrn_call(w, 3, name, &len, 1, r, sizeof(r));

	return ret>=4 ? (GLint)r[0] : -1;
}
GL_APICALL GLint GL_APIENTRY glGetAttribLocation(GLuint program, const GLchar *name){return location(0x203f, program, name);}
GL_APICALL GLint GL_APIENTRY glGetUniformLocation(GLuint program, const GLchar *name){return location(0x202a, program, name);}

GL_APICALL GLenum GL_APIENTRY glGetError(void){
	GLenum e=gl.error;

	gl.error=GL_NO_ERROR;
	return e;
}

GL_APICALL void GL_APIENTRY glGetIntegerv(GLenum pname, GLint *data){
	*data=pname==GL_MAX_VERTEX_ATTRIBS ? MAX_ATTRIBS : 0;
}

GL_APICALL void GL_APIENTRY glGetProgramiv(GLuint program, GLenum pname, GLint *params){
	uint32_t w[3]={0x2027, program, pname}, v=0;

	if(pname==GL_INFO_LOG_LENGTH){*params=0; return;}
	res_word(w, 3, &v);
	*params=v;
}

GL_APICALL void GL_APIENTRY glGetShaderiv(GLuint shader, GLenum pname, GLint *params){
	uint32_t w[3]={0x2058, shader, pname}, v=0;

	if(pname==GL_INFO_LOG_LENGTH){*params=0; return;}
	res_word(w, 3, &v);
	*params=v;
}

/* shortcut: the VC sends logs as a bulk to the host, which the driver can't take yet */
GL_APICALL void GL_APIENTRY glGetProgramInfoLog(GLuint program, GLsizei max, GLsizei *len, GLchar *log){
	(void)program;
	if(len){*len=0;}
	if(max>0){log[0]=0;}
}
GL_APICALL void GL_APIENTRY glGetShaderInfoLog(GLuint shader, GLsizei max, GLsizei *len, GLchar *log){
	glGetProgramInfoLog(shader, max, len, log);
}

GL_APICALL const GLubyte *GL_APIENTRY glGetString(GLenum name){
	switch(name){
	case GL_VENDOR: return (const GLubyte *)"Broadcom";
	case GL_RENDERER: return (const GLubyte *)"VideoCore III (E6, vc-khrn)";
	case GL_VERSION: return (const GLubyte *)"OpenGL ES 2.0";
	case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
	case GL_EXTENSIONS: return (const GLubyte *)"";
	}
	return 0;
}

GL_APICALL void GL_APIENTRY glLineWidth(GLfloat width){call1(0x200e, fbits(width));}

/* answers when the link is done (khclient FUN_000190d8) */
GL_APICALL void GL_APIENTRY glLinkProgram(GLuint program){
	uint32_t w[2]={0x2043, program}, r[4];

	khrn_call(w, 2, 0, 0, 0, r, sizeof(r));
}

GL_APICALL void GL_APIENTRY glPolygonOffset(GLfloat factor, GLfloat units){call2(0x2010, fbits(factor), fbits(units));}
GL_APICALL void GL_APIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h){call4(0x2015, x, y, w, h);}

/*
 * khclient FUN_0001b630, vc-main 0x667fe: {shader, count, workspace, has
 * lengths = 0}, the lengths in the message, then one bulk per string
 */
GL_APICALL void GL_APIENTRY glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length){
	uint32_t w[5+32], sizes[32], total, i;
	char buf[16384];
	uint32_t off=0;

	if(count<=0 || count>32){gl.error=GL_INVALID_VALUE; return;}
	total=count*8;
	for(i=0; i<(uint32_t)count; i++){
		uint32_t len=length && length[i]>=0 ? (uint32_t)length[i] : string[i] ? strlen(string[i])+1 : 1;

		if(off+len>sizeof(buf)){gl.error=GL_OUT_OF_MEMORY; return;}
		memcpy(buf+off, string[i] ? string[i] : "", len);
		off+=len;
		sizes[i]=len;
		w[5+i]=len;
		total+=len;
	}
	w[0]=0x2064;
	w[1]=shader;
	w[2]=count;
	w[3]=total;
	w[4]=0;
	khrn_call(w, 5+count, buf, sizes, count, 0, 0);
}

GL_APICALL void GL_APIENTRY glUniform1f(GLint l, GLfloat x){call2(0x2033, l, fbits(x));}
GL_APICALL void GL_APIENTRY glUniform2f(GLint l, GLfloat x, GLfloat y){
	uint32_t w[4]={0x2034, l, fbits(x), fbits(y)};

	push(w, 4);
}
GL_APICALL void GL_APIENTRY glUniform3f(GLint l, GLfloat x, GLfloat y, GLfloat z){call4(0x2035, l, fbits(x), fbits(y), fbits(z));}
GL_APICALL void GL_APIENTRY glUniform4f(GLint l, GLfloat x, GLfloat y, GLfloat z, GLfloat v){
	uint32_t w[6]={0x2036, l, fbits(x), fbits(y), fbits(z), fbits(v)};

	push(w, 6);
}
GL_APICALL void GL_APIENTRY glUniform1i(GLint l, GLint x){call2(0x202f, l, x);}
GL_APICALL void GL_APIENTRY glUniform2i(GLint l, GLint x, GLint y){
	uint32_t w[4]={0x2030, l, x, y};

	push(w, 4);
}
GL_APICALL void GL_APIENTRY glUniform3i(GLint l, GLint x, GLint y, GLint z){call4(0x2031, l, x, y, z);}
GL_APICALL void GL_APIENTRY glUniform4i(GLint l, GLint x, GLint y, GLint z, GLint v){
	uint32_t w[6]={0x2032, l, x, y, z, v};

	push(w, 6);
}

/* {location, count, bytes} + the data in the message (khclient FUN_0001ae9c) */
static void uniformv(uint32_t id, GLint l, GLsizei count, const void *v, uint32_t elem){
	uint32_t w[4+INLINE_MAX/4], bytes=count*elem;

	if(bytes>INLINE_MAX){gl.error=GL_INVALID_VALUE; return;}
	w[0]=id;
	w[1]=l;
	w[2]=count;
	w[3]=bytes;
	memcpy(w+4, v, bytes);
	push(w, 4+(bytes+3)/4);
}
GL_APICALL void GL_APIENTRY glUniform1iv(GLint l, GLsizei c, const GLint *v){uniformv(0x2046, l, c, v, 4);}
GL_APICALL void GL_APIENTRY glUniform2iv(GLint l, GLsizei c, const GLint *v){uniformv(0x2047, l, c, v, 8);}
GL_APICALL void GL_APIENTRY glUniform3iv(GLint l, GLsizei c, const GLint *v){uniformv(0x2048, l, c, v, 12);}
GL_APICALL void GL_APIENTRY glUniform4iv(GLint l, GLsizei c, const GLint *v){uniformv(0x2049, l, c, v, 16);}
GL_APICALL void GL_APIENTRY glUniform1fv(GLint l, GLsizei c, const GLfloat *v){uniformv(0x204a, l, c, v, 4);}
GL_APICALL void GL_APIENTRY glUniform2fv(GLint l, GLsizei c, const GLfloat *v){uniformv(0x204b, l, c, v, 8);}
GL_APICALL void GL_APIENTRY glUniform3fv(GLint l, GLsizei c, const GLfloat *v){uniformv(0x204c, l, c, v, 12);}
GL_APICALL void GL_APIENTRY glUniform4fv(GLint l, GLsizei c, const GLfloat *v){uniformv(0x204d, l, c, v, 16);}

/* {location, count, transpose, bytes} + data (khclient FUN_0001af98, vc-main 0x67c58) */
static void matrix(uint32_t id, GLint l, GLsizei count, GLboolean t, const GLfloat *v, uint32_t elem){
	uint32_t w[5+INLINE_MAX/4], bytes=count*elem;

	if(bytes>INLINE_MAX){gl.error=GL_INVALID_VALUE; return;}
	w[0]=id;
	w[1]=l;
	w[2]=count;
	w[3]=t;
	w[4]=bytes;
	memcpy(w+5, v, bytes);
	push(w, 5+(bytes+3)/4);
}
GL_APICALL void GL_APIENTRY glUniformMatrix2fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v){matrix(0x204e, l, c, t, v, 16);}
GL_APICALL void GL_APIENTRY glUniformMatrix3fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v){matrix(0x204f, l, c, t, v, 36);}
GL_APICALL void GL_APIENTRY glUniformMatrix4fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v){matrix(0x2050, l, c, t, v, 64);}

GL_APICALL void GL_APIENTRY glUseProgram(GLuint program){call1(0x2016, program);}
GL_APICALL void GL_APIENTRY glValidateProgram(GLuint program){call1(0x2051, program);}

GL_APICALL void GL_APIENTRY glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w){
	if(i>=MAX_ATTRIBS){return;}
	gl.a[i].cur[0]=x;
	gl.a[i].cur[1]=y;
	gl.a[i].cur[2]=z;
	gl.a[i].cur[3]=w;
}
GL_APICALL void GL_APIENTRY glVertexAttrib1f(GLuint i, GLfloat x){glVertexAttrib4f(i, x, 0, 0, 1);}
GL_APICALL void GL_APIENTRY glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y){glVertexAttrib4f(i, x, y, 0, 1);}
GL_APICALL void GL_APIENTRY glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z){glVertexAttrib4f(i, x, y, z, 1);}
GL_APICALL void GL_APIENTRY glVertexAttrib4fv(GLuint i, const GLfloat *v){glVertexAttrib4f(i, v[0], v[1], v[2], v[3]);}

GL_APICALL void GL_APIENTRY glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr){
	if(i>=MAX_ATTRIBS){gl.error=GL_INVALID_VALUE; return;}
	gl.a[i].size=size;
	gl.a[i].type=type;
	gl.a[i].norm=norm;
	gl.a[i].stride=stride;
	gl.a[i].ptr=ptr;
	gl.a[i].buffer=gl.array;
	/* the VC binds the current GL_ARRAY_BUFFER here, draws must match it */
	call1(0x2078, i);
}

GL_APICALL void GL_APIENTRY glViewport(GLint x, GLint y, GLsizei w, GLsizei h){call4(0x2017, x, y, w, h);}

/* client side, the texture rpcs carry the unpack alignment */
GL_APICALL void GL_APIENTRY glPixelStorei(GLenum pname, GLint param){
	if(pname==GL_UNPACK_ALIGNMENT && (param==1 || param==2 || param==4 || param==8)){gl.unpack=param;}
}

GL_APICALL void GL_APIENTRY glBindTexture(GLenum target, GLuint texture){call2(0x203a, target, texture);}
GL_APICALL void GL_APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param){
	uint32_t w[4]={0x202d, target, pname, param};

	push(w, 4);
}
GL_APICALL void GL_APIENTRY glTexParameterf(GLenum target, GLenum pname, GLfloat param){
	uint32_t w[4]={0x202e, target, pname, fbits(param)};

	push(w, 4);
}

/* bytes of one row as the VC reads it (khclient FUN_00019a90) */
static uint32_t pitch(GLsizei width, GLenum format, GLenum type){
	uint32_t bpp;

	if(type==GL_UNSIGNED_SHORT_5_6_5 || type==GL_UNSIGNED_SHORT_4_4_4_4 || type==GL_UNSIGNED_SHORT_5_5_5_1){bpp=2;}
	else if(format==GL_RGBA){bpp=4;}
	else if(format==GL_RGB){bpp=3;}
	else if(format==GL_LUMINANCE_ALPHA){bpp=2;}
	else{bpp=1;}
	return (width*bpp+gl.unpack-1)&~(uint32_t)(gl.unpack-1);
}

/*
 * khclient FUN_0001a758: rows in pieces of up to 128 kb, each {target,
 * level, x, y, width, rows, format, type, alignment, bytes} + bulk
 */
GL_APICALL void GL_APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
					    GLenum format, GLenum type, const void *pixels){
	uint32_t p=pitch(width, format, type), step, row;

	if(!pixels || !p || width<=0 || height<=0){return;}
	step=0x20000/p;
	if(!step){gl.error=GL_INVALID_VALUE; return;}
	for(row=0; row<(uint32_t)height; row+=step){
		uint32_t n=height-row<step ? height-row : step, bytes=n*p;
		uint32_t w[11]={0x2073, target, level, x, y+row, width, n, format, type, gl.unpack, bytes};

		if(khrn_call(w, 11, (const char *)pixels+row*p, &bytes, 1, 0, 0)<0){gl.error=GL_OUT_OF_MEMORY; return;}
	}
}

/* khclient FUN_0001a3f0: the image without data, the data as sub images */
GL_APICALL void GL_APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
					 GLint border, GLenum format, GLenum type, const void *pixels){
	uint32_t w[11]={0x2072, target, level, internalformat, width, height, border, format, type, gl.unpack, 0xffffffff};

	push(w, 11);
	glTexSubImage2D(target, level, 0, 0, width, height, format, type, pixels);
}
GL_APICALL void GL_APIENTRY glHint(GLenum target, GLenum mode){(void)target; (void)mode;}
// SPDX-License-Identifier: GPL-2.0-only
/*
 * GLES 2 for the VideoCore III, ids and layouts from khclient_vc3.dll. not
 * done: read back (info logs, glReadPixels), mipmaps
 */
#include <GLES2/gl2.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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
	/* what blending does to rgb, for draws that change nothing */
	GLboolean blend, mask;
	GLenum eq, sr, dr;
}gl={.names=1, .unpack=4, .mask=GL_TRUE, .eq=GL_FUNC_ADD, .sr=GL_ONE, .dr=GL_ZERO};

static uint32_t fbits(float f){
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

/* VCGL_CHECK=1: ask the VC for its GL error (GL11 glGetError 0x106d, it reads the
 * current context) after calls that can fail there, one round trip each */
static int checking=-1;

static void check(const char *what){
	uint32_t w[1]={0x106d}, e=0;

	if(checking<0){checking=getenv("VCGL_CHECK") ? atoi(getenv("VCGL_CHECK")) : 0;}
	if(!checking || khrn_call_int(w, 1, &e) || !e){return;}
	fprintf(stderr, "vcgl: %s: VC GL error %#x\n", what, e);
}

static int uniform_hook(const uint32_t *w, uint32_t n);
static struct program *in_use;	/* with uniform int variants, see glLinkProgram */

static void send(const uint32_t *w, uint32_t n){
	if(khrn_push(w, n)<0 && !gl.error){gl.error=GL_OUT_OF_MEMORY;}
	/* VCGL_CHECK=2: after every call */
	if(checking>1){
		char id[64];

		snprintf(id, sizeof(id), "call %#x %#x %#x %#x %#x", (unsigned)w[0], n>1 ? (unsigned)w[1] : 0,
			 n>2 ? (unsigned)w[2] : 0, n>3 ? (unsigned)w[3] : 0, n>4 ? (unsigned)w[4] : 0);
		check(id);
	}
}

static void push(const uint32_t *w, uint32_t n){
	if(!uniform_hook(w, n)){send(w, n);}
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

/* vertex array objects (GLES 3, OES_vertex_array_object), which the VC III
 * does not have: the attrib state and the element buffer, swapped client side */
#define MAX_VAOS 64

struct vao{
	int used;
	struct attrib a[MAX_ATTRIBS];
	GLuint element;
};

static struct vao vaos[MAX_VAOS+1];	/* [0] = the default one */
static GLuint vao_bound;

static void attribs_reset(struct attrib *a){
	memset(a, 0, sizeof(struct attrib)*MAX_ATTRIBS);
	for(int i=0; i<MAX_ATTRIBS; i++){a[i].cur[3]=1.0f;}
}

void glgl_reset(void){
	in_use=NULL;
	attribs_reset(gl.a);
	gl.array=gl.element=0;
	gl.blend=GL_FALSE;
	gl.mask=GL_TRUE;
	gl.eq=GL_FUNC_ADD;
	gl.sr=GL_ONE;
	gl.dr=GL_ZERO;
	memset(vaos, 0, sizeof(vaos));
	vao_bound=0;
}

void glGenVertexArrays(GLsizei n, GLuint *arrays){
	for(GLsizei k=0; k<n; k++){
		GLuint i;

		for(i=1; i<=MAX_VAOS && vaos[i].used; i++){}
		if(i>MAX_VAOS){arrays[k]=0; gl.error=GL_OUT_OF_MEMORY; continue;}
		vaos[i].used=1;
		attribs_reset(vaos[i].a);
		vaos[i].element=0;
		arrays[k]=i;
	}
}

void glBindVertexArray(GLuint array){
	struct vao *from=&vaos[vao_bound], *to;

	if(array>MAX_VAOS || (array && !vaos[array].used)){gl.error=GL_INVALID_OPERATION; return;}
	if(array==vao_bound){return;}
	to=&vaos[array];
	memcpy(from->a, gl.a, sizeof(gl.a));
	from->element=gl.element;
	memcpy(gl.a, to->a, sizeof(gl.a));
	gl.element=to->element;
	vao_bound=array;
	call2(0x2039, GL_ELEMENT_ARRAY_BUFFER, gl.element);
	/* the VC keeps the buffer each attrib got from 0x2078 */
	for(int i=0; i<MAX_ATTRIBS; i++){
		if(gl.a[i].buffer){
			call2(0x2039, GL_ARRAY_BUFFER, gl.a[i].buffer);
			call1(0x2078, i);
		}
	}
	call2(0x2039, GL_ARRAY_BUFFER, gl.array);
}

void glDeleteVertexArrays(GLsizei n, const GLuint *arrays){
	for(GLsizei k=0; k<n; k++){
		if(!arrays[k] || arrays[k]>MAX_VAOS){continue;}
		if(arrays[k]==vao_bound){glBindVertexArray(0);}
		vaos[arrays[k]].used=0;
	}
}

GLboolean glIsVertexArray(GLuint array){return array && array<=MAX_VAOS && vaos[array].used;}

void glGenVertexArraysOES(GLsizei n, GLuint *arrays){glGenVertexArrays(n, arrays);}
void glBindVertexArrayOES(GLuint array){glBindVertexArray(array);}
void glDeleteVertexArraysOES(GLsizei n, const GLuint *arrays){glDeleteVertexArrays(n, arrays);}
GLboolean glIsVertexArrayOES(GLuint array){return glIsVertexArray(array);}

GL_APICALL void GL_APIENTRY glActiveTexture(GLenum texture){call1(0x2052, texture);}
/*
 * attribute locations: the VC numbers them from the last declared one down,
 * Mesa from the first one up, and clients like alacritty count on that
 * without asking. a program that binds none itself gets its vertex shader's
 * attributes bound in declaration order before the link.
 *
 * uniform int variants: the VC III runs every branch of a shader (a pass
 * switch on a uniform int made alacritty's background pass 5x slower than a
 * shader with that pass alone). a program with one uniform int gets a
 * variant per value it is set to, the uniform turned into a constant, so the
 * VC compiler drops the other branches. glUniform1i on it picks the
 * variant, the other uniforms are kept here and replayed into it.
 * VCGL_NOSPEC=1: off
 */
#define MAX_TRACK 64
#define MAX_PROGRAMS 32
#define MAX_VARIANTS 4
#define MAX_UNIFORMS 16

static struct shader{
	GLuint id;
	GLenum type;
	char *src;	/* the strings as given, one after the other */
	uint32_t len;
	int n;
	char name[MAX_ATTRIBS][64];
}shaders[MAX_TRACK];

static struct program{
	GLuint id, vs, fs;
	int bound, nbinds;
	struct{
		GLuint index;
		char name[64];
	}binds[MAX_ATTRIBS];
	char spec[64];
	struct shader src[2];	/* copies: apps delete their shaders after the link */
	GLint spec_loc, spec_val;
	int nvar, cur;	/* cur: the variant in use, -1 the program itself */
	struct{
		GLint val;
		GLuint id;
		GLint loc[MAX_UNIFORMS];	/* -2 not asked yet */
	}var[MAX_VARIANTS];
	int nu;
	struct{
		GLint loc;
		char name[64];
		uint32_t *w, n;	/* the last call, as the app sent it */
	}u[MAX_UNIFORMS];
}programs[MAX_PROGRAMS];

static struct shader *shader_find(GLuint id){
	for(int i=0; i<MAX_TRACK; i++){
		if(id && shaders[i].id==id){return &shaders[i];}
	}
	return NULL;
}

static struct program *program(GLuint id){
	for(int i=0; i<MAX_PROGRAMS; i++){
		if(id && programs[i].id==id){return &programs[i];}
	}
	return NULL;
}

static int ident(char c){return isalnum((unsigned char)c) || c=='_';}

/* the next identifier from i on, comments, blanks and NULs skipped */
static uint32_t word(const char *s, uint32_t len, uint32_t i, uint32_t *start){
	while(i<len){
		if(s[i]=='/' && i+1<len && s[i+1]=='/'){while(i<len && s[i]!='\n'){i++;}}
		else if(s[i]=='/' && i+1<len && s[i+1]=='*'){
			for(i+=2; i+1<len && !(s[i]=='*' && s[i+1]=='/'); i++){}
			i+=2;
		}else if(ident(s[i])){break;}
		else{i++;}
	}
	*start=i;
	while(i<len && ident(s[i])){i++;}
	return i;
}

static int is(const char *s, uint32_t a, uint32_t b, const char *w){return b-a==strlen(w) && !memcmp(s+a, w, b-a);}
static int precision(const char *s, uint32_t a, uint32_t b){return is(s, a, b, "lowp") || is(s, a, b, "mediump") || is(s, a, b, "highp");}

/* "attribute [precision] type name", in source order */
static void parse_attribs(struct shader *v){
	const char *s=v->src;
	uint32_t i=0, a, b, len=v->len;

	v->n=0;
	while(v->n<MAX_ATTRIBS && (i=word(s, len, i, &a))>a){
		if(!is(s, a, i, "attribute")){continue;}
		i=word(s, len, i, &a);
		if(precision(s, a, i)){i=word(s, len, i, &a);}
		i=word(s, len, i, &b);
		if(i==b || i-b>=sizeof(v->name[0])){continue;}
		memcpy(v->name[v->n], s+b, i-b);
		v->name[v->n++][i-b]=0;
	}
}

/*
 * "uniform [precision] int name;": how many, the first one's name, and with
 * out the source with that declaration as "const [precision] int name=val;"
 * (or as it is when it has none)
 */
static int int_uniform(const struct shader *sh, char name[64], GLint val, char **out){
	const char *s=sh->src;
	uint32_t i=0, a, b, e, start, pa=0, pb=0, len=sh->len;
	int count=0;

	while((i=word(s, len, i, &a))>a){
		if(!is(s, a, i, "uniform")){continue;}
		start=a;
		i=word(s, len, i, &a);
		pa=pb=0;
		if(precision(s, a, i)){pa=a; pb=i; i=word(s, len, i, &a);}
		if(!is(s, a, i, "int")){continue;}
		e=i=word(s, len, i, &b);
		while(i<len && (s[i]==' ' || s[i]=='\t' || s[i]=='\n')){i++;}
		if(e==b || e-b>=64 || i>=len || s[i]!=';'){continue;}
		if(!out){
			if(!count++){memcpy(name, s+b, e-b); name[e-b]=0;}
			continue;
		}
		if(!is(s, b, e, name)){continue;}
		*out=malloc(len+128);
		if(!*out){return -1;}
		memcpy(*out, s, start);
		b=start+sprintf(*out+start, "const %.*s%sint %s=%d;", (int)(pb-pa), s+pa, pb>pa ? " " : "", name, (int)val);
		memcpy(*out+b, s+i+1, len-i-1);
		(*out)[b+len-i-1]=0;
		return 1;
	}
	if(out){
		*out=malloc(len+1);
		if(!*out){return -1;}
		memcpy(*out, s, len);
		(*out)[len]=0;
	}
	return count;
}

GL_APICALL void GL_APIENTRY glAttachShader(GLuint prog, GLuint shader){
	struct program *p=program(prog);
	struct shader *sh=shader_find(shader);

	if(p && sh && sh->type==GL_VERTEX_SHADER){p->vs=shader;}
	if(p && sh && sh->type==GL_FRAGMENT_SHADER){p->fs=shader;}
	call2(0x2037, prog, shader);
}

static void bind_attrib(GLuint prog, GLuint index, const GLchar *name){
	uint32_t len=strlen(name)+1, w[4]={0x2038, prog, index, len};

	khrn_call(w, 4, name, &len, 1, 0, 0);
}

GL_APICALL void GL_APIENTRY glBindAttribLocation(GLuint prog, GLuint index, const GLchar *name){
	struct program *p=program(prog);

	if(p){
		p->bound=1;
		if(p->nbinds<MAX_ATTRIBS && strlen(name)<64){
			p->binds[p->nbinds].index=index;
			strcpy(p->binds[p->nbinds++].name, name);
		}
	}
	bind_attrib(prog, index, name);
}

static GLint location(uint32_t id, GLuint program, const GLchar *name);

/* a new program from p's shaders with the uniform int = val, 0 if that fails */
static GLuint variant(struct program *p, GLint val){
	char *src[2]={NULL, NULL};
	GLuint id=0, sh[2];
	GLint ok=0;

	if(int_uniform(&p->src[0], p->spec, val, &src[0])<0 || int_uniform(&p->src[1], p->spec, val, &src[1])<0){goto out;}
	id=glCreateProgram();
	for(int i=0; i<2; i++){
		sh[i]=glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
		glShaderSource(sh[i], 1, (const GLchar *const *)&src[i], NULL);
		glCompileShader(sh[i]);
		glAttachShader(id, sh[i]);
	}
	for(int i=0; i<p->nbinds; i++){glBindAttribLocation(id, p->binds[i].index, p->binds[i].name);}
	glLinkProgram(id);
	glGetProgramiv(id, GL_LINK_STATUS, &ok);
	if(!ok){glDeleteProgram(id); id=0;}
	for(int i=0; i<2; i++){glDeleteShader(sh[i]);}
	if(getenv("VCGL_PROFILE")){fprintf(stderr, "vcgl: program %u %s=%d: variant %u\n", p->id, p->spec, (int)val, id);}
out:
	free(src[0]);
	free(src[1]);
	return id;
}

/* the uniform calls p got, into the program in use (variant v or p itself) */
static void replay(struct program *p, int v, int only){
	for(int i=only<0 ? 0 : only; i<p->nu && (only<0 || i==only); i++){
		uint32_t w[5+INLINE_MAX/4];
		GLint *l=v>=0 ? &p->var[v].loc[i] : NULL;

		if(!p->u[i].w){continue;}
		if(l && *l==-2){*l=p->u[i].name[0] ? location(0x202a, p->var[v].id, p->u[i].name) : -1;}
		if(l && *l<0){continue;}
		memcpy(w, p->u[i].w, p->u[i].n*4);
		w[1]=l ? (uint32_t)*l : (uint32_t)p->u[i].loc;
		send(w, p->u[i].n);
	}
}

static void use_variant(struct program *p){
	int v;

	for(v=0; v<p->nvar && p->var[v].val!=p->spec_val; v++){}
	if(v==p->nvar && v<MAX_VARIANTS){
		GLuint id=variant(p, p->spec_val);

		if(id){
			p->var[v].val=p->spec_val;
			p->var[v].id=id;
			for(int i=0; i<MAX_UNIFORMS; i++){p->var[v].loc[i]=-2;}
			p->nvar++;
		}
	}
	if(v<p->nvar){
		uint32_t w[2]={0x2016, p->var[v].id};

		send(w, 2);
	}else{
		/* no variant: the program itself, with the uniform */
		uint32_t w[2]={0x2016, p->id}, u[3]={0x202f, (uint32_t)p->spec_loc, (uint32_t)p->spec_val};

		v=-1;
		send(w, 2);
		send(u, 3);
	}
	p->cur=v;
	replay(p, v, -1);
}

static int is_uniform(uint32_t id){return (id>=0x202f && id<=0x2036) || (id>=0x2046 && id<=0x2050);}

static int uniform_slot(struct program *p, GLint loc){
	int i;

	for(i=0; i<p->nu && p->u[i].loc!=loc; i++){}
	if(i==p->nu){
		if(p->nu==MAX_UNIFORMS){return -1;}
		p->u[p->nu].loc=loc;
		p->u[p->nu].name[0]=0;
		p->u[p->nu++].w=NULL;
	}
	return i;
}

static int uniform_hook(const uint32_t *w, uint32_t n){
	struct program *p=in_use;
	GLint loc;
	int i;
	uint32_t *copy;

	if(!p || !is_uniform(w[0])){return 0;}
	loc=(GLint)w[1];
	if(loc<0){return 1;}
	if(loc==p->spec_loc){
		if(w[0]==0x202f && (GLint)w[2]!=p->spec_val){
			p->spec_val=w[2];
			use_variant(p);
		}
		return 1;
	}
	i=uniform_slot(p, loc);
	copy=i<0 ? NULL : realloc(p->u[i].w, n*4);
	if(!copy){return 0;}
	memcpy(copy, w, n*4);
	p->u[i].w=copy;
	p->u[i].n=n;
	replay(p, p->cur, i);
	return 1;
}

GL_APICALL void GL_APIENTRY glBindBuffer(GLenum target, GLuint buffer){
	if(target==GL_ARRAY_BUFFER){gl.array=buffer;}
	else if(target==GL_ELEMENT_ARRAY_BUFFER){gl.element=buffer;}
	call2(0x2039, target, buffer);
}

GL_APICALL void GL_APIENTRY glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a){
	call4(0x2000, fbits(r), fbits(g), fbits(b), fbits(a));
}
GL_APICALL void GL_APIENTRY glBlendEquation(GLenum mode){gl.eq=mode; call2(0x2001, mode, mode);}
GL_APICALL void GL_APIENTRY glBlendEquationSeparate(GLenum rgb, GLenum alpha){gl.eq=rgb; call2(0x2001, rgb, alpha);}
/* dual source factors (GL_SRC1_*) do not exist in GLES 2, the VC flags them
 * GL_INVALID_ENUM: alacritty resets to them after its rects, ignored */
static int src1(GLenum f){return f==0x88f9 || f==0x88fa || f==0x8589 || f==0x88fb;}
GL_APICALL void GL_APIENTRY glBlendFunc(GLenum s, GLenum d){
	if(!src1(s) && !src1(d)){
		gl.sr=s;
		gl.dr=d;
		call4(0x2002, s, d, s, d);
	}
}
GL_APICALL void GL_APIENTRY glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da){
	if(!src1(sr) && !src1(dr) && !src1(sa) && !src1(da)){
		gl.sr=sr;
		gl.dr=dr;
		call4(0x2002, sr, dr, sa, da);
	}
}

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
	check("glBufferData");
}

GL_APICALL void GL_APIENTRY glClear(GLbitfield mask){call1(0x202c, mask);}
GL_APICALL void GL_APIENTRY glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a){
	call4(0x2003, fbits(r), fbits(g), fbits(b), fbits(a));
}
GL_APICALL void GL_APIENTRY glClearDepthf(GLfloat d){call1(0x2004, fbits(d));}
GL_APICALL void GL_APIENTRY glClearStencil(GLint s){call1(0x2005, s);}
GL_APICALL void GL_APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a){
	gl.mask=r || g || b;
	call4(0x2006, r, g, b, a);
}
GL_APICALL void GL_APIENTRY glCompileShader(GLuint shader){call1(0x2057, shader); check("glCompileShader");}

GL_APICALL GLuint GL_APIENTRY glCreateProgram(void){
	uint32_t w[1]={0x201c};
	GLuint id=call_int(w, 1);

	for(int i=0; id && i<MAX_PROGRAMS; i++){
		if(!programs[i].id){
			memset(&programs[i], 0, sizeof(programs[i]));
			programs[i].id=id;
			programs[i].spec_loc=-1;
			break;
		}
	}
	return id;
}

GL_APICALL GLuint GL_APIENTRY glCreateShader(GLenum type){
	uint32_t w[2]={0x201d, type};
	GLuint id=call_int(w, 2);

	for(int i=0; id && i<MAX_TRACK; i++){
		if(!shaders[i].id){
			shaders[i]=(struct shader){.id=id, .type=type};
			break;
		}
	}
	return id;
}

GL_APICALL void GL_APIENTRY glCullFace(GLenum mode){call1(0x2007, mode);}
GL_APICALL void GL_APIENTRY glDeleteProgram(GLuint prog){
	struct program *p=program(prog);

	if(p){
		if(in_use==p){in_use=NULL;}
		for(int i=0; i<p->nvar; i++){glDeleteProgram(p->var[i].id);}
		for(int i=0; i<p->nu; i++){free(p->u[i].w);}
		free(p->src[0].src);
		free(p->src[1].src);
		p->id=0;
	}
	call1(0x201e, prog);
}
GL_APICALL void GL_APIENTRY glDeleteShader(GLuint shader){
	struct shader *sh=shader_find(shader);

	if(sh){
		free(sh->src);
		memset(sh, 0, sizeof(*sh));
	}
	call1(0x201f, shader);
}
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
GL_APICALL void GL_APIENTRY glDisable(GLenum cap){
	if(cap==GL_BLEND){gl.blend=GL_FALSE;}
	call1(0x200b, cap);
}
GL_APICALL void GL_APIENTRY glEnable(GLenum cap){
	if(cap==GL_BLEND){gl.blend=GL_TRUE;}
	call1(0x200c, cap);
}

/*
 * every surface here is RGB565 without depth or stencil: a draw whose
 * blending keeps the destination (source factor 0, destination 1, add; the
 * destination alpha reads 1) or with rgb masked off changes nothing, and is
 * not sent (alacritty's second text pass, ONE_MINUS_DST_ALPHA / ONE)
 */
static int no_effect(void){
	return !gl.mask || (gl.blend && gl.eq==GL_FUNC_ADD && (gl.sr==GL_ZERO || gl.sr==GL_ONE_MINUS_DST_ALPHA) && gl.dr==GL_ONE);
}

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

	if(count<=0 || no_effect()){return;}
	push(w, 4+attribs(w+4, first+count-1));
	check("glDrawArrays");
}

GL_APICALL void GL_APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices){
	uint32_t w[6+8+6*MAX_ATTRIBS]={0x2074, mode, count, type, (uintptr_t)indices, gl.element};
	uint32_t max=0, i, size=type_size(type);

	if(count<=0 || no_effect()){return;}
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
	check("glDrawElements");
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
GL_APICALL GLint GL_APIENTRY glGetUniformLocation(GLuint prog, const GLchar *name){
	GLint loc=location(0x202a, prog, name);
	struct program *p=program(prog);
	int i;

	/* the name finds the same uniform in the variants */
	if(p && loc>=0 && loc!=p->spec_loc && strlen(name)<64 && (i=uniform_slot(p, loc))>=0){strcpy(p->u[i].name, name);}
	return loc;
}

GL_APICALL GLenum GL_APIENTRY glGetError(void){
	GLenum e=gl.error;

	gl.error=GL_NO_ERROR;
	return e;
}

/* limits as the VC IV has them (the VC III texture unit is the same size), bindings from the client state */
GL_APICALL void GL_APIENTRY glGetIntegerv(GLenum pname, GLint *data){
	switch(pname){
	case GL_MAX_VERTEX_ATTRIBS: *data=MAX_ATTRIBS; break;
	case GL_MAX_TEXTURE_SIZE: case GL_MAX_CUBE_MAP_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: *data=2048; break;
	case GL_MAX_VIEWPORT_DIMS: data[0]=data[1]=2048; break;
	case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *data=8; break;
	case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: *data=0; break;
	case GL_MAX_VARYING_VECTORS: *data=8; break;
	case GL_MAX_VERTEX_UNIFORM_VECTORS: *data=128; break;
	case GL_MAX_FRAGMENT_UNIFORM_VECTORS: *data=16; break;
	case GL_SUBPIXEL_BITS: *data=4; break;
	case GL_UNPACK_ALIGNMENT: *data=gl.unpack; break;
	case GL_PACK_ALIGNMENT: *data=4; break;
	case GL_ARRAY_BUFFER_BINDING: *data=gl.array; break;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING: *data=gl.element; break;
	case 0x85B5: *data=vao_bound; break;	/* GL_VERTEX_ARRAY_BINDING */
	case GL_RED_BITS: case GL_BLUE_BITS: *data=5; break;
	case GL_GREEN_BITS: *data=6; break;
	case GL_ALPHA_BITS: case GL_DEPTH_BITS: case GL_STENCIL_BITS: case GL_SAMPLES: case GL_SAMPLE_BUFFERS: *data=0; break;
	case GL_NUM_COMPRESSED_TEXTURE_FORMATS: case GL_NUM_SHADER_BINARY_FORMATS: *data=0; break;
	case GL_SHADER_COMPILER: *data=GL_TRUE; break;
	default: *data=0; break;
	}
}

GL_APICALL void GL_APIENTRY glGetBooleanv(GLenum pname, GLboolean *data){
	GLint v[2]={0, 0};

	glGetIntegerv(pname, v);
	data[0]=v[0]!=0;
	if(pname==GL_MAX_VIEWPORT_DIMS){data[1]=v[1]!=0;}
}

GL_APICALL void GL_APIENTRY glGetFloatv(GLenum pname, GLfloat *data){
	GLint v[2]={0, 0};

	glGetIntegerv(pname, v);
	data[0]=v[0];
	if(pname==GL_MAX_VIEWPORT_DIMS){data[1]=v[1];}
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
GL_APICALL void GL_APIENTRY glLinkProgram(GLuint prog){
	uint32_t w[2]={0x2043, prog}, r[4];
	struct program *p=program(prog);
	struct shader *v=p && !p->bound ? shader_find(p->vs) : NULL, *f=p ? shader_find(p->fs) : NULL;
	char a[64]="", b[64]="";
	int na, nb;

	for(int i=0; v && i<v->n; i++){bind_attrib(prog, i, v->name[i]);}

	khrn_call(w, 2, 0, 0, 0, r, sizeof(r));
	check("glLinkProgram");
	if(!p || !f || !shader_find(p->vs) || getenv("VCGL_NOSPEC")){return;}
	na=int_uniform(shader_find(p->vs), a, 0, NULL);
	nb=int_uniform(f, b, 0, NULL);
	if(na<=1 && nb<=1 && na+nb>0 && (!na || !nb || !strcmp(a, b))){
		strcpy(p->spec, na ? a : b);
		for(int i=0; i<2; i++){
			const struct shader *sh=i ? f : shader_find(p->vs);

			p->src[i].len=sh->len;
			p->src[i].src=malloc(sh->len);
			if(!p->src[i].src){p->spec[0]=0; return;}
			memcpy(p->src[i].src, sh->src, sh->len);
		}
		p->spec_loc=location(0x202a, prog, p->spec);
		if(p->spec_loc<0){p->spec[0]=0;}
	}
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
	/* has lengths = 0: each string goes with its NUL, the VC drops the last
	 * byte of each, so strings with a given length get one too */
	for(i=0; i<(uint32_t)count; i++){
		uint32_t len=length && length[i]>=0 ? (uint32_t)length[i]+1 : string[i] ? strlen(string[i])+1 : 1;

		if(off+len>sizeof(buf)){gl.error=GL_OUT_OF_MEMORY; return;}
		memcpy(buf+off, string[i] ? string[i] : "", len-1);
		buf[off+len-1]=0;
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
	struct shader *sh=shader_find(shader);

	if(sh){
		free(sh->src);
		sh->src=malloc(off);
		sh->len=0;
		/* the strings joined, without the NULs */
		for(i=0; sh->src && i<(uint32_t)count; i++){
			memcpy(sh->src+sh->len, buf+(sh->len+i), sizes[i]-1);
			sh->len+=sizes[i]-1;
		}
		if(sh->src && sh->type==GL_VERTEX_SHADER){parse_attribs(sh);}
	}
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

GL_APICALL void GL_APIENTRY glUseProgram(GLuint prog){
	struct program *p=program(prog);

	in_use=p && p->spec[0] ? p : NULL;
	if(in_use){use_variant(p);}
	else{call1(0x2016, prog);}
	check("glUseProgram");
}
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

/* the bound 2D texture samples a VC global image in place (glEGLImageTargetTexture2DOES
 * for global images, khclient 0xa964, vc-main 0x5ee94); texture parameters come after.
 * only an image nobody has drawn into yet can be bound (0x502 after), so this goes
 * out now, before the client that owns the image gets to draw */
GLboolean vcglTextureImage(GLuint id0, GLuint id1){
	uint32_t w[4]={KHRN_GL_TEXTURE_IMAGE, GL_TEXTURE_2D, id0, id1};

	push(w, 4);
	return khrn_flush()<0 ? GL_FALSE : GL_TRUE;
}
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
	check("glTexImage2D");
	glTexSubImage2D(target, level, 0, 0, width, height, format, type, pixels);
}
GL_APICALL void GL_APIENTRY glHint(GLenum target, GLenum mode){(void)target; (void)mode;}
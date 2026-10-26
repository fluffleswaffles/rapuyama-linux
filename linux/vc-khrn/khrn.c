// SPDX-License-Identifier: GPL-2.0-only
/*
 * messages to the VC khronos server through /dev/vc-khrn. -DKHRN_MOCK checks
 * them against the vc-main arg counts instead, for qemu-arm on the pc
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "khrn.h"
#include "vc-khrn.h"

#define MAX_WORDS 0x1f0	/* VCHI_MAX_MSG 0x800 / 4, minus room */

static uint32_t display, cur[6];
int khrn_flush(void);
#ifndef KHRN_MOCK
static int fd=-1;
#endif

static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static int gate_busy;
static __thread int gate_owned;
/* VCGL_PROFILE: time other threads waited at the gate */
uint64_t khrn_gate_wait_us;

static void gate_drain(void);

void khrn_gate(void){
	if(gate_owned){return;}
	if(__atomic_load_n(&gate_busy, __ATOMIC_ACQUIRE)){
		struct timespec a, b;

		clock_gettime(CLOCK_MONOTONIC, &a);
		pthread_mutex_lock(&gate_lock);
		while(gate_busy){pthread_cond_wait(&gate_cond, &gate_lock);}
		pthread_mutex_unlock(&gate_lock);
		clock_gettime(CLOCK_MONOTONIC, &b);
		khrn_gate_wait_us+=(b.tv_sec-a.tv_sec)*1000000LL+(b.tv_nsec-a.tv_nsec)/1000;
	}
	gate_drain();
}

void khrn_gate_close(void){
	khrn_gate();
	pthread_mutex_lock(&gate_lock);
	__atomic_store_n(&gate_busy, 1, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&gate_lock);
}

void khrn_gate_open(void){
	pthread_mutex_lock(&gate_lock);
	__atomic_store_n(&gate_busy, 0, __ATOMIC_RELEASE);
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);
}

void khrn_gate_owner(void){gate_owned=1;}

/*
 * the VC answers a flush only when the frame is drawn (vc-main 0x82118
 * spins on the 3D job counter), so the end of a swap runs on a thread of its
 * own and the application goes on meanwhile; its first khrn call that needs
 * the VC waits at the gate
 */
static pthread_mutex_t job_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond=PTHREAD_COND_INITIALIZER;
static int (*job)(void *);
static void *job_arg;
static int job_failed;

static void *swapper(void *unused){
	(void)unused;
	khrn_gate_owner();
	pthread_mutex_lock(&job_lock);
	for(;;){
		int (*fn)(void *);
		void *arg;

		while(!job){pthread_cond_wait(&job_cond, &job_lock);}
		fn=job;
		arg=job_arg;
		pthread_mutex_unlock(&job_lock);
		if(fn(arg)){job_failed=1;}
		pthread_mutex_lock(&job_lock);
		job=NULL;
		khrn_gate_open();
	}
	return NULL;
}

int khrn_async(int (*fn)(void *), void *arg){
	static int mode=-1;	/* 0 here, 1 on the thread */
	pthread_t t;

	khrn_gate();
	if(job_failed){return -1;}
	if(mode<0){
		mode=!getenv("VCGL_SYNC") && !pthread_create(&t, NULL, swapper, NULL);
		if(mode){pthread_detach(t);}
	}
	if(!mode){return fn(arg);}
	khrn_gate_close();
	pthread_mutex_lock(&job_lock);
	job=fn;
	job_arg=arg;
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_lock);
	return 0;
}

void khrn_current(uint32_t type, uint32_t ctx, uint32_t draw, uint32_t read){
	/* the same again changes nothing: no need to wait for a running swap
	 * (the compositor makes its window current at the start of every frame) */
	if(cur[0]==type && cur[1]==ctx && cur[2]==draw && cur[3]==read){return;}
	khrn_gate();
	if(cur[0]!=type || cur[1]!=ctx || cur[2]!=draw || cur[3]!=read){khrn_flush();}
	cur[0]=type;
	cur[1]=ctx;
	cur[2]=draw;
	cur[3]=read;
	cur[4]=0;
	cur[5]=0;
}

#ifdef KHRN_MOCK
/* vc-main khdispatch: KHRN id -> fixed arg count (from the handlers) */
static const struct{uint32_t id, n;} khrn_args[]={
	{KHRN_EGL_CREATE_SURFACE, 12}, {KHRN_EGL_CREATE_GLES20, 2},
	{KHRN_EGL_DESTROY_SURFACE, 1}, {KHRN_EGL_DESTROY_GL, 1},
	{KHRN_EGL_MAKE_CURRENT, 8}, {KHRN_EGL_FLUSH_WAIT, 2}, {KHRN_EGL_FLUSH, 2}, {KHRN_EGL_SWAP, 5},
	{0x2000, 4}, {0x2001, 2}, {0x2002, 4}, {0x2003, 4}, {0x2004, 1},
	{0x2005, 1}, {0x2006, 4}, {0x2007, 1}, {0x2008, 1}, {0x2009, 1},
	{0x200a, 2}, {0x200b, 1}, {0x200c, 1}, {0x200d, 1}, {0x200e, 1},
	{0x2010, 2}, {0x2015, 4}, {0x2016, 1}, {0x2017, 4}, {0x201c, 0},
	{0x201d, 1}, {0x201e, 1}, {0x201f, 1}, {0x2020, 2}, {0x2027, 2},
	{0x202a, 2}, {0x202c, 1}, {0x202f, 2}, {0x2030, 3}, {0x2031, 4},
	{0x2032, 5}, {0x2033, 2}, {0x2034, 3}, {0x2035, 4}, {0x2036, 5},
	{0x2037, 2}, {0x2038, 3}, {0x2039, 2}, {0x203b, 4}, {0x203c, 4},
	{0x203f, 2}, {0x2043, 1}, {0x2051, 1}, {0x2052, 1}, {0x2057, 1},
	{0x2058, 2}, {0x202d, 3}, {0x202e, 3}, {0x203a, 2}, {0x2072, 10},
	{0x2073, 10}, {0x2078, 1}, {0x203d, 2}, {0x203e, 2},
	{0x2082, 3}, {0x4012, 5}, {0x4019, 3}, {0x106d, 0},
};
/* vc-main dispman table gp+0xe3e0: {min, max} args per op */
static const uint8_t disp_args[0x28][2]={
	{0, 0}, {1, 1}, {1, 1}, {3, 3}, {1, 1}, {1, 1}, {1, 1}, {1, 1},
	{1, 1}, {2, 2}, {2, 3}, {2, 2}, {2, 3}, {5, 5}, {1, 1}, {1, 1},
	{1, 1}, {1, 2}, {1, 1}, {14, 16}, {2, 6}, {2, 2}, {3, 3}, {7, 15},
	{2, 6}, {1, 4}, {2, 2}, {1, 1}, {5, 5}, {1, 1}, {17, 17}, {4, 4},
	{1, 1}, {2, 2}, {2, 2}, {3, 3}, {2, 2}, {1, 1}, {5, 5}, {3, 3},
};

static void die(const char *what, uint32_t v){
	fprintf(stderr, "khrn mock: %s %#x\n", what, v);
	exit(1);
}

/* the attrib block of a draw (vc-main 0x195bc): 8 enables, then 6 or 4 words */
static uint32_t attribs(const uint32_t *w){
	uint32_t i, n=8;

	for(i=0; i<8; i++){n+=w[i] ? 6 : 4;}
	return n;
}

/* words of the command at w (id included), *bulks = bulk pieces it reads */
static uint32_t cmd_len(const uint32_t *w, uint32_t left, uint32_t *bulks){
	uint32_t k, id=w[0];

	*bulks=0;
	if(id==0x2064){
		if(left<5){die("short shader source", id);}
		*bulks=w[2]+(w[4] ? 1 : 0);
		return 5+(w[4] ? 0 : w[2]);
	}
	if(id>=0x2046 && id<=0x204d){
		if(left<4 || w[3]>900){die("bad uniform v", id);}
		return 4+(w[3]+3)/4;
	}
	if(id>=0x204e && id<=0x2050){
		if(left<5 || w[4]>900){die("bad uniform matrix", id);}
		return 5+(w[4]+3)/4;
	}
	if(id==0x2076){return left<12 ? 0 : 4+attribs(w+4);}
	if(id==0x2074){return left<14 ? 0 : 6+attribs(w+6);}
	for(k=0; k<sizeof(khrn_args)/sizeof(khrn_args[0]) && khrn_args[k].id!=id; k++){}
	if(k==sizeof(khrn_args)/sizeof(khrn_args[0])){die("unknown id", id);}
	if(id==0x2038 || id==0x203f || id==0x202a || id==0x203d || id==0x203e){*bulks=1;}
	if(id==0x203c && left>4 && w[4]!=0xffffffff){*bulks=1;}
	if((id==0x2072 || id==0x2073) && left>10 && w[10]!=0xffffffff){*bulks=1;}
	return 1+khrn_args[k].n;
}

static int mock_khrn(const uint32_t *w, uint32_t n, uint32_t nbulk, void *reply, uint32_t max){
	uint32_t i=0, k, last=0, bulks=0;
	static uint32_t names=1;

	while(i<n){
		if(bulks){die("bulk reader not last, before", w[i]);}
		k=cmd_len(w+i, n-i, &bulks);
		if(!k){die("short draw", w[i]);}
		last=w[i];
		i+=k;
	}
	if(i!=n){die("args past the end, last id", last);}
	if(bulks!=nbulk){die("bulk pieces wrong, last id", last);}
	printf("khrn mock: %u words, last %#x, %u bulk%s\n", n, last, nbulk, max ? ", reply" : "");
	if(!max){return 0;}
	memset(reply, 0, max);
	if(last==KHRN_EGL_CREATE_SURFACE && max>=20){
		uint32_t r[5]={16, 640, 480, 2, 0x29100};

		memcpy(reply, r, sizeof(r));
		return 20;
	}
	if(last==0x2058 || last==0x2027){
		uint32_t r[2]={4, 1};	/* compile / link status: ok */

		memcpy(reply, r, sizeof(r));
		return 8;
	}
	((uint32_t *)reply)[0]=last==KHRN_EGL_CREATE_GLES20 || last==0x201d || last==0x201c ? names++ : 0;
	return 4;
}

static int mock_disp(const uint32_t *w, uint32_t n){
	uint32_t op=w[0]&~DISP_REPLY;

	if(op>=0x28 || n-1<disp_args[op][0] || n-1>disp_args[op][1]){die("bad dispman op", op);}
	printf("khrn mock: dispman %#x, %u args\n", op, n-1);
	return op==DISP_GCE_REGISTER ? 0x28000 : op==DISP_UPDATE_START ? 0x40000f02 : op==DISP_GCE_ELEMENT_ADD ? 0x20000001 : 0;
}
#endif

int khrn_init(void){
#ifdef KHRN_MOCK
	display=0x10000000;
	return 0;
#else
	fd=open("/dev/vc-khrn", O_RDWR);
	if(fd<0){return -errno;}
	for(int i=0; i<100; i++){
		if(!ioctl(fd, VC_KHRN_IOC_DISPLAY, &display)){return 0;}
		if(errno!=EAGAIN){break;}
		usleep(100000);
	}
	int error=errno;
	close(fd); fd=-1;
	return -error;
#endif
}

uint32_t khrn_display(void){return display;}

static int msg(uint32_t fourcc, const uint32_t *w, uint32_t n, const void *bulk,
	       const uint32_t *sizes, uint32_t nbulk, void *reply, uint32_t max){
#ifdef KHRN_MOCK
	(void)bulk;
	(void)sizes;
	if(fourcc==VC_FCC_DISP){
		int r=mock_disp(w, n);

		if(max){memcpy(reply, &r, 4);}
		return max ? 4 : 0;
	}
	if(fourcc==VC_FCC_UPDH){
		if(max){memcpy(reply, w, 4);}
		return max ? 4 : 0;
	}
	return mock_khrn(w, n, nbulk, reply, max);
#else
	struct vc_khrn_msg m;

	memset(&m, 0, sizeof(m));
	m.fourcc=fourcc;
	m.len=n*4;
	m.data=(uintptr_t)w;
	m.bulk=(uintptr_t)bulk;
	m.bulk_sizes=(uintptr_t)sizes;
	m.nbulk=nbulk;
	m.reply_max=max;
	m.reply=(uintptr_t)reply;
	if(ioctl(fd, VC_KHRN_IOC_MSG, &m)){
		int e=errno;
		static int told;

		if(told++<8){fprintf(stderr, "vc-khrn: %4.4s message %#x (%u words): %s\n",
				     (char *)&fourcc, w[n>9 && fourcc==VC_FCC_KHRN ? n-1 : 0], n, strerror(e));}
		return -e;
	}
	return m.reply_len;
#endif
}

/* calls without reply or bulk wait here and go out with the next message */
static uint32_t q[MAX_WORDS], qn;
/* and here while the gate is closed, behind q */
static uint32_t gq[MAX_WORDS], gqn;

static int send_q(void);

static void gate_drain(void){
	if(!gqn){return;}
	if(9+qn+gqn>MAX_WORDS){send_q();}
	memcpy(q+qn, gq, gqn*4);
	qn+=gqn;
	gqn=0;
}

static uint32_t prefix(uint32_t *m){
	m[0]=KHRN_EGL_MAKE_CURRENT;
	m[1]=(uint32_t)getpid();	/* the VC keeps client state per pid */
	m[2]=0;
	memcpy(m+3, cur, sizeof(cur));
	return 9;
}

int khrn_flush(void){
	khrn_gate();
	return send_q();
}

static int send_q(void){
	uint32_t m[MAX_WORDS], n;

	if(!qn){return 0;}
	n=prefix(m);
	memcpy(m+n, q, qn*4);
	n+=qn;
	qn=0;
	return msg(VC_FCC_KHRN, m, n, 0, 0, 0, 0, 0);
}

int khrn_send_current(void){
	uint32_t m[MAX_WORDS], n;

	khrn_gate();
	n=prefix(m);

	memcpy(m+n, q, qn*4);
	n+=qn;
	qn=0;
	return msg(VC_FCC_KHRN, m, n, 0, 0, 0, 0, 0);
}

int khrn_push(const uint32_t *w, uint32_t n){
	int ret=0;

	if(!gate_owned && __atomic_load_n(&gate_busy, __ATOMIC_ACQUIRE) && 9+gqn+n<=MAX_WORDS){
		memcpy(gq+gqn, w, n*4);
		gqn+=n;
		return 0;
	}
	khrn_gate();
	if(9+qn+n>MAX_WORDS){ret=khrn_flush();}
	if(9+n>MAX_WORDS){return -E2BIG;}
	memcpy(q+qn, w, n*4);
	qn+=n;
	return ret;
}

int khrn_call(const uint32_t *w, uint32_t n, const void *bulk,
	      const uint32_t *sizes, uint32_t nbulk, void *reply, uint32_t reply_max){
	uint32_t m[MAX_WORDS], k;
	int ret;

	khrn_gate();
	if(9+qn+n>MAX_WORDS && (ret=khrn_flush())<0){return ret;}
	if(9+n>MAX_WORDS){return -E2BIG;}
	k=prefix(m);
	memcpy(m+k, q, qn*4);
	k+=qn;
	qn=0;
	memcpy(m+k, w, n*4);
	return msg(VC_FCC_KHRN, m, k+n, bulk, sizes, nbulk, reply, reply_max);
}

uint32_t khrn_res_word(const uint32_t *r, int len, int i){
	if(len>=4 && r[0]==(uint32_t)len-4){i++;}
	return len>=4*(i+1) ? r[i] : 0;
}

int khrn_call_int(const uint32_t *w, uint32_t n, uint32_t *out){
	uint32_t r[4];
	int ret=khrn_call(w, n, 0, 0, 0, r, sizeof(r));

	if(ret<0){return ret;}
	if(ret<4){return -EPROTO;}
	*out=r[0];
	return 0;
}

int disp_call(uint32_t cmd, const uint32_t *a, uint32_t n, int reply, uint32_t *out){
	uint32_t w[18], r=0;
	int ret;

	if(n>17){return -E2BIG;}
	khrn_gate();
	w[0]=cmd|(reply ? DISP_REPLY : 0);
	memcpy(w+1, a, n*4);
	ret=msg(VC_FCC_DISP, w, n+1, 0, 0, 0, reply ? &r : 0, reply ? 4 : 0);
	if(ret<0){return ret;}
	if(out){*out=r;}
	return 0;
}

int khrn_wait_update(uint32_t update){
	uint32_t done=0;
	int ret;

	khrn_gate();
	ret=msg(VC_FCC_UPDH, &update, 1, 0, 0, 0, &done, 4);

	if(ret<0){return ret;}
	return ret==4 && done==update ? 0 : -EPROTO;
}
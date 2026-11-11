// SPDX-License-Identifier: GPL-2.0-only
/* Upstream's linux/isi-runtime/kasumi_runtime.c on the E5 firmware (fw 101.003): UMTS
 * f8/f9 without the hardware accelerator (c6024c00/c6025000) and its semaphore 3, which
 * e5rt cannot reach (phone xg2: GPRS attach spun in the semaphore wait under c66554c7).
 *  - f9 (c66554c7) and the traced f8/f9 wrapper (c66553d5) pick hardware for short
 *    inputs (blo at c6655508, threshold 0x13be bits; blo at c6655426, 0x1400): NOPed,
 *    the firmware's own software KASUMI path runs (E6: the selector c62f222e)
 *  - the f8 batch c6655aac (E6 c62f262a, its own layout on the E5: count byte at +0,
 *    12-byte records {data, bits, COUNT-C} from +4) is replaced by f8 in C on the
 *    firmware's KASUMI key schedule c6655d0f and block cipher c6655c9b (byte-identical
 *    to the E6's c62f3077/c62f3003) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thumb_redirect.h"
#include "kasumi_runtime.h"

/* run fn(args[0..nargs-1]) on another stack (8-byte aligned top); args beyond 4 are passed on it */
__attribute__((naked,target("arm"))) static uint32_t call_on_stack(void *top,void *fn,const uint32_t *args,unsigned nargs){
	__asm__ volatile(
		"push {r4-r7,lr}\n"
		"mov r4,sp\n"
		"mov r5,r1\n"
		"mov r6,r2\n"
		"mov r7,r3\n"
		"mov sp,r0\n"
		"subs r3,r7,#4\n"
		"ble 1f\n"
		"lsl r3,r3,#2\n"
		"add r3,r3,#7\n"
		"bic r3,r3,#7\n"
		"sub sp,sp,r3\n"
		"mov r12,#0\n"
		"2:\n"
		"add r0,r12,#4\n"
		"ldr r1,[r6,r0,lsl #2]\n"
		"str r1,[sp,r12,lsl #2]\n"
		"add r12,r12,#1\n"
		"sub r0,r7,#4\n"
		"cmp r12,r0\n"
		"blt 2b\n"
		"1:\n"
		"ldr r0,[r6]\n"
		"ldr r1,[r6,#4]\n"
		"ldr r2,[r6,#8]\n"
		"ldr r3,[r6,#12]\n"
		"blx r5\n"
		"mov sp,r4\n"
		"pop {r4-r7,pc}\n");
}
/* stack depth of fn on a pre-filled private stack (selftest) */
static uint32_t probe_stack[4096] __attribute__((aligned(8)));
static unsigned stack_depth(void *fn,const uint32_t *args,unsigned nargs){
	for(unsigned i=0;i<4096;i++){probe_stack[i]=0x5a5aa5a5;}
	call_on_stack(probe_stack+4096,fn,args,nargs);
	unsigned i=0;while(i<4096 && probe_stack[i]==0x5a5aa5a5){i++;}
	return (4096-i)*4;
}
static void invalid(void){fprintf(stderr,"invalid KASUMI f8 batch\n");_Exit(1);}
static int private_range(uint32_t address,uint32_t size){
	return (address>=0xc6800000 && size<=0x280000 && address-0xc6800000<=0x280000-size) ||	/* data */
		(address>=0xc6b00000 && size<=0x10000 && address-0xc6b00000<=0x10000-size) ||	/* secure pool */
		(address>=0xcb500000 && size<=0x100000 && address-0xcb500000<=0x100000-size);	/* DSP buffers */
}

static void f8_batch(const uint8_t *batch,const unsigned char *key,uint32_t bearer,uint32_t direction){
	void (*schedule)(const unsigned char *)=(void *)0xc6655d0f;
	void (*cipher)(unsigned char *)=(void *)0xc6655c9b;
	if(!batch){return;}
	uint32_t address=(uintptr_t)batch;
	if(!private_range(address,4) || (address&3) || bearer>31 || direction>1){invalid();}
	static unsigned batches;
	if(!batches++){fprintf(stderr,"software UMTS f8 active\n");}	/* main loop or task stack: once only */
	unsigned count=batch[0];
	if(!count){return;}
	if(!private_range(address,4+count*12) || !private_range((uintptr_t)key,16)){invalid();}
	for(unsigned item=0;item<count;item++){
		const uint32_t *record=(const uint32_t *)(batch+4+item*12);
		unsigned bits=record[1];
		if(bits>524288 || (bits && !private_range(record[0],(bits+7)/8))){invalid();}
		unsigned char modified[16],a[8]={0},stream[8]={0};
		for(unsigned i=0;i<16;i++){modified[i]=key[i]^0x55;}
		for(unsigned i=0;i<4;i++){a[i]=record[2]>>(24-i*8);}
		a[4]=(bearer<<3)|(direction<<2);
		schedule(modified);cipher(a);schedule(key);
		unsigned position=64,block=0;
		unsigned char *data=(void *)(uintptr_t)record[0];
		for(unsigned bit=0;bit<bits;bit++){
			if(position==64){
				for(unsigned j=0;j<8;j++){stream[j]^=a[j];}
				stream[6]^=block>>8;stream[7]^=block;
				cipher(stream);position=0;block++;
			}
			unsigned value=(stream[position/8]>>(7-position%8))&1;
			data[bit/8]^=value<<(7-bit%8);position++;
		}
	}
}

/* Software KASUMI on a private stack. With the selectors NOPed every call takes the software
 * paths, which the original hardware branch never needed; task 0x0c (0x468 bytes of stack) broke
 * its guard under data (phone xg15). The dead hardware blocks after the NOPed selectors hold a
 * veneer, the bl to the software routine is pointed at it, the shim runs the routine here. */
static uint32_t kasumi_stack[2048] __attribute__((aligned(8)));
static uint32_t f9_soft(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3,uint32_t s0,uint32_t s1,uint32_t s2){
	const uint32_t a[7]={a0,a1,a2,a3,s0,s1,s2};
	return call_on_stack(kasumi_stack+2048,(void *)0xc6655f13,a,7);
}
static uint32_t f8_soft(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3,uint32_t s0,uint32_t s1){
	const uint32_t a[6]={a0,a1,a2,a3,s0,s1};
	return call_on_stack(kasumi_stack+2048,(void *)0xc6655e59,a,6);
}
static void f8_batch_private(const uint8_t *batch,const unsigned char *key,uint32_t bearer,uint32_t direction){
	const uint32_t a[4]={(uintptr_t)batch,(uintptr_t)key,bearer,direction};
	call_on_stack(kasumi_stack+2048,(void *)f8_batch,a,4);
}
static void thumb_bl(uint32_t site,uint32_t target){
	int32_t off=(int32_t)(target-(site+4));
	uint16_t hi=0xf000|((off>>12)&0x7ff),lo=0xf800|((off>>1)&0x7ff);
	*(uint16_t *)site=hi;*(uint16_t *)(site+2)=lo;
}
void isi_kasumi_patches(void){
	if(*(uint16_t *)0xc6655426!=0xd30a || *(uint16_t *)0xc6655508!=0xd30b){fprintf(stderr,"unsupported KASUMI selectors\n");_Exit(1);}
	static const unsigned char batch_entry[]={0xff,0xb5,0x84,0xb0,0x00,0x24,0x00,0x28,0x05,0x46,0xa4,0x46};
	if(memcmp((void *)0xc6655aac,batch_entry,sizeof(batch_entry))){fprintf(stderr,"unsupported KASUMI f8 batch\n");_Exit(1);}
	*(uint16_t *)0xc6655426=0x46c0;
	*(uint16_t *)0xc6655508=0x46c0;
	static const unsigned char bl9[]={0x00,0xf0,0xf9,0xfc},bl8[]={0x00,0xf0,0x0e,0xfd};	/* bl c6655f12 / bl c6655e58 */
	if(memcmp((void *)0xc665551c,bl9,4) || memcmp((void *)0xc6655438,bl8,4)){fprintf(stderr,"unsupported KASUMI software calls\n");_Exit(1);}
	isi_thumb_redirect((void *)0xc6655524,(uintptr_t)f9_soft);thumb_bl(0xc665551c,0xc6655524);	/* veneers in the dead hardware blocks */
	isi_thumb_redirect((void *)0xc6655440,(uintptr_t)f8_soft);thumb_bl(0xc6655438,0xc6655440);
	isi_thumb_redirect((void *)0xc6655aac,(uintptr_t)f8_batch_private);
}
int isi_kasumi_selftest(void){
	/* ETSI TS135203 4.5 (f8 test set 1), through the redirected E5 batch entry */
	const unsigned char key[]={0x5a,0xcb,0x1d,0x64,0x4c,0x0d,0x51,0x20,0x4e,0xa5,0xf1,0x45,0x10,0x10,0xd8,0x52};
	const unsigned char plain[]={0xad,0x9c,0x44,0x1f,0x89,0x0b,0x38,0xc4,0x57,0xa4,0x9d,0x42,0x14,0x07,0xe8};
	const unsigned char expected[]={0x9b,0xc9,0x2c,0xa8,0x03,0xc6,0x7b,0x28,0xa1,0x1a,0x4b,0xee,0x5a,0x0c,0x25};
	void *(*allocate)(unsigned)=(void *)0xc63c646d;
	void (*release)(void *)=(void *)0xc63c69bf;
	void (*batch_entry)(void *,void *,unsigned,unsigned)=(void *)0xc6655aad;
	unsigned char *storage=allocate(96);if(!storage){return 1;}
	uint32_t *batch=(void *)storage;
	unsigned char *testkey=storage+48,*data=storage+64;
	memcpy(testkey,key,16);memcpy(data,plain,sizeof(plain));data[15]=0xa5;
	memset(storage,0,48);storage[0]=1;batch[1]=(uintptr_t)data;batch[2]=120;batch[3]=0xfa556b26;
	batch_entry(batch,testkey,3,1);
	if(memcmp(data,expected,15) || data[15]!=0xa5){fprintf(stderr,"UMTS f8 software mismatch\n");return 1;}
	/* two records: the same message split at bit 56 must not continue the keystream (COUNT per record) */
	memcpy(data,plain,sizeof(plain));
	storage[0]=2;batch[1]=(uintptr_t)data;batch[2]=56;batch[3]=0xfa556b26;batch[4]=(uintptr_t)(data+7);batch[5]=64;batch[6]=0xfa556b26;
	batch_entry(batch,testkey,3,1);
	if(memcmp(data,expected,7) || !memcmp(data+7,expected+7,8)){fprintf(stderr,"UMTS f8 records mismatch\n");return 1;}
	release(storage);
	/* UIA1 f9 through the firmware entry c66554c7 (selector NOPed: its software path c6655f12 for
	   every length): ETSI TS 135203 5.3-5.5. Arguments: message, bits, IK, COUNT-I; stack FRESH,
	   MAC out, direction, algorithm (1 = UIA1) */
	static const struct{const char *ik,*msg;uint32_t count,fresh,dir,bits,mac;}sets[]={
		{"2BD6459F82C5B300952C49104881FF48","6B227737296F393C8079353EDC87E2E805D2EC49A4F2D8E0",0x38A6F056,0x05D2EC49,0,189,0xF63BD72C},
		{"D42F682428201CAFCD9F97945E6DE7B7","B5924384328A4AE00B737109F8B6C8DD2B4DB63DD533981CEB19AAD52A5B2BC0",0x3EDC87E2,0xA4F2D8E2,1,254,0xA9DAF1FF},
		{"FDB9CFDF28936CC483A31869D81B8FAB","5932BC0ACE2B0ABA33D8AC188AC54F346FAD10BF9DEE2920B43BD0C53A915CB7DF6CAA72053ABFF2",0x36AF6144,0x9838F03A,1,319,0x1537D316},
	};
	void (*f9)(const void *,uint32_t,const void *,uint32_t,uint32_t,void *,uint32_t,uint32_t)=(void *)0xc66554c7;
	int bad=0;
	for(unsigned t=0;t<3;t++){
		unsigned char *m=allocate(128);if(!m){return 1;}
		unsigned char *ik=m+64;uint32_t *mac=(uint32_t *)(m+96);
		unsigned n=strlen(sets[t].msg)/2;
		for(unsigned i=0;i<n;i++){unsigned b;sscanf(sets[t].msg+2*i,"%2x",&b);m[i]=b;}
		for(unsigned i=0;i<16;i++){unsigned b;sscanf(sets[t].ik+2*i,"%2x",&b);ik[i]=b;}
		*mac=0;
		f9(m,sets[t].bits,ik,sets[t].count,sets[t].fresh,mac,sets[t].dir,1);
		uint32_t got=*mac,swapped=__builtin_bswap32(got);
		fprintf(stderr,"UIA1 test set %u: firmware %08x (bytes %02x%02x%02x%02x), expected %08x\n",t+1,got,((unsigned char *)mac)[0],((unsigned char *)mac)[1],((unsigned char *)mac)[2],((unsigned char *)mac)[3],sets[t].mac);
		if(got!=sets[t].mac && swapped!=sets[t].mac){bad=1;}
		release(m);
	}
	if(bad){fprintf(stderr,"UMTS f9 software mismatch\n");return 1;}
	/* the traced single-message f8 wrapper c66553d5 (selector c6655426 NOPed: software c6655e58):
	   data, bits, CK, COUNT-C; stack bearer, direction, algorithm (1 = UEA1) */
	void (*f8_single)(void *,uint32_t,const void *,uint32_t,uint32_t,uint32_t,uint32_t)=(void *)0xc66553d5;
	unsigned char *w=allocate(64);if(!w){return 1;}
	memcpy(w+32,key,16);memcpy(w,plain,sizeof(plain));w[15]=0xa5;
	f8_single(w,120,w+32,0xfa556b26,3,1,1);
	if(memcmp(w,expected,15) || w[15]!=0xa5){fprintf(stderr,"UMTS f8 wrapper mismatch\n");return 1;}
	release(w);
	{	/* stack depth of each path (task 0x0c has 0x468 bytes; phone xg15: its guard broke under data) */
		unsigned char *m=allocate(128);if(!m){return 1;}
		memcpy(m+32,key,16);memcpy(m,plain,sizeof(plain));
		uint32_t a9[8]={(uintptr_t)m,189,(uintptr_t)(m+32),0x38A6F056,0x05D2EC49,(uintptr_t)(m+96),0,1};
		uint32_t a8[7]={(uintptr_t)m,120,(uintptr_t)(m+32),0xfa556b26,3,1,1};
		unsigned char *st=allocate(96);if(!st){return 1;}
		memset(st,0,48);st[0]=1;((uint32_t *)st)[1]=(uintptr_t)m;((uint32_t *)st)[2]=120;((uint32_t *)st)[3]=0xfa556b26;
		uint32_t ab[4]={(uintptr_t)st,(uintptr_t)(m+32),3,1};
		fprintf(stderr,"KASUMI stack depth: f9 c66554c7 %u, f8 wrapper c66553d5 %u, f8 batch c6655aad %u bytes\n",
			stack_depth((void *)0xc66554c7,a9,8),stack_depth((void *)0xc66553d5,a8,7),stack_depth((void *)0xc6655aad,ab,4));
		release(st);release(m);
	}
	puts("original software KASUMI: ETSI 120-bit UMTS f8 through the E5 batch layout, per-record COUNT, selectors patched; UIA1 f9 ETSI sets 1-3 through c66554c7, f8 set 1 through the wrapper c66553d5");return 0;
}

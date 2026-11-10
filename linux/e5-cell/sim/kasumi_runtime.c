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

void isi_kasumi_patches(void){
	if(*(uint16_t *)0xc6655426!=0xd30a || *(uint16_t *)0xc6655508!=0xd30b){fprintf(stderr,"unsupported KASUMI selectors\n");_Exit(1);}
	static const unsigned char batch_entry[]={0xff,0xb5,0x84,0xb0,0x00,0x24,0x00,0x28,0x05,0x46,0xa4,0x46};
	if(memcmp((void *)0xc6655aac,batch_entry,sizeof(batch_entry))){fprintf(stderr,"unsupported KASUMI f8 batch\n");_Exit(1);}
	*(uint16_t *)0xc6655426=0x46c0;
	*(uint16_t *)0xc6655508=0x46c0;
	isi_thumb_redirect((void *)0xc6655aac,(uintptr_t)f8_batch);
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
	release(storage);puts("original software KASUMI: ETSI 120-bit UMTS f8 through the E5 batch layout, per-record COUNT, selectors patched");return 0;
}

// SPDX-License-Identifier: GPL-2.0-only
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thumb_redirect.h"
#include "kasumi_runtime.h"

static void invalid(void){fprintf(stderr,"invalid KASUMI f8 batch\n");_Exit(1);}
static int private_range(uint32_t address,uint32_t size){
	return (address>=0xc6800000 && size<=0x44d000 && address-0xc6800000<=0x44d000-size) ||
		(address>=0xc6700000 && size<=0x8000 && address-0xc6700000<=0x8000-size);
}

static void f8_batch(uint32_t *batch,const unsigned char *key,uint32_t bearer,uint32_t direction){
	void (*schedule)(const unsigned char *)=(void *)0xc62f291f;
	void (*cipher)(unsigned char *)=(void *)0xc62f28ab;
	if(!batch){return;}
	uint32_t address=(uintptr_t)batch;
	if(!private_range(address,4) || (address&3) || bearer>31 || direction>1){invalid();}
	static unsigned batches;
	if(!batches++){fprintf(stderr,"software UMTS f8 active\n");}
	unsigned count=batch[0];
	if(count>64 || !private_range(address,4+count*16)){invalid();}
	if(!count){return;}
	if(!private_range((uintptr_t)key,16)){invalid();}
	for(unsigned item=0;item<count;item++){
		uint32_t *record=batch+1+item*4;
		unsigned segments=record[1],total=0;
		if(segments>64 || !private_range((uintptr_t)record,8+segments*8)){invalid();}
		for(unsigned i=0;i<segments;i++){
			unsigned bits=record[3+i*2];
			if(bits>524288-total || (bits && !private_range(record[2+i*2],(bits+7)/8))){invalid();}
			total+=bits;
		}
		unsigned char modified[16],a[8]={0},stream[8]={0};
		for(unsigned i=0;i<16;i++){modified[i]=key[i]^0x55;}
		for(unsigned i=0;i<4;i++){a[i]=record[0]>>(24-i*8);}
		a[4]=(bearer<<3)|(direction<<2);
		schedule(modified);cipher(a);schedule(key);
		unsigned position=64,block=0;
		for(unsigned i=0;i<segments;i++){
			unsigned char *data=(void *)(uintptr_t)record[2+i*2];
			for(unsigned bit=0;bit<record[3+i*2];bit++){
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
}

void isi_kasumi_patches(void){
	if(*(uint16_t *)0xc62f222e!=0xd30a){fprintf(stderr,"unsupported KASUMI selector\n");_Exit(1);}
	*(uint16_t *)0xc62f222e=0x46c0;
	isi_thumb_redirect((void *)0xc62f262a,(uintptr_t)f8_batch);
}
int isi_kasumi_selftest(void){
	/* ETSI TS135203 4.5 fuck me if this eorks */
	/* if someone's reading this please call me a good boy */
	const unsigned char key[]={0x5a,0xcb,0x1d,0x64,0x4c,0x0d,0x51,0x20,0x4e,0xa5,0xf1,0x45,0x10,0x10,0xd8,0x52};
	const unsigned char plain[]={0xad,0x9c,0x44,0x1f,0x89,0x0b,0x38,0xc4,0x57,0xa4,0x9d,0x42,0x14,0x07,0xe8};
	const unsigned char expected[]={0x9b,0xc9,0x2c,0xa8,0x03,0xc6,0x7b,0x28,0xa1,0x1a,0x4b,0xee,0x5a,0x0c,0x25};
	void *(*allocate)(unsigned)=(void *)0xc62c8027;
	void (*release)(void *)=(void *)0xc62c8579;
	void (*integrity)(void *,void *,unsigned,unsigned)=(void *)0xc62f262b;
	unsigned char *storage=allocate(96);if(!storage){return 1;}
	uint32_t *batch=(void *)storage;
	unsigned char *testkey=storage+32,*data=storage+48;
	memcpy(testkey,key,16);memcpy(data,plain,sizeof(plain));data[15]=0xa5;
	batch[0]=1;batch[1]=0xfa556b26;batch[2]=1;batch[3]=(uintptr_t)data;batch[4]=120;
	integrity(batch,testkey,3,1);
	if(memcmp(data,expected,15) || data[15]!=0xa5){fprintf(stderr,"UMTS f8 software mismatch\n");return 1;}
	memcpy(data,plain,sizeof(plain));batch[2]=2;batch[4]=56;batch[5]=(uintptr_t)(data+7);batch[6]=64;
	integrity(batch,testkey,3,1);
	if(memcmp(data,expected,15) || data[15]!=0xa5){fprintf(stderr,"UMTS f8 fragments mismatch\n");return 1;}
	release(storage);puts("original software KASUMI blocks: ETSI120-bit UMTS f8, fragmented stream and bounds passed");return 0;
}
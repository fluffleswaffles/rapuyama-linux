// SPDX-License-Identifier: GPL-2.0-only
#ifndef ISI_SBIF_RUNTIME_H
#define ISI_SBIF_RUNTIME_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned char sbif_store[0xfc000];
static int sbif_attached;
static unsigned sbif_writes;

static const void *sbif_test_source;
static _Noreturn void sbif_invalid(const char *reason){
	fprintf(stderr,"private SBIF: %s\n",reason);_exit(124);
}

static int sbif_guest(uintptr_t p,unsigned size){
	return p>=0xc6800000 && size<=0x44d000 && p<=0xc6c4d000-size;
}
static uint32_t sbif_write(uint32_t status,uint32_t sector,uint32_t size,uint32_t reserved,const void *data){
	if(status || reserved || !size || size%512 || size>sizeof(sbif_store) || sector>=2016 || size/512>2016-sector ||
	   (!sbif_guest((uintptr_t)data,size) && (!sbif_test_source || data!=sbif_test_source || size>512))){
		sbif_invalid("invalid sector callback");
	}
	memcpy(sbif_store+sector*512,data,size);sbif_writes+=size/512;return 0;
}
static void sbif_validate_queue(void){
	uint32_t control=*(const uint32_t *)0xc623ae8c;
	if(control!=0xc6813f58){sbif_invalid("unsupported control layout");}
	uint32_t queue=*(const uint32_t *)(uintptr_t)(control+8);
	if(!queue){return;}
	if(!sbif_guest(queue,8)){sbif_invalid("queue range");}
	uint32_t node=*(const uint32_t *)(uintptr_t)queue;
	unsigned count=0;
	while(node){
		if(++count>256 || !sbif_guest(node,8)){sbif_invalid("queue bound or cycle");}
		const uint32_t *entry=(void *)(uintptr_t)node;
		if(!sbif_guest(entry[0],515)){sbif_invalid("queued sector range");}
		const unsigned char *data=(void *)(uintptr_t)entry[0];
		unsigned sector=data[512]|(unsigned)data[513]<<8;
		if(data[514]>1 || sector>=1008){sbif_invalid("queued sector metadata");}
		node=entry[1];
	}
}
static void sbif_service(void){
	if(!sbif_attached){return;}
	sbif_validate_queue();



	
	uint32_t (*api)(uint32_t,uintptr_t)=(void *)0xc623ae0f;
	if(api(13,(uintptr_t)sbif_write)){sbif_invalid("register/drain failed");}
}
static void sbif_attach(void){
	const uint32_t expected[]={0x90d00000,512,0x7e000,0,0x90d7e000,512,0x7e000,1};
	if(memcmp((void *)0xc680aa7c,expected,sizeof(expected))){sbif_invalid("unsupported disk layout");}
	memcpy(sbif_store,(void *)0x90d00000,sizeof(sbif_store));
	sbif_attached=1;sbif_service();
	puts("SBIF consumer: volatile private backing; physical disks unchanged");
}
#endif

// SPDX-License-Identifier: GPL-2.0-only
/* Upstream's linux/isi-runtime/identity_runtime.c on the E5 firmware (fw 101.003): the
 * factory identity (IMEI) comes from a Linux file instead of the secure storage path.
 * E5: reader entry c636a06e (E6 c6262894, same code, own literal offsets), request and
 * response buffers inside the secure pool c6b0002c..c6b08000 (E6 c670202c..c6708000).
 * The selftest calls the E5 reader itself (upstream's E6 harness c625a48d is not mapped).
 * Secure module 0x12 (data item size query c636a9e6, E6 c6263ada; the secure world is not
 * reachable from e5rt, a call faults at c63ebfd4): answered with the service's own failure
 * status -1, which the firmware's item reader (c6362f60) turns into "item unavailable". */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "identity_runtime.h"
#include "thumb_redirect.h"

#define READER 0xc636a06eu
#define POOL_LO 0xc6b0002cu
#define POOL_HI 0xc6b08000u

static unsigned char identity[8];
static int loaded;
int isi_identity_load(const char *path){
	if(!path || !*path){return 0;}
	loaded=0;
	FILE *file=fopen(path,"rb");if(!file){return -1;}
	unsigned char text[18];size_t size=fread(text,1,sizeof(text),file);int failed=ferror(file);fclose(file);
	if(failed || !(size==15 || (size==16 && text[15]=='\n'))){return -1;}
	unsigned sum=0;
	for(unsigned i=0;i<15;i++){
		if(text[i]<'0' || text[i]>'9'){return -1;}
		unsigned digit=text[i]-'0';if(i&1){digit*=2;if(digit>9){digit-=9;}}sum+=digit;
	}
	if(!sum || sum%10){return -1;}
	identity[0]=(text[0]-'0')<<4|3;
	for(unsigned i=1;i<8;i++){identity[i]=(text[i*2]-'0')<<4|(text[i*2-1]-'0');}
	loaded=1;return 0;
}
static uint32_t read_identity(void *request,uint32_t *response){
	uintptr_t input=(uintptr_t)request,output=(uintptr_t)response;
	if(!loaded || input<POOL_LO || input>POOL_HI-4 || output<POOL_LO || output>POOL_HI-12){return 0;}
	memcpy(response+1,identity,sizeof(identity));*response=1;return 1;
}
/* runs on a firmware task stack (0x4b0 bytes for some tasks): no stdio here, isi_identity_report prints */
static volatile uint32_t item_last,item_count,item_reported;
static uint32_t secure_item_size(uint32_t *request,uint32_t *response){
	uintptr_t input=(uintptr_t)request;
	uint32_t item=input>=POOL_LO && input<=POOL_HI-8 ? request[1] : 0;
	item_last=item;item_count++;
	(void)response;return 0xffffffff;
}
void isi_identity_report(void){
	uint32_t count=item_count;
	if(count!=item_reported && item_reported<16){fprintf(stderr,"secure item %08x (module 0x12): unavailable (%u queries)\n",item_last,count);item_reported=count;}
}
int isi_identity_attach(void){
	static const unsigned char item_entry[]={0xfe,0xb5,0x05,0x46,0x0e,0x46,0x9a,0x4c,0x9b,0x48,0xa0,0x60};
	if(memcmp((void *)0xc636a9e6u,item_entry,sizeof(item_entry))){return -1;}
	isi_thumb_redirect((void *)0xc636a9e6u,(uintptr_t)secure_item_size);
	if(!loaded){return 0;}
	void *entry=(void *)READER;
	static const unsigned char expected[]={0x70,0xb5,0x06,0x46,0xe7,0x4d,0xe5,0x48,0x0c,0x46,0xa8,0x60};
	if(memcmp(entry,expected,sizeof(expected))){return -1;}
	isi_thumb_redirect(entry,(uintptr_t)read_identity);
	puts("factory identity provider attached; value not logged");return 0;
}
int isi_identity_selftest(void){
	if(!loaded){fprintf(stderr,"identity selftest needs a test identity file\n");return 2;}
	uint32_t (*reader)(void *,uint32_t *)=(void *)(READER|1);
	uint32_t *request=(void *)(uintptr_t)(POOL_HI-0x40),*response=(void *)(uintptr_t)(POOL_HI-0x20);
	unsigned char saved[0x40];memcpy(saved,request,sizeof(saved));
	memset(request,0xa5,0x40);
	int ok=reader(request,response)==1 && response[0]==1 && !memcmp(response+1,identity,8);
	for(unsigned i=12;ok && i<0x20;i++){ok=((unsigned char *)response)[i]==0xa5;}
	ok=ok && reader((void *)(uintptr_t)(POOL_LO-4),response)==0;
	memcpy(request,saved,sizeof(saved));
	if(!ok){return 1;}
	puts("native factory identity: E5 reader redirect, secure-pool bounds and 8-byte result passed; value not logged");return 0;
}

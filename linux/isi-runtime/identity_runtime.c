// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "identity_runtime.h"
#include "thumb_redirect.h"

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
	if(!loaded || input<0xc670202c || input>0xc6708000-4 || output<0xc670202c || output>0xc6708000-12){return 0;}
	memcpy(response+1,identity,sizeof(identity));*response=1;return 1;
}
int isi_identity_attach(int required){
	if(!loaded){return required ? -1 : 0;}
	void *entry=(void *)0xc6262894;
	static const unsigned char expected[]={0x70,0xb5,0x06,0x46,0x95,0x4d,0x94,0x48,0x0c,0x46,0xa8,0x60};
	if(memcmp(entry,expected,sizeof(expected))){return -1;}
	isi_thumb_redirect(entry,(uintptr_t)read_identity);
	puts("factory identity provider attached; value not logged");return 0;
}
int isi_identity_selftest(void){
	if(!loaded){fprintf(stderr,"identity selftest needs a test identity file\n");return 2;}
	unsigned char *scratch=(void *)0x30000000;
	memset(scratch,0xa5,64);
	if(((uint32_t (*)(void *))0xc625a48d)(scratch)!=1 || memcmp(scratch,identity,8)){return 1;}
	for(unsigned i=8;i<16;i++){if(scratch[i]!=0xff){return 1;}}
	for(unsigned i=16;i<64;i++){if(scratch[i]!=0xa5){return 1;}}
	puts("native factory identity: original reader allocation/event/free and16-byte result passed; value not logged");return 0;
}

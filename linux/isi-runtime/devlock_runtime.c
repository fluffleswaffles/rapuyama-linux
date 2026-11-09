// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "devlock_runtime.h"
#include "thumb_redirect.h"

static int device=-1;
static _Noreturn void failed(void){
	fputs("original PA_DEVLOCK query failed\n",stderr);
	_exit(123);
}
static uint32_t query(void *request,uint32_t *response){
	uintptr_t in=(uintptr_t)request,out=(uintptr_t)response;
	if(in<0xc670202c || in>0xc6708000-12 || out<0xc670202c || out>0xc6708000-8){failed();}
	unsigned char *bytes=request;
	uint32_t input[3],reply[3];
	memcpy(input,request,sizeof(input));
	if(input[1] || bytes[8]){failed();}
	bytes[2]=0x16;*response=0;
	ssize_t n;
	do{n=read(device,reply,sizeof(reply));}while(n<0 && errno==EINTR);
	if(n!=sizeof(reply) || reply[0]!=1 || reply[1]!=1){failed();}
	memcpy(response,reply+1,8);
	return reply[0];
}
int isi_devlock_attach(int required){
	device=open("/dev/rapuyama-devlock",O_RDONLY|O_CLOEXEC);
	if(device<0){return errno==ENOENT && !required ? 0 : -1;}
	void *entry=(void *)0xc6263982;
	static const unsigned char expected[]={0xfe,0xb5,0x05,0x46,0x0e,0x46,0x6d,0x4c,0x6c,0x48,0xa0,0x60};
	if(memcmp(entry,expected,sizeof(expected))){close(device);device=-1;return -1;}
	isi_thumb_redirect(entry,(uintptr_t)query);
	puts("original PA_DEVLOCK query delegated to kernel");
	return 0;
}
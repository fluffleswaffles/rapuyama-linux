// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../kernel/linux/include/uapi/linux/rapuyama_simlock.h"
#include "simlock_runtime.h"
#include "thumb_redirect.h"

static int device=-1;
static _Noreturn void failed(void){
	fputs("original PA_SIMLOC30 check failed\n",stderr);
	_exit(123);
}
static int private_range(uintptr_t address,unsigned size){
	return address>=0xc670202c && address<0xc6708000 && size<=0x5fd4 && address-0xc670202c<=0x5fd4-size;
}
static uint32_t check(void *request,void *response){
	struct rapuyama_simlock query={0};
	uint32_t data;
	uint16_t length;
	if(!private_range((uintptr_t)request,sizeof(query.request)) ||
	   !private_range((uintptr_t)response,sizeof(query.response))){failed();}
	memcpy(&length,(unsigned char *)request+0x1c2,2);
	memcpy(&data,(unsigned char *)request+0x1c4,4);
	if(!private_range(data,length)){failed();}
	((unsigned char *)request)[2]=1;
	memset(response,0,4);
	memcpy(query.request,request,sizeof(query.request));
	memset(query.request+0x1c4,0,4);
	query.data_length=length;query.data=data;
	int result;
	do{result=ioctl(device,RAPUYAMA_SIMLOCK_CHECK,&query);}while(result<0 && errno==EINTR);
	if(result){failed();}
	memcpy(response,query.response,sizeof(query.response));
	return query.transport;
}
int isi_simlock_attach(int required){
	device=open("/dev/rapuyama-simlock",O_RDONLY|O_CLOEXEC);
	if(device<0){return errno==ENOENT && !required ? 0 : -1;}
	void *entry=(void *)0xc6263fc8;
	static const unsigned char expected[]={0xf3,0xb5,0x85,0xb0,0x05,0x46,0xbf,0x48,0xbd,0x49,0x81,0x60};
	if(memcmp(entry,expected,sizeof(expected))){close(device);device=-1;return -1;}
	isi_thumb_redirect(entry,(uintptr_t)check);
	puts("original PA_SIMLOC30 check delegated to kernel");
	return 0;
}
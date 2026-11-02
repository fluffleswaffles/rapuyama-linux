// SPDX-License-Identifier: GPL-2.0-only
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "trace_runtime.h"

static int output=-1;
static unsigned total;
int isi_trace_open(const char *path){
	if(!path || !*path){return 0;}
	output=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
	return output<0 ? -1 : 0;
}
static int emit(const void *data,unsigned size){
	while(size){
		ssize_t n=write(output,data,size);
		if(n<0 && errno==EINTR){continue;}
		if(n<=0){return -1;}
		data=(const unsigned char *)data+n;size-=n;
	}
	return 0;
}
void isi_trace_record(unsigned kind,const uint32_t descriptor[4],const void *payload,unsigned size){
	if(output<0){return;}
	struct timespec time;
	if(size>65545 || total>8*1024*1024-size-36 || clock_gettime(CLOCK_MONOTONIC,&time)){goto stop;}
	uint64_t us=(uint64_t)time.tv_sec*1000000+time.tv_nsec/1000;
	uint32_t header[9]={0x54495349,kind,size,0,0,0,0,us,us>>32};
	if(descriptor){for(unsigned i=0;i<4;i++){header[3+i]=descriptor[i];}}
	if(emit(header,sizeof(header)) || (size && emit(payload,size))){goto stop;}
	total+=sizeof(header)+size;return;
stop:
	close(output);output=-1;fprintf(stderr,"ISI trace stopped at %u bytes\n",total);
}
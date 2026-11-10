// SPDX-License-Identifier: GPL-2.0-only
#ifndef RYISI_THUMB_REDIRECT_H
#define RYISI_THUMB_REDIRECT_H
#include <stdint.h>
#include <string.h>
static inline unsigned int isi_thumb_redirect(void *code,uint32_t target){
	uintptr_t entry=(uintptr_t)code,branch=(entry+3)&~(uintptr_t)3;
	uint32_t load=0xe51ff004;

	if(branch!=entry){*(uint16_t *)entry=0x46c0;}
	*(uint16_t *)branch=0x4778;*(uint16_t *)(branch+2)=0x46c0;
	memcpy((void *)(branch+4),&load,4);memcpy((void *)(branch+8),&target,4);
	return branch+12-entry;
}
#endif
// SPDX-License-Identifier: GPL-2.0-only
#ifndef ISI_FAULT_DIAGNOSTICS_H
#define ISI_FAULT_DIAGNOSTICS_H
#include <stdint.h>
#include <string.h>
struct isi_secure_fault{uint32_t caller;uint16_t module;int has_module;};
static inline int isi_secure_fault_decode(uint32_t pc,uint32_t sp,uint32_t title,struct isi_secure_fault *out){
	static const unsigned char prefix[]={0xab,0x0f,0xbd,0x96,0xaf,0x2a,0xc2,0x71,0xd2,0x62,0x64,0xaf};
	memset(out,0,sizeof(*out));
	if(pc!=0xc62ef9fc || sp&3 || sp<0xc6800000 || sp>0xc6c4d000-32){return 0;}
	const uint32_t *stack=(void *)(uintptr_t)sp;
	if(stack[3]!=0xc6261a51 && stack[3]!=0xc6261a5f){return 0;}
	uint32_t caller=stack[7];
	if((caller&1) && caller>=0xc6200000 && caller<0xc660c000){out->caller=caller;}
	if(title>=0xc6200000 && title<=0xc660c000-16){
		const unsigned char *bytes=(void *)(uintptr_t)title;
		if(!memcmp(bytes,prefix,sizeof(prefix))){out->module=bytes[14]|(uint16_t)bytes[15]<<8;out->has_module=1;}
	}
	return 1;
}
#endif

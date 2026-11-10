// SPDX-License-Identifier: GPL-2.0-only
#ifndef RYISI_THUMB_MMIO_H
#define RYISI_THUMB_MMIO_H
#include <stdint.h>

struct isi_mmio{
	uint32_t address,value;
	uint8_t reg,size,write,sign;
};
static inline int isi_thumb_mmio(uint16_t instruction,const uint32_t regs[16],struct isi_mmio *access){
	unsigned base=(instruction>>3)&7,offset;
	access->reg=instruction&7;access->sign=0;
	if((instruction&0xf000)==0x5000){
		static const uint8_t sizes[]={4,2,1,1,4,2,1,2};
		unsigned kind=(instruction>>9)&7;
		access->size=sizes[kind];access->write=kind<3;access->sign=kind==3 || kind==7;
		offset=regs[(instruction>>6)&7];
	}else if((instruction&0xe000)==0x6000){
		access->size=(instruction&0x1000) ? 1 : 4;access->write=!(instruction&0x0800);
		offset=((instruction>>6)&31)*access->size;
	}else if((instruction&0xf000)==0x8000){
		access->size=2;access->write=!(instruction&0x0800);
		offset=((instruction>>6)&31)*2;
	}else{return 0;}
	access->address=regs[base]+offset;access->value=regs[access->reg];return 1;
}
static inline void isi_thumb_mmio_finish(const struct isi_mmio *access,uint32_t regs[16],uint32_t value){
	if(!access->write){
		if(access->size==1){value=access->sign ? (uint32_t)(int32_t)(int8_t)value : value&255;}
		if(access->size==2){value=access->sign ? (uint32_t)(int32_t)(int16_t)value : value&65535;}
		regs[access->reg]=value;
	}
	regs[15]+=2;
}
#endif
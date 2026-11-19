/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ISI_HARQ_PACK_CAPTURE_H
#define ISI_HARQ_PACK_CAPTURE_H
#include "harq_copy_span.h"
#include "harq_state_runtime.h"

struct isi_harq_allocation { uint32_t reference, shadow, bytes, generation; };
struct isi_harq_packing {
	uint32_t valid, serial, generation, reference, index, bits, attempt, tag0, tag1;
	uint32_t si7, si8, entries, capacity_bits, allocation_bytes, errors;
	struct isi_harq_copy_span span;
};
struct isi_harq_pack_capture {
	uint32_t generation, serial, allocation_valid;
	uint32_t pack_calls, invalid_state_calls, limited_calls;
	struct isi_harq_allocation allocation[8];
	struct isi_harq_packing packing[8];
};

static inline int isi_harq_capture_allocation(struct isi_harq_pack_capture *c,
		uint32_t base,uint32_t bytes,isi_harq_reader read,void *opaque)
{
	memset(c->packing,0,sizeof(c->packing));
	memset(c->allocation,0,sizeof(c->allocation));
	c->allocation_valid=0;
	if(!++c->generation) ++c->generation;
	if(!read || !bytes || (bytes&3) || (base&3) ||
	   base<0x140000 || base>=0x1ff000 || bytes>(0x1ff000-base)/8)
		return -1;
	for(unsigned i=0;i<8;i++){
		uint8_t slot[12];
		if(read(opaque,UINT32_C(0xc685d3a4)+576+i*360,slot,sizeof(slot)))
			return -1;
		uint32_t reference=base+i*bytes, shadow=UINT32_C(0x90c00000)+reference-0x140000;
		if(isi_harq_le32(slot+8)!=reference || isi_harq_le32(slot+4)!=shadow)
			return -1;
		c->allocation[i]=(struct isi_harq_allocation){reference,shadow,bytes,c->generation};
	}
	c->allocation_valid=1;
	return 0;
}

static inline void isi_harq_capture_packing(struct isi_harq_pack_capture *c,
		uint32_t output,uint32_t native_state,isi_harq_reader read,void *opaque)
{
	if(c->pack_calls!=UINT32_MAX) c->pack_calls++;
	uint32_t first=UINT32_C(0xc685d3a4)+576+16;
	if(native_state<first || (native_state-first)%360 || (native_state-first)/360>=8){
		if(c->invalid_state_calls!=UINT32_MAX) c->invalid_state_calls++;
		return;
	}
	unsigned i=(native_state-first)/360;
	struct isi_harq_packing *p=&c->packing[i];
	memset(p,0,sizeof(*p));
	if(c->serial>=64){
		if(c->limited_calls!=UINT32_MAX) c->limited_calls++;
		return;
	}
	p->valid=1;p->serial=++c->serial;p->generation=c->generation;
	uint8_t slot[360], config[8], value[2], packet[30]={0};
	if(!read || read(opaque,native_state-16,slot,sizeof(slot)) ||
	   read(opaque,0xc685e618,config,sizeof(config))) { p->errors=2;return; }
	const uint8_t *s=slot+16;
	p->reference=isi_harq_le32(slot+8);p->index=s[0];
	p->bits=isi_span_le16(s+4);
	p->attempt=slot[15];p->tag0=config[5];p->tag1=config[6];
	p->si7=s[7];p->si8=s[8];p->entries=s[10];
	if(!c->allocation_valid) { p->errors=1;return; }
	const struct isi_harq_allocation *a=&c->allocation[i];
	p->allocation_bytes=a->bytes;
	if(a->generation!=c->generation || a->reference!=p->reference ||
	   a->shadow!=output || isi_harq_le32(slot+4)!=output) { p->errors=4;return; }
	uint32_t table=isi_harq_le32(config);
	if(p->index>=config[4] || table>UINT32_MAX-p->index*2 ||
	   read(opaque,table+p->index*2,value,sizeof(value))) { p->errors=8;return; }
	p->capacity_bits=isi_span_le16(value);
	packet[3]=0xed;packet[5]=24;packet[7]=0x3b;packet[9]=0x61;
	packet[19]=p->attempt;packet[21]=p->index;
	packet[22]=p->bits>>8;packet[23]=p->bits;
	for(unsigned j=0;j<4;j++) packet[26+j]=p->reference>>(24-j*8);
	if(isi_harq_copy_span_plan_phase(packet,30,slot,360,
		p->capacity_bits,a->bytes,0,&p->span)!=1) p->errors=16;
}

static inline const struct isi_harq_packing *isi_harq_capture_match(
		const struct isi_harq_pack_capture *c,const uint8_t packet[30])
{
	uint32_t reference=(uint32_t)packet[26]<<24 | (uint32_t)packet[27]<<16 |
		(uint32_t)packet[28]<<8 | packet[29];
	const struct isi_harq_packing *found=NULL;
	for(unsigned i=0;i<8;i++){
		const struct isi_harq_packing *p=&c->packing[i];
		if(p->valid && p->generation==c->generation && p->reference==reference &&
		   p->index==packet[21] && p->bits==isi_harq_be16(packet+22) &&
		   p->attempt==packet[19] && p->tag0==isi_harq_be16(packet+10) &&
		   p->tag1==isi_harq_be16(packet+12)){
			if(found) return NULL;
			found=p;
		}
	}
	return found;
}
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ISI_HARQ_COPY_SPAN_H
#define ISI_HARQ_COPY_SPAN_H
#include "dsp_payload_runtime.h"

struct isi_harq_copy_span {
	uint32_t legacy_bytes, complete_bytes, packed_bits, si_bits, terminator_bits;
};

static inline uint16_t isi_span_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1]<<8;
}
static inline uint32_t isi_span_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
		(uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

static inline int isi_harq_copy_span_plan_phase(const uint8_t *packet, size_t length,
		const uint8_t *process, size_t process_size,
		uint32_t capacity_bits, uint32_t allocation_bytes,
		unsigned counter_delta,
		struct isi_harq_copy_span *out)
{
	if(!out) return -1;
	*out=(struct isi_harq_copy_span){0};
	if(!packet) return -1;
	uint32_t offset, old_bytes;
	int payload=isi_dsp_harq_payload(packet,length,&offset,&old_bytes);
	if(payload<=0) return payload;
	if(!process || process_size<360 || !capacity_bits || !allocation_bytes || counter_delta>1)
		return -1;
	uint32_t reference=UINT32_C(0x140000)+offset;
	const uint8_t *state=process+16;
	if(isi_span_le32(process+8)!=reference ||
	   isi_span_le32(process+4)!=UINT32_C(0x90c00000)+offset ||
	   process[14]!=1 || state[0]!=packet[21] ||
	   isi_span_le16(state+4)!=((uint32_t)packet[22]<<8 | packet[23]) ||
	   process[15]!=(uint8_t)(packet[19]+counter_delta))
		return -1;
	unsigned entries=state[10];
	if(!entries || entries>15 || state[6]>1 || state[7]>1 || state[8]>1)
		return -1;
	uint64_t data_bits=0;
	unsigned active=0;
	for(unsigned i=0;i<entries;i++){
		const uint8_t *entry=state+12+i*20;
		uint32_t status=isi_span_le32(entry+4);
		if(status>2) return -1;
		if(status!=1) continue;
		unsigned count=entry[10], bits=isi_span_le16(entry+16);
		if(!count || count>63 || !bits || entry[8]>63 || entry[9]>63)
			return -1;
		data_bits+=18+(uint64_t)count*bits;
		active++;
	}
	uint32_t wire_bits=(uint32_t)packet[22]<<8 | packet[23];
	if(!active || data_bits!=wire_bits) return -1;
	uint32_t terminator=state[6] ? 6 : 0;
	uint32_t si=(state[7] || state[8]) ? 18 : 0;
	uint64_t full_bits=data_bits+terminator+si;
	uint64_t bytes=((full_bits+7)/8+3)&~UINT64_C(3);
	if(full_bits>capacity_bits || bytes>4096 || bytes<old_bytes ||
	   bytes>allocation_bytes || offset>=0xbf000 || bytes>0xbf000-offset)
		return -1;
	*out=(struct isi_harq_copy_span){old_bytes,(uint32_t)bytes,
		(uint32_t)full_bits,si,terminator};
	return 1;
}
static inline int isi_harq_copy_span_plan(const uint8_t *packet, size_t length,
		const uint8_t *process, size_t process_size,
		uint32_t capacity_bits, uint32_t allocation_bytes,
		struct isi_harq_copy_span *out)
{
	return isi_harq_copy_span_plan_phase(packet,length,process,process_size,
		capacity_bits,allocation_bytes,1,out);
}
#endif

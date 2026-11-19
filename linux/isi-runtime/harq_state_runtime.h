/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ISI_HARQ_STATE_RUNTIME_H
#define ISI_HARQ_STATE_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct isi_harq_feedback {
	uint32_t valid, tag0, tag1, word14, word16;
	uint16_t grant[8], flags[8];
};
struct isi_harq_flow_state {
	uint32_t grant, flag14, raw_index, selected_index, stored_value, table_value;
};
struct isi_harq_snapshot {
	uint32_t feedback_match, word14, word16, flow_mask;
	uint32_t process_matches, process_index, pending, state_attempt;
	uint32_t current_selection_matches, table_count, errors;
	struct isi_harq_flow_state flow[8];
};
typedef int (*isi_harq_reader)(void *, uint32_t, void *, size_t);

static inline uint16_t isi_harq_be16(const uint8_t *p)
{
	return (uint16_t)p[0]<<8 | p[1];
}
static inline uint32_t isi_harq_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
		(uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

static inline int isi_harq_feedback_decode(const uint8_t *raw, size_t size,
					   struct isi_harq_feedback *out)
{
	if (!raw || !out || size != 64 || raw[7] != 0xed || raw[10] != 0x3d || raw[13] != 0xe8)
		return 0;
	struct isi_harq_feedback feedback = {
		.valid=1, .tag0=isi_harq_be16(raw+14), .tag1=isi_harq_be16(raw+16),
		.word14=isi_harq_be16(raw+18), .word16=isi_harq_be16(raw+20),
	};
	for (unsigned i=0;i<8;i++) {
		feedback.grant[i]=isi_harq_be16(raw+26+i*4);
		feedback.flags[i]=isi_harq_be16(raw+28+i*4);
	}
	*out=feedback;
	return 1;
}

static inline void isi_harq_snapshot_read(const uint8_t packet[30],
		const struct isi_harq_feedback *feedback, isi_harq_reader read,
		void *opaque, struct isi_harq_snapshot *out)
{
	memset(out,0,sizeof(*out));
	out->process_index=UINT32_MAX;
	if (!packet || !feedback || !read) { out->errors=1; return; }
	uint32_t reference=(uint32_t)packet[26]<<24 | (uint32_t)packet[27]<<16 |
		(uint32_t)packet[28]<<8 | packet[29];
	out->feedback_match=feedback->valid &&
		feedback->tag0==isi_harq_be16(packet+10) &&
		feedback->tag1==isi_harq_be16(packet+12);
	if (out->feedback_match) {
		out->word14=feedback->word14;
		out->word16=feedback->word16;
	}
	uint8_t config[8], selection[4];
	if (read(opaque,0xc685e618,config,sizeof(config)) ||
	    read(opaque,0xc685e624,selection,sizeof(selection))) {
		out->errors|=2; return;
	}
	uint32_t table=isi_harq_le32(config);
	out->table_count=config[4];
	uint32_t selected=isi_harq_le32(selection);
	for (unsigned i=0;i<8;i++) {
		uint8_t slot[16];
		uint32_t address=UINT32_C(0xc685d3a4)+576+i*360;
		if (read(opaque,address,slot,sizeof(slot))) { out->errors|=4; continue; }
		if (isi_harq_le32(slot+8)!=reference) continue;
		out->process_matches++;
		out->process_index=i;
		out->pending=slot[14];
		out->state_attempt=slot[15];
		out->current_selection_matches=selected==address;
	}
	if(out->process_matches!=1) out->errors|=32;
	for (unsigned i=0;i<8;i++) {
		uint8_t flow[20], value[2];
		if (read(opaque,UINT32_C(0xc685d3a4)+352+i*28,flow,sizeof(flow))) {
			out->errors|=8; continue;
		}
		if (!flow[0]) continue;
		out->flow_mask|=1u<<i;
		struct isi_harq_flow_state *f=&out->flow[i];
		f->selected_index=flow[15];
		f->stored_value=(uint16_t)flow[16] | (uint16_t)flow[17]<<8;
		if (out->feedback_match) {
			f->grant=feedback->grant[i];
			f->flag14=!!(feedback->flags[i]&0x4000);
			f->raw_index=feedback->flags[i]&255;
		}
		if (!out->table_count || f->selected_index>=out->table_count ||
		    table>UINT32_MAX-f->selected_index*2 ||
		    read(opaque,table+f->selected_index*2,value,sizeof(value))) {
			out->errors|=16; continue;
		}
		f->table_value=(uint16_t)value[0] | (uint16_t)value[1]<<8;
	}
}
#endif

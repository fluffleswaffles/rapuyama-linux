/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ISI_HARQ_PDU_RUNTIME_H
#define ISI_HARQ_PDU_RUNTIME_H
#include "harq_state_runtime.h"

struct isi_harq_pdu_snapshot {
	uint32_t wire_index, wire_bits, state_index, state_bits;
	uint32_t table_count, table_bits, process_matches, selection_match;
	uint32_t index_match, state_length_match, table_length_match, errors;
	uint32_t table_capacity_ok, table_padding_bits;
};

static inline void isi_harq_pdu_snapshot_read(const uint8_t packet[30],
		isi_harq_reader read, void *opaque, struct isi_harq_pdu_snapshot *out)
{
	memset(out,0,sizeof(*out));
	if (!packet || !read) { out->errors=1; return; }
	out->wire_index=packet[21];
	out->wire_bits=isi_harq_be16(packet+22);
	uint32_t reference=(uint32_t)packet[26]<<24 | (uint32_t)packet[27]<<16 |
		(uint32_t)packet[28]<<8 | packet[29];
	uint8_t config[8], selection[4];
	if (read(opaque,0xc685e618,config,sizeof(config)) ||
	    read(opaque,0xc685e624,selection,sizeof(selection))) {
		out->errors|=2; return;
	}
	uint32_t table=isi_harq_le32(config), selected=isi_harq_le32(selection);
	out->table_count=config[4];
	uint8_t value[2];
	if (out->wire_index>=out->table_count ||
	    table>UINT32_MAX-out->wire_index*2 ||
	    read(opaque,table+out->wire_index*2,value,sizeof(value))) {
		out->errors|=8;
	} else {
		out->table_bits=(uint16_t)value[0] | (uint16_t)value[1]<<8;
		out->table_length_match=out->table_bits==out->wire_bits;
		out->table_capacity_ok=out->table_bits>=out->wire_bits;
		if(out->table_capacity_ok)
			out->table_padding_bits=out->table_bits-out->wire_bits;
	}
	for (unsigned i=0;i<8;i++) {
		uint8_t slot[24];
		uint32_t address=UINT32_C(0xc685d3a4)+576+i*360;
		if (read(opaque,address,slot,sizeof(slot))) { out->errors|=4; continue; }
		if (isi_harq_le32(slot+8)!=reference) continue;
		out->process_matches++;
		out->state_index=slot[16];
		out->state_bits=(uint16_t)slot[20] | (uint16_t)slot[21]<<8;
		out->selection_match=selected==address;
	}
	if (out->process_matches!=1) {
		out->errors|=16;
	} else {
		out->index_match=out->state_index==out->wire_index;
		out->state_length_match=out->state_bits==out->wire_bits;
	}
}
#endif

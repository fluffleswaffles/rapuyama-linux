// SPDX-License-Identifier: GPL-2.0-only
#ifndef ISI_DSP_PAYLOAD_RUNTIME_H
#define ISI_DSP_PAYLOAD_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

static inline int isi_dsp_harq_payload(const uint8_t *packet, size_t length,
				      uint32_t *offset, uint32_t *size)
{
	if (length < 10 || packet[3] != 0xed || packet[7] != 0x3b ||
	    packet[9] != 0x61)
		return 0;
	if (length != 30 || packet[4] != 0 || packet[5] != 24)
		return -1;
	uint32_t bits = (uint32_t)packet[22] << 8 | packet[23];
	uint32_t address = (uint32_t)packet[26] << 24 |
		(uint32_t)packet[27] << 16 | (uint32_t)packet[28] << 8 | packet[29];

	if (!bits)
		return 0;
	uint32_t bytes = ((bits + 7) / 8 + 3) & ~UINT32_C(3);

	if (bytes > 4096 || (address & 3) || address < 0x140000 ||
	    address >= 0x1ff000 || bytes > 0x1ff000 - address)
		return -1;
	*offset = address - 0x140000;
	*size = bytes;
	return 1;
}

/*
 * the downlink counterpart
 * PN_WCDMA object 0x3c, ID 0x00 announces received transport block buffers in a 
 * DSP-written ring (0x140000 upwards, observed up to 0x14b52c, 92-byte stride for 360-bit blocks) */
#define ISI_DSP_DL_SPAN 2048
#define ISI_DSP_DL_LIMIT 0x1b0000

static inline int isi_dsp_dl_buffers(const uint8_t *packet, size_t length,
				     uint32_t *offsets, uint32_t *sizes,
				     unsigned max)
{
	if (length < 18 || packet[7] != 0xed || packet[10] != 0x3c ||
	    packet[11] != 0x00)
		return 0;
	unsigned count = (unsigned)packet[16] << 8 | packet[17];

	if (count > max || 18 + 6 * (size_t)count > length)
		return -1;
	for (unsigned i = 0; i < count; i++) {
		const uint8_t *entry = packet + 18 + 6 * i;
		uint32_t address = (uint32_t)entry[2] << 24 |
			(uint32_t)entry[3] << 16 | (uint32_t)entry[4] << 8 |
			entry[5];

		if ((address & 3) || address < 0x140000 ||
		    address >= ISI_DSP_DL_LIMIT)
			return -1;
		offsets[i] = address - 0x140000;
		sizes[i] = ISI_DSP_DL_LIMIT - address < ISI_DSP_DL_SPAN ?
			ISI_DSP_DL_LIMIT - address : ISI_DSP_DL_SPAN;
	}
	return count;
}

#endif

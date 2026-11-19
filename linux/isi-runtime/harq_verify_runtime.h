/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ISI_HARQ_VERIFY_RUNTIME_H
#define ISI_HARQ_VERIFY_RUNTIME_H
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include "../kernel/linux/include/uapi/linux/rapuyama_mdi.h"

#define ISI_HARQ_VERIFY_LIMIT 64
#define ISI_HARQ_VERIFY_WORDS 10
struct isi_harq_verify_state { unsigned enabled, records; };

static inline int isi_harq_verify(struct isi_harq_verify_state *state, int fd,
				   uint32_t offset, const uint8_t *source,
				   uint32_t size,
				   uint32_t metadata[ISI_HARQ_VERIFY_WORDS])
{
	memset(metadata, 0, ISI_HARQ_VERIFY_WORDS * sizeof(*metadata));
	if (!state->enabled || state->records >= ISI_HARQ_VERIFY_LIMIT)
		return 0;
	metadata[0] = ++state->records;
	metadata[5] = size;
	if (!source || !size || size > 4096 || offset >= 0xbf000 ||
	    size > 0xbf000 - offset) {
		metadata[7] = EINVAL;
		errno = EINVAL;
		return -1;
	}
	uint8_t copy[4096] = {0};
	struct rapuyama_mdi_buffer buffer = {
		.offset=offset, .size=size, .buffer=(uintptr_t)copy,
	};
	if (ioctl(fd, RAPUYAMA_MDI_DSP_GET, &buffer)) {
		int error = errno ? errno : EIO;
		explicit_bzero(copy, sizeof(copy));
		metadata[7] = error;
		errno = error;
		return -1;
	}
	for (uint32_t i=0; i<size; i++)
		metadata[6] += copy[i] != source[i];
	explicit_bzero(copy, sizeof(copy));
	return metadata[6] ? -2 : 1;
}
#endif

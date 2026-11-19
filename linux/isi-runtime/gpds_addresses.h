/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ISI_GPDS_ADDRESSES_H
#define ISI_GPDS_ADDRESSES_H
#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct gpds_addresses {
	char ip[INET6_ADDRSTRLEN], dns1[INET6_ADDRSTRLEN], dns2[INET6_ADDRSTRLEN];
	int family;
};

static inline int gpds_addresses_parse(const uint8_t *message, size_t size,
					struct gpds_addresses *result)
{
	if (!message || !result || size < 4 || message[1] != 0x0a)
		return -1;
	struct gpds_addresses parsed = {0};
	size_t offset = 4;
	unsigned seen = 0;
	for (unsigned n = 0; n < message[3]; n++) {
		if (size - offset < 2) return -1;
		size_t length = message[offset + 1];
		if (length < 4 || length > size - offset || (length & 3)) return -1;
		const uint8_t *block = message + offset;
		unsigned bit = block[0] == 4 ? 1 : block[0] == 0x0d ? 2 :
			block[0] == 0x0e ? 4 : 0;
		if (bit) {
			if ((seen & bit) || (block[3] != 4 && block[3] != 16) ||
			    length < (size_t)4 + block[3])
				return -1;
			seen |= bit;
			int family = block[3] == 4 ? AF_INET : AF_INET6;
			char *out = bit == 1 ? parsed.ip : bit == 2 ? parsed.dns1 : parsed.dns2;
			int nonzero = 0;
			for (unsigned i = 0; i < block[3]; i++) nonzero |= block[4+i];
			if (!nonzero || (family == AF_INET && (block[4] >= 224 || block[4] == 127)) ||
			    (family == AF_INET6 && (block[4] == 0xff ||
			     (block[4] == 0xfe && (block[5] & 0xc0) == 0x80))))
				return -1;
			if (family == AF_INET6) {
				struct in6_addr address;
				memcpy(&address, block+4, sizeof(address));
				if (IN6_IS_ADDR_LOOPBACK(&address) || IN6_IS_ADDR_V4MAPPED(&address))
					return -1;
			}
			if (!inet_ntop(family, block+4, out, INET6_ADDRSTRLEN)) return -1;
			if (bit == 1) parsed.family = family;
		}
		offset += length;
	}
	if (offset != size || !(seen & 1)) return -1;
	*result = parsed;
	return 0;
}
#endif

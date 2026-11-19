/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ISI_GPDS_PROFILE_H
#define ISI_GPDS_PROFILE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct gpds_bytes {
	const uint8_t *data;
	size_t length;
};

struct gpds_context_profile {
	struct gpds_bytes apn;
	uint8_t pdp_type; /* 0x21 IPv4, 0x57 IPv6 */
	uint8_t data_compression, header_compression; /* 0, 1, or 0xff */
	int request_dns, request_radio_activity;
};

enum gpds_auth_kind { GPDS_AUTH_NONE, GPDS_AUTH_PAP, GPDS_AUTH_CHAP };

struct gpds_auth_profile {
	enum gpds_auth_kind kind;
	struct gpds_bytes username, password, challenge, response;
	uint8_t identifier;
};

struct gpds_qos99 {
	uint8_t traffic_class, delivery_order, erroneous_sdu;
	uint8_t residual_ber, sdu_error_ratio, priority;
	uint16_t transfer_delay, max_sdu, max_uplink, max_downlink;
	uint16_t guaranteed_uplink, guaranteed_downlink;
};

static inline void gpds_qos99_block(uint8_t *out, uint8_t id,
				    const struct gpds_qos99 *qos)
{
	out[0] = id; out[1] = 20;
	out[2] = qos->traffic_class; out[3] = qos->delivery_order;
	out[4] = qos->erroneous_sdu; out[5] = qos->residual_ber;
	out[6] = qos->sdu_error_ratio; out[7] = qos->priority;
	const uint16_t values[] = {
		qos->transfer_delay, qos->max_sdu, qos->max_uplink,
		qos->max_downlink, qos->guaranteed_uplink, qos->guaranteed_downlink
	};
	for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
		out[8 + i * 2] = values[i] >> 8;
		out[9 + i * 2] = values[i];
	}
}

static inline size_t gpds_qos_configure_body(uint8_t *out, size_t capacity,
					     uint8_t cid, uint8_t pdp_type,
					     int dns,
					     const struct gpds_qos99 *requested,
					     const struct gpds_qos99 *minimum)
{
	size_t total = 7 + (dns ? 4 : 0) + 20 + 20 + 4;
	if (!out || !requested || !minimum || capacity < total ||
	    (pdp_type != 0x21 && pdp_type != 0x57))
		return 0;
	const uint8_t header[] = {6, cid, pdp_type, 0, cid, 0, 3 + !!dns};
	memcpy(out, header, sizeof(header));
	size_t offset = sizeof(header);
	if (dns) {
		const uint8_t request[] = {0x90, 4, 0, 0};
		memcpy(out + offset, request, sizeof(request));
		offset += sizeof(request);
	}
	gpds_qos99_block(out + offset, 6, requested);
	offset += 20;
	gpds_qos99_block(out + offset, 7, minimum);
	offset += 20;
	const uint8_t activity[] = {0xf9, 4, 0, 0};
	memcpy(out + offset, activity, sizeof(activity));
	return total;
}

static inline size_t gpds_block_size(size_t payload)
{
	return (payload + 2 + 3) & ~(size_t)3;
}

static inline int gpds_bytes_valid(struct gpds_bytes value, size_t maximum)
{
	return value.length <= maximum && (!value.length || value.data);
}

static inline size_t gpds_string_block(uint8_t *out, uint8_t id,
				       struct gpds_bytes value)
{
	size_t size = gpds_block_size(1 + value.length);
	memset(out, 0, size);
	out[0] = id;
	out[1] = size;
	out[2] = value.length;
	if (value.length)
		memcpy(out + 3, value.data, value.length);
	return size;
}

static inline int gpds_compression_valid(uint8_t value)
{
	return value == 0 || value == 1 || value == 0xff;
}

/* returns zero on invalid input or insufficient space */
static inline size_t gpds_configure_body(uint8_t *out, size_t capacity,
					 uint8_t cid,
					 const struct gpds_context_profile *profile)
{
	if (!out || !profile || !gpds_bytes_valid(profile->apn, 252) ||
	    !profile->apn.length ||
	    (profile->pdp_type != 0x21 && profile->pdp_type != 0x57) ||
	    !gpds_compression_valid(profile->data_compression) ||
	    !gpds_compression_valid(profile->header_compression))
		return 0;
	size_t apn_size = gpds_block_size(1 + profile->apn.length);
	if (apn_size > 255)
		return 0;
	size_t total = 7 + 4 + apn_size +
		(profile->request_dns ? 4 : 0) +
		(profile->request_radio_activity ? 4 : 0);
	if (total > capacity)
		return 0;
	const uint8_t header[] = {
		0x06, cid, profile->pdp_type, 0, cid, 0,
		2 + !!profile->request_dns + !!profile->request_radio_activity
	};
	memcpy(out, header, sizeof(header));
	size_t offset = sizeof(header);
	if (profile->request_dns) {
		const uint8_t dns[] = {0x90, 4, 0, 0};
		memcpy(out + offset, dns, sizeof(dns));
		offset += sizeof(dns);
	}
	const uint8_t compression[] = {
		0x00, 4, profile->data_compression, profile->header_compression
	};
	memcpy(out + offset, compression, sizeof(compression));
	offset += sizeof(compression);
	offset += gpds_string_block(out + offset, 0x05, profile->apn);
	if (profile->request_radio_activity) {
		const uint8_t activity[] = {0xf9, 4, 0, 0};
		memcpy(out + offset, activity, sizeof(activity));
	}
	return total;
}

static inline size_t gpds_auth_body(uint8_t *out, size_t capacity, uint8_t cid,
				    const struct gpds_auth_profile *profile)
{
	if (!out || !profile ||
	    (profile->kind != GPDS_AUTH_PAP && profile->kind != GPDS_AUTH_CHAP) ||
	    !gpds_bytes_valid(profile->username, 53))
		return 0;
	size_t total = 3 + gpds_block_size(1 + profile->username.length);
	if (profile->kind == GPDS_AUTH_PAP) {
		if (!gpds_bytes_valid(profile->password, 53))
			return 0;
		total += gpds_block_size(1 + profile->password.length);
	} else {
		if (!gpds_bytes_valid(profile->challenge, 53) ||
		    !gpds_bytes_valid(profile->response, 52))
			return 0;
		total += gpds_block_size(1 + profile->challenge.length) +
			gpds_block_size(2 + profile->response.length);
	}
	if (total > capacity)
		return 0;
	out[0] = 0x33;
	out[1] = cid;
	out[2] = profile->kind == GPDS_AUTH_PAP ? 2 : 3;
	size_t offset = 3;
	offset += gpds_string_block(out + offset, 0x0b, profile->username);
	if (profile->kind == GPDS_AUTH_PAP) {
		gpds_string_block(out + offset, 0x0c, profile->password);
	} else {
		offset += gpds_string_block(out + offset, 0x0f, profile->challenge);
		size_t size = gpds_block_size(2 + profile->response.length);
		memset(out + offset, 0, size);
		out[offset] = 0x11;
		out[offset + 1] = size;
		out[offset + 2] = profile->identifier;
		out[offset + 3] = profile->response.length;
		if (profile->response.length)
			memcpy(out + offset + 4, profile->response.data,
			       profile->response.length);
	}
	return total;
}

#endif

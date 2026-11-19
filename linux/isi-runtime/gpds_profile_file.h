/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ISI_GPDS_PROFILE_FILE_H
#define ISI_GPDS_PROFILE_FILE_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gpds_profile.h"

struct gpds_file_profile {
	int qos_r99_subscribed;
	char apn[101];
	uint8_t username[53], password[53], challenge[53], response[52];
	struct gpds_context_profile context;
	struct gpds_auth_profile auth;
};

static inline int gpds_hex_value(unsigned char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static inline int gpds_profile_hex(const char *text, uint8_t *out,
				    size_t maximum, size_t *length)
{
	size_t n = strlen(text);
	if ((n & 1) || n / 2 > maximum)
		return -1;
	for (size_t i = 0; i < n; i += 2) {
		int high = gpds_hex_value(text[i]), low = gpds_hex_value(text[i + 1]);
		if (high < 0 || low < 0) return -1;
		out[i / 2] = high * 16 + low;
	}
	*length = n / 2;
	return 0;
}

static inline int gpds_profile_parse(char *text, struct gpds_file_profile *profile)
{
	memset(profile, 0, sizeof(*profile));
	profile->context = (struct gpds_context_profile){
		.pdp_type = 0x21, .data_compression = 0xff,
		.header_compression = 0xff, .request_dns = 1,
		.request_radio_activity = 1,
	};
	const char *keys[] = {
		"apn", "auth", "username", "password", "challenge_hex",
		"response_hex", "identifier", "data_compression", "header_compression", "qos", "pdp"
	};
	unsigned seen = 0;
	char *position = text, *line;
	while ((line = strsep(&position, "\n"))) {
		size_t n = strlen(line);
		if (n && line[n - 1] == '\r') line[--n] = 0;
		if (!n || line[0] == '#') continue;
		char *value = strchr(line, '=');
		if (!value) return -1;
		*value++ = 0;
		unsigned key;
		for (key = 0; key < sizeof(keys) / sizeof(keys[0]); key++)
			if (!strcmp(line, keys[key])) break;
		if (key == sizeof(keys) / sizeof(keys[0]) || (seen & (1u << key)))
			return -1;
		seen |= 1u << key;
		size_t length = strlen(value);
		switch (key) {
		case 0:
			if (!length || length > 100) return -1;
			for (size_t i = 0; i < length; i++)
				if (!((value[i] >= 'a' && value[i] <= 'z') ||
				      (value[i] >= 'A' && value[i] <= 'Z') ||
				      (value[i] >= '0' && value[i] <= '9') ||
				      value[i] == '.' || value[i] == '-'))
					return -1;
			memcpy(profile->apn, value, length + 1);
			break;
		case 1:
			if (!strcmp(value, "none")) profile->auth.kind = GPDS_AUTH_NONE;
			else if (!strcmp(value, "pap")) profile->auth.kind = GPDS_AUTH_PAP;
			else if (!strcmp(value, "chap")) profile->auth.kind = GPDS_AUTH_CHAP;
			else return -1;
			break;
		case 2:
		case 3:
			if (length > 53) return -1;
			if (key == 2) {
				memcpy(profile->username, value, length);
				profile->auth.username.length = length;
			} else {
				memcpy(profile->password, value, length);
				profile->auth.password.length = length;
			}
			break;
		case 4:
			if (gpds_profile_hex(value, profile->challenge, 53,
					     &profile->auth.challenge.length)) return -1;
			break;
		case 5:
			if (gpds_profile_hex(value, profile->response, 52,
					     &profile->auth.response.length)) return -1;
			break;
		case 6: {
			if (!length || length > 3) return -1;
			unsigned id = 0;
			for (size_t i = 0; i < length; i++) {
				if (value[i] < '0' || value[i] > '9') return -1;
				id = id * 10 + value[i] - '0';
			}
			if (id > 255) return -1;
			profile->auth.identifier = id;
			break;
		}
		case 9:
			if (!strcmp(value, "none")) profile->qos_r99_subscribed = 0;
			else if (!strcmp(value, "r99-subscribed")) profile->qos_r99_subscribed = 1;
			else return -1;
			break;
		case 10:
			if (!strcmp(value, "ipv4")) profile->context.pdp_type = 0x21;
			else if (!strcmp(value, "ipv6")) profile->context.pdp_type = 0x57;
			else return -1;
			break;
		default: {
			uint8_t compression;
			if (!strcmp(value, "default")) compression = 0xff;
			else if (!strcmp(value, "off")) compression = 0;
			else if (!strcmp(value, "on")) compression = 1;
			else return -1;
			if (key == 7) profile->context.data_compression = compression;
			else profile->context.header_compression = compression;
			break;
		}
		}
	}
	if (!(seen & 1) || !(seen & 2)) return -1; /* APN and auth explicit */
	unsigned credential_fields = seen & ((1u << 7) - 4);
	if (profile->auth.kind == GPDS_AUTH_NONE && credential_fields) return -1;
	if (profile->auth.kind == GPDS_AUTH_PAP &&
	    ((seen & 12) != 12 || (seen & 112))) return -1;
	if (profile->auth.kind == GPDS_AUTH_CHAP &&
	    ((seen & 116) != 116 || (seen & 8) ||
	     profile->auth.response.length != 16 || !profile->auth.challenge.length))
		return -1;
	profile->context.apn = (struct gpds_bytes){
		(const uint8_t *)profile->apn, strlen(profile->apn)
	};
	profile->auth.username.data = profile->username;
	profile->auth.password.data = profile->password;
	profile->auth.challenge.data = profile->challenge;
	profile->auth.response.data = profile->response;
	return 0;
}

static inline int gpds_profile_load(const char *path, struct gpds_file_profile *profile)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) return -1;
	struct stat st;
	char text[4097];
	size_t used = 0;
	int result = -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 077) || st.st_size < 1 || st.st_size > 4096)
		goto done;
	while (used < sizeof(text) - 1) {
		ssize_t n = read(fd, text + used, sizeof(text) - 1 - used);
		if (n < 0) {
			if (errno == EINTR) continue;
			goto done;
		}
		if (!n) break;
		used += n;
	}
	if (used != (size_t)st.st_size || memchr(text, 0, used)) goto done;
	text[used] = 0;
	result = gpds_profile_parse(text, profile);
done:
	explicit_bzero(text, sizeof(text));
	close(fd);
	if (result) explicit_bzero(profile, sizeof(*profile));
	return result;
}

static inline int gpds_owner_trace_disabled(void)
{
	FILE *pidfile = fopen("/tmp/isasw.pid", "re");
	unsigned pid;
	char extra;
	if (!pidfile) return 0;
	int fields = fscanf(pidfile, "%u %c", &pid, &extra);
	fclose(pidfile);
	if (fields != 1 || pid <= 1) return 0;
	char path[64], environment[32768];
	snprintf(path, sizeof(path), "/proc/%u/environ", pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return 0;
	ssize_t n = read(fd, environment, sizeof(environment));
	close(fd);
	if (n <= 0 || n == sizeof(environment) || environment[n - 1]) return 0;
	int disabled = 1;
	for (size_t offset = 0; offset < (size_t)n;) {
		const char *entry = environment + offset;
		size_t length = strlen(entry);
		if (length > 10 && !memcmp(entry, "ISI_TRACE=", 10)) disabled = 0;
		offset += length + 1;
	}
	explicit_bzero(environment, sizeof(environment));
	return disabled;
}
#endif

/* packet-data client for the legacy GPDS resource 0x31 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/phonet.h>
#include "gpds_profile.h"
#include "gpds_profile_file.h"
#include "gpds_addresses.h"

#ifndef SOL_PNPIPE
#define SOL_PNPIPE 275
#endif

#define GPDS 0x31
#define PIPE 0xd9
#define COMMGR 0x10
#define HOST 0x6c
#define OBJECT 0xfc

static int fd = -1, pep = -1, data = -1;
static uint8_t tid = 0x40, context, pipe_handle;
static uint8_t gpds_device, gpds_object;
static struct {
	uint8_t message[300];
	ssize_t size;
} pending[8];
static unsigned pending_count;
static int pending_overflow;
static int no_context, attached, context_created, activating;
static int symbian_profile;
static int file_profile;
static struct gpds_file_profile configured_profile;
static volatile sig_atomic_t stopped;

static void stop(int signal)
{
	(void)signal;
	stopped = 1;
}

static int64_t milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return -1;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static ssize_t receive_frame(uint8_t *reply, struct sockaddr_pn *from, int64_t deadline)
{
	for (;;) {
		if (activating && stopped) {
			errno = ECANCELED;
			return -1;
		}
		int64_t left = deadline - milliseconds();
		struct pollfd wait[] = {
			{.fd = fd, .events = POLLIN},
			{.fd = data < 0 ? pep : -1, .events = POLLIN}
		};

		if (left <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		int ready = poll(wait, 2, left > 60000 ? 60000 : left);

		if (ready < 0 && errno == EINTR && !stopped)
			continue;
		if (!ready) {
			if (left > 60000)
				continue;
			errno = ETIMEDOUT;
			return -1;
		}
		if (ready < 0)
			return -1;
		if (wait[0].revents & (POLLERR | POLLHUP | POLLNVAL) ||
		    wait[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			errno = EIO;
			return -1;
		}
		if (wait[1].revents & POLLIN) {
			data = accept(pep, NULL, NULL);
			if (data < 0)
				return -1;
		}
		if (wait[0].revents & POLLIN) {
			socklen_t size = sizeof(*from);

			return recvfrom(fd, reply, 300, 0, (void *)from, &size);
		}
	}
}

static int save_activation(const uint8_t *reply, ssize_t size)
{
	if (size < 4 || size > (ssize_t)sizeof(pending[0].message) ||
	    pending_count == sizeof(pending) / sizeof(pending[0]))
		return -1;
	memcpy(pending[pending_count].message, reply, size);
	pending[pending_count++].size = size;
	return 0;
}

static ssize_t next_activation(uint8_t *reply)
{
	if (!pending_count)
		return -1;
	ssize_t size = pending[0].size;

	memcpy(reply, pending[0].message, size);
	pending_count--;
	memmove(pending, pending + 1, pending_count * sizeof(pending[0]));
	return size;
}

static ssize_t request(uint8_t resource, const uint8_t *body, size_t length,
		       uint8_t expected, uint8_t *reply, unsigned timeout)
{
	uint8_t message[300];
	struct sockaddr_pn destination = {
		.spn_family = AF_PHONET, .spn_resource = resource
	};
	int64_t deadline = milliseconds() + timeout;

	if (activating && stopped) {
		errno = ECANCELED;
		return -1;
	}
	if (length + 1 > sizeof(message))
		return -1;
	message[0] = ++tid;
	memcpy(message + 1, body, length);
	ssize_t sent = sendto(fd, message, length + 1, 0, (void *)&destination,
			     sizeof(destination));
	explicit_bzero(message, sizeof(message));
	if (sent != (ssize_t)length + 1)
		return -1;
	for (;;) {
		struct sockaddr_pn from;
		ssize_t size = receive_frame(reply, &from, deadline);

		if (size < 0)
			return -1;
		if (from.spn_resource == resource && size >= 2 &&
		    reply[0] == tid && reply[1] == expected) {
			if (resource == GPDS && expected == 0xf0) {
				gpds_device = from.spn_dev;
				gpds_object = from.spn_obj;
			}
			return size;
		}
		if (resource == PIPE && from.spn_resource == GPDS &&
		    size >= 4 && (reply[1] == 0x09 || reply[1] == 0x0a ||
				   reply[1] == 0x1f || reply[1] == 0x0d)) {
			if (save_activation(reply, size)) {
				pending_overflow = 1;
				fprintf(stderr, "too many pending GPDS activation messages\n");
				return -1;
			}
		}
	}
}

static int probe(uint8_t *reply)
{
	const uint8_t version[] = {0xf0, 0x12, 0};
	const uint8_t status[] = {0x19};
	ssize_t size = request(GPDS, version, sizeof(version), 0xf0, reply, 3000);

	/* the version reply uses f0 13 and may come from a nonzero object */
	if (size < 5 || reply[2] != 0x13) {
		fprintf(stderr, "GPDS version unavailable\n");
		return -1;
	}
	printf("GPDS version %u.%u\n", reply[3], reply[4]);
	size = request(GPDS, status, sizeof(status), 0x1a, reply, 3000);
	if (size < 14) {
		fprintf(stderr, "GPDS status unavailable\n");
		return -1;
	}
	printf("GPDS status %02x %02x\n", reply[2], reply[3]);
	no_context = reply[3] == 0 && reply[2] <= 1;
	attached = reply[2] == 1;
	return 0;
}

static int create_pipe(uint8_t *reply)
{
	uint16_t handle;
	int encapsulation = PNPIPE_ENCAP_IP;
	ssize_t size;

	pep = socket(AF_PHONET, SOCK_SEQPACKET, 0);
	if (pep < 0 || setsockopt(pep, SOL_SOCKET, SO_BINDTODEVICE,
				  "phonet0", 8) || listen(pep, 1) ||
	    ioctl(pep, SIOCPNGETOBJECT, &handle))
		return -1;
	const uint8_t create[] = {
		0x00, 0, 0, handle >> 8, handle & 0xff, 0x04, 0,
		gpds_device, gpds_object, 0x04, 0
	};

	size = request(PIPE, create, sizeof(create), 0x01, reply, 5000);
	if (size < 4 || reply[3] != 0)
		return -1;
	pipe_handle = reply[2];
	if (data < 0) {
		struct pollfd wait = {.fd = pep, .events = POLLIN};

		if (poll(&wait, 1, 5000) <= 0 || (data = accept(pep, NULL, NULL)) < 0)
			return -1;
	}
	if (setsockopt(data, SOL_PNPIPE, PNPIPE_ENCAP,
		       &encapsulation, sizeof(encapsulation)))
		return -1;
	unsigned index = 0;
	socklen_t length = sizeof(index);
	char name[IF_NAMESIZE];

	if (getsockopt(data, SOL_PNPIPE, PNPIPE_IFINDEX, &index, &length) ||
	    !if_indextoname(index, name))
		return -1;
	printf("IFNAME %s\n", name);
	return 0;
}

static int failed(const char *stage, ssize_t size, const uint8_t *reply)
{
	if (size < 0)
		fprintf(stderr, "%s: no response\n", stage);
	else
		fprintf(stderr, "%s: message %02x, length %zd, result %02x, cause %02x\n",
			stage, size >= 2 ? reply[1] : 0xff, size,
			size >= 4 ? reply[3] : 0xff,
			size >= 5 ? reply[4] : 0xff);
	return -1;
}

static int activation_event(const uint8_t *reply, ssize_t size, uint8_t cid)
{
	if (size < 4 || reply[2] != cid)
		return 0;
	if (reply[1] == 0x09)
		return reply[3] == 1 ? 0 : -1;
	if (reply[1] == 0x1f || reply[1] == 0x0d)
		return -1;
	return reply[1] == 0x0a ? 1 : 0;
}

static int context_lost(const uint8_t *reply, ssize_t size, uint8_t cid)
{
	return size >= 3 && reply[2] == cid &&
		(reply[1] == 0x0d || reply[1] == 0x1f);
}

static int attach_response(const uint8_t *reply, ssize_t size)
{
	if (size >= 5 && reply[2] == 1)
		return 0;
	if (size < 0)
		fprintf(stderr, "GPDS attach: no response: %s\n", strerror(errno));
	else
		fprintf(stderr, "GPDS attach: length %zd, status %02x, cause %02x, type %02x\n",
			size, size >= 3 ? reply[2] : 0xff,
			size >= 4 ? reply[3] : 0xff,
			size >= 5 ? reply[4] : 0xff);
	return -1;
}

/* ACCESS_BARRED (fd) on UMTS */
static int attach_retry(const uint8_t *reply, ssize_t size, int attempt)
{
	return attempt < 3 && !stopped && size >= 4 && reply[3] == 0xfd;
}

static int configure_qos(uint8_t *reply)
{
	if (!file_profile || !configured_profile.qos_r99_subscribed)
		return 0;
	/* Symbian maps Unspecified R99 fields to SUBSCRIBED (zero).
	 * No application-specific rate or minimum is fabricated here.
	 */
	const struct gpds_qos99 subscribed = {0};
	uint8_t message[55];
	size_t length = gpds_qos_configure_body(message, sizeof(message), context,
		configured_profile.context.pdp_type, configured_profile.context.request_dns,
		&subscribed, &subscribed);
	if (!length) {
		errno = EINVAL;
		return -1;
	}
	ssize_t size = request(GPDS, message, length, 0x07, reply, 5000);
	if (size < 4 || reply[2] != context || reply[3] != 1)
		return failed("GPDS QoS configure", size, reply);
	return 0;
}

static int activate(const char *apn, uint8_t *reply)
{
	const uint8_t attach[] = {0x13, 1};
	const uint8_t create[] = {0x02};
	const uint8_t subscribe[] = {0, 0x10, 1, GPDS};
	struct sockaddr_pn manager = {
		.spn_family = AF_PHONET, .spn_resource = COMMGR
	};
	ssize_t size;

	if (stopped) {
		errno = ECANCELED;
		return -1;
	}
	if (sendto(fd, subscribe, sizeof(subscribe), 0, (void *)&manager,
		   sizeof(manager)) != sizeof(subscribe))
		return -1;
	for (int attempt = 1; !attached; attempt++) {
		size = request(GPDS, attach, sizeof(attach), 0x14, reply, 120000);
		if (!attach_response(reply, size))
			break;
		if (!attach_retry(reply, size, attempt))
			return -1;
		fprintf(stderr, "GPDS attach: access barred, retry %d of 2\n", attempt);
		sleep(10);
		if (stopped) {
			errno = ECANCELED;
			return -1;
		}
	}
	if (create_pipe(reply))
		return -1;
	size = request(GPDS, create, sizeof(create), 0x03, reply, 5000);
	if (size < 4 || reply[3] != 1)
		return failed("GPDS context ID create", size, reply);
	context = reply[2];
	context_created = 1;
	const uint8_t link[] = {0x00, context, pipe_handle, 0x02};

	size = request(GPDS, link, sizeof(link), 0x01, reply, 5000);
	if (size < 4 || reply[3] != 1)
		return failed("GPDS link configure", size, reply);
	uint8_t configure[160];
	size_t length = strlen(apn), padded = (3 + length + 3) & ~3u;
	const uint8_t header[] = {
		0x06, context, 0x21, 0, context, 0, 2,
		0x90, 4, 0, 0, 0x05, padded, length
	};
	size_t total = sizeof(header) - 3 + padded;

	memcpy(configure, header, sizeof(header));
	memcpy(configure + sizeof(header), apn, length);
	memset(configure + sizeof(header) + length, 0,
	       total - sizeof(header) - length);
	if (symbian_profile) {
		const struct gpds_context_profile profile = {
			.apn = {(const uint8_t *)apn, length},
			.pdp_type = 0x21,
			.data_compression = 0xff, .header_compression = 0xff,
			.request_dns = 1, .request_radio_activity = 1,
		};
		total = gpds_configure_body(configure, sizeof(configure), context,
			file_profile ? &configured_profile.context : &profile);
		if (!total) {
			errno = EINVAL;
			return -1;
		}
	}
	size = request(GPDS, configure, total, 0x07, reply, 5000);
	if (size < 4 || reply[2] != context || reply[3] != 1)
		return failed("GPDS context configure", size, reply);
	if (file_profile && configured_profile.auth.kind != GPDS_AUTH_NONE) {
		uint8_t authentication[200];
		size_t auth_size = gpds_auth_body(authentication, sizeof(authentication),
						context, &configured_profile.auth);
		if (!auth_size) {
			errno = EINVAL;
			return -1;
		}
		size = request(GPDS, authentication, auth_size, 0x34, reply, 5000);
		explicit_bzero(authentication, sizeof(authentication));
		if (size < 4 || reply[2] != context || reply[3] != 1)
			return failed("GPDS context authentication", size, reply);
	}
	if (configure_qos(reply))
		return -1;

	uint8_t message[] = {++tid, 0x08, context, 0};
	struct sockaddr_pn destination = {
		.spn_family = AF_PHONET, .spn_resource = GPDS
	};
	const uint8_t enable[] = {0x06, pipe_handle, 0};
	int64_t deadline = milliseconds() + 60000;

	if (stopped) {
		errno = ECANCELED;
		return -1;
	}
	if (sendto(fd, message, sizeof(message), 0, (void *)&destination,
		   sizeof(destination)) != sizeof(message))
		return -1;
	size = request(PIPE, enable, sizeof(enable), 0x07, reply, 5000);
	if (pending_overflow)
		return -1;
	if (size >= 4 && reply[3] != 0)
		return failed("pipe enable", size, reply);
	if (size < 4)
		fprintf(stderr, "pipe enable: no response, waiting for GPDS activation\n");
	for (;;) {
		struct sockaddr_pn from;

		if (pending_count) {
			size = next_activation(reply);
			memset(&from, 0, sizeof(from));
			from.spn_resource = GPDS;
		} else {
			size = receive_frame(reply, &from, deadline);
		}
		if (size < 0)
			return failed("GPDS activate", size, reply);
		if (from.spn_resource != GPDS || size < 4)
			continue;
		if (reply[1] == 0x09 && reply[0] != message[0])
			continue;
		int event = activation_event(reply, size, context);

		if (event < 0)
			return failed("GPDS activation response", size, reply);
		if (event > 0)
			break;
	}
	struct gpds_addresses negotiated;
	if (gpds_addresses_parse(reply, size, &negotiated)) {
		fprintf(stderr, "invalid negotiated GPDS addresses\n");
		return -1;
	}
	printf("%s %s\n", negotiated.family == AF_INET ? "IP" : "IPV6", negotiated.ip);
	if (*negotiated.dns1) printf("DNS1 %s\n", negotiated.dns1);
	if (*negotiated.dns2) printf("DNS2 %s\n", negotiated.dns2);
	puts("CONTEXT ACTIVE");
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t reply[300];
	struct sockaddr_pn local = {
		.spn_family = AF_PHONET, .spn_dev = HOST, .spn_obj = OBJECT
	};
	int result = 1;

	if ((argc != 2 || strcmp(argv[1], "--probe")) &&
	    (argc != 3 || (strcmp(argv[1], "--data") &&
			   strcmp(argv[1], "--symbian-data") &&
			   strcmp(argv[1], "--profile")) ||
	     !*argv[2] || strlen(argv[2]) > 100)) {
		fprintf(stderr, "usage: modem-gprs --probe | --data APN | --symbian-data APN | --profile FILE\n");
		return 2;
	}
	file_profile = argc == 3 && !strcmp(argv[1], "--profile");
	symbian_profile = file_profile || (argc == 3 && !strcmp(argv[1], "--symbian-data"));
	if (file_profile) {
		if (gpds_profile_load(argv[2], &configured_profile)) {
			fprintf(stderr, "invalid or insecure GPDS profile; require owner-only regular file\n");
			return 2;
		}
		if (configured_profile.auth.kind != GPDS_AUTH_NONE &&
		    !gpds_owner_trace_disabled()) {
			fprintf(stderr, "credential transmission requires a verified owner without ISI_TRACE\n");
			explicit_bzero(&configured_profile, sizeof(configured_profile));
			return 2;
		}
	}
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	fd = socket(AF_PHONET, SOCK_DGRAM, PN_PROTO_PHONET);
	if (fd < 0 || bind(fd, (void *)&local, sizeof(local))) {
		perror("GPDS socket");
		if (fd >= 0)
			close(fd);
		explicit_bzero(&configured_profile, sizeof(configured_profile));
		return 1;
	}
	if (probe(reply))
		goto done;
	if (argc == 2) {
		result = 0;
		goto done;
	}
	if (!no_context) {
		fprintf(stderr, "existing GPDS context or unknown status; refusing activation\n");
		goto done;
	}
	activating = 1;
	int activation = activate(file_profile ? configured_profile.apn : argv[2], reply);
	explicit_bzero(&configured_profile, sizeof(configured_profile));

	activating = 0;
	if (activation) {
		fprintf(stderr, "GPDS packet-data setup failed\n");
		if (context_created) {
			const uint8_t deactivate[] = {0x0b, context};

			request(GPDS, deactivate, sizeof(deactivate), 0x0c, reply, 5000);
		}
		goto done;
	}
	result = 0;
	while (!stopped) {
		struct pollfd wait = {.fd = fd, .events = POLLIN};
		int ready = poll(&wait, 1, 1000);

		if (ready < 0) {
			if (errno == EINTR)
				continue;
			perror("GPDS session poll");
			result = 1;
			break;
		}
		if (wait.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "GPDS session transport closed\n");
			result = 1;
			break;
		}
		if (wait.revents & POLLIN) {
			struct sockaddr_pn from;
			socklen_t length = sizeof(from);
			ssize_t size = recvfrom(fd, reply, sizeof(reply), 0,
						(void *)&from, &length);

			if (size < 0) {
				if (errno == EINTR)
					continue;
				perror("GPDS session receive");
				result = 1;
				break;
			}
			if (from.spn_resource == GPDS &&
			    context_lost(reply, size, context)) {
				failed("GPDS context lost", size, reply);
				result = 1;
				break;
			}
		}
	}
	const uint8_t deactivate[] = {0x0b, context};

	request(GPDS, deactivate, sizeof(deactivate), 0x0c, reply, 5000);
done:
	explicit_bzero(&configured_profile, sizeof(configured_profile));
	if (data >= 0)
		close(data);
	if (pep >= 0)
		close(pep);
	if (context_created) {
		const uint8_t status[] = {0x19};
		int gone = 0;

		data = pep = -1;
		for (int attempt = 0; attempt < 20; attempt++) {
			ssize_t size = request(GPDS, status, sizeof(status),
					       0x1a, reply, 3000);

			if (size >= 14 && reply[3] == 0) {
				gone = 1;
				break;
			}
			sleep(1);
		}
		if (!gone) {
			fprintf(stderr, "GPDS context still reported after shutdown\n");
			result = 1;
		}
	}
	close(fd);
	return result;
}

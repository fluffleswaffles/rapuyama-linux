/* e5gprs: packet data over the E5 firmware's GPDS server (legacy ISI resource 0x31),
 * the sequence of ofono's isimodem gprs-context driver: a Linux Phonet pipe end point
 * (SOCK_SEQPACKET, accepted with PNPIPE_ENCAP_IP = a "gprsN" netdev), PNS_PIPE_CREATE
 * at the firmware's pipe service (0xd9) between it and GPDS, then GPDS context id
 * create, link configure (plain), context configure (IPv4, APN), activate, pipe enable.
 * Prints the interface, IP and DNS from GPDS_CONTEXT_ACTIVATE_IND, then keeps the pipe
 * (and the netdev) until killed. Needs e5rt live with --backend and a registered modem.
 *   e5gprs <apn> */
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
#include <sys/time.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/phonet.h>
#include <linux/rtnetlink.h>

#ifndef AF_PHONET
#define AF_PHONET 35
#endif
#ifndef SOL_PNPIPE
#define SOL_PNPIPE 275
#endif

#define PN_GPDS 0x31
#define PN_PIPE 0xd9
#define PN_COMMGR 0x10
#define PN_PEP_TYPE_GPRS 0x04
#define HOST 0x6c
#define EVENT_OBJ 0xfc	/* the firmware sends its indications to 6c:fc */

static int dg;	/* datagram socket, bound to 6c:fc */
static uint8_t tid = 0x40;
static int listen_pep = -1, gprs_sock = -1;	/* accepted while a request waits (the kernel answers the
						   firmware's PNS_PEP_CONNECT_REQ only inside accept()) */

static void dump(const char *what, const uint8_t *m, ssize_t n)
{
	printf("%s:", what);
	for (ssize_t i = 0; i < n; i++)
		printf(" %02x", m[i]);
	printf("\n");
}

/* one request, wait for the reply with the same tid and message id (others printed) */
static ssize_t request(uint8_t dev, uint8_t res, uint8_t obj, const uint8_t *body, size_t len, uint8_t want, uint8_t *reply, unsigned ms)
{
	uint8_t m[300];
	m[0] = ++tid;
	memcpy(m + 1, body, len);
	struct sockaddr_pn to = {.spn_family = AF_PHONET, .spn_dev = dev, .spn_obj = obj, .spn_resource = res};
	dump("tx", m, len + 1);
	if (sendto(dg, m, len + 1, 0, (struct sockaddr *)&to, sizeof(to)) != (ssize_t)len + 1) {
		perror("sendto");
		return -1;
	}
	for (;;) {
		struct pollfd p[2] = {{.fd = dg, .events = POLLIN}, {.fd = gprs_sock < 0 ? listen_pep : -1, .events = POLLIN}};
		if (poll(p, 2, ms) <= 0) {
			printf("no reply %02x to res %02x\n", want, res);
			return -1;
		}
		if (p[1].revents & POLLIN) {
			gprs_sock = accept(listen_pep, NULL, NULL);
			printf("pipe end point connected (%d)\n", gprs_sock);
		}
		if (!(p[0].revents & POLLIN))
			continue;
		struct sockaddr_pn from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(dg, reply, 300, 0, (struct sockaddr *)&from, &fl);
		if (n < 0)
			return -1;
		char what[32];
		snprintf(what, sizeof(what), "rx %02x:%02x res %02x", from.spn_dev, from.spn_obj, from.spn_resource);
		dump(what, reply, n);
		if (n >= 2 && reply[0] == tid && reply[1] == want)
			return n;
	}
}

/* wait for an indication (any tid) with message id want from res */
static ssize_t indication(uint8_t res, uint8_t want, uint8_t *m, unsigned ms)
{
	struct timeval start, now;
	gettimeofday(&start, NULL);
	for (;;) {
		gettimeofday(&now, NULL);
		long left = (long)ms - ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000);
		struct pollfd p = {.fd = dg, .events = POLLIN};
		if (left <= 0 || poll(&p, 1, left) <= 0)
			return -1;
		struct sockaddr_pn from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(dg, m, 300, 0, (struct sockaddr *)&from, &fl);
		if (n < 0)
			return -1;
		char what[32];
		snprintf(what, sizeof(what), "ind %02x:%02x res %02x", from.spn_dev, from.spn_obj, from.spn_resource);
		dump(what, m, n);
		if (from.spn_resource == res && n >= 2 && m[1] == want)
			return n;
		if (from.spn_resource == PN_GPDS && n >= 2 && (m[1] == 0x1f || m[1] == 0x0d)) {
			printf("GPDS %s, cause %02x\n", m[1] == 0x1f ? "activation failed" : "context deactivated", n > 3 ? m[3] : 0);
			return -1;
		}
	}
}

static int phonet_address(const char *ifname, uint8_t address)
{
	struct {
		struct nlmsghdr nh;
		struct ifaddrmsg ifa;
		char attrs[16];
	} req;
	memset(&req, 0, sizeof(req));
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return -1;
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifa));
	req.nh.nlmsg_type = RTM_NEWADDR;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;
	req.ifa.ifa_family = AF_PHONET;
	req.ifa.ifa_index = if_nametoindex(ifname);
	struct rtattr *rta = (void *)((char *)&req + NLMSG_ALIGN(req.nh.nlmsg_len));
	rta->rta_type = IFA_LOCAL;
	rta->rta_len = RTA_LENGTH(1);
	*(uint8_t *)RTA_DATA(rta) = address;
	req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);
	if (!req.ifa.ifa_index || send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
		close(fd);
		return -1;
	}
	char buf[512];
	ssize_t n = recv(fd, buf, sizeof(buf), 0);
	close(fd);
	struct nlmsghdr *nh = (void *)buf;
	if (n < (ssize_t)sizeof(*nh) || nh->nlmsg_type != NLMSG_ERROR)
		return -1;
	int err = ((struct nlmsgerr *)NLMSG_DATA(nh))->error;
	return err && err != -EEXIST ? -1 : 0;
}

static int interface_up(const char *ifname)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	int r = ioctl(fd, SIOCGIFFLAGS, &ifr);
	if (!r) {
		ifr.ifr_flags |= IFF_UP;
		r = ioctl(fd, SIOCSIFFLAGS, &ifr);
	}
	close(fd);
	return r;
}

static void print_address(const char *label, const uint8_t *sb, unsigned sblen)
{
	/* sub-block: id, len, ?, addr_len, addr */
	if (sblen < 4 || sb[3] != 4 || sblen < 8u)
		return;
	printf("%s %u.%u.%u.%u\n", label, sb[4], sb[5], sb[6], sb[7]);
}

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	if (argc != 2 || strlen(argv[1]) > 100) {
		fprintf(stderr, "usage: e5gprs <apn>\n");
		return 2;
	}
	const char *apn = argv[1];
	setvbuf(stdout, NULL, _IONBF, 0);
	if (interface_up("phonet0") || phonet_address("phonet0", HOST)) {
		perror("phonet0 setup");
		return 1;
	}
	dg = socket(AF_PHONET, SOCK_DGRAM, PN_PROTO_PHONET);
	struct sockaddr_pn local = {.spn_family = AF_PHONET, .spn_dev = HOST, .spn_obj = EVENT_OBJ};
	if (dg < 0 || bind(dg, (struct sockaddr *)&local, sizeof(local))) {
		perror("bind 6c:fc");
		return 1;
	}
	uint8_t r[300];

	/* subscribe to GPDS indications (the COMMGR frame the host side sends from 6c:fc) */
	const uint8_t sub[] = {0x10, 1, PN_GPDS};
	struct sockaddr_pn commgr = {.spn_family = AF_PHONET, .spn_dev = 0, .spn_resource = PN_COMMGR};
	uint8_t subm[] = {0, sub[0], sub[1], sub[2]};
	sendto(dg, subm, sizeof(subm), 0, (struct sockaddr *)&commgr, sizeof(commgr));
	dump("tx subscribe", subm, sizeof(subm));

	/* the GPDS server's object (ofono: the object of its version reply) */
	const uint8_t version[] = {0xf0, 0x12, 0};
	struct sockaddr_pn from;
	socklen_t fl = sizeof(from);
	uint8_t vm[] = {++tid, version[0], version[1], version[2]};
	struct sockaddr_pn gp = {.spn_family = AF_PHONET, .spn_dev = 0, .spn_resource = PN_GPDS};
	sendto(dg, vm, sizeof(vm), 0, (struct sockaddr *)&gp, sizeof(gp));
	struct pollfd p = {.fd = dg, .events = POLLIN};
	uint8_t gpds_dev = 0, gpds_obj = 0;
	for (;;) {
		if (poll(&p, 1, 3000) <= 0) {
			fprintf(stderr, "GPDS does not answer\n");
			return 1;
		}
		ssize_t n = recvfrom(dg, r, sizeof(r), 0, (struct sockaddr *)&from, &fl);
		dump("rx version", r, n);
		if (n >= 3 && r[0] == tid && r[1] == 0xf0 && r[2] == 0x13 && from.spn_resource == PN_GPDS) {
			gpds_dev = from.spn_dev;
			gpds_obj = from.spn_obj;
			break;
		}
		if (n >= 3 && r[0] == tid && r[1] == 0xf0 && r[2] == 0x14) {
			fprintf(stderr, "GPDS not identified\n");
			return 1;
		}
	}
	printf("GPDS server %02x:%02x, ISI %u.%u\n", gpds_dev, gpds_obj, r[3], r[4]);

	const uint8_t status[] = {0x19};
	if (request(0, PN_GPDS, 0, status, sizeof(status), 0x1a, r, 3000) < 0)
		return 1;
	const uint8_t attach[] = {0x13, 0x00};	/* GPDS_ATTACH_REQ, follow off */
	if (request(0, PN_GPDS, 0, attach, sizeof(attach), 0x14, r, 30000) < 0)
		printf("attach request unanswered, trying the context anyway\n");

	/* the Linux pipe end point */
	int pep = socket(AF_PHONET, SOCK_SEQPACKET, 0);
	uint16_t handle = 0;
	if (pep < 0 || setsockopt(pep, SOL_SOCKET, SO_BINDTODEVICE, "phonet0", 8) || listen(pep, 1) || ioctl(pep, SIOCPNGETOBJECT, &handle)) {
		perror("pipe end point");
		return 1;
	}
	fcntl(pep, F_SETFL, O_NONBLOCK | fcntl(pep, F_GETFL));
	printf("pipe end point %04x\n", handle);
	listen_pep = pep;
	const uint8_t create[] = {0x00 /* PNS_PIPE_CREATE_REQ */, 0 /* disabled after */, 0 /* priority */,
				  handle >> 8, handle & 0xff, PN_PEP_TYPE_GPRS, 0,
				  gpds_dev, gpds_obj, PN_PEP_TYPE_GPRS, 0};
	ssize_t n = request(0, PN_PIPE, 0, create, sizeof(create), 0x01, r, 5000);
	if (n < 4 || r[2] == 0xff) {
		printf("pipe create failed (error %02x)\n", n >= 4 ? r[3] : 0xff);
		return 1;
	}
	uint8_t pipe = r[2];
	printf("pipe handle %02x\n", pipe);
	int gprs = gprs_sock;
	for (int i = 0; i < 50 && gprs < 0; i++) {
		gprs = accept(pep, NULL, NULL);
		if (gprs < 0)
			usleep(100000);
	}
	int encap = PNPIPE_ENCAP_IP;
	if (gprs < 0 || setsockopt(gprs, SOL_PNPIPE, PNPIPE_ENCAP, &encap, sizeof(encap))) {
		perror("pipe accept/encap");
		return 1;
	}
	unsigned ifindex = 0;
	socklen_t il = sizeof(ifindex);
	getsockopt(gprs, SOL_PNPIPE, PNPIPE_IFINDEX, &ifindex, &il);
	char ifname[IF_NAMESIZE] = "?";
	if_indextoname(ifindex, ifname);
	printf("IFNAME %s\n", ifname);

	const uint8_t cid_create[] = {0x02};
	if (request(0, PN_GPDS, 0, cid_create, sizeof(cid_create), 0x03, r, 5000) < 4 || r[3] != 0x01) {
		printf("context id create failed\n");
		return 1;
	}
	uint8_t cid = r[2];
	printf("context id %02x\n", cid);
	const uint8_t ll[] = {0x00, cid, pipe, 0x02 /* GPDS_LL_PLAIN */};
	if (request(0, PN_GPDS, 0, ll, sizeof(ll), 0x01, r, 5000) < 4 || r[3] != 0x01) {
		printf("link configure failed\n");
		return 1;
	}
	uint8_t conf[160];
	size_t al = strlen(apn), sbl = (3 + al + 3) & ~3u, c = 0;
	const uint8_t head[] = {0x06, cid, 0x21 /* IPv4 */, 0x00 /* normal */, cid, 0x00, 2,
				0x90 /* DNS address request */, 4, 0, 0, 0x05 /* APN */, (uint8_t)sbl, (uint8_t)al};
	memcpy(conf, head, sizeof(head));
	c = sizeof(head);
	memcpy(conf + c, apn, al);
	c += al;
	while (c < sizeof(head) - 3 + sbl)
		conf[c++] = 0;
	if (request(0, PN_GPDS, 0, conf, c, 0x07, r, 5000) < 4 || r[3] != 0x01) {
		printf("context configure failed\n");
		return 1;
	}
	const uint8_t act[] = {0x08, cid, 0};
	if (request(0, PN_GPDS, 0, act, sizeof(act), 0x09, r, 30000) < 4) {
		printf("context activate unanswered\n");
		return 1;
	}
	if (r[3] != 0x01)
		printf("activate response status %02x cause %02x\n", r[3], r[4]);
	const uint8_t enable[] = {0x06 /* PNS_PIPE_ENABLE_REQ */, pipe, 0};
	request(0, PN_PIPE, 0, enable, sizeof(enable), 0x07, r, 5000);
	n = indication(PN_GPDS, 0x0a, r, 60000);
	if (n < 0) {
		printf("no context activation\n");
		return 1;
	}
	/* GPDS_CONTEXT_ACTIVATE_IND: tid, id, cid, sub-block count at 3, sub-blocks from 4 */
	for (ssize_t o = 4; o + 2 <= n && r[o + 1] >= 2 && o + r[o + 1] <= n; o += r[o + 1]) {
		if (r[o] == 0x04)
			print_address("IP", r + o, r[o + 1]);
		else if (r[o] == 0x0d)
			print_address("DNS1", r + o, r[o + 1]);
		else if (r[o] == 0x0e)
			print_address("DNS2", r + o, r[o + 1]);
	}
	printf("CONTEXT ACTIVE on %s\n", ifname);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	while (!stop) {	/* keep the pipe, report GPDS indications */
		struct pollfd w = {.fd = dg, .events = POLLIN};
		if (poll(&w, 1, 1000) > 0) {
			n = recvfrom(dg, r, sizeof(r), 0, (struct sockaddr *)&from, &fl);
			if (n > 0)
				dump("ind", r, n);
		}
	}
	const uint8_t deact[] = {0x0b, cid};
	request(0, PN_GPDS, 0, deact, sizeof(deact), 0x0c, r, 5000);
	close(gprs);
	close(pep);
	return 0;
}

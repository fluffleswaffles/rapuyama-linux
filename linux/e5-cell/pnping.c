/* pnping: Linux side of the E5 ISASW runtime test. Gives phonet0 the host
 * address 0x6c (rtnetlink, busybox ip has no Phonet), brings it up and sends
 * ISI version requests (COMMON_MESSAGE 0xf0 / ISI_VERSION_GET_REQ 0x12) to
 * <dev>:<resource> over a Phonet datagram socket, printing the replies.
 *   pnping [dev:res ...]        default 00:15 (the firmware's MTC)
 * needs e5rt running with --backend /dev/rapuyama-isi */
#include <errno.h>
#include <net/if.h>
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
	if (err && err != -EEXIST) {
		errno = -err;
		return -1;
	}
	return 0;
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

int main(int argc, char **argv)
{
	if (interface_up("phonet0") || phonet_address("phonet0", 0x6c)) {
		perror("phonet0 setup");
		return 1;
	}
	int fd = socket(AF_PHONET, SOCK_DGRAM, PN_PROTO_PHONET);
	if (fd < 0) {
		perror("phonet socket");
		return 1;
	}
	struct sockaddr_pn local = {.spn_family = AF_PHONET, .spn_dev = 0x6c, .spn_obj = 0x24};
	if (bind(fd, (struct sockaddr *)&local, sizeof(local))) {
		perror("bind");
		return 1;
	}
	struct timeval tv = {.tv_sec = 3};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	int failures = 0;
	for (int i = 1; i < (argc > 1 ? argc : 2); i++) {
		unsigned dev = 0x00, res = 0x15;
		if (argc > 1 && sscanf(argv[i], "%x:%x", &dev, &res) != 2) {
			fprintf(stderr, "usage: pnping [dev:res ...]\n");
			return 2;
		}
		unsigned char msg[] = {i, 0xf0, 0x12, 0};	/* tid, COMMON_MESSAGE, ISI_VERSION_GET_REQ, filler */
		struct sockaddr_pn to = {.spn_family = AF_PHONET, .spn_dev = dev, .spn_resource = res};
		if (sendto(fd, msg, sizeof(msg), 0, (struct sockaddr *)&to, sizeof(to)) != sizeof(msg)) {
			perror("sendto");
			return 1;
		}
		unsigned char reply[256];
		struct sockaddr_pn from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from, &fl);
		if (n < 0) {
			printf("%02x:%02x: no reply (%s)\n", dev, res, strerror(errno));
			failures++;
			continue;
		}
		printf("%02x:%02x: reply from dev %02x obj %02x res %02x:", dev, res, from.spn_dev, from.spn_obj, from.spn_resource);
		for (ssize_t j = 0; j < n; j++)
			printf(" %02x", reply[j]);
		if (n >= 5 && reply[1] == 0xf0 && reply[2] == 0x13)
			printf("  -> ISI version %u.%u", reply[3], reply[4]);
		else if (n >= 3 && reply[1] == 0xf0 && reply[2] == 0x14)
			printf("  -> service not identified");
		printf("\n");
	}
	close(fd);
	return failures ? 1 : 0;
}

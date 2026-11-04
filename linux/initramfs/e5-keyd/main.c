/* e5-keyd: reads the E5 keypad and power key, shows the Fn key picture on
 * the fb overlay (rapuyama-e5-fb.c) while the Fn layer is on, and switches
 * the screen off/on with the power key. Started by init.sh (E5 only).
 * Logs to the kernel log; exits quietly when the keypad or the overlay is
 * missing (other phone, older Image). */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "keyd.h"

#define OVL	"/sys/class/graphics/fb0/device/overlay"
#define OVL_ON	OVL "_on"
/* the picture's fonts from the sd rootfs (font-terminus, kbd-misc); simple
 * mode has no font files: then the console font (fbcon=font:, VGA8x8) */
#define FONTS	"/usr/share/consolefonts/"
#define BIG	FONTS "ter-u16n.psf.gz"
#define SMALL	FONTS "cp850-8x8.psfu.gz"

static void klog(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		dprintf(fd, "e5-keyd: %s\n", msg);
		close(fd);
	}
}

static int find_input(const char *name)
{
	char path[32], got[64];

	for (int i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

		if (fd < 0)
			continue;
		memset(got, 0, sizeof(got));
		if (ioctl(fd, EVIOCGNAME(sizeof(got) - 1), got) >= 0 && !strcmp(got, name))
			return fd;
		close(fd);
	}
	return -1;
}

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static void put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		write_all(fd, s, strlen(s));
		close(fd);
	}
}

static long read_long(const char *path, long fallback)
{
	char buf[32] = "";
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	long v = fallback;

	if (fd >= 0) {
		if (read(fd, buf, sizeof(buf) - 1) > 0)
			sscanf(buf, "%ld", &v);
		close(fd);
	}
	return v;
}

/* the console blanks itself after consoleblank seconds without a key (and a
 * key, the power key too, wakes it): count that as blanked, so the power key
 * then wakes instead of blanking again. not while X has the screen (vt2) */
static int console_blanked(long idle)
{
	long after = read_long("/sys/module/kernel/parameters/consoleblank", 0);
	char vt[16] = "";
	int fd = open("/sys/class/tty/tty0/active", O_RDONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (read(fd, vt, sizeof(vt) - 1) < 0)
			vt[0] = 0;
		close(fd);
	}
	return after > 0 && idle >= after && !strncmp(vt, "tty1", 4);
}

/* a font file, plain or gzip, through zcat -f; 0 bytes when missing */
static size_t slurp(const char *path, unsigned char *buf, size_t max)
{
	char cmd[128];
	size_t n = 0, got;
	FILE *p;

	if (access(path, R_OK))
		return 0;
	snprintf(cmd, sizeof(cmd), "zcat -f %s 2>/dev/null", path);
	p = popen(cmd, "r");
	if (!p)
		return 0;
	while (n < max && (got = fread(buf + n, 1, max - n, p)) > 0)
		n += got;
	pclose(p);
	return n;
}

/* the tty1 console font; width 8 = the kernel refuses wider fonts instead
 * of writing more than the buffer holds */
static int console_font(struct keyd_font *f)
{
	static unsigned char data[512 * 32];
	struct console_font_op op = {
		.op = KD_FONT_OP_GET, .width = 8, .height = 32, .charcount = 512,
		.data = data,
	};
	int fd = open("/dev/tty1", O_RDONLY | O_NOCTTY | O_CLOEXEC), r;

	if (fd < 0)
		return -1;
	r = ioctl(fd, KDFONTOP, &op);
	close(fd);
	return r < 0 ? -1 : keyd_font_console(f, data, op.width, op.height, op.charcount);
}

static const struct keyd_font *font(struct keyd_font *f, const char *path,
				    unsigned char *buf, size_t max,
				    const struct keyd_font *con)
{
	return keyd_font_psf(f, buf, slurp(path, buf, max)) ? con : f;
}

static long now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static void apply(int fb, unsigned a)
{
	if (a & KEYD_HIDE)
		put(OVL_ON, "0");
	if (a & KEYD_BLANK && fb >= 0)
		ioctl(fb, FBIOBLANK, FB_BLANK_POWERDOWN);
	if (a & KEYD_UNBLANK && fb >= 0)
		ioctl(fb, FBIOBLANK, FB_BLANK_UNBLANK);
	if (a & KEYD_SHOW)
		put(OVL_ON, "1");
}

int main(void)
{
	static uint32_t px[KEYD_H][KEYD_W];
	static unsigned char bigbuf[65536], smallbuf[65536];
	struct keyd_font big, small, con;
	const struct keyd_font *pb, *ps, *cf;
	struct pollfd pfd[2];
	struct keyd_state s = { 0 };
	int n = 0, fd, fb;
	long last;

	pfd[n].fd = find_input("Nokia E5 keyboard");
	if (pfd[n].fd < 0) {
		klog("no E5 keypad, exiting");
		return 0;
	}
	pfd[n++].events = POLLIN;
	pfd[n].fd = find_input("Nokia E5 power key");
	/* ours alone: the console would wake the screen again on this key */
	if (pfd[n].fd >= 0 && ioctl(pfd[n].fd, EVIOCGRAB, 1) < 0)
		klog("power key grab failed, the console sees it too");
	if (pfd[n].fd >= 0)
		pfd[n++].events = POLLIN;
	else
		klog("no power key input, fn picture only");

	cf = console_font(&con) ? NULL : &con;
	pb = font(&big, BIG, bigbuf, sizeof(bigbuf), cf);
	ps = font(&small, SMALL, smallbuf, sizeof(smallbuf), cf);
	klog(pb == &big && ps == &small ? "font: ter-u16n + cp850-8x8" :
	     cf ? "font: the console font (no font files)" : "no font, picture without text");
	keyd_render(px, pb, ps);
	fd = open(OVL, O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write_all(fd, px, sizeof(px))) {
		klog("no fb overlay (older Image?), exiting");
		return 0;
	}
	close(fd);
	put(OVL_ON, "0");
	fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	klog(n == 2 ? "fn picture + power key ready" : "fn picture ready");

	last = now();
	for (;;) {
		struct input_event ev[16];

		if (poll(pfd, n, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (int i = 0; i < n; i++) {
			if (!(pfd[i].revents & POLLIN)) {
				if (pfd[i].revents & (POLLERR | POLLHUP))
					return 0;
				continue;
			}
			ssize_t got = read(pfd[i].fd, ev, sizeof(ev));

			for (int k = 0; k < got / (ssize_t)sizeof(ev[0]); k++) {
				if (ev[k].type != EV_KEY)
					continue;
				/* one latch per edge: act once per tap */
				if (ev[k].code == KEYD_POWER_CODE && ev[k].value == 1 &&
				    !keyd_power_edge(&s, ev[k].input_event_sec * 1000L +
						     ev[k].input_event_usec / 1000))
					continue;
				if (ev[k].code == KEYD_POWER_CODE && ev[k].value == 1 &&
				    !s.blanked && console_blanked(now() - last))
					s.blanked = true;
				if (ev[k].value == 1)
					last = now();
				apply(fb, keyd_event(&s, ev[k].code, ev[k].value));
			}
		}
	}
	return 0;
}

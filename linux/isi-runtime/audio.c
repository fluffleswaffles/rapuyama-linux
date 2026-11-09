#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <linux/phonet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/file.h>
#include <alsa/asoundlib.h>
#include "../yamashell/level.h"

#define RING 65280u
#define PREFILL 19200u

#define PREFETCH_FRAMES ((5 + 7 + 9 + 11 + 13 + 15 + 34 + 17) * 240u)

#ifndef CAPTURE_FRAMES
#define CAPTURE_FRAMES (4 * ((PREFETCH_FRAMES + 959) / 960) * 960)
#endif
#define STATE "/run/rapuyama-audio"

static volatile sig_atomic_t stopped;
static void stop(int sig)
{
	(void)sig;
	stopped = 1;
}
static void fail(const char *what)
{
	perror(what);
	exit(1);
}
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void publish(unsigned char *p, unsigned tid, unsigned pos, unsigned frames)
{
	const unsigned char base[] = {0, 0x34, 0, 0, 0, 0, 0, 0, 0, 5, 0, 1, 2, 0};
	memcpy(p, base, sizeof(base));
	p[0] = (unsigned char)tid;
	p[4] = (unsigned char)(pos >> 24);
	p[5] = (unsigned char)(pos >> 16);
	p[6] = (unsigned char)(pos >> 8);
	p[7] = (unsigned char)pos;
	p[9] = (unsigned char)frames;
}
static int position(const unsigned char *p, size_t n, unsigned tid, unsigned *out)
{
	if (n != 8 || p[0] != tid || p[1] != 0x35 || p[2] || p[3])
		return -1;
	unsigned pos = be32(p + 4);
	if (pos >= RING || pos % 8)
		return -1;
	*out = pos;
	return 0;
}
static void fill(unsigned char *ring, unsigned pos, const unsigned char *data, size_t size, size_t offset, unsigned count)
{
	assert(pos < RING && count <= RING);
	for (unsigned i = 0; i < count; i++)
		ring[(pos + i) % RING] = offset + i < size ? data[offset + i] : 0;
	__sync_synchronize();
}
static void selftest(void)
{
	unsigned char p[14];
	publish(p, 1, 19200, 5);
	assert(!memcmp(p, (unsigned char[]){1, 0x34, 0, 0, 0, 0, 0x4b, 0, 0, 5, 0, 1, 2, 0}, 14));
	unsigned pos;
	unsigned char reply[] = {1, 0x35, 0, 0, 0, 0, 0xf, 0};
	assert(!position(reply, 8, 1, &pos) && pos == 3840);
	assert(position(reply, 7, 1, &pos));
	assert(position(reply, 8, 2, &pos));
	reply[7] = 1;
	assert(position(reply, 8, 1, &pos));
	reply[4] = 1;
	assert(position(reply, 8, 1, &pos));
	unsigned char *ring = malloc(RING);
	assert(ring);
	memset(ring, 0xaa, RING);
	const unsigned char data[] = {1, 2, 3, 4, 5, 6, 7, 8};
	fill(ring, RING - 4, data, sizeof(data), 0, 8);
	assert(!memcmp(ring + RING - 4, data, 4) && !memcmp(ring, data + 4, 4) && ring[4] == 0xaa);
	fill(ring, 4, data, sizeof(data), 6, 8);
	assert(ring[4] == 7 && ring[5] == 8 && ring[6] == 0 && ring[11] == 0 && ring[12] == 0xaa);
	free(ring);
	unsigned written = PREFILL, consumed = 0;
	for (unsigned i = 0; i < 80; i++)
	{
		publish(p, 1, written % RING, 5);
		assert(be32(p + 4) == written % RING && p[9] == 5);
		consumed += 9600;
		unsigned need = PREFILL - (written - consumed);
		assert(need == 9600);
		written += need;
		assert(written - consumed == PREFILL);
	}
	puts("PASS original mode0 fixed depth/field5, strict35 parser, ring wrap and EOF padding");
}

static const unsigned char route_open[] = {0x3f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x07, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const unsigned char route_empty[] = {0x3f, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const unsigned char close_channel[] = {0x3d, 0x00, 0x04};

static const unsigned char close_pcm[] = {0x32, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x03, 0x02, 0x00, 0x00, 0x22};

static const unsigned char open_pcm[] = {0x32, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x03, 0x02, 0x00, 0x00, 0x22};

static const unsigned char open_channel[] = {0x3b, 0x00, 0x04, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x06, 0x00, 0x07};
struct session
{
	int fd, owner;
	unsigned tid, app;
	int opened, channel, published;
};
static double now(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		fail("clock");
	return t.tv_sec + t.tv_nsec / 1e9;
}
static int alive(struct session *s) { return !kill(s->owner, 0) && access("/tmp/isasw.exit", F_OK); }
static int matches(const unsigned char *p, size_t n, unsigned tid, unsigned op) { return n >= 2 && p[0] == tid && p[1] == op; }
static int exchange(struct session *s, unsigned resource, const unsigned char *body, size_t size, unsigned opcode, unsigned char *reply)
{
	unsigned char packet[256];
	if (!size || size > 255)
	{
		errno = EINVAL;
		return -1;
	}
	s->tid = s->tid == 74 ? 1 : s->tid + 1;
	packet[0] = (unsigned char)s->tid;
	memcpy(packet + 1, body, size);
	struct sockaddr_pn remote = {.spn_family = AF_PHONET, .spn_resource = (unsigned char)resource};
	if (sendto(s->fd, packet, size + 1, 0, (void *)&remote, sizeof(remote)) != (ssize_t)size + 1)
	{
		perror("ISI send");
		return -1;
	}
	double end = now() + 3;
	while (now() < end && alive(s))
	{
		struct pollfd wait = {.fd = s->fd, .events = POLLIN};
		int ms = (int)((end - now()) * 1000);
		if (ms < 1)
			break;
		int r = poll(&wait, 1, ms);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		struct sockaddr_pn from;
		socklen_t len = sizeof(from);
		ssize_t n = recvfrom(s->fd, reply, 256, 0, (void *)&from, &len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
		{
			perror("ISI receive");
			return -1;
		}
		if (from.spn_resource != resource || (resource == 0xf3 && (from.spn_dev & 0xfc) != 0x7c))
			continue;
		if (matches(reply, (size_t)n, s->tid, opcode))
			return (int)n;
		if (n >= 2 && reply[0] == s->tid && reply[1] == 0xf0)
		{
			fputs("ISI routing error\n", stderr);
			return -1;
		}
	}
	fprintf(stderr, "ISI timeout resource=%02x tid=%u request=%02x expected=%02x\n", resource, s->tid, body[0], opcode);
	errno = ETIMEDOUT;
	perror("ISI reply timeout");
	return -1;
}
static int dsp_control(struct session *s, const unsigned char *body, size_t size, unsigned opcode, unsigned status_offset)
{
	unsigned char reply[256];
	int n = exchange(s, 0xf3, body, size, opcode, reply);
	if (n < (int)status_offset + 2 || reply[status_offset] || reply[status_offset + 1])
	{
		fputs("DSP control failed\n", stderr);
		return -1;
	}
	return 0;
}
static int pcm_enable(struct session *s, int enable)
{
	unsigned char body[] = {0x51, 0, 0, 0, 0, 0, 1, 0, 0x72, 0, 8, 0, 0, 0, 1};
	body[12] = (unsigned char)enable;
	return dsp_control(s, body, sizeof(body), 0x52, 2);
}
static int audio_control(struct session *s, const unsigned char *parameters, size_t size)
{
	unsigned char body[256], reply[256];
	if (size > 255)
		return -1;
	memcpy(body, parameters, size);
	body[1] = (unsigned char)s->app;
	int n = exchange(s, 0x0b, body, size, 0x15, reply);
	if (n < 4 || reply[2] != body[2] || reply[3])
	{
		fputs("audio control failed\n", stderr);
		return -1;
	}
	return 0;
}
static int audio_start(struct session *s)
{
	unsigned char reply[256];
	const unsigned char reg[] = {0, 1, 0};
	int n = exchange(s, 0x0b, reg, sizeof(reg), 1, reply);
	if (n != 4 || !reply[2] || reply[3])
		return -1;
	s->app = reply[2];
	const unsigned char control0[] = {0x14, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x01, 0x00, 0x00, 0x0d, 0x00, 0x00};
	const unsigned char control1[] = {0x14, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00};
	const unsigned char control2[] = {0x14, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
	const unsigned char control3[] = {0x14, 0x00, 0x05, 0x00, 0x00, 0x00, 0x02, 0x03, 0x08, 0x0c, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x08, 0x05, 0x00, 0x00, 0x10, 0x00, 0x00};
	const unsigned char control4[] = {0x14, 0x00, 0x05, 0x00, 0x00, 0x00, 0x03, 0x03, 0x08, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x03, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x08, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
	if (audio_control(s, control0, sizeof(control0)) || audio_control(s, control1, sizeof(control1)) || audio_control(s, control2, sizeof(control2)) || audio_control(s, control3, sizeof(control3)) || audio_control(s, control4, sizeof(control4)))
		return -1;
	return 0;
}
static int cleanup(struct session *s)
{
	int failed = 0;
	if (!alive(s))
		return -1;
	if (s->published && pcm_enable(s, 0))
		failed = 1;
	if (s->opened)
	{
		if (dsp_control(s, close_pcm, sizeof(close_pcm), 0x33, 4))
			failed = 1;
		else
		{
			s->opened = 0;
			if (s->channel)
			{
				if (dsp_control(s, route_empty, sizeof(route_empty), 0x40, 2))
					failed = 1;
				if (dsp_control(s, close_channel, sizeof(close_channel), 0x3e, 4))
					failed = 1;
				else
					s->channel = 0;
			}
		}
	}
	if (!failed && s->app)
	{
		unsigned char body[] = {4, (unsigned char)s->app, 0}, reply[256];
		int n = exchange(s, 0x0b, body, sizeof(body), 5, reply);
		if (n < 4 || reply[2] || reply[3])
			failed = 1;
		else
			s->app = 0;
	}
	return failed ? -1 : 0;
}
static void keep_endpoint(struct session *s)
{
	puts("SOCKET_HELD cleanup unconfirmed; endpoint24 retained, no further writes");
	fflush(stdout);
	while (alive(s))
	{
		struct pollfd p = {.fd = s->fd, .events = POLLIN};
		int r = poll(&p, 1, 1000);
		if (r > 0)
		{
			unsigned char b[256];
			ssize_t n = recv(s->fd, b, sizeof(b), 0);
			printf("DRAIN bytes=%zd tid=%u op=%02x\n", n, n > 0 ? b[0] : 0, n > 1 ? b[1] : 0);
			fflush(stdout);
		}
	}
}

struct source
{
	snd_pcm_t *pcm;
	snd_ctl_t *ctl;
	snd_ctl_elem_value_t *active;
	int16_t prefetch[PREFETCH_FRAMES * 2];
	unsigned pending, at;
	int eof;
	uint64_t frames, end;
};
static int playback_active(struct source *in)
{
	int r = snd_ctl_elem_read(in->ctl, in->active);
	return r < 0 ? r : snd_ctl_elem_value_get_boolean(in->active, 0);
}
static int speech_active(void)
{
	if (!access("/tmp/isasw-call-lock", F_OK))
		return 1;
	FILE *f = fopen("/run/rapuyama-modem/calls", "r");
	unsigned long long stamp;
	unsigned id;
	int state, incoming, active = 0;
	if (f)
	{
		if (fscanf(f, "%llu %x %d %d", &stamp, &id, &state, &incoming) == 4 && stamp <= (unsigned long long)now() && now() - stamp < 8)
			active = state == 7 || state == 8 || (!incoming && state >= 1 && state <= 4);
		fclose(f);
	}
	return active;
}
static int capture_block(struct source *in, int16_t *pcm)
{
	size_t done = 0;
	while (done < 240 && !stopped)
	{
		snd_pcm_sframes_t n = snd_pcm_readi(in->pcm, pcm + done * 2, 240 - done);
		if (n == -EINTR)
			continue;
		if (n <= 0)
		{
			fprintf(stderr, "ALSA capture: %s\n", snd_strerror((int)n));
			return -1;
		}
		done += (size_t)n;
	}
	return done == 240 ? 1 : 0;
}
static int source_block(struct source *in, int16_t *pcm)
{
	if (in->eof)
		return 0;
	if (stopped || speech_active())
	{
		in->eof = 1;
		in->end = in->frames * 8;
		return 0;
	}
	if (in->pending)
	{
		memcpy(pcm, in->prefetch + in->at * 2, 960);
		in->pending -= 240;
		in->at += 240;
		in->frames += 240;
		return 1;
	}
	int active = playback_active(in);
	if (active < 0)
		return -1;
	if (!active && !in->end)
	{
		snd_pcm_sframes_t available = snd_pcm_avail_update(in->pcm);
		if (available < 0)
			return -1;
		in->end = (in->frames + (size_t)available) * 8;
		if (!in->end)
			in->eof = 1;
	}
	if (in->end && in->frames * 8 >= in->end)
	{
		in->eof = 1;
		return 0;
	}
	int r = capture_block(in, pcm);
	if (r <= 0)
		return r;
	in->frames += 240;
	return 1;
}
static void pack_block(unsigned char *out, const int16_t *pcm, int volume)
{
	for (unsigned channel = 0; channel < 2; channel++)
		for (unsigned frame = 0; frame < 240; frame++)
		{
			int32_t sample = (int32_t)pcm[frame * 2 + channel] * volume * 256 / 100;
			memcpy(out + (channel * 240 + frame) * 4, &sample, 4);
		}
}
static int fill_live(unsigned char *ring, unsigned pos, unsigned count, struct source *in)
{
	if (pos % 1920 || count % 1920 || count > RING)
		return -1;
	int volume = level_get(LEVEL_VOLUME);
	int16_t pcm[480];
	unsigned char block[RING];
	for (unsigned offset = 0; offset < count; offset += 1920)
	{
		int r = source_block(in, pcm);
		if (r < 0)
			return -1;
		if (r)
			pack_block(block + offset, pcm, volume);
		else
			memset(block + offset, 0, 1920);
	}
	fill(ring, pos, block, count, 0, count);
	return 0;
}
static int play(struct session *s, unsigned char *ring, struct source *in)
{
	unsigned writepos = PREFILL, lastread = 0, stalls = 0;
	uint64_t written = PREFILL, consumed = 0;
	double start = now();
	while (!stopped && alive(s))
	{
		if (speech_active())
			return 0;
		unsigned char packet[14], reply[256];
		publish(packet, 0, writepos, 5);
		s->published = 1;
		int n = exchange(s, 0xf3, packet + 1, 13, 0x35, reply);
		unsigned readpos;
		if (n < 0 || position(reply, (size_t)n, s->tid, &readpos))
			return -1;
		unsigned advance = (readpos + RING - lastread) % RING;
		if (advance > written - consumed || advance % 1920)
			return -1;
		consumed += advance;
		lastread = readpos;
		if (in->eof && consumed >= in->end)
		{
			usleep(100000);
			printf("PCM_COMPLETE consumed=%" PRIu64 " seconds=%.3f\n", consumed, now() - start);
			return 0;
		}
		unsigned queued = (unsigned)(written - consumed), target = PREFILL;
		if (queued > target)
			return -1;
		unsigned need = target - queued;
		if (!need)
		{
			if (++stalls > 50)
				return -1;
			usleep(5000);
		}
		else
			stalls = 0;
		if (fill_live(ring, writepos, need, in))
			return -1;
		written += need;
		writepos = (writepos + need) % RING;
	}
	return stopped ? 0 : -1;
}
static int save_state(const char *boot, int owner, unsigned address, int clean)
{
	FILE *f = fopen(STATE "/state.new", "w");
	if (!f)
		return -1;
	int failed = fprintf(f, "%s %d %x %d\n", boot, owner, address, clean) < 0;
	if (fclose(f))
		failed = 1;
	return failed ? -1 : rename(STATE "/state.new", STATE "/state");
}
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--self-test"))
	{
		selftest();
		int16_t pcm[480];
		unsigned char out[1920];
		for (unsigned i = 0; i < 240; i++)
		{
			pcm[2 * i] = (int16_t)(i - 120);
			pcm[2 * i + 1] = (int16_t)(120 - i);
		}
		pack_block(out, pcm, 100);
		for (unsigned c = 0; c < 2; c++)
			for (unsigned i = 0; i < 240; i++)
			{
				int32_t v;
				memcpy(&v, out + (c * 240 + i) * 4, 4);
				assert(v == (int32_t)pcm[i * 2 + c] * 256);
			}
		pack_block(out, pcm, 0);
		for (unsigned i = 0; i < sizeof(out); i++)
			assert(!out[i]);
		puts("PASS original240-sample planes, Q23 conversion and mute");
		return 0;
	}
	if (argc != 1)
	{
		fputs("usage: modem-pcm [--self-test]\n", stderr);
		return 2;
	}
	umask(0077);
	if (mkdir(STATE, 0700) && errno != EEXIST)
		fail("state directory");
	int lock = open(STATE "/lock", O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
		fail("audio already running");
	char boot[64], cached[64];
	FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
	if (!f || !fgets(boot, sizeof(boot), f))
		fail("boot");
	fclose(f);
	boot[strcspn(boot, "\n")] = 0;
	struct session s = {.fd = -1};
	f = fopen("/tmp/isasw.pid", "r");
	if (!f || fscanf(f, "%d", &s.owner) != 1 || s.owner <= 1)
		fail("owner");
	fclose(f);
	if (!alive(&s))
		fail("runtime unavailable");
	unsigned address = 0;
	int owner, clean;
	f = fopen(STATE "/state", "r");
	if (f)
	{
		int n = fscanf(f, "%63s %d %x %d", cached, &owner, &address, &clean);
		fclose(f);
		if (n != 4 || strcmp(boot, cached) || owner != s.owner || clean != 1)
		{
			fputs("unclean or stale audio state; fresh boot required\n", stderr);
			return 1;
		}
	}
	s.fd = socket(AF_PHONET, SOCK_DGRAM, PN_PROTO_PHONET);
	if (s.fd < 0)
		fail("Phonet");
	if (setsockopt(s.fd, SOL_SOCKET, SO_BINDTODEVICE, "phonet0", 8))
		fail("bind device");
	struct sockaddr_pn local = {.spn_family = AF_PHONET, .spn_dev = 0x6c, .spn_obj = 0x24};
	if (bind(s.fd, (void *)&local, sizeof(local)))
		fail("bind endpoint24");
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	signal(SIGHUP, stop);
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!address)
	{
		unsigned char reply[256];
		int n = exchange(&s, 0xc9, (unsigned char[]){0x0d, 0xf0, 1, 0x1c, 4, 0, 0}, 7, 0x0e, reply);
		if (n != 8 || memcmp(reply + 1, (unsigned char[]){0x0e, 0xf0, 1, 8, 4, 2, 1}, 7))
			fail("NO_CALL required for initial audio setup");
		n = exchange(&s, 0xf3, (unsigned char[]){0x30, 0, 0}, 3, 0x31, reply);
		if (n < 36 || reply[2] || reply[3] || be32(reply + 20) != RING)
			fail("DSP buffer info");
		address = be32(reply + 4);
	}
	if (address < 0x140000 || address + RING > 0x1ff000 || address % 4 || sysconf(_SC_PAGESIZE) != 4096)
		fail("invalid DSP buffer");
	if (save_state(boot, s.owner, address, 1))
		fail("save audio state");
	unsigned long physical = 0x10000000UL + address;
	size_t offset = physical % 4096, length = RING + offset;
	int mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem < 0)
		fail("/dev/mem");
	void *map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, mem, physical - offset);
	if (map == MAP_FAILED)
		fail("mmap");
	unsigned char *ring = (unsigned char *)map + offset;
	struct source in = {0};
	int r = snd_pcm_open(&in.pcm, "hw:E6Bridge,1,0", SND_PCM_STREAM_CAPTURE, 0);
	if (r < 0)
	{
		fputs(snd_strerror(r), stderr);
		return 1;
	}
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	if ((r = snd_pcm_hw_params_any(in.pcm, hw)) < 0 ||
		(r = snd_pcm_hw_params_set_access(in.pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
		(r = snd_pcm_hw_params_set_format(in.pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
		(r = snd_pcm_hw_params_set_channels(in.pcm, hw, 2)) < 0 ||
		(r = snd_pcm_hw_params_set_rate(in.pcm, hw, 48000, 0)) < 0 ||
		(r = snd_pcm_hw_params_set_period_size(in.pcm, hw, 960, 0)) < 0 ||
		(r = snd_pcm_hw_params_set_buffer_size(in.pcm, hw, CAPTURE_FRAMES)) < 0 ||
		(r = snd_pcm_hw_params(in.pcm, hw)) < 0 ||
		(r = snd_pcm_sw_params_current(in.pcm, sw)) < 0 ||
		(r = snd_pcm_sw_params_set_avail_min(in.pcm, sw, 240)) < 0 ||
		(r = snd_pcm_sw_params_set_start_threshold(in.pcm, sw, 1)) < 0 ||
		(r = snd_pcm_sw_params(in.pcm, sw)) < 0)
	{
		fputs(snd_strerror(r), stderr);
		return 1;
	}
	if ((r = snd_pcm_start(in.pcm)) < 0)
	{
		fputs(snd_strerror(r), stderr);
		return 1;
	}
	if (snd_ctl_open(&in.ctl, "hw:E6Bridge", 0) < 0 || snd_ctl_elem_value_malloc(&in.active) < 0)
		fail("ALSA control");
	snd_ctl_elem_value_set_interface(in.active, SND_CTL_ELEM_IFACE_PCM);
	snd_ctl_elem_value_set_device(in.active, 1);
	snd_ctl_elem_value_set_subdevice(in.active, 0);
	snd_ctl_elem_value_set_name(in.active, "PCM Slave Active");
	f = fopen(STATE "/pid", "w");
	if (!f)
		fail("pid");
	fprintf(f, "%d\n", getpid());
	if (fclose(f))
		fail("pid write");
	printf("AUDIO_READY buffer=%08lx owner=%d mode=0 capture=%u\n", physical, s.owner, (unsigned)CAPTURE_FRAMES);
	int result = 0;
	while (!stopped && alive(&s))
	{
		in.pending = 0;
		int n = capture_block(&in, in.prefetch);
		if (!n)
			break;
		if (n < 0)
		{
			if (snd_pcm_prepare(in.pcm) < 0 || snd_pcm_start(in.pcm) < 0)
			{
				result = 1;
				break;
			}
			continue;
		}
		int active = playback_active(&in);
		if (active < 0)
		{
			result = 1;
			break;
		}
		if (!active || speech_active())
			continue;
		in.pending = 240;
		in.at = 0;
		in.eof = 0;
		in.frames = 0;
		in.end = 0;
		s.published = 0;
		while (in.pending < PREFETCH_FRAMES && !stopped)
		{
			if (capture_block(&in, in.prefetch + in.pending * 2) <= 0)
			{
				result = 1;
				break;
			}
			in.pending += 240;
		}
		if (result || stopped)
			break;
		if (speech_active())
			continue;
		if (save_state(boot, s.owner, address, 0))
		{
			result = 1;
			break;
		}
		if (dsp_control(&s, open_pcm, sizeof(open_pcm), 0x33, 4))
		{
			keep_endpoint(&s);
			result = 1;
			break;
		}
		s.opened = 1;
		int played = -1;

		if (!fill_live(ring, 0, PREFILL, &in) && !dsp_control(&s, open_channel, sizeof(open_channel), 0x3c, 4))
		{
			s.channel = 1;
			if (!dsp_control(&s, route_open, sizeof(route_open), 0x40, 2) && !audio_start(&s))
				played = play(&s, ring, &in);
		}
		if (cleanup(&s))
		{
			keep_endpoint(&s);
			result = 1;
			break;
		}
		if (save_state(boot, s.owner, address, 1))
		{
			result = 1;
			break;
		}
		if (played < 0)
		{
			fputs("PCM stream failed; DSP closed\n", stderr);
			result = 1;
			break;
		}
		puts("AUDIO_IDLE DSP stream closed");
	}
	if (s.opened || s.app)
	{
		if (cleanup(&s))
			keep_endpoint(&s);
		else
			save_state(boot, s.owner, address, 1);
	}
	unlink(STATE "/pid");
	snd_ctl_elem_value_free(in.active);
	snd_ctl_close(in.ctl);
	snd_pcm_close(in.pcm);
	munmap(map, length);
	close(mem);
	close(s.fd);
	close(lock);
	return result;
}
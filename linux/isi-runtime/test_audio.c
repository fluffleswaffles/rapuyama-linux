#define main pcm_session_main
#define sendto peer_sendto
#define recvfrom peer_recvfrom
#define poll peer_poll
#define kill peer_kill
#define access peer_access
#define usleep peer_usleep
#define snd_pcm_readi peer_pcm_readi
#define snd_pcm_avail_update peer_pcm_avail
#define snd_ctl_elem_read peer_ctl_read
#define snd_ctl_elem_value_get_boolean peer_ctl_boolean
#include "audio.c"

#undef main

/* testing formatters bc people said my code style sucks ass */

static unsigned char response[256], stale[8];
static size_t response_size;
static unsigned response_resource, read_cursor, opcodes[256], ops, publications, enabled_once;
static int active, stale_ready, stale_seen, omit35;
static unsigned step;
static unsigned char *test_ring;

int peer_kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    return 0;
}
int peer_access(const char *path, int mode)
{
    (void)path;
    (void)mode;
    errno = ENOENT;
    return -1;
}
int peer_usleep(useconds_t t)
{
    (void)t;
    return 0;
}
int peer_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)fds;
    (void)count;
    (void)timeout;
    return response_size ? 1 : 0;
}

ssize_t peer_sendto(int fd, const void *buffer, size_t count, int flags, const struct sockaddr *address, socklen_t length)
{
    (void)flags;
    (void)length;
    assert(fd == 7 && count >= 2 && ops < 256);
    const unsigned char *p = buffer;
    const struct sockaddr_pn *to = (const void *)address;
    unsigned cmd = p[1];
    opcodes[ops++] = cmd;
    response_resource = to->spn_resource;
    memset(response, 0, sizeof(response));
    response[0] = p[0];
    response_size = 4;
    if (response_resource == 0xf3)
    {
        switch (cmd)
        {
        case 0x32:
            response[1] = 0x33;
            response_size = 8;
            response[6] = 2;
            break;
        case 0x3b:
            for (unsigned i = 0; i < PREFILL; i += 4)
                assert(!memcmp(test_ring + i, (unsigned char[]){0, 1, 0, 0}, 4));
            response[1] = 0x3c;
            response[3] = 4;
            response_size = 8;
            response[6] = 2;
            break;
        case 0x3f:
            response[1] = 0x40;
            break;
        case 0x3d:
            response[1] = 0x3e;
            response[3] = 4;
            response_size = 6;
            break;
        case 0x51:
            response[1] = 0x52;
            assert(p[13] == 0);
            active = 0;
            break;
        case 0x34:
        {
            assert(be32(p + 4) == (PREFILL + publications * step) % RING);
            assert(p[9] == 5);
            read_cursor = (read_cursor + step) % RING;
            if (!publications)
            {
                memset(stale, 255, sizeof(stale));
                stale[0] = 74;
                stale[1] = 0x35;
                stale_ready = 1;
            }
            publications++;
            if (omit35 && publications == 8)
            {
                response_size = 0;
                return (ssize_t)count;
            }
            response[1] = 0x35;
            response_size = 8;
            response[4] = (unsigned char)(read_cursor >> 24);
            response[5] = (unsigned char)(read_cursor >> 16);
            response[6] = (unsigned char)(read_cursor >> 8);
            response[7] = (unsigned char)read_cursor;
            break;
        }
        default:
            assert(!"unexpected DSP opcode");
        }
    }
    else if (response_resource == 0x0b)
    {
        if (cmd == 0)
        {
            response[1] = 1;
            response[2] = 3;
        }
        else if (cmd == 0x14)
        {
            response[1] = 0x15;
            response[2] = p[3];
            response_size = 8;
        }
        else
        {
            assert(cmd == 4);
            response[1] = 5;
        }
    }
    else
        assert(!"unexpected resource");
    return (ssize_t)count;
}

ssize_t peer_recvfrom(int fd, void *buffer, size_t size, int flags, struct sockaddr *address, socklen_t *length)
{
    (void)flags;
    assert(fd == 7 && response_size && size >= response_size);
    struct sockaddr_pn *from = (void *)address;
    memset(from, 0, sizeof(*from));
    from->spn_family = AF_PHONET;
    from->spn_resource = (unsigned char)response_resource;
    from->spn_dev = response_resource == 0xf3 ? 0x7c : 0;
    from->spn_obj = 0x18;
    *length = sizeof(*from);
    if (stale_ready)
    {
        memcpy(buffer, stale, 8);
        stale_ready = 0;
        stale_seen++;
        return 8;
    }
    memcpy(buffer, response, response_size);
    ssize_t n = (ssize_t)response_size;
    response_size = 0;
    return n;
}

static unsigned input_left;
int level_get(int which)
{
    assert(which == LEVEL_VOLUME);
    return 100;
}
snd_pcm_sframes_t peer_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t count)
{
    (void)pcm;
    assert(count <= input_left && !enabled_once && publications >= 8);
    int16_t *out = buffer;
    for (unsigned i = 0; i < count; i++)
    {
        out[2 * i] = (int16_t)i;
        out[2 * i + 1] = (int16_t)-i;
    }
    input_left -= (unsigned)count;
    return (snd_pcm_sframes_t)count;
}

snd_pcm_sframes_t peer_pcm_avail(snd_pcm_t *pcm)
{
    (void)pcm;
    return 0;
}
int peer_ctl_read(snd_ctl_t *ctl, snd_ctl_elem_value_t *v)
{
    (void)ctl;
    (void)v;
    return 0;
}
int peer_ctl_boolean(const snd_ctl_elem_value_t *v, unsigned n)
{
    (void)v;
    (void)n;
    return input_left != 0;
}
static void run_session(unsigned advance, int missing)
{
    step = advance;
    omit35 = missing;
    read_cursor = ops = publications = enabled_once = 0;
    active = stale_ready = stale_seen = 0;
    struct session s = {.fd = 7, .owner = 165};
    struct source in = {.pending = PREFETCH_FRAMES};
    input_left = 48000 - PREFETCH_FRAMES;
    unsigned char *ring = calloc(1, RING);
    assert(ring);
    test_ring = ring;
    for (unsigned i = 0; i < PREFETCH_FRAMES * 2; i++)
        in.prefetch[i] = 1;
    assert(!dsp_control(&s, open_pcm, sizeof(open_pcm), 0x33, 4));
    s.opened = 1;
    assert(!fill_live(ring, 0, PREFILL, &in));
    assert(!dsp_control(&s, open_channel, sizeof(open_channel), 0x3c, 4));
    s.channel = 1;
    assert(!dsp_control(&s, route_open, sizeof(route_open), 0x40, 2));
    assert(!audio_start(&s));
    int result = play(&s, ring, &in);
    if (missing)
        assert(result == -1 && publications == 8);
    else
    {
        assert(!result && publications > 8);
        assert(in.frames == 48000 && in.eof && input_left == 0);
    }
    assert(enabled_once == 0 && stale_seen);
    assert(!cleanup(&s));
    assert(!s.opened && !s.channel && !s.app && !active);
    assert(!memcmp(opcodes + ops - 5, (unsigned[]){0x51, 0x32, 0x3f, 0x3d, 4}, 5 * sizeof(unsigned)));
    free(ring);
}
int main(void)
{
    run_session(9600, 0);
    run_session(3840, 0);
    run_session(9600, 1);
    puts("PASS mode0 ALSA source, fixed depth/field5, wrap, stale35, missing35 rejection, EOF and ordered close; peer is modeled, not DSP readiness proof");
    return 0;
}
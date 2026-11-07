// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <vorbis/vorbisfile.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
static volatile sig_atomic_t stopped;
static void stop(int sig)
{
    (void)sig;
    stopped = 1;
}
int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fputs("usage: modem-play FILE.ogg\n", stderr);
        return 2;
    }
    OggVorbis_File input;
    if (ov_fopen(argv[1], &input) < 0)
    {
        fputs("cannot open Vorbis audio\n", stderr);
        return 1;
    }
    vorbis_info *info = ov_info(&input, -1);
    snd_pcm_t *pcm = NULL;
    int result = 1;
    if (!info || info->channels < 1 || info->channels > 8 || info->rate < 8000 || info->rate > 192000)
        goto end;
    int channels = info->channels;
    long rate = info->rate;
    int r = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (r < 0)
    {
        fprintf(stderr, "ALSA: %s\n", snd_strerror(r));
        goto end;
    }
    r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned)channels, (unsigned)rate, 1, 200000);
    if (r < 0)
    {
        fprintf(stderr, "ALSA: %s\n", snd_strerror(r));
        goto end;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    signal(SIGHUP, stop);
    char buffer[8192];
    int bitstream;
    while (!stopped)
    {
        long n = ov_read(&input, buffer, sizeof(buffer), 0, 2, 1, &bitstream);
        if (!n)
        {
            result = 0;
            break;
        }
        if (n < 0)
        {
            fputs("Vorbis decode failed\n", stderr);
            goto end;
        }
        info = ov_info(&input, bitstream);
        if (!info || info->channels != channels || info->rate != rate || n % (2 * channels))
        {
            fputs("Vorbis format changed\n", stderr);
            goto end;
        }
        snd_pcm_uframes_t done = 0, count = (snd_pcm_uframes_t)n / (2 * (unsigned)channels);
        while (done < count && !stopped)
        {
            snd_pcm_sframes_t written = snd_pcm_writei(pcm, buffer + done * 2 * (unsigned)channels, count - done);
            if (written == -EINTR)
                continue;
            if (written < 0)
            {
                if (snd_pcm_recover(pcm, (int)written, 1) < 0)
                    goto end;
                continue;
            }
            if (!written)
                goto end;
            done += (snd_pcm_uframes_t)written;
        }
    }
    if (!stopped && result == 0)
    {
        if (snd_pcm_drain(pcm) < 0)
            result = 1;
    }
    else
        snd_pcm_drop(pcm);
    if (stopped)
        result = 0;
end:
    if (pcm)
        snd_pcm_close(pcm);
    ov_clear(&input);
    return result;
}
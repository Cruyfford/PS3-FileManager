/* Audio player: mpg123 (MP3), libvorbisfile (OGG), PCM WAV -> libaudio 48 kHz stereo float port. */
#include "audio.h"
#include "vfs.h"
#include <audio/audio.h>
#include <sys/thread.h>
#include <soundlib/mpg123.h>
#include <vorbis/vorbisfile.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>

#define OUT_RATE 48000
#define PCM_CAP  (64 * 1024)      /* interleaved stereo int16 frames buffered from the decoder */

enum { FMT_MP3, FMT_OGG, FMT_WAV };

static volatile int state, stop_req, paused;
static volatile uint64_t played_frames;
static volatile uint32_t duration_ms;
static volatile float levels[8];
static sys_ppu_thread_t thr;
static int thr_alive;
static char cur_path[1024];

/* decoder output: stereo int16 at src_rate */
static int16_t pcm[PCM_CAP * 2];
static int pcm_len, pcm_pos;
static long src_rate = 44100;
static double frac;

static const char *ext(const char *n) { const char *d = strrchr(n, '.'); return d ? d + 1 : ""; }

int audio_is_supported(const char *n)
{
    const char *e = ext(n);
    return !strcasecmp(e, "mp3") || !strcasecmp(e, "ogg") || !strcasecmp(e, "wav");
}

/* ---------- vorbis callbacks over vfs ---------- */
typedef struct { vfs_file_t *f; uint64_t pos, size; } ogg_src_t;
static size_t ov_read_cb(void *ptr, size_t size, size_t n, void *ds)
{
    ogg_src_t *o = (ogg_src_t *)ds;
    int64_t r = vfs_read(o->f, ptr, (uint32_t)(size * n));
    if (r <= 0) return 0;
    o->pos += (uint64_t)r;
    return (size_t)r / size;
}
static int ov_seek_cb(void *ds, ogg_int64_t off, int whence)
{
    ogg_src_t *o = (ogg_src_t *)ds;
    int64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)o->pos + off : (int64_t)o->size + off;
    if (np < 0 || vfs_seek(o->f, (uint64_t)np) != 0) return -1;
    o->pos = (uint64_t)np;
    return 0;
}
static int ov_close_cb(void *ds) { (void)ds; return 0; }
static long ov_tell_cb(void *ds) { return (long)((ogg_src_t *)ds)->pos; }

/* ---------- decoders: fill pcm[] with stereo frames, return frames or 0 at end ---------- */
typedef struct {
    int fmt;
    vfs_file_t *f;
    mpg123_handle *mh;
    int mp3_channels;
    OggVorbis_File vf;
    ogg_src_t src;
    int ogg_channels;
    int wav_channels, wav_bits;
    uint64_t wav_left;
    uint8_t in[16384];
} dec_t;

static void to_stereo(const int16_t *src, int frames, int ch)
{
    if (frames > PCM_CAP) frames = PCM_CAP;
    for (int i = 0; i < frames; i++) {
        pcm[i * 2] = src[i * ch];
        pcm[i * 2 + 1] = ch > 1 ? src[i * ch + 1] : src[i * ch];
    }
    pcm_len = frames; pcm_pos = 0;
}

static int decode_more(dec_t *d)
{
    static int16_t tmp[PCM_CAP * 2];
    if (d->fmt == FMT_MP3) {
        for (;;) {
            size_t done = 0;
            int r = mpg123_decode(d->mh, NULL, 0, (unsigned char *)tmp, sizeof(tmp) / 2, &done);
            if (r == MPG123_NEW_FORMAT) {
                long rate; int ch, enc;
                mpg123_getformat(d->mh, &rate, &ch, &enc);
                src_rate = rate; d->mp3_channels = ch;
            }
            if (done > 0) { to_stereo(tmp, (int)(done / 2 / (d->mp3_channels ? d->mp3_channels : 2)), d->mp3_channels ? d->mp3_channels : 2); return pcm_len; }
            if (r == MPG123_NEED_MORE || r == MPG123_OK) {
                int64_t n = vfs_read(d->f, d->in, sizeof(d->in));
                if (n <= 0) return 0;
                mpg123_feed(d->mh, d->in, (size_t)n);
                continue;
            }
            if (r == MPG123_NEW_FORMAT) continue;
            return 0;   /* MPG123_DONE or error */
        }
    }
    if (d->fmt == FMT_OGG) {
        int bs;
        long n = ov_read(&d->vf, (char *)tmp, sizeof(tmp) / 2, 1, 2, 1, &bs);
        if (n <= 0) return 0;
        to_stereo(tmp, (int)(n / 2 / d->ogg_channels), d->ogg_channels);
        return pcm_len;
    }
    /* WAV: little-endian PCM16 */
    if (d->wav_left == 0) return 0;
    uint32_t want = sizeof(tmp) / 2;
    if (want > d->wav_left) want = (uint32_t)d->wav_left;
    int64_t n = vfs_read(d->f, tmp, want);
    if (n <= 0) return 0;
    d->wav_left -= (uint64_t)n;
    int samples = (int)(n / 2);
    for (int i = 0; i < samples; i++) { uint16_t v = (uint16_t)tmp[i]; tmp[i] = (int16_t)((v >> 8) | (v << 8)); }
    to_stereo(tmp, samples / d->wav_channels, d->wav_channels);
    return pcm_len;
}

static int open_decoder(dec_t *d, const char *path)
{
    memset(d, 0, sizeof(*d));
    d->f = vfs_open_read(path);
    if (!d->f) return 0;
    const char *e = ext(path);
    duration_ms = 0;
    if (!strcasecmp(e, "mp3")) {
        d->fmt = FMT_MP3;
        static int inited;
        if (!inited) { mpg123_init(); inited = 1; }
        int err;
        d->mh = mpg123_new(NULL, &err);
        if (!d->mh) return 0;
        mpg123_format_none(d->mh);
        const long rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
        for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) mpg123_format(d->mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
        if (mpg123_open_feed(d->mh) != MPG123_OK) return 0;
        d->mp3_channels = 2;
        return 1;
    }
    if (!strcasecmp(e, "ogg")) {
        d->fmt = FMT_OGG;
        ov_callbacks cb = { ov_read_cb, ov_seek_cb, ov_close_cb, ov_tell_cb };
        d->src.f = d->f; d->src.pos = 0; d->src.size = vfs_size(d->f);
        if (ov_open_callbacks(&d->src, &d->vf, NULL, 0, cb) != 0) return 0;
        vorbis_info *vi = ov_info(&d->vf, -1);
        if (!vi) return 0;
        src_rate = vi->rate; d->ogg_channels = vi->channels > 0 ? vi->channels : 2;
        double t = ov_time_total(&d->vf, -1);
        if (t > 0) duration_ms = (uint32_t)(t * 1000);
        return 1;
    }
    if (!strcasecmp(e, "wav")) {
        d->fmt = FMT_WAV;
        uint8_t h[12];
        if (vfs_read(d->f, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
        for (int guard = 0; guard < 32; guard++) {
            uint8_t ch[8];
            if (vfs_read(d->f, ch, 8) != 8) return 0;
            uint32_t sz = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
            if (!memcmp(ch, "fmt ", 4)) {
                uint8_t fm[40] = { 0 };
                uint32_t rd = sz < sizeof(fm) ? sz : sizeof(fm);
                if (vfs_read(d->f, fm, rd) != (int64_t)rd) return 0;
                if (sz > rd) { uint8_t skip[256]; uint32_t left = sz - rd; while (left) { uint32_t k = left > 256 ? 256 : left; vfs_read(d->f, skip, k); left -= k; } }
                int fmt = fm[0] | fm[1] << 8;
                d->wav_channels = fm[2] | fm[3] << 8;
                src_rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (long)fm[7] << 24;
                d->wav_bits = fm[14] | fm[15] << 8;
                if (fmt != 1 || d->wav_bits != 16 || d->wav_channels < 1 || d->wav_channels > 2) return 0;
            } else if (!memcmp(ch, "data", 4)) {
                d->wav_left = sz;
                if (src_rate && d->wav_channels) duration_ms = (uint32_t)((uint64_t)sz * 1000 / (src_rate * d->wav_channels * 2));
                return d->wav_channels > 0;
            } else {
                uint8_t skip[256]; uint32_t left = sz + (sz & 1);
                while (left) { uint32_t k = left > 256 ? 256 : left; if (vfs_read(d->f, skip, k) <= 0) return 0; left -= k; }
            }
        }
    }
    return 0;
}

static void close_decoder(dec_t *d)
{
    if (d->fmt == FMT_MP3 && d->mh) { mpg123_close(d->mh); mpg123_delete(d->mh); }
    if (d->fmt == FMT_OGG) ov_clear(&d->vf);
    if (d->f) vfs_close(d->f);
}

/* produce one output block (AUDIO_BLOCK_SAMPLES stereo float frames at 48 kHz) */
static int fill_block(dec_t *d, float *out)
{
    double step = (double)src_rate / OUT_RATE;
    float peak[8] = { 0 };
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        if (paused) { out[i * 2] = out[i * 2 + 1] = 0; continue; }
        while (pcm_pos + 1 >= pcm_len) {
            if (!decode_more(d)) return 0;
            step = (double)src_rate / OUT_RATE;
        }
        int p = pcm_pos;
        float t = (float)frac;
        float l = (pcm[p * 2] * (1 - t) + pcm[(p + 1) * 2] * t) / 32768.0f;
        float r = (pcm[p * 2 + 1] * (1 - t) + pcm[(p + 1) * 2 + 1] * t) / 32768.0f;
        out[i * 2] = l; out[i * 2 + 1] = r;
        float a = fabsf(l) + fabsf(r);
        int b = (i * 8) / AUDIO_BLOCK_SAMPLES;
        if (a > peak[b]) peak[b] = a;
        frac += step;
        while (frac >= 1.0) { frac -= 1.0; pcm_pos++; }
    }
    if (!paused) {
        played_frames += AUDIO_BLOCK_SAMPLES;
        for (int b = 0; b < 8; b++) levels[b] = levels[b] * 0.6f + (peak[b] > 1 ? 1 : peak[b]) * 0.4f;
    }
    return 1;
}

static void player_thread(void *arg)
{
    (void)arg;
    dec_t *d = (dec_t *)calloc(1, sizeof(dec_t));
    u32 port = 0;
    int port_open = 0;
    if (!d || !open_decoder(d, cur_path)) { state = AUD_ERROR; goto out; }

    audioPortParam prm = { AUDIO_PORT_2CH, AUDIO_BLOCK_8, AUDIO_PORT_INITLEVEL, 1.0f };
    audioPortConfig cfg;
    if (audioPortOpen(&prm, &port) != 0) { state = AUD_ERROR; goto out; }
    port_open = 1;
    audioGetPortConfig(port, &cfg);
    audioPortStart(port);

    volatile u64 *read_index = (volatile u64 *)(u64)cfg.readIndex;
    float *base = (float *)(u64)cfg.audioDataStart;
    u32 next = 1;
    pcm_len = pcm_pos = 0; frac = 0;

    while (!stop_req) {
        if (next == (u32)*read_index) { usleep(1000); continue; }
        float *blk = base + 2 * AUDIO_BLOCK_SAMPLES * next;
        if (!fill_block(d, blk)) { memset(blk, 0, sizeof(float) * 2 * AUDIO_BLOCK_SAMPLES); state = AUD_ENDED; break; }
        next = (next + 1) % AUDIO_BLOCK_8;
    }
    usleep(60000);   /* let the last blocks drain */
out:
    if (port_open) { audioPortStop(port); audioPortClose(port); }
    if (d) { close_decoder(d); free(d); }
    if (state == AUD_PLAYING || state == AUD_PAUSED) state = AUD_STOPPED;
    sysThreadExit(0);
}

static void join_thread(void)
{
    if (thr_alive) { u64 rv; sysThreadJoin(thr, &rv); thr_alive = 0; }
}

static int audio_inited;
int audio_sys_init(void)
{
    if (!audio_inited) { if (audioInit() != 0) return 0; audio_inited = 1; }
    return 1;
}

int audio_play(const char *path)
{
    audio_stop();
    if (!audio_sys_init()) return 0;
    snprintf(cur_path, sizeof(cur_path), "%s", path);
    stop_req = 0; paused = 0; played_frames = 0;
    memset((void *)levels, 0, sizeof(levels));
    state = AUD_PLAYING;
    if (sysThreadCreate(&thr, player_thread, NULL, 900, 0x40000, THREAD_JOINABLE, "audio") != 0) { state = AUD_ERROR; return 0; }
    thr_alive = 1;
    return 1;
}

void audio_stop(void)
{
    if (thr_alive) { stop_req = 1; join_thread(); }
    if (state != AUD_ERROR) state = AUD_STOPPED;
}

void audio_toggle_pause(void)
{
    if (state == AUD_PLAYING) { paused = 1; state = AUD_PAUSED; }
    else if (state == AUD_PAUSED) { paused = 0; state = AUD_PLAYING; }
}

int audio_state(void) { return state; }
uint32_t audio_position_ms(void) { return (uint32_t)(played_frames * 1000ULL / OUT_RATE); }
uint32_t audio_duration_ms(void) { return duration_ms; }
void audio_levels(float out[8]) { for (int i = 0; i < 8; i++) out[i] = levels[i]; }

/* Audio decoders: AAC (faad, raw or ADTS), MP1/2/3 (mpg123), AC3 (liba52), Vorbis (libvorbis), PCM */
#include "adec.h"
#include <neaacdec.h>
#include <soundlib/mpg123.h>
#include <vorbis/codec.h>
#include <inttypes.h>
#include "a52.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct adec {
    int codec, channels, rate, bits;
    /* aac */
    NeAACDecHandle aac; int aac_inited, adts;
    /* mp3 */
    mpg123_handle *mh;
    /* ac3 */
    a52_state_t *a52;
    /* vorbis */
    vorbis_info vi; vorbis_comment vc; vorbis_dsp_state vd; vorbis_block vb; int vorbis_ok; int64_t vpkt;
    /* stream reassembly for ADTS / AC3 */
    uint8_t *acc; int acc_len, acc_cap;
    int16_t *out; int out_cap;
    uint8_t *extra; int extra_len;
};

static int16_t *outbuf(adec_t *a, int samples)
{
    if (samples > a->out_cap) {
        int16_t *n = (int16_t *)realloc(a->out, samples * sizeof(int16_t));
        if (!n) return NULL;
        a->out = n; a->out_cap = samples;
    }
    return a->out;
}

static inline int16_t clip16(float v) { if (v > 32767.f) return 32767; if (v < -32768.f) return -32768; return (int16_t)v; }

/* generic float planar -> stereo downmix; order: vorbis (L,C,R,SL,SR,LFE) when vorbis_order, else WAV (L,R,C,LFE,SL,SR) */
static void downmix(float **pcm, int ch, int n, int16_t *o, int vorbis_order, float scale)
{
    for (int i = 0; i < n; i++) {
        float l, r;
        if (ch == 1) { l = r = pcm[0][i]; }
        else if (ch == 2) { l = pcm[0][i]; r = pcm[1][i]; }
        else {
            float L, R, C = 0, SL = 0, SR = 0;
            if (vorbis_order) {
                L = pcm[0][i];
                if (ch == 3) { C = pcm[1][i]; R = pcm[2][i]; }
                else if (ch == 4) { R = pcm[1][i]; SL = pcm[2][i]; SR = pcm[3][i]; }
                else { C = pcm[1][i]; R = pcm[2][i]; SL = pcm[3][i]; SR = ch > 4 ? pcm[4][i] : 0; }
            } else {
                L = pcm[0][i]; R = pcm[1][i];
                if (ch >= 3) C = pcm[2][i];
                if (ch >= 6) { SL = pcm[4][i]; SR = pcm[5][i]; }
                else if (ch == 4) { SL = pcm[2][i]; SR = pcm[3][i]; C = 0; }
            }
            l = (L + 0.707f * C + 0.707f * SL) * 0.6f;
            r = (R + 0.707f * C + 0.707f * SR) * 0.6f;
        }
        o[i * 2] = clip16(l * scale); o[i * 2 + 1] = clip16(r * scale);
    }
}

adec_t *adec_open(const stream_t *s)
{
    if (!audio_codec_playable(s->codec)) return NULL;
    adec_t *a = (adec_t *)calloc(1, sizeof(adec_t));
    if (!a) return NULL;
    a->codec = s->codec; a->channels = s->channels ? s->channels : 2; a->rate = s->rate ? s->rate : 48000; a->bits = s->bits;
    if (s->extra_len) { a->extra = (uint8_t *)malloc(s->extra_len); if (a->extra) { memcpy(a->extra, s->extra, s->extra_len); a->extra_len = s->extra_len; } }
    switch (a->codec) {
    case AC_AAC: {
        a->aac = NeAACDecOpen();
        if (!a->aac) goto fail;
        NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(a->aac);
        c->outputFormat = FAAD_FMT_16BIT;
        c->downMatrix = 1;
        NeAACDecSetConfiguration(a->aac, c);
        if (a->extra_len >= 2) {
            unsigned long r; unsigned char ch;
            if (NeAACDecInit2(a->aac, a->extra, a->extra_len, &r, &ch) < 0) goto fail;
            a->aac_inited = 1;
        } else a->adts = 1;
        break;
    }
    case AC_MP3: {
        static int inited;
        if (!inited) { mpg123_init(); inited = 1; }
        int err;
        a->mh = mpg123_new(NULL, &err);
        if (!a->mh) goto fail;
        mpg123_format_none(a->mh);
        static const long rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
        for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) mpg123_format(a->mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
        if (mpg123_open_feed(a->mh) != MPG123_OK) goto fail;
        break;
    }
    case AC_AC3:
        a->a52 = a52_init(0);
        if (!a->a52) goto fail;
        break;
    case AC_VORBIS: {
        /* Xiph-laced CodecPrivate: count-1, sizes..., then the three headers */
        if (a->extra_len < 3 || a->extra[0] != 2) goto fail;
        int o = 1, sz[3], k;
        for (k = 0; k < 2; k++) { int v = 0, c; do { if (o >= a->extra_len) goto fail; c = a->extra[o++]; v += c; } while (c == 255); sz[k] = v; }
        sz[2] = a->extra_len - o - sz[0] - sz[1];
        if (sz[2] <= 0) goto fail;
        vorbis_info_init(&a->vi); vorbis_comment_init(&a->vc);
        for (k = 0; k < 3; k++) {
            ogg_packet op; memset(&op, 0, sizeof(op));
            op.packet = a->extra + o; op.bytes = sz[k]; op.b_o_s = k == 0; op.packetno = k;
            if (vorbis_synthesis_headerin(&a->vi, &a->vc, &op) < 0) goto fail;
            o += sz[k];
        }
        if (vorbis_synthesis_init(&a->vd, &a->vi) != 0) goto fail;
        vorbis_block_init(&a->vd, &a->vb);
        a->vorbis_ok = 1; a->vpkt = 3;
        a->rate = a->vi.rate; a->channels = a->vi.channels;
        break;
    }
    }
    return a;
fail:
    adec_close(a);
    return NULL;
}

static void acc_push(adec_t *a, const uint8_t *d, int n)
{
    if (a->acc_len + n > a->acc_cap) {
        int nc = (a->acc_len + n) * 2 + 4096;
        uint8_t *x = (uint8_t *)realloc(a->acc, nc);
        if (!x) return;
        a->acc = x; a->acc_cap = nc;
    }
    memcpy(a->acc + a->acc_len, d, n);
    a->acc_len += n;
}

static void acc_drop(adec_t *a, int n) { if (n >= a->acc_len) { a->acc_len = 0; return; } memmove(a->acc, a->acc + n, a->acc_len - n); a->acc_len -= n; }

static void aac_frame(adec_t *a, const uint8_t *d, int n, pcm_sink_t sink, void *u)
{
    if (!a->aac_inited) {
        unsigned long r; unsigned char ch;
        if (NeAACDecInit(a->aac, (unsigned char *)d, n, &r, &ch) < 0) return;
        a->aac_inited = 1;
    }
    NeAACDecFrameInfo fi;
    int16_t *pcm = (int16_t *)NeAACDecDecode(a->aac, &fi, (unsigned char *)d, (unsigned long)n);
    if (!pcm || fi.error || !fi.samples || !fi.channels) return;
    int frames = fi.samples / fi.channels;
    if (fi.channels <= 2) { sink(pcm, frames, fi.channels, fi.samplerate, u); return; }
    int16_t *o = outbuf(a, frames * 2);
    if (!o) return;
    for (int i = 0; i < frames; i++) { o[i * 2] = pcm[i * fi.channels]; o[i * 2 + 1] = pcm[i * fi.channels + 1]; }
    sink(o, frames, 2, fi.samplerate, u);
}

void adec_decode(adec_t *a, const uint8_t *d, int n, pcm_sink_t sink, void *u)
{
    if (!a || n <= 0) return;
    switch (a->codec) {
    case AC_AAC:
        if (!a->adts) { aac_frame(a, d, n, sink, u); return; }
        acc_push(a, d, n);
        for (;;) {
            int i = 0;
            while (i + 7 <= a->acc_len && !(a->acc[i] == 0xFF && (a->acc[i + 1] & 0xF6) == 0xF0)) i++;
            if (i) acc_drop(a, i);
            if (a->acc_len < 7) return;
            int fl = ((a->acc[3] & 3) << 11) | (a->acc[4] << 3) | (a->acc[5] >> 5);
            if (fl < 7) { acc_drop(a, 1); continue; }
            if (a->acc_len < fl) return;
            aac_frame(a, a->acc, fl, sink, u);
            acc_drop(a, fl);
        }
    case AC_MP3: {
        mpg123_feed(a->mh, (unsigned char *)d, (size_t)n);
        int16_t tmp[4608];
        for (;;) {
            size_t done = 0;
            int r = mpg123_decode(a->mh, NULL, 0, (unsigned char *)tmp, sizeof(tmp), &done);
            if (r == MPG123_NEW_FORMAT) { long rate; int ch, enc; mpg123_getformat(a->mh, &rate, &ch, &enc); a->rate = rate; a->channels = ch; continue; }
            if (done) sink(tmp, (int)(done / 2 / (a->channels ? a->channels : 2)), a->channels ? a->channels : 2, a->rate, u);
            if (r != MPG123_OK || !done) break;
        }
        return;
    }
    case AC_AC3:
        acc_push(a, d, n);
        for (;;) {
            if (a->acc_len < 7) return;
            int flags, rate, br;
            int fl = a52_syncinfo(a->acc, &flags, &rate, &br);
            if (!fl) { acc_drop(a, 1); continue; }
            if (a->acc_len < fl) return;
            flags = A52_STEREO | A52_ADJUST_LEVEL;
            sample_t level = 1, bias = 0;
            if (a52_frame(a->a52, a->acc, &flags, &level, bias) == 0) {
                a52_dynrng(a->a52, NULL, NULL);
                int16_t *o = outbuf(a, 256 * 6 * 2);
                if (o) {
                    int fr = 0;
                    for (int blk = 0; blk < 6; blk++) {
                        if (a52_block(a->a52) != 0) break;
                        sample_t *s = a52_samples(a->a52);
                        int mono = (flags & A52_CHANNEL_MASK) == A52_MONO;
                        for (int i = 0; i < 256; i++) { o[(fr + i) * 2] = clip16(s[i] * 32767.f); o[(fr + i) * 2 + 1] = clip16(s[(mono ? 0 : 256) + i] * 32767.f); }
                        fr += 256;
                    }
                    if (fr) sink(o, fr, 2, rate, u);
                }
            }
            acc_drop(a, fl);
        }
    case AC_VORBIS: {
        if (!a->vorbis_ok) return;
        ogg_packet op; memset(&op, 0, sizeof(op));
        op.packet = (unsigned char *)d; op.bytes = n; op.packetno = a->vpkt++; op.granulepos = -1;
        if (vorbis_synthesis(&a->vb, &op) == 0) vorbis_synthesis_blockin(&a->vd, &a->vb);
        float **pcm;
        int fr;
        while ((fr = vorbis_synthesis_pcmout(&a->vd, &pcm)) > 0) {
            int16_t *o = outbuf(a, fr * 2);
            if (!o) break;
            downmix(pcm, a->vi.channels, fr, o, 1, 32767.f);
            sink(o, fr, 2, a->vi.rate, u);
            vorbis_synthesis_read(&a->vd, fr);
        }
        return;
    }
    case AC_PCM_S16LE: {
        int ch = a->channels, frames = n / 2 / ch;
        int16_t *o = outbuf(a, frames * 2);
        if (!o) return;
        for (int i = 0; i < frames; i++) {
            const uint8_t *p = d + i * ch * 2;
            int16_t l = (int16_t)(p[0] | (p[1] << 8)), r = ch > 1 ? (int16_t)(p[2] | (p[3] << 8)) : l;
            o[i * 2] = l; o[i * 2 + 1] = r;
        }
        sink(o, frames, 2, a->rate, u);
        return;
    }
    case AC_PCM_S16BE: {
        int ch = a->channels, frames = n / 2 / ch;
        int16_t *o = outbuf(a, frames * 2);
        if (!o) return;
        for (int i = 0; i < frames; i++) {
            const uint8_t *p = d + i * ch * 2;
            int16_t l = (int16_t)((p[0] << 8) | p[1]), r = ch > 1 ? (int16_t)((p[2] << 8) | p[3]) : l;
            o[i * 2] = l; o[i * 2 + 1] = r;
        }
        sink(o, frames, 2, a->rate, u);
        return;
    }
    case AC_PCM_S24LE: {
        int ch = a->channels, frames = n / 3 / ch;
        int16_t *o = outbuf(a, frames * 2);
        if (!o) return;
        for (int i = 0; i < frames; i++) {
            const uint8_t *p = d + i * ch * 3;
            o[i * 2] = (int16_t)(p[1] | (p[2] << 8));
            o[i * 2 + 1] = ch > 1 ? (int16_t)(p[4] | (p[5] << 8)) : o[i * 2];
        }
        sink(o, frames, 2, a->rate, u);
        return;
    }
    case AC_PCM_U8: {
        int ch = a->channels, frames = n / ch;
        int16_t *o = outbuf(a, frames * 2);
        if (!o) return;
        for (int i = 0; i < frames; i++) { o[i * 2] = (int16_t)((d[i * ch] - 128) << 8); o[i * 2 + 1] = ch > 1 ? (int16_t)((d[i * ch + 1] - 128) << 8) : o[i * 2]; }
        sink(o, frames, 2, a->rate, u);
        return;
    }
    }
}

void adec_reset(adec_t *a)
{
    if (!a) return;
    a->acc_len = 0;
    if (a->codec == AC_AAC && a->aac) NeAACDecPostSeekReset(a->aac, 0);
    if (a->codec == AC_MP3 && a->mh) { mpg123_close(a->mh); mpg123_open_feed(a->mh); }
    if (a->codec == AC_VORBIS && a->vorbis_ok) { vorbis_synthesis_restart(&a->vd); }
}

void adec_close(adec_t *a)
{
    if (!a) return;
    if (a->aac) NeAACDecClose(a->aac);
    if (a->mh) { mpg123_close(a->mh); mpg123_delete(a->mh); }
    if (a->a52) a52_free(a->a52);
    if (a->vorbis_ok) { vorbis_block_clear(&a->vb); vorbis_dsp_clear(&a->vd); }
    if (a->codec == AC_VORBIS) { vorbis_comment_clear(&a->vc); vorbis_info_clear(&a->vi); }
    free(a->acc); free(a->out); free(a->extra);
    free(a);
}

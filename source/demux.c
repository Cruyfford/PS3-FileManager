/* demuxer front-end: buffered I/O, probing, codec names */
#include "demux.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define BIO_BUF (512 * 1024)

int bio_open(bio_t *b, const char *path)
{
    memset(b, 0, sizeof(*b));
    b->f = vfs_open_read(path);
    if (!b->f) return 0;
    b->size = vfs_size(b->f);
    b->buf = (uint8_t *)malloc(BIO_BUF);
    if (!b->buf) { vfs_close(b->f); b->f = NULL; return 0; }
    return 1;
}

void bio_close(bio_t *b)
{
    if (b->f) vfs_close(b->f);
    free(b->buf);
    memset(b, 0, sizeof(*b));
}

static int bio_fill(bio_t *b)
{
    b->bstart = b->pos;
    b->boff = 0;
    b->blen = 0;
    if (b->pos >= b->size) return 0;
    if (vfs_seek(b->f, b->pos) != 0) return 0;
    uint64_t want = b->size - b->pos;
    if (want > BIO_BUF) want = BIO_BUF;
    int64_t r = vfs_read(b->f, b->buf, (uint32_t)want);
    if (r <= 0) return 0;
    b->blen = (uint32_t)r;
    return 1;
}

int bio_read(bio_t *b, void *dst, uint32_t n)
{
    uint8_t *o = (uint8_t *)dst;
    uint32_t done = 0;
    while (done < n) {
        if (b->pos < b->bstart || b->pos >= b->bstart + b->blen) {
            /* big direct reads skip the buffer */
            if (n - done >= BIO_BUF / 2) {
                if (vfs_seek(b->f, b->pos) != 0) break;
                int64_t r = vfs_read(b->f, o + done, n - done);
                if (r <= 0) break;
                done += (uint32_t)r; b->pos += r;
                b->blen = 0;
                continue;
            }
            if (!bio_fill(b)) break;
        }
        uint32_t off = (uint32_t)(b->pos - b->bstart);
        uint32_t k = b->blen - off;
        if (k > n - done) k = n - done;
        memcpy(o + done, b->buf + off, k);
        done += k; b->pos += k;
    }
    return (int)done;
}

int bio_seek(bio_t *b, uint64_t pos) { if (pos > b->size) pos = b->size; b->pos = pos; return 1; }
int bio_skip(bio_t *b, uint64_t n) { return bio_seek(b, b->pos + n); }
int bio_getc(bio_t *b) { uint8_t c; return bio_read(b, &c, 1) == 1 ? c : -1; }
uint32_t bio_r8(bio_t *b) { int c = bio_getc(b); return c < 0 ? 0 : (uint32_t)c; }
uint32_t bio_rb16(bio_t *b) { uint8_t x[2] = { 0 }; bio_read(b, x, 2); return (x[0] << 8) | x[1]; }
uint32_t bio_rb24(bio_t *b) { uint8_t x[3] = { 0 }; bio_read(b, x, 3); return (x[0] << 16) | (x[1] << 8) | x[2]; }
uint32_t bio_rb32(bio_t *b) { uint8_t x[4] = { 0 }; bio_read(b, x, 4); return ((uint32_t)x[0] << 24) | (x[1] << 16) | (x[2] << 8) | x[3]; }
uint64_t bio_rb64(bio_t *b) { uint64_t h = bio_rb32(b); return (h << 32) | bio_rb32(b); }
uint32_t bio_rl16(bio_t *b) { uint8_t x[2] = { 0 }; bio_read(b, x, 2); return x[0] | (x[1] << 8); }
uint32_t bio_rl32(bio_t *b) { uint8_t x[4] = { 0 }; bio_read(b, x, 4); return x[0] | (x[1] << 8) | (x[2] << 16) | ((uint32_t)x[3] << 24); }

uint8_t *demux_buf(demux_t *d, int need)
{
    if (need > d->pcap) {
        int nc = need + 65536;
        uint8_t *p = (uint8_t *)realloc(d->pbuf, nc);
        if (!p) return NULL;
        d->pbuf = p; d->pcap = nc;
    }
    return d->pbuf;
}

demux_t *demux_open(const char *path)
{
    demux_t *d = (demux_t *)calloc(1, sizeof(demux_t));
    if (!d) return NULL;
    if (!bio_open(&d->io, path)) { free(d); return NULL; }
    uint8_t h[512];
    memset(h, 0, sizeof(h));
    int n = bio_read(&d->io, h, sizeof(h));
    bio_seek(&d->io, 0);
    int ok = 0;
    if (n >= 12 && (!memcmp(h + 4, "ftyp", 4) || !memcmp(h + 4, "moov", 4) || !memcmp(h + 4, "mdat", 4) || !memcmp(h + 4, "free", 4) || !memcmp(h + 4, "wide", 4) || !memcmp(h + 4, "skip", 4))) {
        bio_close(&d->io);
        ok = demux_mp4_open(d, path);
    }
    else if (n >= 4 && h[0] == 0x1A && h[1] == 0x45 && h[2] == 0xDF && h[3] == 0xA3) ok = demux_mkv_open(d);
    else if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "AVI ", 4)) ok = demux_avi_open(d);
    else if (n >= 4 && h[0] == 0 && h[1] == 0 && h[2] == 1 && h[3] == 0xBA) ok = demux_ps_open(d);
    else {
        /* transport stream: 188 (TS) or 192 (M2TS) byte packets with sync bytes */
        if ((n >= 377 && h[0] == 0x47 && h[188] == 0x47) || (n >= 389 && h[4] == 0x47 && h[196] == 0x47)) ok = demux_ts_open(d);
        else {
            /* PS without a pack header at offset 0 (some .mpg/.vob) */
            for (int i = 0; i + 4 < n; i++) if (h[i] == 0 && h[i + 1] == 0 && h[i + 2] == 1 && (h[i + 3] == 0xBA || h[i + 3] == 0xB3)) { ok = demux_ps_open(d); break; }
        }
    }
    if (!ok) { demux_close(d); return NULL; }
    return d;
}

void demux_close(demux_t *d)
{
    if (!d) return;
    if (d->close) d->close(d);
    for (int i = 0; i < d->nst; i++) free(d->st[i].extra);
    bio_close(&d->io);
    free(d->pbuf);
    free(d);
}

const char *codec_name(const stream_t *s)
{
    if (s->type == ST_VIDEO) {
        static const char *v[] = { "?", "H.264", "MPEG-2", "MPEG-4 (DivX/Xvid)", "H.265 (HEVC)", "VP9", "AV1", "?" };
        return v[s->codec < 8 ? s->codec : 7];
    }
    if (s->type == ST_AUDIO) {
        static const char *a[] = { "?", "AAC", "MP3", "AC3", "E-AC3", "Vorbis", "PCM", "PCM", "PCM", "PCM", "DTS", "Opus", "FLAC", "TrueHD", "LPCM", "?" };
        return a[s->codec < 16 ? s->codec : 15];
    }
    return s->codec == SC_SRT ? "SRT" : s->codec == SC_ASS ? "ASS" : "?";
}

int video_codec_playable(int vc) { return vc == VC_H264 || vc == VC_MPEG2 || vc == VC_MPEG4; }
int audio_codec_playable(int ac) { return ac == AC_AAC || ac == AC_MP3 || ac == AC_AC3 || ac == AC_VORBIS || ac == AC_PCM_S16LE || ac == AC_PCM_S16BE || ac == AC_PCM_U8 || ac == AC_PCM_S24LE; }

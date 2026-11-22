/* AVI demuxer (incl. OpenDML/AVIX continuation). Video time = frame counter; seeking via idx1. */
#include "demux.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

#define FCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct { uint32_t id, flags, off, size; } idx_t;

typedef struct {
    int map[100];               /* avi stream number -> stream index (-1 = ignored) */
    uint32_t scale[100], rate[100], ssize[100];
    uint64_t movi_start, movi_end;
    idx_t *idx; int nidx;
    int idx_rel;
    int vstream;                /* avi number of the first video stream */
    int64_t vframes;            /* video chunks read since the last seek point */
    uint64_t abytes[100], achunks[100];
    double vbase;
    int nav;                    /* avi stream count */
} avi_t;

static int is_digit2(uint32_t id) { return isdigit(id & 0xFF) && isdigit((id >> 8) & 0xFF); }
static int chunk_stream(uint32_t id) { return ((id & 0xFF) - '0') * 10 + (((id >> 8) & 0xFF) - '0'); }

static int vcodec_from_fcc(const char *f)
{
    char u[5]; for (int i = 0; i < 4; i++) u[i] = toupper((unsigned char)f[i]); u[4] = 0;
    if (!strcmp(u, "H264") || !strcmp(u, "AVC1") || !strcmp(u, "X264") || !strcmp(u, "DAVC")) return VC_H264;
    if (!strcmp(u, "XVID") || !strcmp(u, "DIVX") || !strcmp(u, "DX50") || !strcmp(u, "FMP4") || !strcmp(u, "MP4V") || !strcmp(u, "3IV2") || !strcmp(u, "M4S2")) return VC_MPEG4;
    if (!strcmp(u, "MPG2") || !strcmp(u, "MPEG") || !strcmp(u, "MPG1") || !strcmp(u, "MX5P")) return VC_MPEG2;
    if (!strcmp(u, "HEVC") || !strcmp(u, "H265") || !strcmp(u, "HVC1")) return VC_HEVC;
    return VC_OTHER;
}

static void parse_strl(demux_t *d, avi_t *a, uint64_t end)
{
    bio_t *b = &d->io;
    int n = a->nav++;
    if (n >= 100) { bio_seek(b, end); return; }
    a->map[n] = -1;
    uint32_t type = 0, handler = 0;
    stream_t s; memset(&s, 0, sizeof(s));
    while (bio_tell(b) + 8 <= end) {
        uint32_t id = bio_rl32(b), sz = bio_rl32(b);
        uint64_t next = bio_tell(b) + sz + (sz & 1);
        if (id == FCC('s','t','r','h') && sz >= 36) {
            type = bio_rl32(b); handler = bio_rl32(b);
            bio_skip(b, 12);           /* flags, priority, language, initial frames */
            a->scale[n] = bio_rl32(b); a->rate[n] = bio_rl32(b);
            bio_skip(b, 12);           /* start, length, buffer size */
            bio_skip(b, 4);            /* quality */
            a->ssize[n] = bio_rl32(b);
            if (!a->scale[n]) a->scale[n] = 1;
        } else if (id == FCC('s','t','r','f')) {
            uint8_t *x = (uint8_t *)malloc(sz + 1);
            if (x && bio_read(b, x, sz) == (int)sz) {
                if (type == FCC('v','i','d','s') && sz >= 40) {
                    s.type = ST_VIDEO;
                    s.w = x[4] | (x[5] << 8); s.h = abs((int)(int32_t)(x[8] | (x[9] << 8) | (x[10] << 16) | ((uint32_t)x[11] << 24)));
                    char fc[5] = { (char)x[16], (char)x[17], (char)x[18], (char)x[19], 0 };
                    s.codec = vcodec_from_fcc(fc);
                    if (s.codec == VC_OTHER) { char hf[5]; memcpy(hf, &handler, 4); hf[4] = 0; for (int i = 0; i < 4; i++) hf[i] = ((uint8_t *)&handler)[i]; s.codec = vcodec_from_fcc(hf); }
                    if (sz > 40) { s.extra = (uint8_t *)malloc(sz - 40); if (s.extra) { memcpy(s.extra, x + 40, sz - 40); s.extra_len = sz - 40; } }
                    s.fps = a->scale[n] ? (double)a->rate[n] / a->scale[n] : 25;
                } else if (type == FCC('a','u','d','s') && sz >= 14) {
                    s.type = ST_AUDIO;
                    int tag = x[0] | (x[1] << 8);
                    s.channels = x[2] | (x[3] << 8);
                    s.rate = x[4] | (x[5] << 8) | (x[6] << 16);
                    s.block_align = x[12] | (x[13] << 8);
                    s.bits = sz >= 16 ? (x[14] | (x[15] << 8)) : 16;
                    switch (tag) {
                    case 0x55: case 0x50: s.codec = AC_MP3; break;
                    case 0x2000: s.codec = AC_AC3; break;
                    case 0x2001: s.codec = AC_DTS; break;
                    case 0x01: s.codec = s.bits == 8 ? AC_PCM_U8 : s.bits == 24 ? AC_PCM_S24LE : AC_PCM_S16LE; break;
                    case 0xFF: case 0x1600: case 0x706D: case 0x1601: s.codec = AC_AAC; break;
                    case 0x566F: s.codec = AC_VORBIS; break;
                    default: s.codec = AC_OTHER;
                    }
                    int cb = sz >= 18 ? (x[16] | (x[17] << 8)) : 0;
                    if (cb > 0 && 18 + cb <= (int)sz && s.codec == AC_AAC) { s.extra = (uint8_t *)malloc(cb); if (s.extra) { memcpy(s.extra, x + 18, cb); s.extra_len = cb; } }
                }
            }
            free(x);
        } else if (id == FCC('s','t','r','n')) {
            int k = sz < sizeof(s.name) - 1 ? (int)sz : (int)sizeof(s.name) - 1;
            bio_read(b, s.name, k); s.name[k] = 0;
        }
        bio_seek(b, next);
    }
    if (s.type && d->nst < MAX_STREAMS) {
        s.id = n;
        a->map[n] = d->nst;
        if (s.type == ST_VIDEO && a->vstream < 0) a->vstream = n;
        d->st[d->nst++] = s;
    } else free(s.extra);
}

static double stream_time(avi_t *a, int n)
{
    if (a->ssize[n]) return (double)a->abytes[n] / a->ssize[n] * a->scale[n] / a->rate[n];
    return (double)a->achunks[n] * a->scale[n] / (a->rate[n] ? a->rate[n] : 1);
}

static int avi_read(demux_t *d, packet_t *pk)
{
    avi_t *a = (avi_t *)d->priv;
    bio_t *b = &d->io;
    for (;;) {
        if (bio_tell(b) + 8 > b->size) return 0;
        uint64_t at = bio_tell(b);
        uint32_t id = bio_rl32(b), sz = bio_rl32(b);
        if (id == FCC('L','I','S','T') || id == FCC('R','I','F','F')) {
            uint32_t t = bio_rl32(b);
            if (t == FCC('m','o','v','i') || t == FCC('r','e','c',' ') || t == FCC('A','V','I','X')) continue;
            bio_seek(b, at + 8 + sz + (sz & 1));
            continue;
        }
        if (is_digit2(id)) {
            int n = chunk_stream(id);
            int si = n < 100 ? a->map[n] : -1;
            if (si < 0 || sz > 32 * 1024 * 1024) { bio_skip(b, sz + (sz & 1)); continue; }
            uint8_t *buf = demux_buf(d, sz + 64);
            if (!buf) return -1;
            if (bio_read(b, buf, sz) != (int)sz) return 0;
            if (sz & 1) bio_skip(b, 1);
            stream_t *s = &d->st[si];
            pk->stream = si; pk->data = buf; pk->size = sz; pk->key = -1;
            if (s->type == ST_VIDEO) {
                pk->pts = a->vbase + (double)a->vframes * a->scale[n] / (a->rate[n] ? a->rate[n] : 25);
                a->vframes++;
                if (!sz) continue;   /* dropped frame */
            } else {
                pk->pts = stream_time(a, n);
                a->abytes[n] += sz; a->achunks[n]++;
            }
            return 1;
        }
        /* junk, idx1, ix##, unknown: skip; guard against garbage */
        if (sz > b->size) {
            /* resync to the next ##dc / ##wb */
            bio_seek(b, at + 1);
            uint8_t w[4] = { 0 }; int c;
            while ((c = bio_getc(b)) >= 0) {
                w[0] = w[1]; w[1] = w[2]; w[2] = w[3]; w[3] = c;
                if (isdigit(w[0]) && isdigit(w[1]) && ((w[2] == 'd' && (w[3] == 'c' || w[3] == 'b')) || (w[2] == 'w' && w[3] == 'b'))) { bio_seek(b, bio_tell(b) - 4); break; }
            }
            if (c < 0) return 0;
            continue;
        }
        bio_skip(b, sz + (sz & 1));
    }
}

static int avi_seek(demux_t *d, double t)
{
    avi_t *a = (avi_t *)d->priv;
    bio_t *b = &d->io;
    int vs = a->vstream;
    if (vs < 0) return 0;
    double fdur = (double)a->scale[vs] / (a->rate[vs] ? a->rate[vs] : 25);
    if (a->nidx) {
        int64_t want = (int64_t)(t / fdur);
        int64_t vf = 0; int best = -1; int64_t best_vf = 0;
        for (int i = 0; i < a->nidx; i++) {
            uint32_t id = a->idx[i].id;
            if (!is_digit2(id) || chunk_stream(id) != vs) continue;
            if (vf > want) break;
            if (a->idx[i].flags & 0x10) { best = i; best_vf = vf; }
            vf++;
        }
        if (best < 0) best = 0;
        memset(a->abytes, 0, sizeof(a->abytes)); memset(a->achunks, 0, sizeof(a->achunks));
        for (int i = 0; i < best; i++) {
            uint32_t id = a->idx[i].id;
            if (!is_digit2(id)) continue;
            int n = chunk_stream(id);
            if (n < 100 && n != vs) { a->abytes[n] += a->idx[i].size; a->achunks[n]++; }
        }
        uint64_t pos = (a->idx_rel ? a->movi_start : 0) + a->idx[best].off;
        a->vframes = best_vf; a->vbase = 0;
        bio_seek(b, pos);
        return 1;
    }
    /* no index: bitrate estimate + resync */
    uint64_t pos = a->movi_start + 4 + (d->duration > 0 ? (uint64_t)((a->movi_end - a->movi_start) * (t / d->duration)) : 0);
    bio_seek(b, pos);
    uint8_t w[4] = { 0 }; int c;
    char want0 = '0' + vs / 10, want1 = '0' + vs % 10;
    while ((c = bio_getc(b)) >= 0) {
        w[0] = w[1]; w[1] = w[2]; w[2] = w[3]; w[3] = c;
        if (w[0] == want0 && w[1] == want1 && w[2] == 'd' && (w[3] == 'c' || w[3] == 'b')) { bio_seek(b, bio_tell(b) - 4); break; }
    }
    a->vframes = 0; a->vbase = t;
    for (int n = 0; n < a->nav && n < 100; n++) {
        if (n == vs) continue;
        double sec = t;
        if (a->ssize[n]) a->abytes[n] = (uint64_t)(sec * a->rate[n] / a->scale[n] * a->ssize[n]);
        else a->achunks[n] = (uint64_t)(sec * a->rate[n] / a->scale[n]);
    }
    return 1;
}

static void avi_close(demux_t *d)
{
    avi_t *a = (avi_t *)d->priv;
    if (!a) return;
    free(a->idx);
    free(a);
    d->priv = NULL;
}

int demux_avi_open(demux_t *d)
{
    avi_t *a = (avi_t *)calloc(1, sizeof(avi_t));
    if (!a) return 0;
    d->priv = a; d->fmt = "AVI";
    d->read = avi_read; d->seek = avi_seek; d->close = avi_close;
    d->frame_count_ts = 1;
    a->vstream = -1;
    for (int i = 0; i < 100; i++) a->map[i] = -1;
    bio_t *b = &d->io;
    bio_seek(b, 12);
    uint32_t us_per_frame = 0, total_frames = 0;
    while (bio_tell(b) + 8 <= b->size) {
        uint64_t at = bio_tell(b);
        uint32_t id = bio_rl32(b), sz = bio_rl32(b);
        uint64_t next = at + 8 + sz + (sz & 1);
        if (id == FCC('L','I','S','T')) {
            uint32_t t = bio_rl32(b);
            if (t == FCC('h','d','r','l')) {
                uint64_t end = at + 8 + sz;
                while (bio_tell(b) + 8 <= end) {
                    uint64_t a2 = bio_tell(b);
                    uint32_t i2 = bio_rl32(b), s2 = bio_rl32(b);
                    uint64_t n2 = a2 + 8 + s2 + (s2 & 1);
                    if (i2 == FCC('a','v','i','h')) { us_per_frame = bio_rl32(b); bio_skip(b, 12); total_frames = bio_rl32(b); }
                    else if (i2 == FCC('L','I','S','T') && bio_rl32(b) == FCC('s','t','r','l')) parse_strl(d, a, a2 + 8 + s2);
                    bio_seek(b, n2);
                }
            } else if (t == FCC('m','o','v','i')) {
                a->movi_start = at + 8;
                a->movi_end = at + 8 + sz;
                if (sz == 0 || a->movi_end > b->size) a->movi_end = b->size;
            }
        } else if (id == FCC('i','d','x','1') && sz >= 16) {
            a->nidx = sz / 16;
            a->idx = (idx_t *)malloc(sizeof(idx_t) * a->nidx);
            if (!a->idx) a->nidx = 0;
            for (int i = 0; i < a->nidx; i++) { a->idx[i].id = bio_rl32(b); a->idx[i].flags = bio_rl32(b); a->idx[i].off = bio_rl32(b); a->idx[i].size = bio_rl32(b); }
        }
        if (next <= at || (sz == 0 && id != FCC('L','I','S','T'))) next = at + 8;
        bio_seek(b, next);
        if (a->movi_start && a->idx) break;
    }
    if (!a->movi_start) return 0;
    if (a->nidx) a->idx_rel = a->idx[0].off < a->movi_start;
    if (a->vstream >= 0) {
        int vs = a->vstream;
        double fdur = (double)a->scale[vs] / (a->rate[vs] ? a->rate[vs] : 25);
        int64_t vcount = 0;
        for (int i = 0; i < a->nidx; i++) if (is_digit2(a->idx[i].id) && chunk_stream(a->idx[i].id) == vs) vcount++;
        if (!vcount) vcount = total_frames;
        d->duration = vcount * fdur;
    } else if (us_per_frame && total_frames) d->duration = total_frames * us_per_frame / 1e6;
    bio_seek(b, a->movi_start + 4);
    return d->nst > 0;
}

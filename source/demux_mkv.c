/* Matroska / WebM demuxer: tracks, lacing (Xiph/EBML/fixed), header-stripping, cues-based seeking */
#include "demux.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define ID_SEGMENT   0x18538067
#define ID_SEEKHEAD  0x114D9B74
#define ID_INFO      0x1549A966
#define ID_TRACKS    0x1654AE6B
#define ID_CUES      0x1C53BB6B
#define ID_CLUSTER   0x1F43B675
#define ID_TAGS      0x1254C367
#define ID_CHAPTERS  0x1043A770
#define ID_ATTACH    0x1941A469

typedef struct { double t; uint64_t pos; } cue_t;

typedef struct {
    uint64_t seg_start, seg_end;
    uint64_t scale;            /* timecode scale (ns) */
    int tracknum[MAX_STREAMS]; /* mkv track number per stream */
    uint8_t *strip[MAX_STREAMS]; int strip_len[MAX_STREAMS];
    cue_t *cues; int ncues, capcues;
    uint64_t cues_pos;
    uint64_t first_cluster;
    /* cluster state */
    uint64_t cluster_end;      /* 0 = not inside a cluster; ~0 = unknown size */
    int64_t cluster_tc;
    /* pending laced frames */
    uint8_t *lace; int lace_len;
    int lace_n, lace_i, lace_off, lace_sizes[256];
    int lace_stream; double lace_pts, lace_fdur; int lace_key;
} mkv_t;

static int read_id(bio_t *b, uint32_t *id)
{
    int c = bio_getc(b);
    if (c < 0) return 0;
    int len = 1;
    if (c & 0x80) len = 1; else if (c & 0x40) len = 2; else if (c & 0x20) len = 3; else if (c & 0x10) len = 4; else return 0;
    uint32_t v = c;
    for (int i = 1; i < len; i++) { int k = bio_getc(b); if (k < 0) return 0; v = (v << 8) | k; }
    *id = v;
    return 1;
}

static int read_vint(bio_t *b, uint64_t *v, int *unknown)
{
    int c = bio_getc(b);
    if (c < 0 || c == 0) return 0;
    int len = 1;
    while (!(c & (0x80 >> (len - 1)))) len++;
    uint64_t x = c & (0xFF >> len);
    int allones = x == (uint64_t)(0xFF >> len);
    for (int i = 1; i < len; i++) { int k = bio_getc(b); if (k < 0) return 0; x = (x << 8) | k; if (k != 0xFF) allones = 0; }
    *v = x;
    if (unknown) *unknown = allones;
    return 1;
}

static int mem_vint(const uint8_t *p, int n, int64_t *v, int is_signed)
{
    if (n < 1 || !p[0]) return 0;
    int len = 1;
    while (!(p[0] & (0x80 >> (len - 1)))) len++;
    if (len > n) return 0;
    uint64_t x = p[0] & (0xFF >> len);
    for (int i = 1; i < len; i++) x = (x << 8) | p[i];
    if (is_signed) x -= ((uint64_t)1 << (7 * len - 1)) - 1;
    *v = (int64_t)x;
    return len;
}

static uint64_t read_uint(bio_t *b, uint64_t n) { uint64_t v = 0; for (uint64_t i = 0; i < n && i < 8; i++) v = (v << 8) | bio_r8(b); if (n > 8) bio_skip(b, n - 8); return v; }

static double read_float(bio_t *b, uint64_t n)
{
    if (n == 4) { uint32_t u = bio_rb32(b); float f; memcpy(&f, &u, 4); return f; }
    if (n == 8) { uint64_t u = bio_rb64(b); double f; memcpy(&f, &u, 8); return f; }
    bio_skip(b, n);
    return 0;
}

static void read_str(bio_t *b, uint64_t n, char *out, int cap)
{
    int k = n < (uint64_t)cap - 1 ? (int)n : cap - 1;
    bio_read(b, out, k);
    out[k] = 0;
    if (n > (uint64_t)k) bio_skip(b, n - k);
}

static void map_codec(stream_t *s, const char *cid, int type)
{
    if (type == 1) {
        s->type = ST_VIDEO;
        if (!strcmp(cid, "V_MPEG4/ISO/AVC")) s->codec = VC_H264;
        else if (!strcmp(cid, "V_MPEGH/ISO/HEVC")) s->codec = VC_HEVC;
        else if (!strncmp(cid, "V_MPEG4/ISO/", 12)) s->codec = VC_MPEG4;
        else if (!strcmp(cid, "V_MPEG2") || !strcmp(cid, "V_MPEG1")) s->codec = VC_MPEG2;
        else if (!strcmp(cid, "V_VP9")) s->codec = VC_VP9;
        else if (!strcmp(cid, "V_AV1")) s->codec = VC_AV1;
        else if (!strcmp(cid, "V_MS/VFW/FOURCC")) s->codec = VC_OTHER;   /* resolved from BITMAPINFOHEADER */
        else s->codec = VC_OTHER;
    } else if (type == 2) {
        s->type = ST_AUDIO;
        if (!strncmp(cid, "A_AAC", 5)) s->codec = AC_AAC;
        else if (!strcmp(cid, "A_MPEG/L3") || !strcmp(cid, "A_MPEG/L2") || !strcmp(cid, "A_MPEG/L1")) s->codec = AC_MP3;
        else if (!strcmp(cid, "A_AC3")) s->codec = AC_AC3;
        else if (!strcmp(cid, "A_EAC3")) s->codec = AC_EAC3;
        else if (!strcmp(cid, "A_VORBIS")) s->codec = AC_VORBIS;
        else if (!strcmp(cid, "A_PCM/INT/LIT")) s->codec = AC_PCM_S16LE;
        else if (!strcmp(cid, "A_PCM/INT/BIG")) s->codec = AC_PCM_S16BE;
        else if (!strncmp(cid, "A_DTS", 5)) s->codec = AC_DTS;
        else if (!strcmp(cid, "A_OPUS")) s->codec = AC_OPUS;
        else if (!strcmp(cid, "A_FLAC")) s->codec = AC_FLAC;
        else if (!strcmp(cid, "A_TRUEHD")) s->codec = AC_TRUEHD;
        else s->codec = AC_OTHER;
    } else if (type == 0x11) {
        s->type = ST_SUB;
        if (!strcmp(cid, "S_TEXT/UTF8") || !strcmp(cid, "S_TEXT/ASCII")) s->codec = SC_SRT;
        else if (!strcmp(cid, "S_TEXT/ASS") || !strcmp(cid, "S_TEXT/SSA") || !strcmp(cid, "S_ASS") || !strcmp(cid, "S_SSA")) s->codec = SC_ASS;
        else s->codec = SC_OTHER;
    }
}

static void parse_track(demux_t *d, mkv_t *m, uint64_t end)
{
    bio_t *b = &d->io;
    stream_t s; memset(&s, 0, sizeof(s));
    int num = 0, type = 0;
    char cid[64] = "";
    uint64_t defdur = 0;
    uint8_t *strip = NULL; int strip_len = 0;
    while (bio_tell(b) < end) {
        uint32_t id; uint64_t sz;
        if (!read_id(b, &id) || !read_vint(b, &sz, NULL)) break;
        uint64_t next = bio_tell(b) + sz;
        switch (id) {
        case 0xD7: num = (int)read_uint(b, sz); break;
        case 0x83: type = (int)read_uint(b, sz); break;
        case 0x86: read_str(b, sz, cid, sizeof(cid)); break;
        case 0x63A2: if (sz < 1 << 20) { s.extra = (uint8_t *)malloc(sz); if (s.extra) { bio_read(b, s.extra, (uint32_t)sz); s.extra_len = (int)sz; } } break;
        case 0x22B59C: read_str(b, sz, s.lang, sizeof(s.lang)); break;
        case 0x536E: read_str(b, sz, s.name, sizeof(s.name)); break;
        case 0x23E383: defdur = read_uint(b, sz); break;
        case 0xE0: {   /* Video */
            uint64_t ve = next;
            while (bio_tell(b) < ve) {
                uint32_t i2; uint64_t s2;
                if (!read_id(b, &i2) || !read_vint(b, &s2, NULL)) break;
                uint64_t n2 = bio_tell(b) + s2;
                if (i2 == 0xB0) s.w = (int)read_uint(b, s2);
                else if (i2 == 0xBA) s.h = (int)read_uint(b, s2);
                bio_seek(b, n2);
            }
            break;
        }
        case 0xE1: {   /* Audio */
            uint64_t ae = next;
            while (bio_tell(b) < ae) {
                uint32_t i2; uint64_t s2;
                if (!read_id(b, &i2) || !read_vint(b, &s2, NULL)) break;
                uint64_t n2 = bio_tell(b) + s2;
                if (i2 == 0xB5) s.rate = (int)read_float(b, s2);
                else if (i2 == 0x9F) s.channels = (int)read_uint(b, s2);
                else if (i2 == 0x6264) s.bits = (int)read_uint(b, s2);
                bio_seek(b, n2);
            }
            break;
        }
        case 0x6D80: {   /* ContentEncodings -> header stripping (algo 3) */
            uint64_t ce = next;
            int algo = -1; uint8_t *set = NULL; int setl = 0;
            /* flat scan for ContentCompAlgo (0x4254) and ContentCompSettings (0x4255) */
            while (bio_tell(b) < ce) {
                uint32_t i2; uint64_t s2;
                if (!read_id(b, &i2) || !read_vint(b, &s2, NULL)) break;
                if (i2 == 0x6240 || i2 == 0x5034) continue;           /* containers: descend */
                uint64_t n2 = bio_tell(b) + s2;
                if (i2 == 0x4254) algo = (int)read_uint(b, s2);
                else if (i2 == 0x4255 && s2 < 256) { set = (uint8_t *)malloc(s2); if (set) { bio_read(b, set, (uint32_t)s2); setl = (int)s2; } }
                bio_seek(b, n2);
            }
            if ((algo == 3 || algo == -1) && set) { strip = set; strip_len = setl; } else free(set);
            break;
        }
        }
        bio_seek(b, next);
    }
    if (!type || d->nst >= MAX_STREAMS) { free(s.extra); free(strip); return; }
    map_codec(&s, cid, type);
    if (!s.type) { free(s.extra); free(strip); return; }
    if (s.type == ST_VIDEO && defdur) s.fps = 1e9 / (double)defdur;
    if (s.type == ST_AUDIO && !s.channels) s.channels = 2;
    if (s.type == ST_AUDIO && !s.rate) s.rate = 48000;
    if (s.type == ST_AUDIO && (s.codec == AC_PCM_S16LE || s.codec == AC_PCM_S16BE) && s.bits == 24) s.codec = s.codec == AC_PCM_S16LE ? AC_PCM_S24LE : AC_OTHER;
    if (s.type == ST_AUDIO && s.codec == AC_PCM_S16LE && s.bits == 8) s.codec = AC_PCM_U8;
    if (s.codec == VC_H264 && s.extra_len >= 7) s.nal_len = (s.extra[4] & 3) + 1;
    if (s.type == ST_VIDEO && !strcmp(cid, "V_MS/VFW/FOURCC") && s.extra_len >= 40) {
        char fc[5] = { (char)s.extra[16], (char)s.extra[17], (char)s.extra[18], (char)s.extra[19], 0 };
        if (!strcasecmp(fc, "H264") || !strcasecmp(fc, "AVC1") || !strcasecmp(fc, "X264")) s.codec = VC_H264;
        else if (!strcasecmp(fc, "XVID") || !strcasecmp(fc, "DIVX") || !strcasecmp(fc, "DX50") || !strcasecmp(fc, "FMP4") || !strcasecmp(fc, "MP4V") || !strcasecmp(fc, "DIV3")) s.codec = !strcasecmp(fc, "DIV3") ? VC_OTHER : VC_MPEG4;
        else if (!strcasecmp(fc, "MPG2") || !strcasecmp(fc, "MPEG")) s.codec = VC_MPEG2;
        if (s.w == 0) { s.w = s.extra[4] | (s.extra[5] << 8) | (s.extra[6] << 16); s.h = s.extra[8] | (s.extra[9] << 8); }
        /* keep only the codec-specific part after the 40-byte header */
        int rest = s.extra_len - 40;
        if (rest > 0) memmove(s.extra, s.extra + 40, rest);
        s.extra_len = rest > 0 ? rest : 0;
    }
    if (s.type == ST_AUDIO && s.codec == AC_AAC && !s.extra_len) {
        /* old-style A_AAC/MPEG4/LC... without private data: synthesize an AudioSpecificConfig */
        static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };
        int fi = 4;
        for (int i = 0; i < 12; i++) if (rates[i] == s.rate) fi = i;
        int obj = strstr(cid, "SBR") ? 5 : strstr(cid, "MAIN") ? 1 : 2;
        s.extra = (uint8_t *)malloc(2);
        if (s.extra) { s.extra[0] = (obj << 3) | (fi >> 1); s.extra[1] = ((fi & 1) << 7) | (s.channels << 3); s.extra_len = 2; }
    }
    s.id = num;
    m->tracknum[d->nst] = num;
    m->strip[d->nst] = strip; m->strip_len[d->nst] = strip_len;
    d->st[d->nst++] = s;
}

static void parse_cues(demux_t *d, mkv_t *m, uint64_t end)
{
    bio_t *b = &d->io;
    while (bio_tell(b) < end) {
        uint32_t id; uint64_t sz;
        if (!read_id(b, &id) || !read_vint(b, &sz, NULL)) break;
        uint64_t next = bio_tell(b) + sz;
        if (id == 0xBB) {
            double t = -1; uint64_t pos = 0; int has = 0;
            while (bio_tell(b) < next) {
                uint32_t i2; uint64_t s2;
                if (!read_id(b, &i2) || !read_vint(b, &s2, NULL)) break;
                uint64_t n2 = bio_tell(b) + s2;
                if (i2 == 0xB3) t = (double)read_uint(b, s2) * m->scale / 1e9;
                else if (i2 == 0xB7 && !has) {
                    while (bio_tell(b) < n2) {
                        uint32_t i3; uint64_t s3;
                        if (!read_id(b, &i3) || !read_vint(b, &s3, NULL)) break;
                        uint64_t n3 = bio_tell(b) + s3;
                        if (i3 == 0xF1) { pos = read_uint(b, s3); has = 1; }
                        bio_seek(b, n3);
                    }
                }
                bio_seek(b, n2);
            }
            if (has && t >= 0) {
                if (m->ncues >= m->capcues) { int nc = m->capcues ? m->capcues * 2 : 256; cue_t *nq = (cue_t *)realloc(m->cues, sizeof(cue_t) * nc); if (!nq) return; m->cues = nq; m->capcues = nc; }
                m->cues[m->ncues++] = (cue_t){ t, m->seg_start + pos };
            }
        }
        bio_seek(b, next);
    }
}

static void parse_seekhead(demux_t *d, mkv_t *m, uint64_t end, uint64_t *tracks_pos, uint64_t *info_pos)
{
    bio_t *b = &d->io;
    while (bio_tell(b) < end) {
        uint32_t id; uint64_t sz;
        if (!read_id(b, &id) || !read_vint(b, &sz, NULL)) break;
        uint64_t next = bio_tell(b) + sz;
        if (id == 0x4DBB) {
            uint32_t sid = 0; uint64_t spos = 0;
            while (bio_tell(b) < next) {
                uint32_t i2; uint64_t s2;
                if (!read_id(b, &i2) || !read_vint(b, &s2, NULL)) break;
                uint64_t n2 = bio_tell(b) + s2;
                if (i2 == 0x53AB) sid = (uint32_t)read_uint(b, s2);
                else if (i2 == 0x53AC) spos = read_uint(b, s2);
                bio_seek(b, n2);
            }
            if (sid == ID_CUES) m->cues_pos = m->seg_start + spos;
            if (sid == ID_TRACKS) *tracks_pos = m->seg_start + spos;
            if (sid == ID_INFO) *info_pos = m->seg_start + spos;
        }
        bio_seek(b, next);
    }
}

static void parse_info(demux_t *d, mkv_t *m, uint64_t end, double *dur_raw)
{
    bio_t *b = &d->io;
    while (bio_tell(b) < end) {
        uint32_t id; uint64_t sz;
        if (!read_id(b, &id) || !read_vint(b, &sz, NULL)) break;
        uint64_t next = bio_tell(b) + sz;
        if (id == 0x2AD7B1) m->scale = read_uint(b, sz);
        else if (id == 0x4489) *dur_raw = read_float(b, sz);
        bio_seek(b, next);
    }
}

static int stream_for_track(demux_t *d, mkv_t *m, int num)
{
    for (int i = 0; i < d->nst; i++) if (m->tracknum[i] == num) return i;
    return -1;
}

/* returns 1 with a packet from the pending lace */
static int emit_lace(demux_t *d, mkv_t *m, packet_t *pk)
{
    if (m->lace_i >= m->lace_n) return 0;
    int sz = m->lace_sizes[m->lace_i];
    int si = m->lace_stream;
    int strip = m->strip_len[si];
    uint8_t *buf = demux_buf(d, sz + strip + 16);
    if (!buf) return -1;
    if (strip) memcpy(buf, m->strip[si], strip);
    memcpy(buf + strip, m->lace + m->lace_off, sz);
    pk->stream = si; pk->data = buf; pk->size = sz + strip;
    pk->pts = m->lace_pts + m->lace_i * m->lace_fdur;
    if (m->lace_i > 0 && m->lace_fdur <= 0) pk->pts = -1;
    pk->key = m->lace_key;
    m->lace_off += sz;
    m->lace_i++;
    return 1;
}

static int handle_block(demux_t *d, mkv_t *m, uint64_t sz, int simple, int key_hint, packet_t *pk)
{
    bio_t *b = &d->io;
    if (sz < 4 || sz > 64 * 1024 * 1024) { bio_skip(b, sz); return 0; }
    uint8_t *raw = (uint8_t *)realloc(m->lace, sz);
    if (!raw) { bio_skip(b, sz); return 0; }
    m->lace = raw;
    if (bio_read(b, raw, (uint32_t)sz) != (int)sz) return -1;
    int64_t tn;
    int o = mem_vint(raw, (int)sz, &tn, 0);
    if (!o || o + 3 > (int)sz) return 0;
    int si = stream_for_track(d, m, (int)tn);
    if (si < 0) return 0;
    int16_t rel = (int16_t)((raw[o] << 8) | raw[o + 1]);
    uint8_t flags = raw[o + 2];
    o += 3;
    double pts = (double)(m->cluster_tc + rel) * m->scale / 1e9;
    int key = simple ? (flags & 0x80) != 0 : key_hint;
    int lacing = (flags >> 1) & 3;
    int n = 1;
    int total = (int)sz - o;
    if (lacing) {
        n = raw[o++] + 1;
        if (n > 256) n = 256;
        total = (int)sz - o;
        int sum = 0;
        if (lacing == 1) {          /* Xiph */
            for (int i = 0; i < n - 1; i++) { int v = 0, c; do { if (o >= (int)sz) return 0; c = raw[o++]; v += c; } while (c == 255); m->lace_sizes[i] = v; sum += v; }
        } else if (lacing == 3) {   /* EBML */
            int64_t v; int k = mem_vint(raw + o, (int)sz - o, &v, 0); if (!k) return 0; o += k;
            m->lace_sizes[0] = (int)v; sum = (int)v;
            for (int i = 1; i < n - 1; i++) { int64_t dv; k = mem_vint(raw + o, (int)sz - o, &dv, 1); if (!k) return 0; o += k; v += dv; m->lace_sizes[i] = (int)v; sum += (int)v; }
        } else {                    /* fixed */
            for (int i = 0; i < n - 1; i++) { m->lace_sizes[i] = total / n; sum += total / n; }
        }
        total = (int)sz - o;
        m->lace_sizes[n - 1] = total - sum;
        if (m->lace_sizes[n - 1] < 0) return 0;
    } else m->lace_sizes[0] = total;
    m->lace_n = n; m->lace_i = 0; m->lace_off = o;
    m->lace_stream = si; m->lace_pts = pts; m->lace_key = key;
    stream_t *s = &d->st[si];
    m->lace_fdur = 0;
    if (s->type == ST_AUDIO && n > 1) {
        int spf = s->codec == AC_AAC ? 1024 : s->codec == AC_MP3 ? 1152 : s->codec == AC_AC3 ? 1536 : 0;
        if (spf && s->rate) m->lace_fdur = (double)spf / s->rate;
    }
    return emit_lace(d, m, pk);
}

static int mkv_read(demux_t *d, packet_t *pk)
{
    mkv_t *m = (mkv_t *)d->priv;
    bio_t *b = &d->io;
    int r = emit_lace(d, m, pk);
    if (r) return r;
    for (;;) {
        if (bio_eof(b) || bio_tell(b) >= m->seg_end) return 0;
        uint64_t at = bio_tell(b);
        uint32_t id; uint64_t sz; int unk = 0;
        if (!read_id(b, &id) || !read_vint(b, &sz, &unk)) {
            /* resync: look for the next cluster id */
            bio_seek(b, at + 1);
            uint32_t w = 0; int c;
            while ((c = bio_getc(b)) >= 0) { w = (w << 8) | c; if (w == ID_CLUSTER) { bio_seek(b, bio_tell(b) - 4); break; } }
            if (c < 0) return 0;
            continue;
        }
        if (id == ID_CLUSTER) { m->cluster_end = unk ? ~0ULL : bio_tell(b) + sz; continue; }   /* descend */
        if (id == 0xE7) { m->cluster_tc = (int64_t)read_uint(b, sz); continue; }
        if (id == 0xA3) { int k = handle_block(d, m, sz, 1, 0, pk); if (k) return k; continue; }
        if (id == 0xA0) {   /* BlockGroup */
            uint64_t ge = bio_tell(b) + sz;
            uint64_t bpos = 0, bsz = 0; int ref = 0; uint64_t bdur = 0;
            while (bio_tell(b) < ge) {
                uint32_t i2; uint64_t s2;
                if (!read_id(b, &i2) || !read_vint(b, &s2, NULL)) break;
                uint64_t n2 = bio_tell(b) + s2;
                if (i2 == 0xA1) { bpos = bio_tell(b); bsz = s2; }
                else if (i2 == 0xFB) ref = 1;
                else if (i2 == 0x9B) bdur = read_uint(b, s2);
                bio_seek(b, n2);
            }
            (void)bdur;
            if (bsz) {
                bio_seek(b, bpos);
                int k = handle_block(d, m, bsz, 0, !ref, pk);
                bio_seek(b, ge);
                if (k) return k;
            }
            continue;
        }
        if (id == ID_SEGMENT) continue;
        /* skip anything else (cues, tags, void, crc ...) */
        if (unk) return 0;
        bio_skip(b, sz);
    }
}

static int mkv_seek(demux_t *d, double t)
{
    mkv_t *m = (mkv_t *)d->priv;
    m->lace_n = m->lace_i = 0;
    uint64_t pos = 0;
    if (m->ncues) {
        int lo = 0, hi = m->ncues - 1, best = 0;
        while (lo <= hi) { int mid = (lo + hi) / 2; if (m->cues[mid].t <= t) { best = mid; lo = mid + 1; } else hi = mid - 1; }
        pos = m->cues[best].pos;
    } else {
        /* no index: estimate by bitrate, then resync to a cluster */
        uint64_t span = m->seg_end - m->first_cluster;
        pos = m->first_cluster + (d->duration > 0 ? (uint64_t)(span * (t / d->duration)) : 0);
        if (t <= 0.5) pos = m->first_cluster;
        bio_seek(&d->io, pos);
        uint32_t w = 0; int c;
        while ((c = bio_getc(&d->io)) >= 0) { w = (w << 8) | c; if (w == ID_CLUSTER) break; }
        if (c < 0) return 0;
        pos = bio_tell(&d->io) - 4;
    }
    bio_seek(&d->io, pos);
    return 1;
}

static void mkv_close(demux_t *d)
{
    mkv_t *m = (mkv_t *)d->priv;
    if (!m) return;
    for (int i = 0; i < MAX_STREAMS; i++) free(m->strip[i]);
    free(m->cues);
    free(m->lace);
    free(m);
    d->priv = NULL;
}

int demux_mkv_open(demux_t *d)
{
    mkv_t *m = (mkv_t *)calloc(1, sizeof(mkv_t));
    if (!m) return 0;
    d->priv = m; d->fmt = "MKV";
    d->read = mkv_read; d->seek = mkv_seek; d->close = mkv_close;
    m->scale = 1000000;
    bio_t *b = &d->io;
    uint32_t id; uint64_t sz; int unk;
    /* EBML header */
    if (!read_id(b, &id) || id != 0x1A45DFA3 || !read_vint(b, &sz, NULL)) return 0;
    bio_skip(b, sz);
    /* Segment */
    for (;;) {
        if (!read_id(b, &id) || !read_vint(b, &sz, &unk)) return 0;
        if (id == ID_SEGMENT) break;
        bio_skip(b, sz);
    }
    m->seg_start = bio_tell(b);
    m->seg_end = unk ? b->size : m->seg_start + sz;
    if (m->seg_end > b->size) m->seg_end = b->size;
    double dur_raw = 0;
    uint64_t tracks_pos = 0, info_pos = 0;
    int have_tracks = 0, have_info = 0;
    /* top-level until the first cluster */
    while (bio_tell(b) < m->seg_end) {
        uint64_t at = bio_tell(b);
        if (!read_id(b, &id) || !read_vint(b, &sz, &unk)) break;
        uint64_t next = bio_tell(b) + sz;
        if (id == ID_CLUSTER) { m->first_cluster = at; break; }
        if (id == ID_SEEKHEAD) parse_seekhead(d, m, next, &tracks_pos, &info_pos);
        else if (id == ID_INFO) { parse_info(d, m, next, &dur_raw); have_info = 1; }
        else if (id == ID_TRACKS) {
            while (bio_tell(b) < next) {
                uint32_t i2; uint64_t s2;
                if (!read_id(b, &i2) || !read_vint(b, &s2, NULL)) break;
                uint64_t n2 = bio_tell(b) + s2;
                if (i2 == 0xAE) parse_track(d, m, n2);
                bio_seek(b, n2);
            }
            have_tracks = 1;
        }
        else if (id == ID_CUES) { m->cues_pos = at; parse_cues(d, m, next); }
        if (unk) break;
        bio_seek(b, next);
    }
    if (!have_info && info_pos) { bio_seek(b, info_pos); if (read_id(b, &id) && id == ID_INFO && read_vint(b, &sz, NULL)) parse_info(d, m, bio_tell(b) + sz, &dur_raw); }
    if (!have_tracks && tracks_pos) {
        bio_seek(b, tracks_pos);
        if (read_id(b, &id) && id == ID_TRACKS && read_vint(b, &sz, NULL)) {
            uint64_t end = bio_tell(b) + sz;
            while (bio_tell(b) < end) {
                uint32_t i2; uint64_t s2;
                if (!read_id(b, &i2) || !read_vint(b, &s2, NULL)) break;
                uint64_t n2 = bio_tell(b) + s2;
                if (i2 == 0xAE) parse_track(d, m, n2);
                bio_seek(b, n2);
            }
        }
    }
    if (!m->ncues && m->cues_pos) {
        bio_seek(b, m->cues_pos);
        if (read_id(b, &id) && id == ID_CUES && read_vint(b, &sz, NULL)) parse_cues(d, m, bio_tell(b) + sz);
    }
    d->duration = dur_raw * m->scale / 1e9;
    if (!m->first_cluster) return 0;
    bio_seek(b, m->first_cluster);
    return d->nst > 0;
}

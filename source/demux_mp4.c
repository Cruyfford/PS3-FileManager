/* MP4 / MOV / M4V / 3GP demuxer: own atom scan for track types and codec configs, mp4ff for sample tables */
#include "demux.h"
#include <mp4ff.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int mtrack;             /* mp4ff track index */
    int count, next;
    int32_t scale;
    int64_t *dts;           /* per sample */
    int stream;
} mtrk_t;

typedef struct {
    vfs_file_t *f;
    mp4ff_callback_t cb;
    mp4ff_t *mp4;
    mtrk_t t[MAX_STREAMS];
    int nt;
} mp4_t;

static uint32_t cb_read(void *ud, void *buf, uint32_t len) { int64_t r = vfs_read((vfs_file_t *)ud, buf, len); return r > 0 ? (uint32_t)r : 0; }
static uint32_t cb_seek(void *ud, uint64_t pos) { return vfs_seek((vfs_file_t *)ud, pos) == 0 ? 0 : (uint32_t)-1; }
static uint32_t cb_write(void *ud, void *b, uint32_t l) { (void)ud; (void)b; (void)l; return 0; }
static uint32_t cb_trunc(void *ud) { (void)ud; return 0; }

static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int atom_find(vfs_file_t *f, uint64_t start, uint64_t end, const char *type, uint64_t *pos, uint64_t *len, int nth)
{
    uint64_t p = start;
    uint8_t h[16];
    int seen = 0;
    while (p + 8 <= end) {
        if (vfs_seek(f, p) != 0 || vfs_read(f, h, 8) != 8) return 0;
        uint64_t sz = rd32(h);
        uint32_t hl = 8;
        if (sz == 1) { if (vfs_read(f, h + 8, 8) != 8) return 0; sz = ((uint64_t)rd32(h + 8) << 32) | rd32(h + 12); hl = 16; }
        else if (sz == 0) sz = end - p;
        if (sz < hl) return 0;
        if (!memcmp(h + 4, type, 4)) { if (seen++ == nth) { *pos = p + hl; *len = sz - hl; return 1; } }
        p += sz;
    }
    return 0;
}

/* esds: returns DecoderConfigDescriptor objectTypeIndication */
static int esds_object_type(vfs_file_t *f, uint64_t p, uint64_t l)
{
    uint8_t b[256];
    if (l > sizeof(b)) l = sizeof(b);
    vfs_seek(f, p);
    if (vfs_read(f, b, (uint32_t)l) != (int64_t)l) return 0;
    for (uint32_t i = 4; i + 2 < l; i++) if (b[i] == 0x04) {   /* DecoderConfigDescr tag */
        uint32_t k = i + 1;
        while (k < l && (b[k] & 0x80)) k++;
        k++;
        if (k < l) return b[k];
    }
    return 0;
}

static void scan_tracks(demux_t *d, mp4_t *m)
{
    vfs_file_t *f = m->f;
    uint64_t mp, ml, end = vfs_size(f);
    if (!atom_find(f, 0, end, "moov", &mp, &ml, 0)) return;
    for (int t = 0; t < 32 && d->nst < MAX_STREAMS; t++) {
        uint64_t tp, tl, p, l;
        if (!atom_find(f, mp, mp + ml, "trak", &tp, &tl, t)) break;
        if (!atom_find(f, tp, tp + tl, "mdia", &p, &l, 0)) continue;
        uint64_t mdp = p, mdl = l;
        char handler[5] = { 0 };
        if (atom_find(f, mdp, mdp + mdl, "hdlr", &p, &l, 0) && l >= 12) { vfs_seek(f, p + 8); vfs_read(f, handler, 4); }
        int type = !strcmp(handler, "vide") ? ST_VIDEO : !strcmp(handler, "soun") ? ST_AUDIO : 0;
        if (!type) continue;
        stream_t *s = &d->st[d->nst];
        memset(s, 0, sizeof(*s));
        s->type = type; s->id = t;
        /* language from mdhd */
        if (atom_find(f, mdp, mdp + mdl, "mdhd", &p, &l, 0) && l >= 24) {
            uint8_t hb[36]; vfs_seek(f, p); vfs_read(f, hb, l > 36 ? 36 : (uint32_t)l);
            int off = hb[0] == 1 ? 32 : 20;
            uint16_t lg = (hb[off] << 8) | hb[off + 1];
            if (lg && lg != 0x7FFF) { s->lang[0] = ((lg >> 10) & 31) + 0x60; s->lang[1] = ((lg >> 5) & 31) + 0x60; s->lang[2] = (lg & 31) + 0x60; }
        }
        if (!atom_find(f, mdp, mdp + mdl, "minf", &p, &l, 0) || !atom_find(f, p, p + l, "stbl", &p, &l, 0) || !atom_find(f, p, p + l, "stsd", &p, &l, 0)) continue;
        /* first sample entry */
        uint8_t eh[8];
        vfs_seek(f, p + 8);
        if (vfs_read(f, eh, 8) != 8) continue;
        uint64_t ep = p + 16, el = rd32(eh) > 8 ? rd32(eh) - 8 : 0;
        char fourcc[5] = { (char)eh[4], (char)eh[5], (char)eh[6], (char)eh[7], 0 };
        if (type == ST_VIDEO) {
            uint8_t vh[78];
            vfs_seek(f, ep);
            if (vfs_read(f, vh, 78) == 78) { s->w = (vh[24] << 8) | vh[25]; s->h = (vh[26] << 8) | vh[27]; }
            if (!strcmp(fourcc, "avc1") || !strcmp(fourcc, "avc3")) {
                s->codec = VC_H264;
                uint64_t cp, cl;
                if (atom_find(f, ep + 78, ep + el, "avcC", &cp, &cl, 0) && cl >= 8 && cl < 8192) {
                    s->extra = (uint8_t *)malloc(cl);
                    vfs_seek(f, cp);
                    if (s->extra && vfs_read(f, s->extra, (uint32_t)cl) == (int64_t)cl) { s->extra_len = (int)cl; s->nal_len = (s->extra[4] & 3) + 1; }
                }
            } else if (!strcmp(fourcc, "hvc1") || !strcmp(fourcc, "hev1")) s->codec = VC_HEVC;
            else if (!strcmp(fourcc, "mp4v")) {
                uint64_t cp, cl;
                int ot = atom_find(f, ep + 78, ep + el, "esds", &cp, &cl, 0) ? esds_object_type(f, cp, cl) : 0x20;
                s->codec = (ot >= 0x60 && ot <= 0x65) || ot == 0x6A ? VC_MPEG2 : VC_MPEG4;
            } else if (!strcmp(fourcc, "vp09")) s->codec = VC_VP9;
            else if (!strcmp(fourcc, "av01")) s->codec = VC_AV1;
            else s->codec = VC_OTHER;
        } else {
            uint8_t ah[28];
            vfs_seek(f, ep);
            if (vfs_read(f, ah, 28) == 28) { s->channels = (ah[16] << 8) | ah[17]; s->bits = (ah[18] << 8) | ah[19]; s->rate = (ah[24] << 8) | ah[25]; }
            if (!strcmp(fourcc, "mp4a")) {
                uint64_t cp, cl;
                int ot = 0x40;
                int base = ah[8] == 0 && ah[9] == 1 ? 28 + 16 : ah[8] == 0 && ah[9] == 2 ? 28 + 36 : 28;
                if (atom_find(f, ep + base, ep + el, "esds", &cp, &cl, 0) || atom_find(f, ep + 28, ep + el, "esds", &cp, &cl, 0)) ot = esds_object_type(f, cp, cl);
                s->codec = (ot == 0x69 || ot == 0x6B) ? AC_MP3 : AC_AAC;
            } else if (!strcmp(fourcc, "ac-3")) s->codec = AC_AC3;
            else if (!strcmp(fourcc, "ec-3")) s->codec = AC_EAC3;
            else if (!strcmp(fourcc, ".mp3")) s->codec = AC_MP3;
            else if (!strcmp(fourcc, "Opus")) s->codec = AC_OPUS;
            else if (!strcmp(fourcc, "fLaC")) s->codec = AC_FLAC;
            else if (!strcmp(fourcc, "sowt")) s->codec = AC_PCM_S16LE;
            else if (!strcmp(fourcc, "twos")) s->codec = AC_PCM_S16BE;
            else s->codec = AC_OTHER;
        }
        d->nst++;
    }
}

static int mp4_read(demux_t *d, packet_t *pk)
{
    mp4_t *m = (mp4_t *)d->priv;
    /* earliest next sample across tracks */
    int best = -1; double bt = 1e18;
    for (int i = 0; i < m->nt; i++) {
        mtrk_t *t = &m->t[i];
        if (t->next >= t->count) continue;
        double tt = (double)t->dts[t->next] / t->scale;
        if (tt < bt) { bt = tt; best = i; }
    }
    if (best < 0) return 0;
    mtrk_t *t = &m->t[best];
    int idx = t->next++;
    int need = mp4ff_read_sample_getsize(m->mp4, t->mtrack, idx);
    uint8_t *b = demux_buf(d, need + 64);
    if (!b) return -1;
    int n = need ? mp4ff_read_sample_v2(m->mp4, t->mtrack, idx, b) : 0;
    pk->stream = t->stream;
    pk->data = b; pk->size = n > 0 ? n : 0;
    int64_t pts = t->dts[idx] + (d->st[t->stream].type == ST_VIDEO ? mp4ff_get_sample_offset(m->mp4, t->mtrack, idx) : 0);
    pk->pts = (double)pts / t->scale;
    pk->key = -1;
    return 1;
}

static int sample_key(stream_t *s, const uint8_t *b, int n)
{
    if (s->codec == VC_H264 && s->nal_len) {
        for (int k = 0; k + s->nal_len <= n;) {
            uint32_t l = 0;
            for (int i = 0; i < s->nal_len; i++) l = (l << 8) | b[k + i];
            k += s->nal_len;
            if (k >= n) break;
            if ((b[k] & 31) == 5) return 1;
            k += l;
        }
        return 0;
    }
    if (s->codec == VC_MPEG4) {
        for (int i = 0; i + 4 < n; i++) if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 0xB6) return (b[i + 4] >> 6) == 0;
        return 0;
    }
    if (s->codec == VC_MPEG2) {
        for (int i = 0; i + 3 < n; i++) if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && (b[i + 3] == 0xB3 || b[i + 3] == 0xB8)) return 1;
        return 0;
    }
    return 1;
}

static int mp4_seek(demux_t *d, double target)
{
    mp4_t *m = (mp4_t *)d->priv;
    /* video track decides the keyframe */
    double vt = target;
    int vi = -1;
    for (int i = 0; i < m->nt; i++) if (d->st[m->t[i].stream].type == ST_VIDEO) { vi = i; break; }
    if (vi >= 0) {
        mtrk_t *t = &m->t[vi];
        stream_t *s = &d->st[t->stream];
        int64_t tt = (int64_t)(target * t->scale);
        int lo = 0, hi = t->count - 1;
        while (lo < hi) { int mid = (lo + hi + 1) / 2; if (t->dts[mid] <= tt) lo = mid; else hi = mid - 1; }
        int idx = lo;
        for (int k = idx; k >= 0 && k > idx - 900; k--) {
            int need = mp4ff_read_sample_getsize(m->mp4, t->mtrack, k);
            uint8_t *b = demux_buf(d, need + 64);
            if (b && need && mp4ff_read_sample_v2(m->mp4, t->mtrack, k, b) > 0 && sample_key(s, b, need)) { idx = k; break; }
            if (k == 0) idx = 0;
        }
        t->next = idx;
        vt = (double)t->dts[idx] / t->scale;
    }
    for (int i = 0; i < m->nt; i++) {
        if (i == vi) continue;
        mtrk_t *t = &m->t[i];
        int64_t want = (int64_t)(vt * t->scale);
        int lo = 0, hi = t->count - 1;
        while (lo < hi) { int mid = (lo + hi + 1) / 2; if (t->dts[mid] <= want) lo = mid; else hi = mid - 1; }
        t->next = lo;
    }
    return 1;
}

static void mp4_close(demux_t *d)
{
    mp4_t *m = (mp4_t *)d->priv;
    if (!m) return;
    for (int i = 0; i < m->nt; i++) free(m->t[i].dts);
    if (m->mp4) mp4ff_close(m->mp4);
    if (m->f) vfs_close(m->f);
    free(m);
    d->priv = NULL;
}

int demux_mp4_open(demux_t *d, const char *path)
{
    mp4_t *m = (mp4_t *)calloc(1, sizeof(mp4_t));
    if (!m) return 0;
    d->priv = m;
    d->fmt = "MP4";
    d->close = mp4_close; d->read = mp4_read; d->seek = mp4_seek;
    m->f = vfs_open_read(path);
    if (!m->f) return 0;
    scan_tracks(d, m);
    m->cb.read = cb_read; m->cb.write = cb_write; m->cb.seek = cb_seek; m->cb.truncate = cb_trunc; m->cb.user_data = m->f;
    vfs_seek(m->f, 0);
    m->mp4 = mp4ff_open_read(&m->cb);
    if (!m->mp4) return 0;
    int ntr = mp4ff_total_tracks(m->mp4);
    for (int i = 0; i < d->nst; i++) {
        stream_t *s = &d->st[i];
        if (s->id >= ntr) continue;
        mtrk_t *t = &m->t[m->nt];
        t->mtrack = s->id; t->stream = i;
        t->count = mp4ff_num_samples(m->mp4, s->id);
        t->scale = mp4ff_time_scale(m->mp4, s->id);
        if (t->scale <= 0) t->scale = 1;
        if (t->count <= 0) continue;
        t->dts = (int64_t *)malloc(sizeof(int64_t) * (t->count + 1));
        if (!t->dts) continue;
        int64_t acc = 0;
        for (int k = 0; k < t->count; k++) { t->dts[k] = acc; acc += mp4ff_get_sample_duration(m->mp4, s->id, k); }
        t->dts[t->count] = acc;
        double dur = (double)acc / t->scale;
        if (dur > d->duration) d->duration = dur;
        if (s->type == ST_VIDEO && dur > 0) s->fps = t->count / dur;
        if (s->type == ST_AUDIO && (s->codec == AC_AAC || s->codec == AC_MP3)) {
            unsigned char *cfg = NULL; unsigned int cl = 0;
            mp4ff_get_decoder_config(m->mp4, s->id, &cfg, &cl);
            if (cfg && cl) { s->extra = (uint8_t *)malloc(cl); if (s->extra) { memcpy(s->extra, cfg, cl); s->extra_len = cl; } }
            free(cfg);
            int sr = mp4ff_get_sample_rate(m->mp4, s->id);
            if (sr > 0) s->rate = sr;
        }
        if (s->type == ST_VIDEO && s->codec == VC_MPEG4 && !s->extra) {
            unsigned char *cfg = NULL; unsigned int cl = 0;
            mp4ff_get_decoder_config(m->mp4, s->id, &cfg, &cl);
            if (cfg && cl) { s->extra = (uint8_t *)malloc(cl); if (s->extra) { memcpy(s->extra, cfg, cl); s->extra_len = cl; } }
            free(cfg);
        }
        m->nt++;
    }
    return d->nst > 0;
}

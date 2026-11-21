/* MPEG transport stream (TS / M2TS / MTS) and program stream (VOB / MPG) demuxers.
 * Packets are PES payloads; video is marked es=1 so the player splits it into access units. */
#include "demux.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

static int64_t parse_pts5(const uint8_t *p)
{
    return ((int64_t)(p[0] & 0x0E) << 29) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] & 0xFE) << 14) | ((int64_t)p[3] << 7) | (p[4] >> 1);
}

/* MPEG-2 PES header at p (after 00 00 01 id len): returns header length, pts or -1 */
static int pes_header2(const uint8_t *p, int n, int64_t *pts)
{
    *pts = -1;
    if (n < 3) return -1;
    if ((p[0] & 0xC0) == 0x80) {
        int hl = p[2];
        if ((p[1] & 0x80) && n >= 8) *pts = parse_pts5(p + 3);
        return 3 + hl;
    }
    /* MPEG-1 */
    int i = 0;
    while (i < n && p[i] == 0xFF) i++;
    if (i < n && (p[i] & 0xC0) == 0x40) i += 2;
    if (i < n && (p[i] & 0xF0) == 0x20) { if (i + 5 <= n) *pts = parse_pts5(p + i); return i + 5; }
    if (i < n && (p[i] & 0xF0) == 0x30) { if (i + 5 <= n) *pts = parse_pts5(p + i); return i + 10; }
    return i + 1;
}

/* ======================================================= TS */
typedef struct {
    int pid, stream;
    uint8_t *buf; int len, cap;
    int64_t pts;
    int started;
} tspes_t;

typedef struct {
    int psize, poff;            /* 188/192 and offset of the sync byte */
    int pmt_pid;
    tspes_t pes[MAX_STREAMS];
    int npes;
    int64_t start_pts;
    uint64_t data_start;
    int ready_idx;              /* pes with a completed packet */
    uint8_t *out; int outcap;
} ts_t;

static tspes_t *ts_find(ts_t *t, int pid) { for (int i = 0; i < t->npes; i++) if (t->pes[i].pid == pid) return &t->pes[i]; return NULL; }

static void ts_add_stream(demux_t *d, ts_t *t, int type, int pid, const uint8_t *desc, int dl)
{
    if (ts_find(t, pid) || d->nst >= MAX_STREAMS) return;
    stream_t s; memset(&s, 0, sizeof(s));
    int ac3_desc = 0, eac3_desc = 0;
    for (int i = 0; i + 2 <= dl;) {
        int tag = desc[i], l = desc[i + 1];
        if (i + 2 + l > dl) break;
        if (tag == 0x0A && l >= 3) { memcpy(s.lang, desc + i + 2, 3); s.lang[3] = 0; }
        if (tag == 0x6A) ac3_desc = 1;
        if (tag == 0x7A) eac3_desc = 1;
        if (tag == 0x05 && l >= 4 && !memcmp(desc + i + 2, "AC-3", 4)) ac3_desc = 1;
        i += 2 + l;
    }
    switch (type) {
    case 0x01: case 0x02: s.type = ST_VIDEO; s.codec = VC_MPEG2; s.es = 1; break;
    case 0x1B: s.type = ST_VIDEO; s.codec = VC_H264; s.es = 1; break;
    case 0x24: s.type = ST_VIDEO; s.codec = VC_HEVC; s.es = 1; break;
    case 0x10: s.type = ST_VIDEO; s.codec = VC_MPEG4; s.es = 1; break;
    case 0xEA: s.type = ST_VIDEO; s.codec = VC_OTHER; break;
    case 0x0F: s.type = ST_AUDIO; s.codec = AC_AAC; break;
    case 0x11: s.type = ST_AUDIO; s.codec = AC_OTHER; break;   /* LATM */
    case 0x03: case 0x04: s.type = ST_AUDIO; s.codec = AC_MP3; break;
    case 0x81: s.type = ST_AUDIO; s.codec = AC_AC3; break;
    case 0x87: s.type = ST_AUDIO; s.codec = AC_EAC3; break;
    case 0x82: case 0x85: case 0x86: s.type = ST_AUDIO; s.codec = AC_DTS; break;
    case 0x83: s.type = ST_AUDIO; s.codec = AC_TRUEHD; break;
    case 0x80: s.type = ST_AUDIO; s.codec = AC_LPCM; break;
    case 0x06: if (ac3_desc) { s.type = ST_AUDIO; s.codec = AC_AC3; } else if (eac3_desc) { s.type = ST_AUDIO; s.codec = AC_EAC3; } break;
    }
    if (!s.type) return;
    s.id = pid;
    s.channels = 2; s.rate = 48000;
    tspes_t *p = &t->pes[t->npes++];
    memset(p, 0, sizeof(*p));
    p->pid = pid; p->stream = d->nst; p->pts = -1;
    d->st[d->nst++] = s;
}

static void ts_parse_psi(demux_t *d, ts_t *t, const uint8_t *p, int n, int pid)
{
    int ptr = p[0];
    p += 1 + ptr; n -= 1 + ptr;
    if (n < 8) return;
    int tid = p[0];
    int slen = ((p[1] & 0x0F) << 8) | p[2];
    if (slen + 3 > n) slen = n - 3;
    if (pid == 0 && tid == 0x00) {
        for (int i = 8; i + 4 <= slen + 3 - 4; i += 4) {
            int prog = (p[i] << 8) | p[i + 1];
            int pp = ((p[i + 2] & 0x1F) << 8) | p[i + 3];
            if (prog != 0 && !t->pmt_pid) t->pmt_pid = pp;
        }
    } else if (tid == 0x02 && pid == t->pmt_pid) {
        int pil = ((p[10] & 0x0F) << 8) | p[11];
        int i = 12 + pil;
        int end = slen + 3 - 4;
        while (i + 5 <= end) {
            int type = p[i], epid = ((p[i + 1] & 0x1F) << 8) | p[i + 2];
            int il = ((p[i + 3] & 0x0F) << 8) | p[i + 4];
            ts_add_stream(d, t, type, epid, p + i + 5, il < end - i - 5 ? il : end - i - 5);
            i += 5 + il;
        }
    }
}

/* complete the PES in slot e into a packet */
static int ts_emit(demux_t *d, ts_t *t, tspes_t *e, packet_t *pk)
{
    if (e->len < 9 || e->buf[0] != 0 || e->buf[1] != 0 || e->buf[2] != 1) { e->len = 0; return 0; }
    int64_t pts;
    int hl = pes_header2(e->buf + 6, e->len - 6, &pts);
    if (hl < 0 || 6 + hl > e->len) { e->len = 0; return 0; }
    int n = e->len - 6 - hl;
    uint8_t *o = demux_buf(d, n + 64);
    if (!o) return -1;
    memcpy(o, e->buf + 6 + hl, n);
    e->len = 0;
    if (pts >= 0 && t->start_pts < 0) t->start_pts = pts;
    pk->stream = e->stream; pk->data = o; pk->size = n;
    pk->pts = pts >= 0 ? (double)(pts - t->start_pts) / 90000.0 : -1;
    if (pk->pts < -1) pk->pts += (double)(1LL << 33) / 90000.0;
    pk->key = -1;
    return n > 0;
}

static int ts_next_packet(demux_t *d, ts_t *t, uint8_t *pkt)
{
    bio_t *b = &d->io;
    for (;;) {
        uint8_t raw[192];
        if (bio_read(b, raw, t->psize) != t->psize) return 0;
        if (raw[t->poff] != 0x47) {
            /* lost sync: search forward */
            uint64_t at = bio_tell(b) - t->psize + 1;
            bio_seek(b, at);
            int c;
            while ((c = bio_getc(b)) >= 0 && c != 0x47) {}
            if (c < 0) return 0;
            bio_seek(b, bio_tell(b) - 1 - t->poff);
            continue;
        }
        memcpy(pkt, raw + t->poff, 188);
        return 1;
    }
}

static int ts_read(demux_t *d, packet_t *pk)
{
    ts_t *t = (ts_t *)d->priv;
    uint8_t p[188];
    for (;;) {
        if (!ts_next_packet(d, t, p)) {
            /* flush whatever is pending at EOF */
            for (int i = 0; i < t->npes; i++) if (t->pes[i].len) { int r = ts_emit(d, t, &t->pes[i], pk); if (r) return r; }
            return 0;
        }
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        int pusi = p[1] & 0x40;
        int afc = (p[3] >> 4) & 3;
        int o = 4;
        if (afc == 2) continue;
        if (afc == 3) o = 5 + p[4];
        if (o >= 188) continue;
        if (pid == 0 || (t->pmt_pid && pid == t->pmt_pid)) { if (pusi) ts_parse_psi(d, t, p + o, 188 - o, pid); continue; }
        tspes_t *e = ts_find(t, pid);
        if (!e) continue;
        int r = 0;
        packet_t tmp;
        if (pusi) {
            if (e->len && e->started) r = ts_emit(d, t, e, &tmp);
            e->started = 1;
        }
        if (!e->started) continue;
        int n = 188 - o;
        if (e->len + n > e->cap) {
            int nc = e->cap ? e->cap * 2 : 65536;
            while (nc < e->len + n) nc *= 2;
            if (nc > 8 * 1024 * 1024) { e->len = 0; continue; }
            uint8_t *nb = (uint8_t *)realloc(e->buf, nc);
            if (!nb) return -1;
            e->buf = nb; e->cap = nc;
        }
        memcpy(e->buf + e->len, p + o, n);
        e->len += n;
        if (r < 0) return -1;
        if (r > 0) { *pk = tmp; return 1; }
    }
}

static int64_t ts_scan_pts(demux_t *d, ts_t *t, uint64_t from, uint64_t to, int last)
{
    bio_t *b = &d->io;
    int64_t found = -1;
    uint64_t pos = from - (from % t->psize);
    bio_seek(b, pos);
    uint8_t raw[192];
    while (bio_tell(b) + t->psize <= to) {
        if (bio_read(b, raw, t->psize) != t->psize) break;
        uint8_t *p = raw + t->poff;
        if (p[0] != 0x47) continue;
        if (!(p[1] & 0x40)) continue;
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (!ts_find(t, pid)) continue;
        int afc = (p[3] >> 4) & 3, o = 4;
        if (afc == 2) continue;
        if (afc == 3) o = 5 + p[4];
        if (o + 14 > 188) continue;
        if (p[o] || p[o + 1] || p[o + 2] != 1) continue;
        int64_t pts;
        pes_header2(p + o + 6, 188 - o - 6, &pts);
        if (pts >= 0) { found = pts; if (!last) break; }
    }
    return found;
}

static int ts_seek(demux_t *d, double tm)
{
    ts_t *t = (ts_t *)d->priv;
    for (int i = 0; i < t->npes; i++) { t->pes[i].len = 0; t->pes[i].started = 0; }
    uint64_t span = d->io.size - t->data_start;
    uint64_t pos = t->data_start + (d->duration > 0 ? (uint64_t)(span * (tm / d->duration)) : 0);
    if (tm <= 0.5) pos = t->data_start;
    pos -= (pos - t->data_start) % t->psize;
    bio_seek(&d->io, pos);
    return 1;
}

static void ts_close(demux_t *d)
{
    ts_t *t = (ts_t *)d->priv;
    if (!t) return;
    for (int i = 0; i < t->npes; i++) free(t->pes[i].buf);
    free(t);
    d->priv = NULL;
}

int demux_ts_open(demux_t *d)
{
    ts_t *t = (ts_t *)calloc(1, sizeof(ts_t));
    if (!t) return 0;
    d->priv = t; d->fmt = "TS";
    d->read = ts_read; d->seek = ts_seek; d->close = ts_close;
    t->start_pts = -1;
    bio_t *b = &d->io;
    uint8_t h[400];
    bio_read(b, h, sizeof(h));
    if (h[0] == 0x47 && h[188] == 0x47) { t->psize = 188; t->poff = 0; }
    else { t->psize = 192; t->poff = 4; }
    t->data_start = 0;
    /* find PAT/PMT in the first few MB */
    bio_seek(b, 0);
    uint8_t p[188];
    uint64_t limit = 8 * 1024 * 1024;
    while (bio_tell(b) < limit && ts_next_packet(d, t, p)) {
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        int afc = (p[3] >> 4) & 3, o = 4;
        if (afc == 2) continue;
        if (afc == 3) o = 5 + p[4];
        if (o >= 188 || !(p[1] & 0x40)) continue;
        if (pid == 0 || (t->pmt_pid && pid == t->pmt_pid)) ts_parse_psi(d, t, p + o, 188 - o, pid);
        if (t->npes && pid == t->pmt_pid) break;
    }
    if (!t->npes) return 0;
    int64_t first = ts_scan_pts(d, t, 0, b->size < limit ? b->size : limit, 0);
    uint64_t tail = b->size > 6 * 1024 * 1024 ? b->size - 6 * 1024 * 1024 : 0;
    int64_t last = ts_scan_pts(d, t, tail, b->size, 1);
    if (first >= 0) t->start_pts = first;
    if (first >= 0 && last > first) d->duration = (double)(last - first) / 90000.0;
    bio_seek(b, 0);
    return 1;
}

/* ======================================================= PS (VOB / MPG) */
typedef struct {
    int map[256];          /* stream key -> stream index; key = id (0xC0..0xEF) or 0x100 + substream */
    int64_t start_pts;
    int scanning;
} ps_t;

static int ps_stream(demux_t *d, ps_t *s, int key, int id, int sub, const uint8_t *payload, int n)
{
    int k = key & 0xFF;
    if (key >= 0x100) k = 0x80 | (sub & 0x7F);
    (void)k;
    for (int i = 0; i < d->nst; i++) if (d->st[i].id == key) return i;
    if (!s->scanning || d->nst >= MAX_STREAMS) return -1;
    stream_t st; memset(&st, 0, sizeof(st));
    st.id = key; st.channels = 2; st.rate = 48000;
    if (id >= 0xE0 && id <= 0xEF) {
        st.type = ST_VIDEO; st.es = 1; st.codec = VC_MPEG2;
        for (int i = 0; i + 4 < n; i++) if (payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
            if (payload[i + 3] == 0xB3) break;
            if ((payload[i + 3] & 0x1F) == 7 || (payload[i + 3] & 0x1F) == 9) { st.codec = VC_H264; break; }
        }
    } else if (id >= 0xC0 && id <= 0xDF) { st.type = ST_AUDIO; st.codec = AC_MP3; }
    else if (id == 0xBD) {
        if (sub >= 0x80 && sub <= 0x87) { st.type = ST_AUDIO; st.codec = AC_AC3; }
        else if (sub >= 0x88 && sub <= 0x8F) { st.type = ST_AUDIO; st.codec = AC_DTS; }
        else if (sub >= 0xA0 && sub <= 0xA7) { st.type = ST_AUDIO; st.codec = AC_LPCM; }
        else return -1;
    } else return -1;
    d->st[d->nst] = st;
    return d->nst++;
}

static int ps_read(demux_t *d, packet_t *pk)
{
    ps_t *s = (ps_t *)d->priv;
    bio_t *b = &d->io;
    uint32_t w = 0xFFFFFFFF;
    for (;;) {
        int c = bio_getc(b);
        if (c < 0) return 0;
        w = (w << 8) | c;
        if ((w & 0xFFFFFF00) != 0x00000100) continue;
        int id = w & 0xFF;
        if (id == 0xBA) {   /* pack header */
            uint8_t h[10];
            if (bio_read(b, h, 10) != 10) return 0;
            if ((h[0] & 0xC0) == 0x40) bio_skip(b, h[9] & 7);   /* MPEG-2 stuffing */
            else bio_seek(b, bio_tell(b) - 2);                     /* MPEG-1: 8 bytes */
            w = 0xFFFFFFFF;
            continue;
        }
        if (id < 0xBB) continue;
        int len = bio_rb16(b);
        w = 0xFFFFFFFF;
        if (id == 0xBB || id == 0xBE || id == 0xBF || id == 0xBC || len == 0) { bio_skip(b, len); continue; }
        if (!((id >= 0xC0 && id <= 0xEF) || id == 0xBD)) { bio_skip(b, len); continue; }
        uint8_t *buf = demux_buf(d, len + 64);
        if (!buf) return -1;
        if (bio_read(b, buf, len) != len) return 0;
        int64_t pts;
        int hl = pes_header2(buf, len, &pts);
        if (hl < 0 || hl > len) continue;
        uint8_t *pl = buf + hl; int n = len - hl;
        int key = id, sub = 0;
        if (id == 0xBD) {
            if (n < 1) continue;
            sub = pl[0];
            key = 0x100 + sub;
            if (sub >= 0x80 && sub <= 0x8F) { pl += 4; n -= 4; }
            else if (sub >= 0xA0 && sub <= 0xA7) { pl += 7; n -= 7; }
            else { pl += 1; n -= 1; }
            if (n <= 0) continue;
        }
        int si = ps_stream(d, s, key, id, sub, pl, n);
        if (si < 0) continue;
        if (pts >= 0 && s->start_pts < 0) s->start_pts = pts;
        pk->stream = si; pk->data = pl; pk->size = n; pk->key = -1;
        pk->pts = pts >= 0 ? (double)(pts - s->start_pts) / 90000.0 : -1;
        return 1;
    }
}

static int ps_seek(demux_t *d, double t)
{
    bio_t *b = &d->io;
    uint64_t pos = d->duration > 0 ? (uint64_t)(b->size * (t / d->duration)) : 0;
    if (t <= 0.5) pos = 0;
    bio_seek(b, pos);
    /* align to a pack header */
    uint32_t w = 0xFFFFFFFF; int c;
    while ((c = bio_getc(b)) >= 0) { w = (w << 8) | c; if (w == 0x000001BA) { bio_seek(b, bio_tell(b) - 4); break; } }
    return 1;
}

static void ps_close(demux_t *d) { free(d->priv); d->priv = NULL; }

static int64_t ps_scan_pts(demux_t *d, uint64_t from, uint64_t to, int last)
{
    bio_t *b = &d->io;
    bio_seek(b, from);
    int64_t found = -1;
    uint32_t w = 0xFFFFFFFF; int c;
    while (bio_tell(b) < to && (c = bio_getc(b)) >= 0) {
        w = (w << 8) | c;
        int id = w & 0xFF;
        if ((w & 0xFFFFFF00) != 0x100 || !((id >= 0xC0 && id <= 0xEF) || id == 0xBD)) continue;
        uint8_t h[20];
        uint64_t at = bio_tell(b);
        if (bio_read(b, h, 20) != 20) break;
        int64_t pts;
        pes_header2(h + 2, 18, &pts);
        if (pts >= 0) { found = pts; if (!last) break; }
        bio_seek(b, at);
        w = 0xFFFFFFFF;
    }
    return found;
}

int demux_ps_open(demux_t *d)
{
    ps_t *s = (ps_t *)calloc(1, sizeof(ps_t));
    if (!s) return 0;
    d->priv = s; d->fmt = "MPEG-PS";
    d->read = ps_read; d->seek = ps_seek; d->close = ps_close;
    s->start_pts = -1;
    bio_t *b = &d->io;
    /* discover streams from the first part of the file */
    s->scanning = 1;
    packet_t pk;
    uint64_t limit = 6 * 1024 * 1024;
    while (bio_tell(b) < limit && ps_read(d, &pk) == 1) {}
    s->scanning = 0;
    uint64_t lim2 = b->size < limit ? b->size : limit;
    int64_t first = ps_scan_pts(d, 0, lim2, 0);
    uint64_t tail = b->size > 4 * 1024 * 1024 ? b->size - 4 * 1024 * 1024 : 0;
    int64_t last = ps_scan_pts(d, tail, b->size, 1);
    if (first >= 0) s->start_pts = first;
    if (first >= 0 && last > first) d->duration = (double)(last - first) / 90000.0;
    bio_seek(b, 0);
    return d->nst > 0;
}

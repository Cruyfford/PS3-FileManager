/* Patch engine for Arabization / translation patches. */
#include "patcher.h"
#include "fileops.h"
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define BUF (1024 * 1024)

static char *E; static int EN;
static int fail(int code, const char *m) { if (E) snprintf(E, EN, "%s", m); return code; }

int patch_kind(const char *n)
{
    const char *e = strrchr(n, '.');
    if (!e) return PATCH_NONE;
    if (!strcasecmp(e, ".ips")) return PATCH_IPS;
    if (!strcasecmp(e, ".bps")) return PATCH_BPS;
    if (!strcasecmp(e, ".ppf")) return PATCH_PPF;
    if (!strcasecmp(e, ".xdelta") || !strcasecmp(e, ".vcdiff") || !strcasecmp(e, ".delta") || !strcasecmp(e, ".xd3")) return PATCH_XDELTA;
    return PATCH_NONE;
}

static uint8_t *load_all(const char *path, uint64_t *n, uint64_t max)
{
    vfs_file_t *f = vfs_open_read(path);
    if (!f) return NULL;
    uint64_t s = vfs_size(f);
    if (s > max) { vfs_close(f); return NULL; }
    uint8_t *b = malloc(s ? s : 1);
    uint64_t got = 0;
    while (b && got < s) { int64_t r = vfs_read(f, b + got, (uint32_t)(s - got > BUF ? BUF : s - got)); if (r <= 0) break; got += r; }
    vfs_close(f);
    if (!b || got != s) { free(b); return NULL; }
    *n = s;
    return b;
}

/* copy src -> dst fully, keeps dst open for in-place patching */
static vfs_file_t *copy_base(const char *src, const char *dst, uint64_t *size, volatile int *cancel, patch_cb cb, uint64_t limit)
{
    vfs_file_t *in = vfs_open_read(src);
    if (!in) { fail(OPE_OPEN_SRC, src); return NULL; }
    vfs_file_t *out = vfs_open_write(dst);
    if (!out) { vfs_close(in); fail(OPE_OPEN_DST, dst); return NULL; }
    uint8_t *b = malloc(BUF);
    uint64_t s = vfs_size(in), done = 0;
    if (s > limit) s = limit;
    int ok = b != NULL;
    while (ok && done < s) {
        if (cancel && *cancel) { ok = 0; break; }
        int64_t r = vfs_read(in, b, (uint32_t)(s - done > BUF ? BUF : s - done));
        if (r <= 0 || vfs_write(out, b, (uint32_t)r) != r) { ok = 0; break; }
        done += r; if (cb) cb(r, NULL);
    }
    free(b); vfs_close(in);
    if (!ok) { vfs_close(out); vfs_unlink(dst); fail(OPE_WRITE, dst); return NULL; }
    *size = s;
    return out;
}

static int put_at(vfs_file_t *f, uint64_t off, const uint8_t *d, uint32_t n) { return vfs_seek(f, off) == 0 && vfs_write(f, d, n) == (int64_t)n; }

/* ---------------------------------------------------------- IPS */
static int apply_ips(const uint8_t *p, uint64_t pn, const char *src, const char *dst, volatile int *cancel, patch_cb cb, volatile uint64_t *total)
{
    if (pn < 8 || memcmp(p, "PATCH", 5)) return fail(OPE_READ, "not an IPS patch");
    vfs_file_t *in = vfs_open_read(src); if (!in) return fail(OPE_OPEN_SRC, src);
    *total = vfs_size(in) + pn; vfs_close(in);
    uint64_t size, trunc = ~0ULL;
    if (pn >= 11 && !memcmp(p + pn - 6, "EOF", 3)) trunc = p[pn - 3] << 16 | p[pn - 2] << 8 | p[pn - 1];
    vfs_file_t *o = copy_base(src, dst, &size, cancel, cb, trunc);
    if (!o) return OPE_WRITE;
    uint64_t i = 5; int ok = 1;
    uint8_t *rle = malloc(65536);
    while (ok && i + 3 <= pn) {
        if (!memcmp(p + i, "EOF", 3) && (i + 3 == pn || i + 6 == pn)) {
            break;
        }
        if (i + 5 > pn) { ok = 0; break; }
        uint32_t off = p[i] << 16 | p[i + 1] << 8 | p[i + 2], len = p[i + 3] << 8 | p[i + 4];
        i += 5;
        if (len) { if (i + len > pn || !put_at(o, off, p + i, len)) ok = 0; i += len; }
        else {
            if (i + 3 > pn || !rle) { ok = 0; break; }
            uint32_t n = p[i] << 8 | p[i + 1]; memset(rle, p[i + 2], n); i += 3;
            if (!put_at(o, off, rle, n)) ok = 0;
        }
        if (cancel && *cancel) ok = 0;
    }
    free(rle); vfs_close(o);
    if (cb) cb(pn, NULL);
    if (!ok) { vfs_unlink(dst); return fail(OPE_WRITE, "IPS: damaged patch or write error"); }
    return 0;
}

/* ---------------------------------------------------------- PPF */
static int apply_ppf(const uint8_t *p, uint64_t pn, const char *src, const char *dst, volatile int *cancel, patch_cb cb, volatile uint64_t *total)
{
    if (pn < 60 || memcmp(p, "PPF", 3)) return fail(OPE_READ, "not a PPF patch");
    int ver = p[5];                               /* 0 = 1.0, 1 = 2.0, 2 = 3.0 */
    uint64_t i; int undo = 0, imagetype = 0, blockcheck = 0;
    if (ver == 0) i = 56;
    else if (ver == 1) i = 60 + 1024;
    else { imagetype = p[56]; blockcheck = p[57]; undo = p[58]; i = 60 + (blockcheck ? 1024 : 0); }
    (void)imagetype;
    /* PPF2/3 may end with a file_id.diz block */
    uint64_t end = pn;
    if (ver >= 1) {
        static const char tag[] = "@BEGIN_FILE_ID.DIZ";
        for (uint64_t k = i; k + 18 <= pn; k++) if (p[k] == '@' && !memcmp(p + k, tag, 18)) { end = k; break; }
    }
    vfs_file_t *in = vfs_open_read(src); if (!in) return fail(OPE_OPEN_SRC, src);
    *total = vfs_size(in) + pn; vfs_close(in);
    uint64_t size;
    vfs_file_t *o = copy_base(src, dst, &size, cancel, cb, ~0ULL);
    if (!o) return OPE_WRITE;
    int ok = 1;
    while (ok && i < end) {
        uint64_t off; uint32_t n;
        if (ver == 2) { if (i + 9 > end) break; off = 0; for (int k = 7; k >= 0; k--) off = off << 8 | p[i + k]; n = p[i + 8]; i += 9; }
        else { if (i + 5 > end) break; off = p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24; n = p[i + 4]; i += 5; }
        if (i + n > end) { ok = 0; break; }
        if (!put_at(o, off, p + i, n)) ok = 0;
        i += n;
        if (undo) i += n;
        if (cancel && *cancel) ok = 0;
    }
    vfs_close(o);
    if (cb) cb(pn, NULL);
    if (!ok) { vfs_unlink(dst); return fail(OPE_WRITE, "PPF: damaged patch or write error"); }
    return 0;
}

/* ---------------------------------------------------------- BPS */
static uint64_t vlq(const uint8_t *p, uint64_t *i, uint64_t n)
{
    uint64_t data = 0, shift = 1;
    while (*i < n) { uint8_t x = p[(*i)++]; data += (x & 0x7f) * shift; if (x & 0x80) break; shift <<= 7; data += shift; }
    return data;
}

static int apply_bps(const uint8_t *p, uint64_t pn, const char *src, const char *dst, volatile int *cancel, patch_cb cb, volatile uint64_t *total)
{
    if (pn < 19 || memcmp(p, "BPS1", 4)) return fail(OPE_READ, "not a BPS patch");
    uint64_t sn, i = 4;
    uint8_t *s = load_all(src, &sn, 512ULL * 1024 * 1024);
    if (!s) return fail(OPE_OPEN_SRC, "source too big (BPS max 512 MB) or unreadable");
    uint32_t pcrc = p[pn - 4] | p[pn - 3] << 8 | p[pn - 2] << 16 | (uint32_t)p[pn - 1] << 24;
    if ((uint32_t)crc32(0, p, pn - 4) != pcrc) { free(s); return fail(OPE_READ, "BPS: patch checksum mismatch"); }
    uint32_t scrc = p[pn - 12] | p[pn - 11] << 8 | p[pn - 10] << 16 | (uint32_t)p[pn - 9] << 24;
    if ((uint32_t)crc32(0, s, sn) != scrc) { free(s); return fail(OPE_READ, "BPS: this patch is for a different source file"); }
    uint64_t ss = vlq(p, &i, pn), ts = vlq(p, &i, pn), ms = vlq(p, &i, pn);
    i += ms;
    if (ss != sn || ts > 1024ULL * 1024 * 1024) { free(s); return fail(OPE_READ, "BPS: size mismatch"); }
    uint8_t *t = malloc(ts ? ts : 1);
    if (!t) { free(s); return fail(OPE_MEM, "memory"); }
    *total = ts;
    uint64_t op = 0, srel = 0, trel = 0, end = pn - 12;
    int ok = 1;
    while (ok && i < end && op < ts) {
        uint64_t d = vlq(p, &i, pn), len = (d >> 2) + 1;
        if (op + len > ts) { ok = 0; break; }
        switch (d & 3) {
        case 0: if (op + len > sn) ok = 0; else memcpy(t + op, s + op, len); op += len; break;
        case 1: if (i + len > end) ok = 0; else memcpy(t + op, p + i, len); i += len; op += len; break;
        case 2: { uint64_t x = vlq(p, &i, pn); srel += (x & 1 ? -1 : 1) * (int64_t)(x >> 1); if (srel + len > sn) { ok = 0; break; } memcpy(t + op, s + srel, len); srel += len; op += len; break; }
        case 3: { uint64_t x = vlq(p, &i, pn); trel += (x & 1 ? -1 : 1) * (int64_t)(x >> 1); if (trel >= op) { ok = 0; break; } while (len--) t[op++] = t[trel++]; break; }
        }
        if (cancel && *cancel) ok = 0;
    }
    free(s);
    uint32_t tcrc = p[pn - 8] | p[pn - 7] << 8 | p[pn - 6] << 16 | (uint32_t)p[pn - 5] << 24;
    if (ok && (op != ts || (uint32_t)crc32(0, t, ts) != tcrc)) { free(t); return fail(OPE_READ, "BPS: result checksum mismatch"); }
    if (!ok) { free(t); return fail(OPE_READ, "BPS: damaged patch"); }
    vfs_file_t *o = vfs_open_write(dst);
    if (!o) { free(t); return fail(OPE_OPEN_DST, dst); }
    uint64_t w = 0;
    while (w < ts) { uint32_t k = ts - w > BUF ? BUF : (uint32_t)(ts - w); if (vfs_write(o, t + w, k) != k) break; w += k; if (cb) cb(k, NULL); }
    vfs_close(o); free(t);
    if (w != ts) { vfs_unlink(dst); return fail(OPE_WRITE, dst); }
    return 0;
}

#ifdef HAVE_XDELTA
int xdelta_apply(const char *patch, const char *src, const char *dst, volatile int *cancel, patch_cb cb, volatile uint64_t *total, char *err, int en);
#endif

int patch_apply(const char *patch, const char *src, const char *dst, volatile int *cancel, patch_cb cb, volatile uint64_t *total, char *err, int en)
{
    E = err; EN = en;
    int k = patch_kind(patch);
    if (k == PATCH_XDELTA) {
#ifdef HAVE_XDELTA
        return xdelta_apply(patch, src, dst, cancel, cb, total, err, en);
#else
        return fail(OPE_READ, "xdelta not supported in this build");
#endif
    }
    uint64_t pn;
    uint8_t *p = load_all(patch, &pn, 256ULL * 1024 * 1024);
    if (!p) return fail(OPE_OPEN_SRC, patch);
    int r;
    if (pn >= 5 && !memcmp(p, "PATCH", 5)) r = apply_ips(p, pn, src, dst, cancel, cb, total);
    else if (pn >= 4 && !memcmp(p, "BPS1", 4)) r = apply_bps(p, pn, src, dst, cancel, cb, total);
    else if (pn >= 3 && !memcmp(p, "PPF", 3)) r = apply_ppf(p, pn, src, dst, cancel, cb, total);
    else r = fail(OPE_READ, "unknown patch format");
    free(p);
    return r;
}

/* ---------------------------------------------------------- create */
int patch_create(const char *orig, const char *mod, const char *out, volatile int *cancel, patch_cb cb, volatile uint64_t *total, char *err, int en)
{
    E = err; EN = en;
    vfs_file_t *a = vfs_open_read(orig), *b = vfs_open_read(mod);
    if (!a || !b) { if (a) vfs_close(a); if (b) vfs_close(b); return fail(OPE_OPEN_SRC, !a ? orig : mod); }
    uint64_t an = vfs_size(a), bn = vfs_size(b);
    int ips = bn <= 0xFFFFFF && an <= 0xFFFFFF && patch_kind(out) == PATCH_IPS;
    vfs_file_t *o = vfs_open_write(out);
    if (!o) { vfs_close(a); vfs_close(b); return fail(OPE_OPEN_DST, out); }
    *total = bn;
    uint8_t *ba = malloc(BUF), *bb = malloc(BUF);
    int ok = ba && bb;
    if (ips) ok = ok && vfs_write(o, "PATCH", 5) == 5;
    else {
        uint8_t h[60]; memset(h, 0, sizeof(h));
        memcpy(h, "PPF30", 5); h[5] = 2;
        snprintf((char *)h + 6, 51, "%-50s", "File Manager (CRUYFFORD) patch");
        h[56] = 0; h[57] = 0; h[58] = 0; h[59] = 0;
        ok = ok && vfs_write(o, h, 60) == 60;
    }
    uint64_t pos = 0;
    while (ok && pos < bn) {
        if (cancel && *cancel) { ok = 0; break; }
        int64_t rb = vfs_read(b, bb, BUF);
        if (rb <= 0) { ok = 0; break; }
        int64_t ra = pos < an ? vfs_read(a, ba, (uint32_t)rb) : 0;
        if (ra < 0) ra = 0;
        for (int64_t i = 0; i < rb && ok;) {
            if (i < ra && ba[i] == bb[i]) { i++; continue; }
            int64_t j = i, lim = ips ? 0xFFFF : 255;
            while (j < rb && j - i < lim && !(j < ra && ba[j] == bb[j])) j++;
            uint64_t off = pos + i; uint32_t n = (uint32_t)(j - i);
            if (ips) {
                if (off == 0x454F46) { off--; i--; n++; }          /* "EOF" offset is reserved */
                uint8_t h[5] = { off >> 16, off >> 8, off, n >> 8, n };
                ok = vfs_write(o, h, 5) == 5 && vfs_write(o, bb + i, n) == n;
            } else {
                uint8_t h[9]; for (int k = 0; k < 8; k++) h[k] = (uint8_t)(off >> (8 * k)); h[8] = (uint8_t)n;
                ok = vfs_write(o, h, 9) == 9 && vfs_write(o, bb + i, n) == n;
            }
            i = j;
        }
        pos += rb; if (cb) cb(rb, NULL);
    }
    if (ok && ips) ok = vfs_write(o, "EOF", 3) == 3;
    if (ok && ips && bn < an) { uint8_t t[3] = { bn >> 16, bn >> 8, bn }; ok = vfs_write(o, t, 3) == 3; }
    free(ba); free(bb); vfs_close(a); vfs_close(b); vfs_close(o);
    if (!ok) { vfs_unlink(out); return fail(OPE_WRITE, out); }
    return 0;
}

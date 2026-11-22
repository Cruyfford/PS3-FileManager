/* PKG extract / build and PARAM.SFO editing.
 * Build output matches PSL1GHT pkg.py + package_finalize (retail, AES-CTR, revision 0x8000). */
#include "pkgtool.h"
#include "fileops.h"
#include "vfs.h"
#include <polarssl/aes.h>
#include <polarssl/sha1.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define CHUNK (1024 * 1024)
#define MAX_ITEMS 30000

static const uint8_t PS3_PKG_KEY[16] = {
    0x2E, 0x7B, 0x71, 0xD7, 0xC9, 0xC9, 0xA1, 0x4E, 0xA3, 0x22, 0x1F, 0x18, 0x88, 0x28, 0xB8, 0xF8
};

static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static inline void wbe32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline void wbe64(uint8_t *p, uint64_t v) { wbe32(p, (uint32_t)(v >> 32)); wbe32(p + 4, (uint32_t)v); }
static inline uint32_t le32(const uint8_t *p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[1] << 8 | p[0]); }
static inline void wle32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void set_err(char *dst, int n, const char *p) { if (dst && n > 0) snprintf(dst, n, "%s", p); }

static void path_join(char *out, int n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (l && dir[l - 1] == '/') snprintf(out, n, "%s%s", dir, name);
    else snprintf(out, n, "%s/%s", dir, name);
}

static void mkdirs(const char *path)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' && p[-1] != ':' && p[-1] != '/') { *p = 0; vfs_mkdir(tmp); *p = '/'; }
    }
    vfs_mkdir(tmp);
}

/* ---------- keystreams ---------- */

typedef struct {
    int retail;
    aes_context aes;
    uint8_t iv[16];       /* retail: AES-CTR start counter */
    uint8_t dctx[0x40];   /* debug: SHA1 keystream context */
} crypt_t;

static void debug_ctx(uint8_t ctx[0x40], const uint8_t key16[16])
{
    memset(ctx, 0, 0x40);
    memcpy(ctx + 0x00, key16, 8);
    memcpy(ctx + 0x08, key16, 8);
    memcpy(ctx + 0x10, key16 + 8, 8);
    memcpy(ctx + 0x18, key16 + 8, 8);
}

/* XOR `len` bytes (len multiple of 16) with the keystream starting at block `blk` */
static void apply_keystream(crypt_t *c, uint8_t *buf, uint32_t len, uint64_t blk)
{
    uint8_t ks[20], ctr[16];
    for (uint32_t i = 0; i < len; i += 16, blk++) {
        if (c->retail) {
            uint64_t hi = be64(c->iv), lo = be64(c->iv + 8);
            uint64_t nlo = lo + blk;
            if (nlo < lo) hi++;
            wbe64(ctr, hi); wbe64(ctr + 8, nlo);
            aes_crypt_ecb(&c->aes, AES_ENCRYPT, ctr, ks);
        } else {
            wbe64(c->dctx + 0x38, blk);
            sha1(c->dctx, 0x40, ks);
        }
        for (int k = 0; k < 16; k++) buf[i + k] ^= ks[k];
    }
}

/* debug-mode crypt of a small buffer (pkg.py crypt()) */
static void debug_crypt(const uint8_t key16[16], uint64_t start_ctr, uint8_t *buf, uint32_t len)
{
    uint8_t ctx[0x40], ks[20];
    debug_ctx(ctx, key16);
    for (uint32_t i = 0; i < len; i += 16) {
        wbe64(ctx + 0x38, start_ctr + i / 16);
        sha1(ctx, 0x40, ks);
        for (uint32_t k = 0; k < 16 && i + k < len; k++) buf[i + k] ^= ks[k];
    }
}

/* ======================================================= extract */

static int read_full(vfs_file_t *f, void *buf, uint32_t len)
{
    uint32_t got = 0;
    while (got < len) {
        int64_t r = vfs_read(f, (uint8_t *)buf + got, len - got);
        if (r <= 0) return 0;
        got += (uint32_t)r;
    }
    return 1;
}

int pkgtool_extract(const char *pkg_path, const char *out_dir, volatile int *cancel,
                    pt_progress_fn cb, volatile uint64_t *total, char *err_path, int err_len)
{
    vfs_file_t *f = vfs_open_read(pkg_path);
    if (!f) { set_err(err_path, err_len, pkg_path); return OPE_OPEN_SRC; }

    uint8_t h[0x80];
    if (!read_full(f, h, sizeof(h)) || be32(h) != 0x7F504B47 || (be32(h + 4) & 0xFFFF) != 1) {
        vfs_close(f); set_err(err_path, err_len, pkg_path); return OPE_READ;
    }
    crypt_t c;
    memset(&c, 0, sizeof(c));
    c.retail = (h[4] & 0x80) ? 1 : 0;
    memcpy(c.iv, h + 0x70, 16);
    debug_ctx(c.dctx, h + 0x60);
    aes_setkey_enc(&c.aes, PS3_PKG_KEY, 128);

    uint32_t items = be32(h + 0x14);
    uint64_t data_off = be64(h + 0x20), data_size = be64(h + 0x28);
    char cid[40];
    memcpy(cid, h + 0x30, 36); cid[36] = 0;
    for (char *p = cid; *p; p++) if (*p == '/' || *p == ':') *p = '_';
    if (items == 0 || items > MAX_ITEMS) { vfs_close(f); return OPE_READ; }

    /* item table */
    uint32_t tlen = items * 32;
    uint8_t *tab = (uint8_t *)malloc(tlen);
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    if (!tab || !buf) { free(tab); free(buf); vfs_close(f); return OPE_MEM; }
    if (vfs_seek(f, data_off) != 0 || !read_full(f, tab, tlen)) { free(tab); free(buf); vfs_close(f); return OPE_READ; }
    apply_keystream(&c, tab, tlen, 0);

    *total = data_size;
    char root[1100];
    path_join(root, sizeof(root), out_dir, cid);
    mkdirs(root);

    int err = OPE_NONE;
    for (uint32_t i = 0; i < items && !err && !*cancel; i++) {
        const uint8_t *e = tab + i * 32;
        uint32_t noff = be32(e), nlen = be32(e + 4);
        uint64_t doff = be64(e + 8), dsize = be64(e + 16);
        uint32_t flags = be32(e + 24);
        if (nlen == 0 || nlen > 1000) continue;

        /* name (16-aligned region) */
        uint64_t nblk = noff / 16;
        uint32_t nskip = noff % 16, nal = (nskip + nlen + 15) & ~15u;
        uint8_t nb[1040];
        if (vfs_seek(f, data_off + nblk * 16) != 0 || !read_full(f, nb, nal)) { err = OPE_READ; break; }
        apply_keystream(&c, nb, nal, nblk);
        char name[1024];
        memcpy(name, nb + nskip, nlen); name[nlen] = 0;
        if (strstr(name, "..")) continue;

        char out[1400];
        path_join(out, sizeof(out), root, name);
        if ((flags & 0xFF) == 4) { mkdirs(out); continue; }

        /* make sure the parent exists */
        char parent[1400];
        snprintf(parent, sizeof(parent), "%s", out);
        char *sl = strrchr(parent, '/');
        if (sl) { *sl = 0; mkdirs(parent); }

        const char *bn = strrchr(name, '/');
        if (cb) cb(0, bn ? bn + 1 : name);
        vfs_file_t *o = vfs_open_write(out);
        if (!o) { set_err(err_path, err_len, out); err = OPE_OPEN_DST; break; }
        uint64_t left = dsize, blk = doff / 16;
        if (vfs_seek(f, data_off + doff) != 0) { err = OPE_READ; }
        while (!err && left > 0 && !*cancel) {
            uint32_t want = left > CHUNK ? CHUNK : (uint32_t)((left + 15) & ~15ULL);
            if (!read_full(f, buf, want)) {
                /* last block may be short at EOF: retry with exact size */
                err = OPE_READ; break;
            }
            apply_keystream(&c, buf, want, blk);
            uint32_t wlen = left < want ? (uint32_t)left : want;
            if (vfs_write(o, buf, wlen) != wlen) { set_err(err_path, err_len, out); err = OPE_WRITE; break; }
            left -= wlen; blk += want / 16;
            if (cb) cb(wlen, NULL);
        }
        vfs_close(o);
        if (err || *cancel) vfs_unlink(out);
    }
    free(tab); free(buf);
    vfs_close(f);
    return err;
}

/* ======================================================= build */

typedef struct { char rel[256]; uint64_t size; uint32_t flags; uint32_t name_off; uint64_t data_off; } item_t;

static item_t *g_items;
static int g_count;

/* order follows pkg.py: files of a folder first, then each sub-folder entry followed by its contents */
static int collect(const char *base, const char *rel, int depth)
{
    char dir[1100];
    if (rel[0]) path_join(dir, sizeof(dir), base, rel); else snprintf(dir, sizeof(dir), "%s", base);
    vfs_entry_t *ents = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * 2048);
    if (!ents) return 0;
    int n = vfs_list(dir, ents, 2048);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            if (ents[i].is_dir != pass) continue;
            if (g_count >= MAX_ITEMS) { free(ents); return 0; }
            item_t *it = &g_items[g_count++];
            memset(it, 0, sizeof(*it));
            if (rel[0]) snprintf(it->rel, sizeof(it->rel), "%s/%s", rel, ents[i].name);
            else snprintf(it->rel, sizeof(it->rel), "%s", ents[i].name);
            if (ents[i].is_dir) {
                it->flags = 0x80000004;
                if (depth < 24 && !collect(base, it->rel, depth + 1)) { free(ents); return 0; }
            } else {
                it->size = ents[i].size;
                it->flags = !strcmp(it->rel, "USRDIR/EBOOT.BIN") ? 0x80000001 : 0x80000003;
            }
        }
    }
    free(ents);
    return 1;
}

typedef struct {
    vfs_file_t *out;
    crypt_t c;
    sha1_context sha;
    uint8_t *buf;
    uint32_t fill;
    uint64_t blk;
    int err;
} writer_t;

static void w_raw(writer_t *w, const void *p, uint32_t n)
{
    if (w->err) return;
    if (vfs_write(w->out, p, n) != n) w->err = OPE_WRITE;
    sha1_update(&w->sha, (const unsigned char *)p, n);
}

static void w_flush(writer_t *w)
{
    if (!w->fill) return;
    uint32_t n = (w->fill + 15) & ~15u;
    memset(w->buf + w->fill, 0, n - w->fill);
    apply_keystream(&w->c, w->buf, n, w->blk);
    w->blk += n / 16;
    w_raw(w, w->buf, n);
    w->fill = 0;
}

static void w_data(writer_t *w, const void *p, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)p;
    while (n && !w->err) {
        uint32_t k = CHUNK - w->fill;
        if (k > n) k = n;
        memcpy(w->buf + w->fill, s, k);
        w->fill += k; s += k; n -= k;
        if (w->fill == CHUNK) w_flush(w);
    }
}

static int sfo_get(const uint8_t *s, uint32_t size, const char *want, char *out, int n)
{
    if (size < 0x14 || be32(s) != 0x00505346) return 0;
    uint32_t ko = le32(s + 8), dof = le32(s + 12), cnt = le32(s + 16);
    for (uint32_t i = 0; i < cnt && 0x14 + i * 16 + 16 <= size; i++) {
        const uint8_t *e = s + 0x14 + i * 16;
        uint32_t k = ko + le16(e), len = le32(e + 4), d = dof + le32(e + 12);
        if (k >= size || d + len > size) continue;
        if (strcmp((const char *)s + k, want)) continue;
        int m = (int)len < n - 1 ? (int)len : n - 1;
        memcpy(out, s + d, m); out[m] = 0;
        return 1;
    }
    return 0;
}

static int load_small(const char *path, uint8_t **data, uint32_t *size)
{
    int d = 0; uint64_t sz = 0;
    if (vfs_stat(path, &d, &sz) != 0 || d || sz == 0 || sz > 256 * 1024) return 0;
    vfs_file_t *f = vfs_open_read(path);
    if (!f) return 0;
    *data = (uint8_t *)malloc((size_t)sz);
    int ok = *data && read_full(f, *data, (uint32_t)sz);
    vfs_close(f);
    if (!ok) { free(*data); *data = NULL; return 0; }
    *size = (uint32_t)sz;
    return 1;
}

static int is_content_id(const char *s)
{
    if (strlen(s) != 36) return 0;
    return s[6] == '-' && s[16] == '_' && s[19] == '-';
}

int pkgtool_content_id_for(const char *folder, char *out, int n)
{
    const char *bn = strrchr(folder, '/');
    bn = bn ? bn + 1 : folder;
    if (is_content_id(bn)) { snprintf(out, n, "%s", bn); return 1; }
    char sp[1100];
    path_join(sp, sizeof(sp), folder, "PARAM.SFO");
    uint8_t *d = NULL; uint32_t sz = 0;
    char tid[32] = "";
    if (load_small(sp, &d, &sz)) { sfo_get(d, sz, "TITLE_ID", tid, sizeof(tid)); free(d); }
    if (strlen(tid) != 9) return 0;
    snprintf(out, n, "UP0001-%s_00-0000000000000000", tid);
    return 1;
}

int pkgtool_build(const char *folder, const char *out_pkg, const char *content_id, volatile int *cancel,
                  pt_progress_fn cb, volatile uint64_t *total, char *err_path, int err_len)
{
    int err = OPE_NONE;
    g_items = (item_t *)malloc(sizeof(item_t) * MAX_ITEMS);
    if (!g_items) return OPE_MEM;
    g_count = 0;
    if (!collect(folder, "", 0) || g_count == 0) { free(g_items); set_err(err_path, err_len, folder); return OPE_OPEN_SRC; }

    /* content type from PARAM.SFO category */
    uint32_t content_type = 5, pkg_type = 0x0E;
    {
        char sp[1100], cat[8] = "";
        path_join(sp, sizeof(sp), folder, "PARAM.SFO");
        uint8_t *d = NULL; uint32_t sz = 0;
        if (load_small(sp, &d, &sz)) { sfo_get(d, sz, "CATEGORY", cat, sizeof(cat)); free(d); }
        if (!strcmp(cat, "GD")) { content_type = 4; pkg_type = 0x08; }
    }

    /* layout */
    uint64_t off = (uint64_t)g_count * 32, fsum = 0;
    for (int i = 0; i < g_count; i++) { g_items[i].name_off = (uint32_t)off; off += (strlen(g_items[i].rel) + 15) & ~15ULL; }
    uint64_t desc_len = off;
    for (int i = 0; i < g_count; i++) {
        g_items[i].data_off = off;
        if ((g_items[i].flags & 0xFF) != 4) { off += (g_items[i].size + 15) & ~15ULL; fsum += g_items[i].size; }
    }
    uint64_t data_size = off;
    *total = fsum * 2;

    uint8_t *table = (uint8_t *)calloc(1, (size_t)desc_len);
    uint8_t *buf = (uint8_t *)malloc(CHUNK + 16);
    if (!table || !buf) { free(table); free(buf); free(g_items); return OPE_MEM; }
    for (int i = 0; i < g_count; i++) {
        uint8_t *e = table + i * 32;
        uint32_t nl = (uint32_t)strlen(g_items[i].rel);
        wbe32(e, g_items[i].name_off); wbe32(e + 4, nl);
        wbe64(e + 8, g_items[i].data_off);
        wbe64(e + 16, g_items[i].size);
        wbe32(e + 24, g_items[i].flags);
        memcpy(table + g_items[i].name_off, g_items[i].rel, nl);
    }

    /* header (pre-digest) */
    uint8_t hdr[0x80];
    memset(hdr, 0, sizeof(hdr));
    wbe32(hdr + 0x00, 0x7F504B47);
    wbe32(hdr + 0x04, 0x00000001);
    wbe32(hdr + 0x08, 0xC0);
    wbe32(hdr + 0x0C, 5);
    wbe32(hdr + 0x10, 0x80);
    wbe32(hdr + 0x14, (uint32_t)g_count);
    wbe64(hdr + 0x18, data_size + 0x1A0);
    wbe64(hdr + 0x20, 0x140);
    wbe64(hdr + 0x28, data_size);

    /* pass 1: QA digest over all file data, the header and the item table */
    sha1_context qa;
    sha1_starts(&qa);
    char path[1400];
    for (int i = 0; i < g_count && !err && !*cancel; i++) {
        if ((g_items[i].flags & 0xFF) == 4) continue;
        path_join(path, sizeof(path), folder, g_items[i].rel);
        if (cb) cb(0, g_items[i].rel);
        vfs_file_t *f = vfs_open_read(path);
        if (!f) { set_err(err_path, err_len, path); err = OPE_OPEN_SRC; break; }
        for (;;) {
            int64_t r = vfs_read(f, buf, CHUNK);
            if (r < 0) { err = OPE_READ; break; }
            if (r == 0) break;
            sha1_update(&qa, buf, (size_t)r);
            if (cb) cb((uint64_t)r, NULL);
        }
        vfs_close(f);
    }
    if (err || *cancel) { free(table); free(buf); free(g_items); return err; }
    sha1_update(&qa, hdr, sizeof(hdr));
    sha1_update(&qa, table, (size_t)desc_len);
    uint8_t digest[20];
    sha1_finish(&qa, digest);

    memcpy(hdr + 0x30, content_id, strlen(content_id) < 0x30 ? strlen(content_id) : 0x30);
    memcpy(hdr + 0x60, digest, 16);
    uint8_t klic[16];
    memset(klic, 0, 16);
    debug_crypt(hdr + 0x60, 0xFFFFFFFFFFFFFFFFULL, klic, 16);
    memcpy(hdr + 0x70, klic, 16);

    uint8_t hsha_full[20], header_sha[16];
    sha1(hdr, sizeof(hdr), hsha_full);
    memcpy(header_sha, hsha_full + 3, 16);

    /* metadata block (5 entries) */
    uint8_t meta[0x40];
    memset(meta, 0, sizeof(meta));
    uint32_t mv[] = { 1, 4, 3, 2, 4, content_type, 3, 4, pkg_type, 4, 8 };
    for (int i = 0; i < 11; i++) wbe32(meta + i * 4, mv[i]);
    /* 0x2C: secondary version (u16) + pad (u16) = 0 */
    wbe32(meta + 0x30, (uint32_t)data_size);
    wbe32(meta + 0x34, 5);
    wbe32(meta + 0x38, 4);
    meta[0x3C] = 0x10; meta[0x3D] = 0x61;   /* packagedBy 0x1061, packageVersion 0 */

    uint8_t msha_full[20], meta_sha[16], pad1[0x30], pad2[0x30];
    sha1(meta, sizeof(meta), msha_full);
    memcpy(meta_sha, msha_full + 3, 16);
    memset(pad1, 0, sizeof(pad1));
    debug_crypt(meta_sha, 0, pad1, 0x30);
    memcpy(pad2, pad1, 0x30);
    debug_crypt(header_sha, 0, pad2, 0x30);

    /* pass 2: write */
    writer_t w;
    memset(&w, 0, sizeof(w));
    w.out = vfs_open_write(out_pkg);
    if (!w.out) { free(table); free(buf); free(g_items); set_err(err_path, err_len, out_pkg); return OPE_OPEN_DST; }
    w.buf = buf;
    w.c.retail = 1;
    memcpy(w.c.iv, klic, 16);
    aes_setkey_enc(&w.c.aes, PS3_PKG_KEY, 128);
    sha1_starts(&w.sha);

    uint8_t final_hdr[0x80];
    memcpy(final_hdr, hdr, sizeof(hdr));
    final_hdr[4] = 0x80;                      /* finalized (retail) */
    w_raw(&w, final_hdr, sizeof(final_hdr));
    w_raw(&w, header_sha, 16);
    w_raw(&w, pad2, 0x30);
    w_raw(&w, meta, sizeof(meta));
    w_raw(&w, meta_sha, 16);
    w_raw(&w, pad1, 0x30);

    w_data(&w, table, (uint32_t)desc_len);
    uint8_t *rbuf = (uint8_t *)malloc(CHUNK);
    if (!rbuf) err = OPE_MEM;
    for (int i = 0; i < g_count && !err && !w.err && !*cancel; i++) {
        if ((g_items[i].flags & 0xFF) == 4) continue;
        path_join(path, sizeof(path), folder, g_items[i].rel);
        if (cb) cb(0, g_items[i].rel);
        vfs_file_t *f = vfs_open_read(path);
        if (!f) { set_err(err_path, err_len, path); err = OPE_OPEN_SRC; break; }
        uint64_t got = 0;
        for (;;) {
            int64_t r = vfs_read(f, rbuf, CHUNK);
            if (r < 0) { err = OPE_READ; break; }
            if (r == 0) break;
            w_data(&w, rbuf, (uint32_t)r);
            got += (uint64_t)r;
            if (cb) cb((uint64_t)r, NULL);
        }
        vfs_close(f);
        if (!err && got != g_items[i].size) { set_err(err_path, err_len, path); err = OPE_READ; }
        uint32_t padn = (uint32_t)(((g_items[i].size + 15) & ~15ULL) - g_items[i].size);
        if (padn) { uint8_t z[16] = { 0 }; w_data(&w, z, padn); }
    }
    free(rbuf);
    w_flush(&w);

    if (!err && !w.err && !*cancel) {
        uint8_t z[0x60], fin[20];
        memset(z, 0, sizeof(z));
        w_raw(&w, z, 0x40);
        sha1_finish(&w.sha, fin);
        memcpy(z, fin, 20);
        memset(z + 20, 0, 12);
        if (vfs_write(w.out, z, 0x20) != 0x20) w.err = OPE_WRITE;
    }
    vfs_close(w.out);
    if (w.err && !err) { err = w.err; set_err(err_path, err_len, out_pkg); }
    if (err || *cancel) vfs_unlink(out_pkg);
    free(table); free(buf); free(g_items);
    g_items = NULL;
    return err;
}

/* ======================================================= PARAM.SFO */

int sfo_load(const char *path, sfo_field_t *fields, int max)
{
    uint8_t *s = NULL; uint32_t size = 0;
    if (!load_small(path, &s, &size)) return -1;
    if (size < 0x14 || be32(s) != 0x00505346) { free(s); return -1; }
    uint32_t ko = le32(s + 8), dof = le32(s + 12), cnt = le32(s + 16);
    int n = 0;
    for (uint32_t i = 0; i < cnt && n < max && 0x14 + i * 16 + 16 <= size; i++) {
        const uint8_t *e = s + 0x14 + i * 16;
        uint32_t k = ko + le16(e), fmt = le16(e + 2), len = le32(e + 4), mlen = le32(e + 8), d = dof + le32(e + 12);
        if (k >= size || d + mlen > size) continue;
        sfo_field_t *f = &fields[n++];
        memset(f, 0, sizeof(*f));
        snprintf(f->key, sizeof(f->key), "%s", (const char *)s + k);
        f->max_len = mlen;
        if (fmt == 0x0404) { f->is_string = 0; f->int_value = le32(s + d); snprintf(f->value, sizeof(f->value), "%u", f->int_value); }
        else {
            f->is_string = 1;
            uint32_t m = len < sizeof(f->value) - 1 ? len : sizeof(f->value) - 1;
            memcpy(f->value, s + d, m); f->value[m] = 0;
        }
    }
    free(s);
    return n;
}

int sfo_set_string(const char *path, const char *key, const char *value)
{
    uint8_t *s = NULL; uint32_t size = 0;
    if (!load_small(path, &s, &size)) return -1;
    int rc = -1;
    if (size >= 0x14 && be32(s) == 0x00505346) {
        uint32_t ko = le32(s + 8), dof = le32(s + 12), cnt = le32(s + 16);
        for (uint32_t i = 0; i < cnt && 0x14 + i * 16 + 16 <= size; i++) {
            uint8_t *e = s + 0x14 + i * 16;
            uint32_t k = ko + le16(e), fmt = le16(e + 2), mlen = le32(e + 8), d = dof + le32(e + 12);
            if (k >= size || d + mlen > size || strcmp((const char *)s + k, key)) continue;
            uint32_t vl = (uint32_t)strlen(value);
            if (fmt == 0x0204) { if (vl + 1 > mlen) break; memset(s + d, 0, mlen); memcpy(s + d, value, vl); wle32(e + 4, vl + 1); }
            else if (fmt == 0x0004) { if (vl > mlen) break; memset(s + d, 0, mlen); memcpy(s + d, value, vl); wle32(e + 4, vl); }
            else if (fmt == 0x0404) { wle32(s + d, (uint32_t)strtoul(value, NULL, 10)); }
            else break;
            vfs_file_t *f = vfs_open_write(path);
            if (f) { rc = vfs_write(f, s, size) == size ? 0 : -1; vfs_close(f); }
            break;
        }
    }
    free(s);
    return rc;
}

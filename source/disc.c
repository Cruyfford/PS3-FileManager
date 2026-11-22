/* Disc dumping + ISO9660 image extraction.
 *   PS3 game disc  -> ISO (makeps3iso, files are read decrypted through /dev_bdvd) or JB folder
 *   PS2 DVD        -> ISO (2048-byte sectors via sys_storage_read)
 *   PS1 / PS2 CD   -> BIN + CUE (raw 2352-byte sectors via ATAPI READ CD, all tracks)
 *   .iso/.bin/.img/.cue/.iso.0 -> folder (2048 / 2352 mode1 / 2352 mode2 / 2336 layouts, Joliet, XA form2 raw) */
#include "disc.h"
#include "fileops.h"
#include "vfs.h"
#include "pkgtool.h"
#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <ctype.h>

#ifndef PART_SZ
#define PART_SZ 0xFFFF0000ULL
#endif
#define CHUNK 32

extern int makeps3iso_main(int argc, const char *argv[]);
extern volatile int *mkiso_cancel;
extern void (*mkiso_progress)(unsigned long long add);

static volatile int *g_cancel;
static disc_cb g_cb;
static char *g_ep; static int g_en;
static void seterr(const char *p) { if (g_ep) snprintf(g_ep, g_en, "%s", p); }
#define CANCELLED (g_cancel && *g_cancel)

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void join(char *out, int n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    snprintf(out, n, l && dir[l - 1] == '/' ? "%s%s" : "%s/%s", dir, name);
}

static void clean_name(char *s)
{
    for (char *p = s; *p; p++) if (strchr("/\\:*?\"<>|", *p) || (unsigned char)*p < 32) *p = ' ';
    int l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '.')) s[--l] = 0;
    char *b = s; while (*b == ' ') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

/* ================================================== split writer (FAT32) */
typedef struct { char path[1024]; int limited, iso, idx; uint64_t in_part; vfs_file_t *f; } sw_t;

static void sw_part(const sw_t *w, int i, char *o, int n)
{
    if (w->iso) snprintf(o, n, "%s.%d", w->path, i); else snprintf(o, n, "%s.666%02d", w->path, i);
}

static int sw_open(sw_t *w, const char *path, int limited)
{
    memset(w, 0, sizeof(*w));
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->limited = limited;
    size_t l = strlen(path);
    w->iso = l > 4 && !strcasecmp(path + l - 4, ".iso");
    w->f = vfs_open_write(path);
    if (!w->f) { seterr(path); return OPE_OPEN_DST; }
    return 0;
}

static int sw_write(sw_t *w, const uint8_t *b, uint32_t n)
{
    while (n) {
        uint32_t can = n;
        if (w->limited && w->in_part + can > PART_SZ) can = (uint32_t)(PART_SZ - w->in_part);
        if (can) {
            if (vfs_write(w->f, b, can) != can) { seterr(w->path); return OPE_WRITE; }
            w->in_part += can; b += can; n -= can;
        }
        if (n) {   /* start next part */
            char pn[1100];
            vfs_close(w->f); w->f = NULL;
            if (w->idx == 0) { sw_part(w, 0, pn, sizeof(pn)); vfs_unlink(pn); if (vfs_rename(w->path, pn)) { seterr(pn); return OPE_WRITE; } }
            w->idx++;
            sw_part(w, w->idx, pn, sizeof(pn));
            w->f = vfs_open_write(pn);
            if (!w->f) { seterr(pn); return OPE_OPEN_DST; }
            w->in_part = 0;
        }
    }
    return 0;
}

static void sw_close(sw_t *w, int remove_all)
{
    if (w->f) vfs_close(w->f);
    w->f = NULL;
    if (remove_all) {
        char pn[1100];
        vfs_unlink(w->path);
        for (int i = 0; i < 100; i++) { sw_part(w, i, pn, sizeof(pn)); if (!vfs_exists(pn)) break; vfs_unlink(pn); }
    }
}

/* ================================================== sector sources */
typedef struct src src_t;
struct src {
    int (*raw)(src_t *s, uint32_t lba, uint32_t n, uint8_t *out);   /* n sectors of ssize bytes */
    uint32_t ssize, uoff;
    uint64_t total;                  /* bytes */
    vfs_file_t *pf[100]; uint64_t ps[100]; int np;
    uint8_t *tmp;
};

/* ---- image files (possibly split into parts) */
static int img_raw(src_t *s, uint32_t lba, uint32_t n, uint8_t *out)
{
    uint64_t off = (uint64_t)lba * s->ssize, len = (uint64_t)n * s->ssize;
    while (len) {
        uint64_t base = 0; int i = 0;
        while (i < s->np && off >= base + s->ps[i]) { base += s->ps[i]; i++; }
        if (i >= s->np) { memset(out, 0, len); return 0; }   /* past end: zeros */
        uint64_t in = base + s->ps[i] - off, take = len < in ? len : in;
        if (vfs_seek(s->pf[i], off - base)) return -1;
        if (vfs_read(s->pf[i], out, (uint32_t)take) != (int64_t)take) return -1;
        out += take; off += take; len -= take;
    }
    return 0;
}

static void src_close(src_t *s)
{
    for (int i = 0; i < s->np; i++) if (s->pf[i]) vfs_close(s->pf[i]);
    s->np = 0;
    free(s->tmp); s->tmp = NULL;
}

/* read n user-data sectors (2048 bytes each) */
static int rd_user(src_t *s, uint32_t lba, uint32_t n, uint8_t *out)
{
    if (s->ssize == 2048) return s->raw(s, lba, n, out);
    while (n) {
        uint32_t k = n > CHUNK ? CHUNK : n;
        if (s->raw(s, lba, k, s->tmp)) return -1;
        for (uint32_t i = 0; i < k; i++) memcpy(out + i * 2048, s->tmp + i * s->ssize + s->uoff, 2048);
        out += k * 2048; lba += k; n -= k;
    }
    return 0;
}

static int is_pvd(const uint8_t *u) { return !memcmp(u + 1, "CD001", 5); }

static int detect_layout(src_t *s)
{
    static const uint32_t lay[][2] = { { 2048, 0 }, { 2352, 24 }, { 2352, 16 }, { 2336, 8 } };
    uint8_t *u = memalign(128, 2048);
    if (!u) return -1;
    for (int i = 0; i < 4; i++) {
        s->ssize = lay[i][0]; s->uoff = lay[i][1];
        if (s->total < (uint64_t)17 * s->ssize) continue;
        if (rd_user(s, 16, 1, u) == 0 && is_pvd(u)) { free(u); return 0; }
    }
    free(u);
    return -1;
}

static int img_open(src_t *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    s->raw = img_raw;
    s->tmp = memalign(128, CHUNK * 2352);
    if (!s->tmp) return OPE_MEM;
    char p[1024]; snprintf(p, sizeof(p), "%s", path);
    size_t l = strlen(p);
    /* .cue -> referenced bin */
    if (l > 4 && !strcasecmp(p + l - 4, ".cue")) {
        vfs_file_t *f = vfs_open_read(p);
        if (!f) { seterr(p); return OPE_OPEN_SRC; }
        char txt[4096]; int64_t r = vfs_read(f, txt, sizeof(txt) - 1); vfs_close(f);
        txt[r > 0 ? r : 0] = 0;
        char *q = strstr(txt, "FILE"), *a, *b;
        if (!q || !(a = strchr(q, '"')) || !(b = strchr(a + 1, '"'))) { seterr(p); return OPE_READ; }
        *b = 0;
        char dir[1024]; snprintf(dir, sizeof(dir), "%s", path);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; join(p, sizeof(p), dir, a + 1); } else snprintf(p, sizeof(p), "%s", a + 1);
        l = strlen(p);
    }
    /* split image: name.iso.0 / name.0 */
    if (l > 2 && p[l - 2] == '.' && p[l - 1] == '0') {
        p[l - 2] = 0;
        char pn[1100];
        for (int i = 0; i < 100; i++) {
            snprintf(pn, sizeof(pn), "%s.%d", p, i);
            if (!vfs_exists(pn)) break;
            s->pf[s->np] = vfs_open_read(pn);
            if (!s->pf[s->np]) { seterr(pn); src_close(s); return OPE_OPEN_SRC; }
            s->ps[s->np] = vfs_size(s->pf[s->np]); s->total += s->ps[s->np]; s->np++;
        }
    } else {
        s->pf[0] = vfs_open_read(p);
        if (!s->pf[0]) { seterr(p); src_close(s); return OPE_OPEN_SRC; }
        s->ps[0] = vfs_size(s->pf[0]); s->total = s->ps[0]; s->np = 1;
    }
    if (!s->np || detect_layout(s)) { seterr(p); src_close(s); return OPE_READ; }
    return 0;
}

/* ---- the Blu-ray drive */
static int bd_fd = -1;
static int atapi_proto = ATAPI_DMA_PROTO;

static int bd_open(void)
{
    if (bd_fd >= 0) return 0;
    int fd;
    if (sys_storage_open(BD_DEVICE, &fd)) return -1;
    bd_fd = fd;
    return 0;
}
static void bd_close(void) { if (bd_fd >= 0) sys_storage_close(bd_fd); bd_fd = -1; }

static int atapi_in(const uint8_t *pkt, uint8_t *buf, uint32_t len)
{
    struct lv2_atapi_cmnd_block c;
    for (int tries = 0; tries < 2; tries++) {
        sys_storage_init_atapi_cmnd(&c, len, atapi_proto, ATAPI_DIR_READ);
        memcpy(c.pkt, pkt, 12);
        if (sys_storage_send_atapi_command(bd_fd, &c, buf) == 0) return 0;
        atapi_proto = atapi_proto == ATAPI_DMA_PROTO ? ATAPI_PIO_DATA_IN_PROTO : ATAPI_DMA_PROTO;
    }
    return -1;
}

static int bd_read2048(src_t *s, uint32_t lba, uint32_t n, uint8_t *out)
{
    (void)s;
    uint32_t r = 0;
    for (int t = 0; t < 3; t++) if (sys_storage_read(bd_fd, lba, n, out, &r) == 0 && r == n) return 0;
    return -1;
}

static int cd_audio_flag;   /* current track is CD-DA */
static int bd_readcd(src_t *s, uint32_t lba, uint32_t n, uint8_t *out)
{
    (void)s;
    uint8_t pkt[12] = { 0xBE, 0, lba >> 24, lba >> 16, lba >> 8, lba, n >> 16, n >> 8, n, cd_audio_flag ? 0x10 : 0xF8, 0, 0 };
    for (int t = 0; t < 3; t++) if (atapi_in(pkt, out, n * 2352) == 0) return 0;
    return -1;
}

static int bd_profile(void)
{
    uint8_t pkt[12] = { 0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0 };
    uint8_t *b = memalign(128, 128);
    if (!b) return -1;
    memset(b, 0, 128);
    int p = atapi_in(pkt, b, 8) == 0 ? (b[6] << 8 | b[7]) : -1;
    free(b);
    return p;
}

typedef struct { int no, data; uint32_t lba; } track_t;
static track_t trk[100];
static int ntrk;
static uint32_t leadout;

static int bd_toc(void)
{
    uint8_t *b = memalign(128, 1024);
    if (!b) return -1;
    memset(b, 0, 1024);
    uint8_t pkt[12] = { 0x43, 0, 0, 0, 0, 0, 1, 0, 12, 0, 0, 0 };
    ntrk = 0; leadout = 0;
    if (atapi_in(pkt, b, 12)) { free(b); return -1; }
    int len = (b[0] << 8 | b[1]) + 2;
    if (len < 12 || len > 804) { free(b); return -1; }
    pkt[7] = len >> 8; pkt[8] = len & 255;
    if (len > 12 && atapi_in(pkt, b, len)) { free(b); return -1; }
    for (int off = 4; off + 8 <= len && ntrk < 99; off += 8) {
        int no = b[off + 2];
        uint32_t lba = be32(b + off + 4);
        if (no == 0xAA) leadout = lba;
        else { trk[ntrk].no = no; trk[ntrk].data = (b[off + 1] & 0x04) != 0; trk[ntrk].lba = lba; ntrk++; }
    }
    free(b);
    return ntrk && leadout ? 0 : -1;
}

/* ================================================== ISO9660 */
typedef struct { char name[256]; uint32_t lba, size; int dir, more, form2; } ient_t;

static int iso_root(src_t *s, uint32_t *rlba, uint32_t *rsize, int *joliet)
{
    uint8_t *u = memalign(128, 2048);
    if (!u) return -1;
    int found = 0;
    *joliet = 0;
    for (uint32_t l = 16; l < 64; l++) {
        if (rd_user(s, l, 1, u) || !is_pvd(u)) break;
        if (u[0] == 255) break;
        if (u[0] == 1 && !found) { *rlba = le32(u + 156 + 2); *rsize = le32(u + 156 + 10); found = 1; }
        if (u[0] == 2 && u[88] == '%' && u[89] == '/' && (u[90] == '@' || u[90] == 'C' || u[90] == 'E')) {
            *rlba = le32(u + 156 + 2); *rsize = le32(u + 156 + 10); *joliet = 1; found = 1;
        }
    }
    free(u);
    return found ? 0 : -1;
}

static void ucs2_to_utf8(const uint8_t *in, int n, char *out, int on)
{
    int o = 0;
    for (int i = 0; i + 1 < n && o < on - 4; i += 2) {
        unsigned c = in[i] << 8 | in[i + 1];
        if (c < 0x80) out[o++] = c;
        else if (c < 0x800) { out[o++] = 0xC0 | c >> 6; out[o++] = 0x80 | (c & 63); }
        else { out[o++] = 0xE0 | c >> 12; out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
    }
    out[o] = 0;
}

/* list a directory; returns count (malloc'd array in *out) or -1 */
static int iso_list(src_t *s, uint32_t lba, uint32_t size, int joliet, ient_t **out)
{
    uint32_t ns = (size + 2047) / 2048;
    if (ns > 4096) ns = 4096;
    uint8_t *d = memalign(128, ns * 2048);
    if (!d) return -1;
    if (rd_user(s, lba, ns, d)) { free(d); return -1; }
    int cap = 64, n = 0;
    ient_t *e = malloc(sizeof(ient_t) * cap);
    if (!e) { free(d); return -1; }
    for (uint32_t sec = 0; sec < ns; sec++) {
        uint8_t *b = d + sec * 2048;
        uint32_t p = 0;
        while (p + 34 <= 2048) {
            int len = b[p];
            if (!len || p + len > 2048) break;
            uint8_t *r = b + p;
            int nl = r[32];
            if (!(nl == 1 && (r[33] == 0 || r[33] == 1)) && 33 + nl <= len) {
                if (n == cap) { ient_t *t = realloc(e, sizeof(ient_t) * cap * 2); if (!t) break; e = t; cap *= 2; }
                ient_t *x = &e[n];
                memset(x, 0, sizeof(*x));
                if (joliet) ucs2_to_utf8(r + 33, nl, x->name, sizeof(x->name));
                else { memcpy(x->name, r + 33, nl); x->name[nl] = 0; }
                char *sc = strchr(x->name, ';'); if (sc) *sc = 0;
                int l = strlen(x->name); if (l > 1 && x->name[l - 1] == '.') x->name[l - 1] = 0;
                x->lba = le32(r + 2); x->size = le32(r + 10);
                x->dir = (r[25] & 2) != 0; x->more = (r[25] & 0x80) != 0;
                int su = 33 + nl + ((nl & 1) ? 0 : 1);
                if (su + 14 <= len && r[su + 6] == 'X' && r[su + 7] == 'A') {
                    unsigned attr = r[su + 4] << 8 | r[su + 5];
                    if (attr & 0x3000) x->form2 = 1;
                }
                if (x->name[0]) n++;
            }
            p += len;
        }
    }
    free(d);
    *out = e;
    return n;
}

static uint8_t *xbuf;

static int extract_file(src_t *s, const ient_t *e, sw_t *w, int raw)
{
    uint32_t ns = (e->size + 2047) / 2048, lba = e->lba;
    uint64_t left = e->size;
    while (ns && !CANCELLED) {
        uint32_t k = ns > CHUNK ? CHUNK : ns;
        uint32_t bytes;
        if (raw) {
            if (s->raw(s, lba, k, xbuf)) { seterr(e->name); return OPE_READ; }
            bytes = k * s->ssize;
        } else {
            if (rd_user(s, lba, k, xbuf)) { seterr(e->name); return OPE_READ; }
            bytes = k * 2048; if (bytes > left) bytes = (uint32_t)left;
        }
        int er = sw_write(w, xbuf, bytes);
        if (er) return er;
        uint64_t used = (uint64_t)k * 2048; if (used > left) used = left;
        left -= used; ns -= k; lba += k;
        if (g_cb) g_cb(used, NULL);
    }
    return 0;
}

static int extract_dir(src_t *s, uint32_t lba, uint32_t size, int joliet, const char *dst, int depth)
{
    if (depth > 24) return 0;
    vfs_mkdir(dst);
    ient_t *e = NULL;
    int n = iso_list(s, lba, size, joliet, &e);
    if (n < 0) { seterr(dst); return OPE_READ; }
    int err = 0, limited = vfs_4gb_limit(dst);
    char path[1024];
    sw_t w; int open = 0;
    for (int i = 0; i < n && !err && !CANCELLED; i++) {
        join(path, sizeof(path), dst, e[i].name);
        if (e[i].dir) { err = extract_dir(s, e[i].lba, e[i].size, joliet, path, depth + 1); continue; }
        if (!open) {
            if (g_cb) g_cb(0, e[i].name);
            err = sw_open(&w, path, limited);
            if (err) break;
            open = 1;
        }
        err = extract_file(s, &e[i], &w, e[i].form2 && s->ssize == 2352);
        if (!e[i].more || err) { sw_close(&w, err || CANCELLED); open = 0; }
    }
    if (open) sw_close(&w, 1);
    free(e);
    return err;
}

int iso_is_image(const char *n)
{
    size_t l = strlen(n);
    if (l > 4 && (!strcasecmp(n + l - 4, ".iso") || !strcasecmp(n + l - 4, ".bin") || !strcasecmp(n + l - 4, ".img") ||
                  !strcasecmp(n + l - 4, ".cue") || !strcasecmp(n + l - 4, ".mdf"))) return 1;
    if (l > 6 && !strcasecmp(n + l - 6, ".iso.0")) return 1;
    return 0;
}

int iso_extract(const char *img, const char *dst_dir, volatile int *cancel, disc_cb cb, volatile uint64_t *total, char *ep, int en)
{
    g_cancel = cancel; g_cb = cb; g_ep = ep; g_en = en;
    src_t s;
    int err = img_open(&s, img);
    if (err) return err;
    xbuf = memalign(128, CHUNK * 2352);
    if (!xbuf) { src_close(&s); return OPE_MEM; }
    *total = s.total / s.ssize * 2048;
    uint32_t rl, rs; int jol;
    if (iso_root(&s, &rl, &rs, &jol)) { seterr(img); err = OPE_READ; }
    else err = extract_dir(&s, rl, rs, jol, dst_dir, 0);
    free(xbuf); xbuf = NULL;
    src_close(&s);
    return err;
}

/* ================================================== probe */
static src_t dsrc;

static int find_root_file(src_t *s, const char *name, ient_t *out)
{
    uint32_t rl, rs; int jol;
    if (iso_root(s, &rl, &rs, &jol)) return -1;
    ient_t *e; int n = iso_list(s, rl, rs, jol, &e);
    if (n < 0) return -1;
    int r = -1;
    for (int i = 0; i < n; i++) if (!strcasecmp(e[i].name, name)) { *out = e[i]; r = 0; break; }
    free(e);
    return r;
}

static void boot_id(const char *cnf, const char *key, char *id, int n)
{
    const char *p = strstr(cnf, key);
    if (!p) return;
    const char *eol = strpbrk(p, "\r\n"); if (!eol) eol = p + strlen(p);
    const char *a = p;
    for (const char *q = p; q < eol; q++) if (*q == '\\' || *q == ':' || *q == '/') a = q + 1;
    int o = 0;
    while (a < eol && *a != ';' && *a != ' ' && o < n - 1) id[o++] = *a++;
    id[o] = 0;
}

/* SLUS_200.62 -> SLUS-20062 */
static void norm_id(const char *in, char *out, int n)
{
    int o = 0;
    for (const char *p = in; *p && o < n - 1; p++) {
        if (*p == '.') continue;
        out[o++] = *p == '_' ? '-' : toupper((unsigned char)*p);
    }
    out[o] = 0;
}

static int setup_disc_src(int is_cd)
{
    memset(&dsrc, 0, sizeof(dsrc));
    dsrc.tmp = memalign(128, CHUNK * 2352);
    if (!dsrc.tmp) return -1;
    dsrc.raw = bd_read2048; dsrc.ssize = 2048; dsrc.total = ~0ULL;
    uint8_t *u = memalign(128, 2048);
    if (!u) return -1;
    int ok = rd_user(&dsrc, 16, 1, u) == 0 && is_pvd(u);
    if (!ok && is_cd) {
        cd_audio_flag = 0;
        dsrc.raw = bd_readcd; dsrc.ssize = 2352;
        uint8_t *r = memalign(128, 2352);
        if (r && bd_readcd(&dsrc, 16, 1, r) == 0) {
            dsrc.uoff = r[15] == 1 ? 16 : 24;
            memcpy(u, r + dsrc.uoff, 2048);
            ok = is_pvd(u);
        }
        free(r);
    }
    free(u);
    return ok ? 0 : -1;
}

const char *disc_type_name(int t)
{
    switch (t) { case DISC_PS3: return "PS3"; case DISC_PS2: return "PS2"; case DISC_PS1: return "PS1"; case DISC_DATA: return "DATA"; }
    return "";
}

#define DLIST 4096
/* names of sub-folders, so the big listing can be freed before recursing */
static char *subdir_names(vfs_entry_t *e, int n, int depth, int *count)
{
    size_t tot = 1; int k = 0;
    for (int i = 0; i < n; i++) if (e[i].is_dir) tot += strlen(e[i].name) + 1;
    char *b = malloc(tot), *p = b;
    *count = 0;
    if (!b) return NULL;
    for (int i = 0; i < n; i++) if (e[i].is_dir && !(depth == 0 && !strcmp(e[i].name, "PS3_UPDATE"))) {
        size_t l = strlen(e[i].name) + 1; memcpy(p, e[i].name, l); p += l; k++;
    }
    *count = k;
    return b;
}

static uint64_t tree_size(const char *path, int depth)
{
    vfs_entry_t *e = malloc(sizeof(vfs_entry_t) * DLIST);
    if (!e) return 0;
    int n = vfs_list(path, e, DLIST);
    if (n < 0) n = 0;
    uint64_t t = 0;
    for (int i = 0; i < n; i++) if (!e[i].is_dir) t += e[i].size;
    int nd; char *sd = subdir_names(e, n, depth, &nd), *p = sd;
    free(e);
    char c[1024];
    for (int i = 0; i < nd && depth < 24; i++) { join(c, sizeof(c), path, p); t += tree_size(c, depth + 1); p += strlen(p) + 1; }
    free(sd);
    return t;
}

static int probe(disc_info_t *d)
{
    memset(d, 0, sizeof(*d));
    if (vfs_exists("/dev_bdvd/PS3_GAME/PARAM.SFO")) {
        d->type = DISC_PS3;
        sfo_field_t *f = malloc(sizeof(sfo_field_t) * SFO_MAX_FIELDS);
        if (f) {
            int nf = sfo_load("/dev_bdvd/PS3_GAME/PARAM.SFO", f, SFO_MAX_FIELDS);
            for (int i = 0; i < nf; i++) {
                if (!strcmp(f[i].key, "TITLE_ID")) snprintf(d->id, sizeof(d->id), "%s", f[i].value);
                if (!strcmp(f[i].key, "TITLE")) snprintf(d->title, sizeof(d->title), "%s", f[i].value);
            }
            free(f);
        }
        for (char *p = d->title; *p; p++) if (*p == '\n') *p = ' ';
        d->bytes = tree_size("/dev_bdvd", 0);
        d->sectors = d->bytes / 2048;
        return 0;
    }
    if (bd_open()) return -1;
    device_info_t info; memset(&info, 0, sizeof(info));
    int have_info = sys_storage_get_device_info(BD_DEVICE, &info) == 0 && info.total_sectors;
    int prof = bd_profile();
    int toc = bd_toc() == 0;
    if (prof > 0) d->is_cd = prof >= 0x08 && prof <= 0x0A;
    else d->is_cd = toc && (!have_info || info.total_sectors < 400000);
    if (d->is_cd && toc) { d->ntracks = ntrk; d->sectors = leadout; }
    else if (have_info) d->sectors = info.total_sectors;
    if (!d->sectors) { bd_close(); return -1; }
    d->bytes = d->sectors * (d->is_cd ? 2352ULL : 2048ULL);
    d->type = DISC_DATA;
    if (setup_disc_src(d->is_cd) == 0) {
        ient_t e;
        if (find_root_file(&dsrc, "SYSTEM.CNF", &e) == 0) {
            char cnf[2049]; memset(cnf, 0, sizeof(cnf));
            if (rd_user(&dsrc, e.lba, 1, (uint8_t *)cnf) == 0) {
                char raw[64] = "";
                if (strstr(cnf, "BOOT2")) { d->type = DISC_PS2; boot_id(cnf, "BOOT2", raw, sizeof(raw)); }
                else if (strstr(cnf, "BOOT")) { d->type = DISC_PS1; boot_id(cnf, "BOOT", raw, sizeof(raw)); }
                norm_id(raw, d->id, sizeof(d->id));
            }
        } else if (find_root_file(&dsrc, "PSX.EXE", &e) == 0) d->type = DISC_PS1;
    }
    free(dsrc.tmp); dsrc.tmp = NULL;
    snprintf(d->title, sizeof(d->title), "%s", d->id[0] ? d->id : "DISC");
    return 0;
}

int disc_probe(disc_info_t *d) { int r = probe(d); bd_close(); return r; }

/* ================================================== dumping */
static void disc_mkiso_tick(unsigned long long add) { if (g_cb) g_cb(add, NULL); }

static int dump_ps3_iso(const disc_info_t *d, const char *dst, char *out, int on, volatile uint64_t *total)
{
    if (vfs_backend(dst) != VFS_NATIVE) { seterr(dst); return OPE_OPEN_DST; }
    char nm[200];
    snprintf(nm, sizeof(nm), "%s [%s]", d->title[0] ? d->title : "GAME", d->id);
    clean_name(nm);
    char path[1024], file[1024];
    snprintf(file, sizeof(file), "%s.iso", nm);
    join(path, sizeof(path), dst, file);
    snprintf(out, on, "%s", path);
    *total = d->bytes;
    if (g_cb) g_cb(0, file);
    mkiso_cancel = g_cancel;
    const char *argv[6]; int ac = 0;
    argv[ac++] = "makeps3iso"; argv[ac++] = "-p0";
    if (vfs_4gb_limit(dst)) argv[ac++] = "-s";
    argv[ac++] = "/dev_bdvd"; argv[ac++] = path; argv[ac] = NULL;
    mkiso_progress = disc_mkiso_tick;
    int r = makeps3iso_main(ac, argv);
    mkiso_progress = NULL; mkiso_cancel = NULL;
    if (r != 0 || CANCELLED) {
        sw_t w; memset(&w, 0, sizeof(w)); snprintf(w.path, sizeof(w.path), "%s", path); w.iso = 1;
        sw_close(&w, 1);
        if (!CANCELLED) { seterr(path); return OPE_WRITE; }
    }
    return 0;
}


static uint8_t *cbuf;

static int copy_tree_disc(const char *src, const char *dst, int depth)
{
    vfs_mkdir(dst);
    vfs_entry_t *e = malloc(sizeof(vfs_entry_t) * DLIST);
    if (!e) return OPE_MEM;
    int n = vfs_list(src, e, DLIST), err = 0, limited = vfs_4gb_limit(dst);
    if (n < 0) { free(e); seterr(src); return OPE_READ; }
    char s[1024], t[1024];
    for (int i = 0; i < n && !err && !CANCELLED; i++) {
        if (e[i].is_dir) continue;
        join(s, sizeof(s), src, e[i].name); join(t, sizeof(t), dst, e[i].name);
        if (g_cb) g_cb(0, e[i].name);
        vfs_file_t *in = vfs_open_read(s);
        if (!in) { seterr(s); err = OPE_OPEN_SRC; break; }
        sw_t w;
        err = sw_open(&w, t, limited && e[i].size >= 0xFFFFFFFFULL);
        if (err) { vfs_close(in); break; }
        while (!err && !CANCELLED) {
            int64_t r = vfs_read(in, cbuf, 1024 * 1024);
            if (r < 0) { seterr(s); err = OPE_READ; break; }
            if (!r) break;
            err = sw_write(&w, cbuf, (uint32_t)r);
            if (g_cb) g_cb((uint64_t)r, NULL);
        }
        vfs_close(in);
        sw_close(&w, err || CANCELLED);
    }
    int nd; char *sd = subdir_names(e, n, depth, &nd), *p = sd;
    free(e);
    for (int i = 0; i < nd && !err && !CANCELLED && depth < 24; i++) {
        join(s, sizeof(s), src, p); join(t, sizeof(t), dst, p);
        err = copy_tree_disc(s, t, depth + 1);
        p += strlen(p) + 1;
    }
    free(sd);
    return err;
}

static int dump_sectors(src_t *s, sw_t *w, uint32_t from, uint32_t to, int sz)
{
    uint32_t lba = from;
    while (lba < to && !CANCELLED) {
        uint32_t k = to - lba > CHUNK ? CHUNK : to - lba;
        if (s->raw(s, lba, k, cbuf)) {
            /* retry sector by sector */
            for (uint32_t i = 0; i < k; i++) {
                if (s->raw(s, lba + i, 1, cbuf + i * sz)) { char m[64]; snprintf(m, sizeof(m), "LBA %u", lba + i); seterr(m); return OPE_READ; }
            }
        }
        int err = sw_write(w, cbuf, k * sz);
        if (err) return err;
        if (g_cb) g_cb((uint64_t)k * sz, NULL);
        lba += k;
    }
    return 0;
}

static void lba_msf(uint32_t lba, char *o, int n) { snprintf(o, n, "%02u:%02u:%02u", lba / 4500, (lba / 75) % 60, lba % 75); }

static int dump_cd(const disc_info_t *d, const char *dst, char *out, int on, volatile uint64_t *total)
{
    char base[200], path[1024], fn[256];
    snprintf(base, sizeof(base), "%s", d->id[0] ? d->id : "DISC");
    clean_name(base);
    snprintf(fn, sizeof(fn), "%s.bin", base);
    join(path, sizeof(path), dst, fn);
    snprintf(out, on, "%s", path);
    if (bd_toc()) { ntrk = 1; trk[0].no = 1; trk[0].data = 1; trk[0].lba = 0; leadout = (uint32_t)d->sectors; }
    *total = (uint64_t)leadout * 2352;
    memset(&dsrc, 0, sizeof(dsrc)); dsrc.raw = bd_readcd; dsrc.ssize = 2352;
    if (g_cb) g_cb(0, fn);
    sw_t w;
    int err = sw_open(&w, path, 0);
    if (err) return err;
    for (int t = 0; t < ntrk && !err && !CANCELLED; t++) {
        uint32_t end = t + 1 < ntrk ? trk[t + 1].lba : leadout;
        cd_audio_flag = !trk[t].data;
        err = dump_sectors(&dsrc, &w, trk[t].lba, end, 2352);
    }
    cd_audio_flag = 0;
    sw_close(&w, err || CANCELLED);
    if (err || CANCELLED) return err;
    /* data track mode from the first sector header */
    int mode = 2;
    uint8_t *r = memalign(128, 2352);
    if (r && trk[0].data && bd_readcd(&dsrc, trk[0].lba + 16, 1, r) == 0 && r[15] == 1) mode = 1;
    free(r);
    char cue[8192]; int o = 0;
    o += snprintf(cue + o, sizeof(cue) - o, "FILE \"%s\" BINARY\r\n", fn);
    for (int t = 0; t < ntrk && o < (int)sizeof(cue) - 128; t++) {
        char msf[16]; lba_msf(trk[t].lba, msf, sizeof(msf));
        o += snprintf(cue + o, sizeof(cue) - o, "  TRACK %02d %s\r\n", t + 1, trk[t].data ? (mode == 1 ? "MODE1/2352" : "MODE2/2352") : "AUDIO");
        o += snprintf(cue + o, sizeof(cue) - o, "    INDEX 01 %s\r\n", msf);
    }
    char cpath[1024]; snprintf(fn, sizeof(fn), "%s.cue", base); join(cpath, sizeof(cpath), dst, fn);
    vfs_file_t *f = vfs_open_write(cpath);
    if (!f) { seterr(cpath); return OPE_OPEN_DST; }
    vfs_write(f, cue, o);
    vfs_close(f);
    return 0;
}

static int dump_dvd(const disc_info_t *d, const char *dst, char *out, int on, volatile uint64_t *total)
{
    char base[200], path[1024], fn[256];
    snprintf(base, sizeof(base), "%s", d->id[0] ? d->id : "DISC");
    clean_name(base);
    snprintf(fn, sizeof(fn), "%s.iso", base);
    join(path, sizeof(path), dst, fn);
    snprintf(out, on, "%s", path);
    *total = d->sectors * 2048;
    memset(&dsrc, 0, sizeof(dsrc)); dsrc.raw = bd_read2048; dsrc.ssize = 2048;
    if (g_cb) g_cb(0, fn);
    sw_t w;
    int err = sw_open(&w, path, vfs_4gb_limit(dst));
    if (err) return err;
    err = dump_sectors(&dsrc, &w, 0, (uint32_t)d->sectors, 2048);
    sw_close(&w, err || CANCELLED);
    return err;
}

int disc_dump(int mode, const char *dst, volatile int *cancel, disc_cb cb, volatile uint64_t *total, char *ep, int en, char *out, int on)
{
    g_cancel = cancel; g_cb = cb; g_ep = ep; g_en = en;
    disc_info_t d;
    if (probe(&d)) { bd_close(); seterr("/dev_bdvd"); return OPE_OPEN_SRC; }
    cbuf = memalign(128, 1024 * 1024 + CHUNK * 2352);
    if (!cbuf) return OPE_MEM;
    vfs_mkdir(dst);
    int err;
    if (d.type == DISC_PS3) {
        if (mode == 1) {
            char nm[200], path[1024];
            snprintf(nm, sizeof(nm), "%s [%s]", d.title[0] ? d.title : "GAME", d.id);
            clean_name(nm);
            join(path, sizeof(path), dst, nm);
            snprintf(out, on, "%s", path);
            *total = d.bytes;
            err = copy_tree_disc("/dev_bdvd", path, 0);
        } else err = dump_ps3_iso(&d, dst, out, on, total);
    } else {
        if (bd_open()) { free(cbuf); cbuf = NULL; seterr("BD"); return OPE_OPEN_SRC; }
        err = d.is_cd ? dump_cd(&d, dst, out, on, total) : dump_dvd(&d, dst, out, on, total);
    }
    bd_close();
    free(cbuf); cbuf = NULL;
    return err;
}

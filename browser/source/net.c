/* Nova browser: HTTP(S) via libcurl. Page fetches, an image worker and a download manager.
 * Cookies and DNS are shared across all handles. */
#include "net.h"
#include "vfs.h"
#include "gfx.h"
#include <curl/curl.h>
#include <net/net.h>
#include <net/netctl.h>
#include <sysmodule/sysmodule.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

#define UA "Mozilla/5.0 (PlayStation 3 4.93) AppleWebKit/531.22.8 (KHTML, like Gecko) Nova/1.0"
#define PAGE_MAX (12 * 1024 * 1024)
#define IMG_MAX  (4 * 1024 * 1024)
#define COOKIE_FILE "/dev_hdd0/game/CRBR00001/USRDIR/cookies.txt"

static CURLSH *share;
static sys_mutex_t share_mtx[4];
static int net_ok;

static uint64_t now_us(void) { u64 s, ns; sysGetCurrentTime(&s, &ns); return s * 1000000ULL + ns / 1000; }

static void sh_lock(CURL *h, curl_lock_data d, curl_lock_access a, void *u) { (void)h; (void)a; (void)u; sysMutexLock(share_mtx[d & 3], 0); }
static void sh_unlock(CURL *h, curl_lock_data d, void *u) { (void)h; (void)u; sysMutexUnlock(share_mtx[d & 3]); }

int net_init(void)
{
    if (net_ok) return 1;
    sysModuleLoad(SYSMODULE_NET);
    sysModuleLoad(SYSMODULE_NETCTL);
    if (netInitialize() < 0) return 0;
    netCtlInit();
    curl_global_init(CURL_GLOBAL_ALL);
    sys_mutex_attr_t at;
    memset(&at, 0, sizeof(at));
    at.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    at.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    at.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
    at.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
    for (int i = 0; i < 4; i++) sysMutexCreate(&share_mtx[i], &at);
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_LOCKFUNC, sh_lock);
    curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, sh_unlock);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    net_ok = 1;
    return 1;
}

void net_shutdown(void)
{
    img_cancel_all();
    for (int i = 0; i < DL_MAX; i++) dl_cancel(i);
}

static CURL *mk_handle(const char *url)
{
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_SHARE, share);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, COOKIE_FILE);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    return c;
}

/* ======================= page fetch ======================= */

static int is_page_type(const char *ct)
{
    if (!ct[0]) return 1;
    return !strncasecmp(ct, "text/html", 9) || !strncasecmp(ct, "application/xhtml", 17) || !strncasecmp(ct, "text/plain", 10) ||
           !strncasecmp(ct, "text/xml", 8);
}

static size_t f_header(char *b, size_t sz, size_t n, void *u)
{
    fetch_t *f = (fetch_t *)u;
    size_t len = sz * n;
    char line[600];
    size_t k = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, b, k); line[k] = 0;
    while (k && (line[k - 1] == '\r' || line[k - 1] == '\n')) line[--k] = 0;
    if (!strncasecmp(line, "HTTP/", 5)) { f->ctype[0] = 0; f->disposition[0] = 0; }   /* new response after redirect */
    else if (!strncasecmp(line, "Content-Type:", 13)) { char *v = line + 13; while (*v == ' ') v++; snprintf(f->ctype, sizeof(f->ctype), "%s", v); }
    else if (!strncasecmp(line, "Content-Disposition:", 20)) { char *v = line + 20; while (*v == ' ') v++; snprintf(f->disposition, sizeof(f->disposition), "%s", v); }
    return len;
}

static size_t f_write(char *b, size_t sz, size_t n, void *u)
{
    fetch_t *f = (fetch_t *)u;
    size_t len = sz * n;
    if (f->cancel) return 0;
    if (!is_page_type(f->ctype) || !strncasecmp(f->disposition, "attachment", 10)) { f->state = F_DOWNLOAD; return 0; }
    if (f->len + len + 1 > f->cap) {
        size_t nc = f->cap ? f->cap * 2 : 256 * 1024;
        while (nc < f->len + len + 1) nc *= 2;
        if (nc > PAGE_MAX) return 0;
        char *d = (char *)realloc(f->data, nc);
        if (!d) return 0;
        f->data = d; f->cap = nc;
    }
    memcpy(f->data + f->len, b, len);
    f->len += len;
    f->data[f->len] = 0;
    return len;
}

static int f_progress(void *u, double dt, double dn, double ut, double un)
{
    (void)ut; (void)un;
    fetch_t *f = (fetch_t *)u;
    f->total = dt; f->now = dn;
    return f->cancel ? 1 : 0;
}

static void fetch_thread(void *arg)
{
    fetch_t *f = (fetch_t *)arg;
    CURL *c = mk_handle(f->url);
    if (!c) { snprintf(f->err, sizeof(f->err), "curl init failed"); f->state = F_ERROR; sysThreadExit(0); return; }
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, f_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, f);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, f_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_PROGRESSFUNCTION, f_progress);
    curl_easy_setopt(c, CURLOPT_PROGRESSDATA, f);
    if (f->post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, f->post);
    CURLcode r = curl_easy_perform(c);
    char *eff = NULL;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) snprintf(f->url, sizeof(f->url), "%s", eff);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &f->code);
    curl_easy_cleanup(c);
    if (f->state == F_DOWNLOAD) { }
    else if (f->cancel) f->state = F_CANCELLED;
    else if (r != CURLE_OK) { snprintf(f->err, sizeof(f->err), "%s", curl_easy_strerror(r)); f->state = F_ERROR; }
    else f->state = F_DONE;
    sysThreadExit(0);
}

void fetch_wait(fetch_t *f)
{
    if (f->thread_on) { u64 rv; sysThreadJoin((sys_ppu_thread_t)f->thread, &rv); f->thread_on = 0; }
}

void fetch_reset(fetch_t *f)
{
    if (f->state == F_RUNNING) f->cancel = 1;
    fetch_wait(f);
    free(f->data); f->data = NULL; f->len = f->cap = 0;
    free(f->post); f->post = NULL;
    f->state = F_IDLE;
}

int fetch_start(fetch_t *f, const char *url, const char *post)
{
    fetch_reset(f);
    snprintf(f->url, sizeof(f->url), "%s", url);
    f->post = post ? strdup(post) : NULL;
    f->ctype[0] = f->disposition[0] = f->err[0] = 0;
    f->code = 0; f->now = f->total = 0; f->cancel = 0;
    f->state = F_RUNNING;
    sys_ppu_thread_t t;
    if (sysThreadCreate(&t, fetch_thread, f, 1200, 0x20000, THREAD_JOINABLE, "fetch") != 0) { f->state = F_ERROR; snprintf(f->err, sizeof(f->err), "thread"); return 0; }
    f->thread = t; f->thread_on = 1;
    return 1;
}

/* ======================= images ======================= */

#define IQ 64
typedef struct { uint32_t doc; int idx; char *url; } ireq_t;
typedef struct { uint32_t doc; int idx; void *tex; } ires_t;
static ireq_t iq[IQ]; static volatile int iq_r, iq_w;
static ires_t ir[IQ]; static volatile int ir_r, ir_w;
static sys_mutex_t img_mtx; static int img_mtx_ok;
static sys_ppu_thread_t img_th; static volatile int img_running, img_stop;

typedef struct { char *d; size_t n, cap; } mem_t;
static size_t m_write(char *b, size_t sz, size_t n, void *u)
{
    mem_t *m = (mem_t *)u;
    size_t len = sz * n;
    if (img_stop) return 0;
    if (m->n + len > IMG_MAX) return 0;
    if (m->n + len > m->cap) { size_t nc = m->cap ? m->cap * 2 : 64 * 1024; while (nc < m->n + len) nc *= 2; char *d = realloc(m->d, nc); if (!d) return 0; m->d = d; m->cap = nc; }
    memcpy(m->d + m->n, b, len); m->n += len;
    return len;
}

static void *decode_image(const uint8_t *d, size_t n)
{
    texture_t t = NULL;
    if (n > 8 && !memcmp(d, "\x89PNG", 4)) t = gfx_load_png_buffer(d, n);
    else if (n > 3 && d[0] == 0xFF && d[1] == 0xD8) t = gfx_load_jpg_buffer(d, n);
    if (!t) return NULL;
    int w, h;
    gfx_texture_size(t, &w, &h);
    if (w > 780 || h > 1200) t = gfx_make_thumb(t, 780, 1200);
    return t;
}

static int b64v(int c) { if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26; if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+' || c == '-') return 62; if (c == '/' || c == '_') return 63; return -1; }

static void img_thread(void *a)
{
    (void)a;
    while (!img_stop) {
        sysMutexLock(img_mtx, 0);
        int have = iq_r != iq_w;
        ireq_t q = { 0, 0, NULL };
        if (have) { q = iq[iq_r % IQ]; iq_r++; }
        sysMutexUnlock(img_mtx);
        if (!have) { usleep(20000); continue; }
        void *tex = NULL;
        if (!strncmp(q.url, "data:", 5)) {
            char *comma = strchr(q.url, ',');
            if (comma && strstr(q.url, ";base64")) {
                size_t L = strlen(comma + 1), o = 0;
                uint8_t *buf = malloc(L * 3 / 4 + 4);
                int acc = 0, bits = 0;
                for (size_t i = 0; buf && i < L; i++) { int v = b64v(comma[1 + i]); if (v < 0) continue; acc = (acc << 6) | v; bits += 6; if (bits >= 8) { bits -= 8; buf[o++] = (acc >> bits) & 0xFF; } }
                if (buf) tex = decode_image(buf, o);
                free(buf);
            }
        } else {
            mem_t m = { 0 };
            CURL *c = mk_handle(q.url);
            if (c) {
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, m_write);
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
                curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
                if (curl_easy_perform(c) == CURLE_OK && m.n) tex = decode_image((uint8_t *)m.d, m.n);
                curl_easy_cleanup(c);
            }
            free(m.d);
        }
        free(q.url);
        sysMutexLock(img_mtx, 0);
        if (ir_w - ir_r < IQ) { ir[ir_w % IQ] = (ires_t){ q.doc, q.idx, tex }; ir_w++; }
        else if (tex) gfx_free_texture((texture_t)tex);
        sysMutexUnlock(img_mtx);
    }
    img_running = 0;
    sysThreadExit(0);
}

void img_request(uint32_t doc_id, int index, const char *url)
{
    if (!img_mtx_ok) {
        sys_mutex_attr_t at; memset(&at, 0, sizeof(at));
        at.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO; at.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
        at.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED; at.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
        sysMutexCreate(&img_mtx, &at); img_mtx_ok = 1;
    }
    if (!img_running) {
        img_stop = 0;
        if (sysThreadCreate(&img_th, img_thread, NULL, 1300, 0x20000, THREAD_JOINABLE, "img") == 0) img_running = 1;
    }
    sysMutexLock(img_mtx, 0);
    if (iq_w - iq_r < IQ) { iq[iq_w % IQ] = (ireq_t){ doc_id, index, strdup(url) }; iq_w++; }
    sysMutexUnlock(img_mtx);
}

int img_poll(uint32_t *doc_id, int *index, void **tex)
{
    if (!img_mtx_ok) return 0;
    int ok = 0;
    sysMutexLock(img_mtx, 0);
    if (ir_r != ir_w) { ires_t r = ir[ir_r % IQ]; ir_r++; *doc_id = r.doc; *index = r.idx; *tex = r.tex; ok = 1; }
    sysMutexUnlock(img_mtx);
    return ok;
}

void img_cancel_all(void)
{
    if (!img_mtx_ok) return;
    sysMutexLock(img_mtx, 0);
    while (iq_r != iq_w) { free(iq[iq_r % IQ].url); iq_r++; }
    sysMutexUnlock(img_mtx);
}

/* ======================= downloads ======================= */

static download_t dls[DL_MAX];
typedef struct { int idx; vfs_file_t *fp; uint64_t t0, tlast; double nlast; } dlctx_t;
static dlctx_t dctx[DL_MAX];

download_t *dl_get(int i) { return (i >= 0 && i < DL_MAX) ? &dls[i] : NULL; }

static size_t d_write(char *b, size_t sz, size_t n, void *u)
{
    dlctx_t *x = (dlctx_t *)u;
    download_t *d = &dls[x->idx];
    if (d->cancel) return 0;
    size_t len = sz * n;
    if (!x->fp) {
        x->fp = vfs_open_write(d->path);
        if (!x->fp) { snprintf(d->err, sizeof(d->err), "can't create file"); return 0; }
    }
    if (vfs_write(x->fp, b, (uint32_t)len) != (int64_t)len) { snprintf(d->err, sizeof(d->err), "write failed (disk full?)"); return 0; }
    return len;
}

static int d_progress(void *u, double dt, double dn, double ut, double un)
{
    (void)ut; (void)un;
    dlctx_t *x = (dlctx_t *)u;
    download_t *d = &dls[x->idx];
    d->total = dt; d->now = dn;
    uint64_t t = now_us();
    if (t - x->tlast > 700000) { d->speed = (dn - x->nlast) * 1e6 / (double)(t - x->tlast); x->tlast = t; x->nlast = dn; }
    return d->cancel ? 1 : 0;
}

static void dl_thread(void *arg)
{
    int i = (int)(intptr_t)arg;
    download_t *d = &dls[i];
    dlctx_t *x = &dctx[i];
    memset(x, 0, sizeof(*x));
    x->idx = i; x->t0 = x->tlast = now_us();
    CURL *c = mk_handle(d->url);
    CURLcode r = CURLE_FAILED_INIT;
    long code = 0;
    if (c) {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, d_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, x);
        curl_easy_setopt(c, CURLOPT_PROGRESSFUNCTION, d_progress);
        curl_easy_setopt(c, CURLOPT_PROGRESSDATA, x);
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
        r = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
    }
    if (x->fp) { vfs_close(x->fp); x->fp = NULL; }
    if (d->cancel) { d->state = DL_CANCELLED; vfs_unlink(d->path); }
    else if (r != CURLE_OK) { if (!d->err[0]) snprintf(d->err, sizeof(d->err), "%s", curl_easy_strerror(r)); d->state = DL_FAILED; vfs_unlink(d->path); }
    else { if (!vfs_exists(d->path)) { vfs_file_t *f = vfs_open_write(d->path); if (f) vfs_close(f); } d->now = d->total > 0 ? d->total : d->now; d->state = DL_DONE; }
    sysThreadExit(0);
}

int dl_start(const char *url, const char *dest_path, const char *name)
{
    int i;
    for (i = 0; i < DL_MAX; i++) if (dls[i].state == DL_FREE) break;
    if (i == DL_MAX) for (i = 0; i < DL_MAX; i++) if (dls[i].state != DL_RUNNING) break;
    if (i == DL_MAX) return -1;
    download_t *d = &dls[i];
    memset(d, 0, sizeof(*d));
    snprintf(d->url, sizeof(d->url), "%s", url);
    snprintf(d->path, sizeof(d->path), "%s", dest_path);
    snprintf(d->name, sizeof(d->name), "%s", name);
    d->state = DL_RUNNING;
    sys_ppu_thread_t t;
    if (sysThreadCreate(&t, dl_thread, (void *)(intptr_t)i, 1250, 0x10000, 0, "dl") != 0) { d->state = DL_FAILED; snprintf(d->err, sizeof(d->err), "thread"); }
    return i;
}

void dl_cancel(int i) { if (i >= 0 && i < DL_MAX && dls[i].state == DL_RUNNING) dls[i].cancel = 1; }
void dl_remove(int i) { if (i >= 0 && i < DL_MAX && dls[i].state != DL_RUNNING) dls[i].state = DL_FREE; }
int dl_active_count(void) { int n = 0; for (int i = 0; i < DL_MAX; i++) if (dls[i].state == DL_RUNNING) n++; return n; }

/* ======================= url helpers ======================= */

int url_is_https(const char *u) { return !strncasecmp(u, "https://", 8); }

void url_host(const char *u, char *out, int n)
{
    const char *p = strstr(u, "://");
    p = p ? p + 3 : u;
    int k = 0;
    while (p[k] && p[k] != '/' && p[k] != '?' && p[k] != '#' && k < n - 1) { out[k] = p[k]; k++; }
    out[k] = 0;
}

static void normalize_path(char *path)
{
    /* resolve . and .. segments in place (path starts with '/') */
    char *segs[256]; int ns = 0;
    char tmp[URL_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    int trailing = strlen(tmp) > 0 && tmp[strlen(tmp) - 1] == '/';
    char *s = tmp + 1, *tok;
    while ((tok = strsep(&s, "/")) != NULL) {
        if (!strcmp(tok, "") || !strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (ns) ns--; continue; }
        if (ns < 256) segs[ns++] = tok;
    }
    char out[URL_MAX]; int o = 0;
    out[o++] = '/';
    for (int i = 0; i < ns; i++) {
        int l = strlen(segs[i]);
        if (o + l + 2 >= (int)sizeof(out)) break;
        memcpy(out + o, segs[i], l); o += l;
        if (i < ns - 1 || trailing) out[o++] = '/';
    }
    out[o] = 0;
    strcpy(path, out);
}

void url_join(const char *base, const char *rel, char *out, int n)
{
    while (*rel == ' ' || *rel == '\t' || *rel == '\n' || *rel == '\r') rel++;
    const char *colon = strchr(rel, ':');
    const char *slash = strchr(rel, '/');
    if (colon && (!slash || colon < slash)) { int ok = 1; for (const char *p = rel; p < colon; p++) if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.') ok = 0; if (ok) { snprintf(out, n, "%s", rel); return; } }
    char scheme[16] = "https";
    const char *ss = strstr(base, "://");
    if (ss && ss - base < 15) { memcpy(scheme, base, ss - base); scheme[ss - base] = 0; }
    if (rel[0] == '/' && rel[1] == '/') { snprintf(out, n, "%s:%s", scheme, rel); return; }
    char host[512];
    url_host(base, host, sizeof(host));
    const char *bp = ss ? ss + 3 : base;
    bp += strlen(host);
    char bpath[URL_MAX];
    int k = 0;
    while (bp[k] && bp[k] != '?' && bp[k] != '#' && k < (int)sizeof(bpath) - 1) { bpath[k] = bp[k]; k++; }
    bpath[k] = 0;
    if (!bpath[0]) strcpy(bpath, "/");
    if (rel[0] == '#') { char b2[URL_MAX]; snprintf(b2, sizeof(b2), "%s", base); char *h = strchr(b2, '#'); if (h) *h = 0; snprintf(out, n, "%s%s", b2, rel); return; }
    if (rel[0] == '?') { snprintf(out, n, "%s://%s%s%s", scheme, host, bpath, rel); return; }
    if (!rel[0]) { snprintf(out, n, "%s", base); return; }
    char path[URL_MAX];
    if (rel[0] == '/') snprintf(path, sizeof(path), "%s", rel);
    else {
        char *ls = strrchr(bpath, '/');
        if (ls) ls[1] = 0;
        snprintf(path, sizeof(path), "%s%s", bpath, rel);
    }
    /* keep query/fragment out of normalization */
    char *q = strpbrk(path, "?#");
    char qs[URL_MAX] = "";
    if (q) { snprintf(qs, sizeof(qs), "%s", q); *q = 0; }
    normalize_path(path);
    snprintf(out, n, "%s://%s%s%s", scheme, host, path, qs);
}

void url_encode(const char *in, char *out, int n)
{
    static const char hx[] = "0123456789ABCDEF";
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < n - 4; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') out[o++] = *p;
        else if (*p == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hx[*p >> 4]; out[o++] = hx[*p & 15]; }
    }
    out[o] = 0;
}

static void pct_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { char h[3] = { p[1], p[2], 0 }; *o++ = (char)strtol(h, NULL, 16); p += 2; }
        else *o++ = *p;
    }
    *o = 0;
}

void url_filename(const char *url, const char *disp, char *out, int n)
{
    out[0] = 0;
    if (disp && disp[0]) {
        const char *f = strcasestr(disp, "filename*=");
        if (f) { f += 10; const char *q = strstr(f, "''"); if (q) f = q + 2; }
        else if ((f = strcasestr(disp, "filename=")) != NULL) f += 9;
        if (f) {
            if (*f == '"') f++;
            int k = 0;
            while (f[k] && f[k] != '"' && f[k] != ';' && k < n - 1) { out[k] = f[k]; k++; }
            out[k] = 0;
            pct_decode(out);
        }
    }
    if (!out[0]) {
        char tmp[URL_MAX];
        snprintf(tmp, sizeof(tmp), "%s", url);
        char *q = strpbrk(tmp, "?#"); if (q) *q = 0;
        char *s = strrchr(tmp, '/');
        const char *nm = (s && s[1] && s > strstr(tmp, "://") + 2) ? s + 1 : "download";
        snprintf(out, n, "%s", nm);
        pct_decode(out);
    }
    /* make it a safe file name */
    for (char *p = out; *p; p++) if (strchr("/\\:*?\"<>|", *p) || (unsigned char)*p < 32) *p = '_';
    if (!out[0]) snprintf(out, n, "download");
}

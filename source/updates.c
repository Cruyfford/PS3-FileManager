#include "updates.h"
#include "ftp.h"
#include <curl/curl.h>
#include <sys/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UMAX 64
static upd_pkg_t pk[UMAX];
static volatile int np, state;
static char title[160], err[128], tid[16];
static sys_ppu_thread_t thr; static int thr_on, curl_ok;

int upd_state(void) { return state; }
int upd_count(void) { return np; }
const upd_pkg_t *upd_get(int i) { return i >= 0 && i < np ? &pk[i] : NULL; }
const char *upd_title(void) { return title; }
const char *upd_error(void) { return err; }

typedef struct { char *d; size_t n; } buf_t;
static size_t wr(void *p, size_t s, size_t m, void *u)
{
    buf_t *b = u; size_t k = s * m;
    if (b->n + k > 1024 * 1024) return 0;
    char *d = realloc(b->d, b->n + k + 1); if (!d) return 0;
    b->d = d; memcpy(d + b->n, p, k); b->n += k; d[b->n] = 0;
    return k;
}

static void attr(const char *tag, const char *name, char *out, int on)
{
    out[0] = 0;
    char key[40]; snprintf(key, sizeof(key), " %s=\"", name);
    const char *a = strstr(tag, key);
    if (!a) return;
    a += strlen(key);
    const char *e = strchr(a, '"');
    if (!e) return;
    int l = e - a; if (l >= on) l = on - 1;
    memcpy(out, a, l); out[l] = 0;
}

static void worker(void *a)
{
    (void)a;
    np = 0; title[0] = 0; err[0] = 0;
    if (net_up() < 0) { snprintf(err, sizeof(err), "network"); state = UPD_ERROR; sysThreadExit(0); return; }
    if (!curl_ok) { curl_global_init(CURL_GLOBAL_ALL); curl_ok = 1; }
    char url[256];
    snprintf(url, sizeof(url), "https://a0.ww.np.dl.playstation.net/tpl/np/%s/%s-ver.xml", tid, tid);
    buf_t b = { 0 };
    CURL *c = curl_easy_init();
    long code = 0; CURLcode r = CURLE_FAILED_INIT;
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);   /* Sony's own CA */
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (PLAYSTATION 3; 4.93)");
        r = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
    }
    if (r != CURLE_OK && code != 404) { snprintf(err, sizeof(err), "%s", curl_easy_strerror(r)); state = UPD_ERROR; }
    else if (code == 404 || !b.d || !b.n) state = UPD_NONE;
    else {
        const char *p = b.d;
        while (np < UMAX && (p = strstr(p, "<package ")) != NULL) {
            const char *e = strchr(p, '>');
            if (!e) break;
            char tag[1024]; int l = e - p; if (l >= (int)sizeof(tag)) l = sizeof(tag) - 1;
            memcpy(tag, p, l); tag[l] = 0;
            upd_pkg_t *u = &pk[np];
            char sz[32];
            attr(tag, "version", u->ver, sizeof(u->ver));
            attr(tag, "size", sz, sizeof(sz)); u->size = strtoull(sz, NULL, 10);
            attr(tag, "url", u->url, sizeof(u->url));
            attr(tag, "ps3_system_ver", u->sysver, sizeof(u->sysver));
            if (u->url[0]) np++;
            p = e;
        }
        const char *t = strstr(b.d, "<TITLE>");
        if (t) { t += 7; const char *e = strstr(t, "</TITLE>"); if (e) { int l = e - t; if (l >= (int)sizeof(title)) l = sizeof(title) - 1; memcpy(title, t, l); title[l] = 0; } }
        state = np ? UPD_DONE : UPD_NONE;
    }
    free(b.d);
    sysThreadExit(0);
}

void upd_check(const char *title_id)
{
    if (state == UPD_BUSY) return;
    if (thr_on) { u64 rv; sysThreadJoin(thr, &rv); thr_on = 0; }
    snprintf(tid, sizeof(tid), "%s", title_id);
    state = UPD_BUSY;
    if (sysThreadCreate(&thr, worker, NULL, 1200, 0x10000, THREAD_JOINABLE, "updchk") == 0) thr_on = 1;
    else state = UPD_ERROR;
}

#include "dlmgr.h"
#include "vfs.h"
#include "ftp.h"
#include <curl/curl.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static dl_item_t items[DL_MAX];
static int n;
static char state_file[512];
static sys_mutex_t mtx; static int mtx_ok;
static sys_ppu_thread_t thr; static volatile int thr_on, stop_req, pause_cur;
static volatile int cur = -1;
static int curl_ok;

static void LK(void) { if (mtx_ok) sysMutexLock(mtx, 0); }
static void UL(void) { if (mtx_ok) sysMutexUnlock(mtx); }
static uint64_t now_ms(void) { u64 s, ns; sysGetCurrentTime(&s, &ns); return s * 1000ULL + ns / 1000000ULL; }

int dl_count(void) { return n; }
const dl_item_t *dl_get(int i) { return i >= 0 && i < n ? &items[i] : NULL; }
int dl_active(void) { return cur >= 0; }

static void save(void)
{
    FILE *f = fopen(state_file, "wb");
    if (!f) return;
    for (int i = 0; i < n; i++) {
        int st = items[i].state == DL_RUNNING ? DL_QUEUED : items[i].state;
        fprintf(f, "%d|%s|%s\n", st, items[i].path, items[i].url);
    }
    fclose(f);
}

static void name_from_url(const char *url, char *out, int on)
{
    const char *e = url + strcspn(url, "?#");
    const char *s = e;
    while (s > url && s[-1] != '/') s--;
    int l = e - s;
    if (l <= 0) { snprintf(out, on, "download.bin"); return; }
    if (l >= on) l = on - 1;
    int o = 0;
    for (int i = 0; i < l; i++) {
        if (s[i] == '%' && i + 2 < l) { char h[3] = { s[i + 1], s[i + 2], 0 }; out[o++] = (char)strtol(h, NULL, 16); i += 2; }
        else out[o++] = strchr("\\:*?\"<>|", s[i]) ? '_' : s[i];
    }
    out[o] = 0;
}

typedef struct { vfs_file_t *f; dl_item_t *it; uint64_t t0, b0; int checked; long resume_from; CURL *c; int restart; } ctx_t;

static size_t on_write(void *p, size_t sz, size_t nm, void *u)
{
    ctx_t *x = u;
    size_t k = sz * nm;
    if (!x->checked) {
        x->checked = 1;
        long code = 0; curl_easy_getinfo(x->c, CURLINFO_RESPONSE_CODE, &code);
        if (x->resume_from > 0 && code == 200) {          /* server ignored Range: start over */
            vfs_close(x->f);
            char part[1100]; snprintf(part, sizeof(part), "%s.part", x->it->path);
            x->f = vfs_open_write(part);
            x->it->done = 0;
            if (!x->f) return 0;
        }
    }
    if (vfs_write(x->f, p, (uint32_t)k) != (int64_t)k) return 0;
    x->it->done += k;
    uint64_t t = now_ms();
    if (t - x->t0 >= 1000) { x->it->speed = (uint32_t)((x->it->done - x->b0) * 1000 / (t - x->t0)); x->t0 = t; x->b0 = x->it->done; }
    return k;
}

static int on_progress(void *u, double dt, double dn, double ut, double un)
{
    ctx_t *x = u; (void)dn; (void)ut; (void)un;
    if (dt > 0 && !x->it->total) x->it->total = (uint64_t)dt + (uint64_t)x->resume_from;
    return (stop_req || pause_cur) ? 1 : 0;
}

static int fetch(dl_item_t *it)
{
    char part[1100]; snprintf(part, sizeof(part), "%s.part", it->path);
    int d = 0; uint64_t have = 0;
    vfs_file_t *f;
    if (vfs_stat(part, &d, &have) == 0 && have > 0 && (f = vfs_open_rw(part))) vfs_seek(f, have);
    else { have = 0; f = vfs_open_write(part); }
    if (!f) { snprintf(it->err, sizeof(it->err), "cannot create file"); return -1; }
    it->done = have; it->total = 0;
    CURL *c = curl_easy_init();
    if (!c) { vfs_close(f); return -1; }
    ctx_t x = { f, it, now_ms(), have, 0, (long)have, c, 0 };
    char eb[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(c, CURLOPT_URL, it->url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_PROGRESSFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_PROGRESSDATA, &x);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, eb);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (PLAYSTATION 3; 4.93)");
    if (have) curl_easy_setopt(c, CURLOPT_RESUME_FROM, (long)have);
    CURLcode r = curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    vfs_close(x.f);
    if (r == CURLE_OK) {
        vfs_unlink(it->path);
        if (vfs_rename(part, it->path)) { snprintf(it->err, sizeof(it->err), "rename failed"); return -1; }
        it->total = it->done;
        return 0;
    }
    if (code == 416 && have) {                       /* already complete */
        vfs_unlink(it->path);
        return vfs_rename(part, it->path) == 0 ? 0 : -1;
    }
    if (r == CURLE_ABORTED_BY_CALLBACK) return 1;
    snprintf(it->err, sizeof(it->err), "%s", eb[0] ? eb : curl_easy_strerror(r));
    return -1;
}

static void worker(void *a)
{
    (void)a;
    if (net_up() >= 0 && !curl_ok) { curl_global_init(CURL_GLOBAL_ALL); curl_ok = 1; }
    while (!stop_req) {
        int pick = -1;
        LK();
        for (int i = 0; i < n; i++) if (items[i].state == DL_QUEUED) { pick = i; break; }
        if (pick >= 0) { items[pick].state = DL_RUNNING; items[pick].err[0] = 0; cur = pick; pause_cur = 0; }
        UL();
        if (pick < 0) { usleep(300000); continue; }
        dl_item_t *it = &items[pick];
        int r = -1;
        for (int attempt = 0; attempt < 4 && !stop_req && !pause_cur; attempt++) {
            r = fetch(it);
            if (r >= 0) break;
            sleep(2);
        }
        LK();
        if (stop_req) it->state = DL_QUEUED;
        else if (r == 0) it->state = DL_DONE;
        else if (r == 1) it->state = DL_PAUSED;
        else it->state = DL_FAILED;
        it->speed = 0;
        cur = -1;
        save();
        UL();
    }
    thr_on = 0;
    sysThreadExit(0);
}

static void start_worker(void)
{
    if (thr_on) return;
    stop_req = 0;
    if (sysThreadCreate(&thr, worker, NULL, 1250, 0x20000, THREAD_JOINABLE, "dlmgr") == 0) thr_on = 1;
}

void dl_init(const char *file)
{
    snprintf(state_file, sizeof(state_file), "%s", file);
    if (!mtx_ok) {
        sys_mutex_attr_t at; memset(&at, 0, sizeof(at));
        at.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO; at.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
        at.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED; at.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
        strcpy(at.name, "dlmgr");
        mtx_ok = sysMutexCreate(&mtx, &at) == 0;
    }
    n = 0;
    FILE *f = fopen(file, "rb");
    if (f) {
        char line[1700];
        while (n < DL_MAX && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *a = strchr(line, '|'); if (!a) continue;
            char *b = strchr(a + 1, '|'); if (!b) continue;
            *a = 0; *b = 0;
            dl_item_t *it = &items[n++];
            memset(it, 0, sizeof(*it));
            it->state = atoi(line);
            if (it->state == DL_RUNNING) it->state = DL_QUEUED;
            snprintf(it->path, sizeof(it->path), "%s", a + 1);
            snprintf(it->url, sizeof(it->url), "%s", b + 1);
            const char *bn = strrchr(it->path, '/');
            snprintf(it->name, sizeof(it->name), "%s", bn ? bn + 1 : it->path);
            if (it->state == DL_DONE) { int d; uint64_t s; if (vfs_stat(it->path, &d, &s) == 0) it->done = it->total = s; }
            else { char part[1100]; int d; uint64_t s; snprintf(part, sizeof(part), "%s.part", it->path); if (vfs_stat(part, &d, &s) == 0) it->done = s; }
        }
        fclose(f);
    }
    for (int i = 0; i < n; i++) if (items[i].state == DL_QUEUED) { start_worker(); break; }
}

int dl_add(const char *url, const char *dir)
{
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return -1;
    LK();
    if (n >= DL_MAX) {                               /* drop the oldest finished entry */
        int k = -1; for (int i = 0; i < n; i++) if (items[i].state == DL_DONE) { k = i; break; }
        if (k < 0) { UL(); return -1; }
        memmove(&items[k], &items[k + 1], sizeof(dl_item_t) * (n - k - 1)); n--;
    }
    dl_item_t *it = &items[n];
    memset(it, 0, sizeof(*it));
    snprintf(it->url, sizeof(it->url), "%s", url);
    name_from_url(url, it->name, sizeof(it->name));
    size_t l = strlen(dir);
    snprintf(it->path, sizeof(it->path), l && dir[l - 1] == '/' ? "%s%s" : "%s/%s", dir, it->name);
    vfs_mkdir(dir);
    it->state = DL_QUEUED;
    int idx = n++;
    save();
    UL();
    start_worker();
    return idx;
}

void dl_pause(int i) { LK(); if (i >= 0 && i < n) { if (items[i].state == DL_RUNNING && cur == i) pause_cur = 1; else if (items[i].state == DL_QUEUED) items[i].state = DL_PAUSED; save(); } UL(); }
void dl_resume(int i) { LK(); if (i >= 0 && i < n && (items[i].state == DL_PAUSED || items[i].state == DL_FAILED)) { items[i].state = DL_QUEUED; save(); } UL(); start_worker(); }

void dl_remove(int i)
{
    LK();
    if (i < 0 || i >= n || cur == i) { UL(); return; }
    if (items[i].state != DL_DONE) { char part[1100]; snprintf(part, sizeof(part), "%s.part", items[i].path); vfs_unlink(part); }
    memmove(&items[i], &items[i + 1], sizeof(dl_item_t) * (n - i - 1)); n--;
    save();
    UL();
}

void dl_shutdown(void)
{
    if (!thr_on) return;
    stop_req = 1;
    u64 rv; sysThreadJoin(thr, &rv);
    thr_on = 0;
}

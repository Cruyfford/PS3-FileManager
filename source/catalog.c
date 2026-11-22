/*
 * ARCADIA catalog: bounded TSV line reader + paged search + explicit PKG download.
 * User must supply ARCADIA_PS3_CATALOG.tsv in the app's USRDIR or on a mounted USB.
 * There is no license management or automatic installation.
 */
#include "catalog.h"
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/thread.h>
#include <curl/curl.h>

static char cat_path[1024];
static catalog_item_t cat_page[CAT_PAGE_SIZE];
static int cat_n, cat_total;

typedef struct {
    vfs_file_t *file;
    unsigned pos, len;
    unsigned char bytes[8192];
} cat_stream_t;

static int next_line(cat_stream_t *s, char *out, int cap)
{
    int n = 0, overflow = 0;
    for (;;) {
        if (s->pos >= s->len) {
            int64_t got = vfs_read(s->file, s->bytes, sizeof(s->bytes));
            if (got <= 0) {
                if (!n || overflow) return 0;
                out[n] = 0;
                return 1;
            }
            s->len = (unsigned)got;
            s->pos = 0;
        }
        unsigned char c = s->bytes[s->pos++];
        if (c == '\n') {
            if (overflow) { n = 0; overflow = 0; continue; }
            if (n && out[n-1] == '\r') n--;
            out[n] = 0;
            return 1;
        }
        if (!overflow && n < cap-1) out[n++] = (char)c;
        else overflow = 1;
    }
}

/* Preserve empty TSV fields (strtok_r collapses them). */
static int parse_record(char *line, catalog_item_t *out)
{
    char *fields[6];
    fields[0] = line;
    if (line[0] == '#') return 0;
    for (int i = 1; i < 6; i++) {
        char *tab = strchr(fields[i-1], '\t');
        if (!tab) return 0;
        *tab = 0;
        fields[i] = tab + 1;
    }
    if (strncmp(fields[5], "http://", 7) && strncmp(fields[5], "https://", 8)) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->category, sizeof(out->category), "%s", fields[0]);
    snprintf(out->title, sizeof(out->title), "%s", fields[1]);
    snprintf(out->size, sizeof(out->size), "%s", fields[2]);
    snprintf(out->title_id, sizeof(out->title_id), "%s", fields[3]);
    snprintf(out->kind, sizeof(out->kind), "%s", fields[4]);
    snprintf(out->url, sizeof(out->url), "%s", fields[5]);
    return 1;
}

static int contains_ascii_casefold(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return 1;
    for (const char *a = hay; *a; a++) {
        const char *p = a, *q = needle;
        while (*p && *q && tolower((unsigned char)*p) == tolower((unsigned char)*q)) { p++; q++; }
        if (!*q) return 1;
    }
    return 0;
}

int catalog_set_source(const char *path)
{
    cat_path[0] = 0;
    if (!path || !path[0]) return 0;
    vfs_file_t *f = vfs_open_read(path);
    if (!f) return 0;
    vfs_close(f);
    snprintf(cat_path, sizeof(cat_path), "%s", path);
    return 1;
}

int catalog_scan(int category, const char *query, int page)
{
    cat_n = cat_total = 0;
    if (!cat_path[0]) return 0;
    vfs_file_t *f = vfs_open_read(cat_path);
    if (!f) return 0;
    cat_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.file = f;
    char line[1200];
    catalog_item_t item;
    int offset = page < 0 ? 0 : page * CAT_PAGE_SIZE;
    while (next_line(&stream, line, sizeof(line))) {
        if (!parse_record(line, &item)) continue;
        /* The v2 catalog uses explicit platforms instead of broad GAMES/TOOLS buckets. */
        static const char *const categories[CAT_CATEGORY_COUNT] =
            {"ALL","PS3","PS2","PSP","PS1","APPS","THEMES","DLC"};
        if (category > CAT_ALL && category < CAT_CATEGORY_COUNT &&
            strcmp(item.category,categories[category])) continue;
        if (query && query[0] &&
            !contains_ascii_casefold(item.title, query) &&
            !contains_ascii_casefold(item.title_id, query)) continue;
        if (cat_total >= offset && cat_n < CAT_PAGE_SIZE)
            cat_page[cat_n++] = item;
        cat_total++;
    }
    vfs_close(f);
    return cat_total;
}
int catalog_result_count(void) { return cat_n; }
int catalog_total_matches(void) { return cat_total; }
const catalog_item_t *catalog_result(int i) { return (i >= 0 && i < cat_n) ? &cat_page[i] : NULL; }
const char *catalog_source(void) { return cat_path; }

/* Download manager: one active transfer. Never silently install packages. */
static volatile int dl_state, dl_cancel;
static volatile uint64_t dl_done, dl_total;
static int dl_thread_created;
static sys_ppu_thread_t dl_thread;
static char dl_url[256], dl_file[128], dl_message[192], dl_ext[5];

static uint32_t url_fingerprint(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static const char *catalog_direct_ext(const catalog_item_t *item)
{
    if (!item) return NULL;
    /* No MediaFire HTML pages, redirect scrapers or non-HTTP URLs. */
    if (strcmp(item->kind,"playstation_cdn") &&
        strcmp(item->kind,"github_release") &&
        strcmp(item->kind,"external_unverified")) return NULL;
    if (strncmp(item->url,"http://",7) && strncmp(item->url,"https://",8)) return NULL;
    const char *last = strrchr(item->url,'/');
    if (!last) return NULL;
    const char *end=strchr(last,'?');
    if (!end) end=last+strlen(last);
    if (end-last>=4 && !strncasecmp(end-4,".pkg",4)) return "pkg";
    if (end-last>=4 && !strncasecmp(end-4,".p3t",4)) return "p3t";
    return NULL;
}
int catalog_is_direct_pkg(const catalog_item_t *item)
{
    return catalog_direct_ext(item)!=NULL;
}

static size_t on_write(void *ptr, size_t size, size_t nmemb, void *user)
{
    /* Native PS3 paths must go through the unified VFS (sysFs).
     * stdio fopen() may not map /dev_hdd0 in all PSL1GHT runtimes. */
    if (!size || !nmemb || nmemb > UINT32_MAX / size) return 0;
    uint32_t bytes = (uint32_t)(size * nmemb);
    int64_t written = vfs_write((vfs_file_t *)user, ptr, bytes);
    return written < 0 ? 0 : (size_t)written;
}

static int on_progress(void *unused, double total, double now, double upload_total, double upload_now)
{
    (void)unused; (void)upload_total; (void)upload_now;
    if (total > 0) dl_total = (uint64_t)total;
    if (now >= 0) dl_done = (uint64_t)now;
    return dl_cancel ? 1 : 0;
}

static void download_worker(void *arg)
{
    (void)arg;
    char tmp[512], full[512];
    snprintf(full, sizeof(full), "/dev_hdd0/packages/%s", dl_file);
    snprintf(tmp, sizeof(tmp), "%s.part", full);
    if (vfs_exists(full)) {
        snprintf(dl_message, sizeof(dl_message), "Package already exists in /dev_hdd0/packages");
        dl_state = CAT_DL_FAILED;
        sysThreadExit(0);
        return;
    }
    if (vfs_mkdir("/dev_hdd0/packages") != 0 && !vfs_exists("/dev_hdd0/packages")) {
        snprintf(dl_message,sizeof(dl_message),"Cannot create /dev_hdd0/packages");
        dl_state = CAT_DL_FAILED;
        sysThreadExit(0);
        return;
    }
    vfs_file_t *f = vfs_open_write(tmp);
    if (!f) {
        snprintf(dl_message,sizeof(dl_message),"Cannot write to internal HDD (space or permissions)");
        dl_state = CAT_DL_FAILED;
        sysThreadExit(0);
        return;
    }
    CURL *c = curl_easy_init();
    CURLcode code = CURLE_FAILED_INIT;
    long http = 0;
    char curl_error[CURL_ERROR_SIZE] = {0};
    if (c) {
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, curl_error);
        curl_easy_setopt(c, CURLOPT_URL, dl_url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 25L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 90L);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_PROGRESSFUNCTION, on_progress);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "PS3-FileManager/ARCADIA");
        code = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
        curl_easy_cleanup(c);
    }
    vfs_close(f);
    if (dl_cancel) {
        vfs_unlink(tmp);
        dl_state = CAT_DL_CANCELLED;
        snprintf(dl_message,sizeof(dl_message),"Download cancelled");
        sysThreadExit(0);
        return;
    }
    if (code != CURLE_OK || (http != 200 && http != 206)) {
        vfs_unlink(tmp);
        dl_state = CAT_DL_FAILED;
        snprintf(dl_message,sizeof(dl_message),"Failed: HTTP %ld, curl %d (%s)", http, (int)code,
                 curl_error[0] ? curl_error : curl_easy_strerror(code));
        sysThreadExit(0);
        return;
    }
    f = vfs_open_read(tmp);
    unsigned char magic[4] = {0};
    int valid = f && vfs_read(f,magic,4) == 4 &&
               ((!strcmp(dl_ext,"pkg") &&
                 magic[0]==0x7f && magic[1]=='P' && magic[2]=='K' && magic[3]=='G') ||
                (!strcmp(dl_ext,"p3t") &&
                 magic[0]=='P' && magic[1]=='3' && magic[2]=='T' && magic[3]=='F'));
    if (f) vfs_close(f);
    if (!valid || vfs_rename(tmp,full) != 0) {
        vfs_unlink(tmp);
        dl_state = CAT_DL_FAILED;
        snprintf(dl_message,sizeof(dl_message),"Invalid PKG/P3T header or failed to save");
        sysThreadExit(0);
        return;
    }
    dl_state = CAT_DL_DONE;
    snprintf(dl_message,sizeof(dl_message),"Saved to /dev_hdd0/packages");
    sysThreadExit(0);
}

int catalog_download_start(const catalog_item_t *item)
{
    if (!catalog_is_direct_pkg(item) || dl_state == CAT_DL_RUNNING) return 0;
    if (dl_thread_created) {
        u64 code;
        sysThreadJoin(dl_thread,&code);
        dl_thread_created=0;
    }
    snprintf(dl_url,sizeof(dl_url),"%s",item->url);
    snprintf(dl_ext,sizeof(dl_ext),"%s",catalog_direct_ext(item));
    snprintf(dl_file,sizeof(dl_file),"ARCADIA_%08X.%s",(unsigned)url_fingerprint(item->url),dl_ext);
    dl_message[0]=0;
    dl_done=dl_total=0;
    dl_cancel=0;
    curl_global_init(CURL_GLOBAL_ALL);
    dl_state=CAT_DL_RUNNING;
    if (sysThreadCreate(&dl_thread,download_worker,NULL,1300,0x30000,THREAD_JOINABLE,"arcadia_dl") != 0) {
        dl_state=CAT_DL_FAILED;
        snprintf(dl_message,sizeof(dl_message),"Could not start download thread");
        return 0;
    }
    dl_thread_created=1;
    return 1;
}
int catalog_download_state(void) { return dl_state; }
void catalog_download_progress(uint64_t *done, uint64_t *total)
{
    if (done) *done=dl_done;
    if (total) *total=dl_total;
}
const char *catalog_download_message(void) { return dl_message; }
const char *catalog_download_name(void) { return dl_file; }
void catalog_download_cancel(void) { if (dl_state == CAT_DL_RUNNING) dl_cancel=1; }
void catalog_shutdown(void)
{
    if (dl_thread_created) {
        dl_cancel=1;
        u64 code;
        sysThreadJoin(dl_thread,&code);
        dl_thread_created=0;
    }
}

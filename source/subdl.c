/* Online subtitles: SubDL (JSON API, free key) + Podnapisi (XML, no key). Downloads are zip archives. */
#include "subdl.h"
#include "ftp.h"
#include "vfs.h"
#include "covers.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <zip.h>
#include <sys/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAXR 60
static subres_t res[MAXR];
static volatile int nres, state;
static char key[96], query[200], result[1400], err[160];
static char v_name[512], dl_base[1300];
static int dl_index;
static sys_ppu_thread_t thr; static int thr_on;
static int curl_ok;

int subdl_state(void) { return state; }
int subdl_count(void) { return nres; }
const subres_t *subdl_get(int i) { return i >= 0 && i < nres ? &res[i] : NULL; }
const char *subdl_result(void) { return result; }
const char *subdl_error(void) { return err; }
const char *subdl_query(void) { return query; }
const char *subdl_key(void) { return key; }
void subdl_ack(void) { if (state == SUBDL_READY || state == SUBDL_ERROR) state = nres ? SUBDL_RESULTS : SUBDL_IDLE; }

void subdl_load_key(const char *path)
{
    key[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fgets(key, sizeof(key), f)) { size_t l = strlen(key); while (l && isspace((unsigned char)key[l - 1])) key[--l] = 0; }
    fclose(f);
}

void subdl_save_key(const char *path, const char *k)
{
    snprintf(key, sizeof(key), "%s", k);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(key, f); fclose(f); }
}

/* ------------------------------------------------------------ http */
typedef struct { char *d; size_t n, cap; } buf_t;
static size_t wr(void *p, size_t s, size_t m, void *u)
{
    buf_t *b = u; size_t k = s * m;
    if (b->n + k + 1 > 8 * 1024 * 1024) return 0;
    if (b->n + k + 1 > b->cap) { size_t c = (b->n + k + 1) * 2; char *d = realloc(b->d, c); if (!d) return 0; b->d = d; b->cap = c; }
    memcpy(b->d + b->n, p, k); b->n += k; b->d[b->n] = 0;
    return k;
}

static int get(const char *url, buf_t *b, const char *accept)
{
    CURL *c = curl_easy_init();
    if (!c) return 0;
    b->n = 0;
    struct curl_slist *h = NULL;
    if (accept) { char a[96]; snprintf(a, sizeof(a), "Accept: %s", accept); h = curl_slist_append(h, a); }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "PS3-FileManager/1.0");
    if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    CURLcode r = curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (h) curl_slist_free_all(h);
    if (r != CURLE_OK) { snprintf(err, sizeof(err), "%s", curl_easy_strerror(r)); return 0; }
    if (code != 200) { snprintf(err, sizeof(err), "HTTP %ld", code); return 0; }
    return b->n > 0;
}

static void urlenc(const char *in, char *out, int n)
{
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < n - 4; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.') out[o++] = *p;
        else o += snprintf(out + o, n - o, "%%%02X", *p);
    }
    out[o] = 0;
}

/* S01E05 / 1x05 */
static void season_episode(const char *n, int *s, int *e)
{
    *s = *e = 0;
    for (const char *p = n; *p; p++) {
        if ((p[0] == 'S' || p[0] == 's') && isdigit((unsigned char)p[1])) {
            int a = atoi(p + 1); const char *q = p + 1; while (isdigit((unsigned char)*q)) q++;
            if ((*q == 'E' || *q == 'e') && isdigit((unsigned char)q[1])) { *s = a; *e = atoi(q + 1); return; }
        }
        if (isdigit((unsigned char)p[0]) && (p[1] == 'x' || p[2] == 'x')) {
            const char *q = strchr(p, 'x');
            if (q && q - p <= 2 && isdigit((unsigned char)q[1])) { *s = atoi(p); *e = atoi(q + 1); return; }
        }
    }
}

static void add(const char *title, const char *url, const char *lang, int src)
{
    if (nres >= MAXR) return;
    subres_t *r = &res[nres];
    snprintf(r->title, sizeof(r->title), "%s", title);
    snprintf(r->url, sizeof(r->url), "%s", url);
    snprintf(r->lang, sizeof(r->lang), "%s", lang);
    r->src = src;
    nres++;
}

static void search_subdl(const char *q, int se, int ep, const char *lang)
{
    if (!key[0]) return;
    char qe[400], url[900];
    urlenc(q, qe, sizeof(qe));
    int o = snprintf(url, sizeof(url), "https://api.subdl.com/api/v1/subtitles?api_key=%s&film_name=%s&languages=%s&subs_per_page=30", key, qe, lang);
    if (se) snprintf(url + o, sizeof(url) - o, "&type=tv&season_number=%d&episode_number=%d", se, ep);
    buf_t b = { 0 };
    if (get(url, &b, "application/json")) {
        cJSON *j = cJSON_Parse(b.d);
        cJSON *subs = j ? cJSON_GetObjectItem(j, "subtitles") : NULL;
        cJSON *it;
        cJSON_ArrayForEach(it, subs) {
            cJSON *u = cJSON_GetObjectItem(it, "url"), *rn = cJSON_GetObjectItem(it, "release_name"), *nm = cJSON_GetObjectItem(it, "name");
            if (!cJSON_IsString(u)) continue;
            char full[400]; snprintf(full, sizeof(full), "https://dl.subdl.com%s", u->valuestring);
            add(cJSON_IsString(rn) ? rn->valuestring : cJSON_IsString(nm) ? nm->valuestring : q, full, lang, 0);
        }
        if (j) {
            cJSON *e = cJSON_GetObjectItem(j, "error");
            if (!subs && cJSON_IsString(e)) snprintf(err, sizeof(err), "SubDL: %s", e->valuestring);
            cJSON_Delete(j);
        }
    }
    free(b.d);
}

/* text between <tag> and </tag> after p, limited to end */
static int xml_val(const char *p, const char *end, const char *tag, char *out, int n)
{
    char o[40], c[40];
    snprintf(o, sizeof(o), "<%s>", tag); snprintf(c, sizeof(c), "</%s>", tag);
    const char *a = strstr(p, o);
    if (!a || a >= end) return 0;
    a += strlen(o);
    const char *b = strstr(a, c);
    if (!b || b > end) return 0;
    int l = b - a; if (l >= n) l = n - 1;
    memcpy(out, a, l); out[l] = 0;
    return 1;
}

static void search_podnapisi(const char *q, int se, int ep, const char *lang)
{
    char qe[400], url[900];
    urlenc(q, qe, sizeof(qe));
    int o = snprintf(url, sizeof(url), "https://www.podnapisi.net/subtitles/search/old?sXML=1&sL=%s&sK=%s", lang, qe);
    if (se) snprintf(url + o, sizeof(url) - o, "&sTS=%d&sTE=%d", se, ep);
    buf_t b = { 0 };
    if (get(url, &b, "application/xml")) {
        const char *p = b.d;
        while ((p = strstr(p, "<subtitle>")) != NULL) {
            const char *e = strstr(p, "</subtitle>");
            if (!e) break;
            char pid[32] = "", rel[200] = "", ti[160] = "";
            xml_val(p, e, "pid", pid, sizeof(pid));
            if (!xml_val(p, e, "release", rel, sizeof(rel))) xml_val(p, e, "releases", rel, sizeof(rel));
            xml_val(p, e, "title", ti, sizeof(ti));
            if (pid[0]) {
                char d[200]; snprintf(d, sizeof(d), "https://www.podnapisi.net/subtitles/%s/download", pid);
                add(rel[0] ? rel : ti[0] ? ti : q, d, lang, 1);
            }
            p = e;
        }
    }
    free(b.d);
}

static int net_ready(void)
{
    if (net_up() < 0) { snprintf(err, sizeof(err), "network"); return 0; }
    if (!curl_ok) { curl_global_init(CURL_GLOBAL_ALL); curl_ok = 1; }
    return 1;
}

static void search_thread(void *a)
{
    (void)a;
    nres = 0; err[0] = 0;
    if (net_ready()) {
        int se, ep; season_episode(v_name, &se, &ep);
        search_subdl(query, se, ep, "AR");
        search_podnapisi(query, se, ep, "ar");
        search_subdl(query, se, ep, "EN");
        search_podnapisi(query, se, ep, "en");
    }
    state = nres ? SUBDL_RESULTS : SUBDL_ERROR;
    if (!nres && !err[0]) snprintf(err, sizeof(err), "0");
    sysThreadExit(0);
}

static int is_sub_ext(const char *n)
{
    const char *e = strrchr(n, '.');
    return e && (!strcasecmp(e, ".srt") || !strcasecmp(e, ".ass") || !strcasecmp(e, ".ssa") || !strcasecmp(e, ".vtt"));
}

static int save_vfs(const char *path, const void *d, size_t n)
{
    vfs_file_t *f = vfs_open_write(path);
    if (!f) return 0;
    int ok = vfs_write(f, d, (uint32_t)n) == (int64_t)n;
    vfs_close(f);
    return ok;
}

static void download_thread(void *a)
{
    (void)a;
    err[0] = 0; result[0] = 0;
    const subres_t *r = &res[dl_index];
    buf_t b = { 0 };
    int ok = 0;
    if (net_ready() && get(r->url, &b, NULL) && b.n > 4) {
        char lang[4]; snprintf(lang, sizeof(lang), "%s", r->lang);
        for (char *p = lang; *p; p++) *p = tolower((unsigned char)*p);
        if (b.d[0] == 'P' && b.d[1] == 'K') {
            const char *tmp = "/dev_hdd0/tmp/fm_sub.zip";
            mkdir("/dev_hdd0/tmp", 0777);
            FILE *f = fopen(tmp, "wb");
            if (f) { fwrite(b.d, 1, b.n, f); fclose(f); }
            int ze = 0;
            struct zip *z = zip_open(tmp, 0, &ze);
            if (z) {
                int n = zip_get_num_files(z), best = -1; uint64_t bsz = 0;
                for (int i = 0; i < n; i++) {
                    struct zip_stat st;
                    if (zip_stat_index(z, i, 0, &st) || !st.name || !is_sub_ext(st.name)) continue;
                    /* prefer .srt, then the largest */
                    uint64_t score = st.size + (strcasestr(st.name, ".srt") ? (1ULL << 40) : 0);
                    if (score > bsz) { bsz = score; best = i; }
                }
                if (best >= 0) {
                    struct zip_stat st; zip_stat_index(z, best, 0, &st);
                    char *d = malloc((size_t)st.size + 1);
                    struct zip_file *zf = zip_fopen_index(z, best, 0);
                    if (d && zf && zip_fread(zf, d, st.size) == (ssize_t)st.size) {
                        const char *ext = strrchr(st.name, '.');
                        snprintf(result, sizeof(result), "%s.%s%s", dl_base, lang, ext);
                        ok = save_vfs(result, d, (size_t)st.size);
                        if (!ok) {
                            const char *bn = strrchr(dl_base, '/');
                            snprintf(result, sizeof(result), "/dev_hdd0/game/CRPK00001/USRDIR/subs");
                            mkdir(result, 0777);
                            snprintf(result, sizeof(result), "/dev_hdd0/game/CRPK00001/USRDIR/subs/%s.%s%s", bn ? bn + 1 : dl_base, lang, ext);
                            ok = save_vfs(result, d, (size_t)st.size);
                        }
                    }
                    if (zf) zip_fclose(zf);
                    free(d);
                } else snprintf(err, sizeof(err), "no .srt in archive");
                zip_close(z);
            } else snprintf(err, sizeof(err), "zip");
            unlink(tmp);
        } else {
            /* plain subtitle file */
            snprintf(result, sizeof(result), "%s.%s.srt", dl_base, lang);
            ok = save_vfs(result, b.d, b.n);
        }
    }
    free(b.d);
    state = ok ? SUBDL_READY : SUBDL_ERROR;
    sysThreadExit(0);
}

static void join_thr(void) { if (thr_on) { u64 rv; sysThreadJoin(thr, &rv); thr_on = 0; } }

void subdl_search(const char *video_name)
{
    if (state == SUBDL_SEARCHING || state == SUBDL_DOWNLOADING) return;
    join_thr();
    snprintf(v_name, sizeof(v_name), "%s", video_name);
    covers_video_query(video_name, query, sizeof(query));
    if (!query[0]) snprintf(query, sizeof(query), "%s", video_name);
    state = SUBDL_SEARCHING; nres = 0;
    if (sysThreadCreate(&thr, search_thread, NULL, 1200, 0x20000, THREAD_JOINABLE, "subsearch") == 0) thr_on = 1;
    else state = SUBDL_ERROR;
}

void subdl_download(int i, const char *base)
{
    if (i < 0 || i >= nres || state == SUBDL_SEARCHING || state == SUBDL_DOWNLOADING) return;
    join_thr();
    dl_index = i;
    snprintf(dl_base, sizeof(dl_base), "%s", base);
    state = SUBDL_DOWNLOADING;
    if (sysThreadCreate(&thr, download_thread, NULL, 1200, 0x20000, THREAD_JOINABLE, "subdl") == 0) thr_on = 1;
    else state = SUBDL_ERROR;
}

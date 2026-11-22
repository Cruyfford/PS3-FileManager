/* Automatic game covers.
 *  PS1 / PS2 : serial read from SYSTEM.CNF inside the ISO/BIN -> xlenore/psx-covers, xlenore/ps2-covers
 *  PS3       : TITLE_ID from PARAM.SFO (ISO or JB folder; JB folders use their own ICON0.PNG) -> GameTDB
 *  NES/SNES/GB/GBA/MD (+ PS1/PS2 fallback): libretro-thumbnails by game name
 * Downloads run on their own thread; results are cached on the HDD. */
#include "covers.h"
#include "vfs.h"
#include "ftp.h"
#include <curl/curl.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum { S_NONE, S_NES, S_SNES, S_GB, S_GBA, S_MD, S_PS1, S_PS2, S_PS3, S_DISC /* not yet known */, S_VIDEO };

static char cache_dir[512];
static volatile int enabled = 1, gen, quit;
static sys_ppu_thread_t thr;
static sys_mutex_t qlock;
static int started;

#define QMAX 64
typedef struct { int sys; char id[160]; char key[200]; } job_t;
static job_t q[QMAX];
static volatile int qn;

static const char *ext(const char *n) { const char *d = strrchr(n, '.'); return d ? d + 1 : ""; }

int covers_generation(void) { return gen; }
void covers_enable(int on) { enabled = on; }

static int rom_sys(const char *name)
{
    const char *e = ext(name);
    if (!strcasecmp(e, "nes") || !strcasecmp(e, "fds")) return S_NES;
    if (!strcasecmp(e, "smc") || !strcasecmp(e, "sfc") || !strcasecmp(e, "fig") || !strcasecmp(e, "swc")) return S_SNES;
    if (!strcasecmp(e, "gb") || !strcasecmp(e, "gbc")) return S_GB;
    if (!strcasecmp(e, "gba")) return S_GBA;
    if (!strcasecmp(e, "md") || !strcasecmp(e, "gen") || !strcasecmp(e, "smd")) return S_MD;
    if (!strcasecmp(e, "iso") || !strcasecmp(e, "bin") || !strcasecmp(e, "cue") || !strcasecmp(e, "img")) return S_DISC;
    if (!strcasecmp(e, "mp4") || !strcasecmp(e, "m4v") || !strcasecmp(e, "mov") || !strcasecmp(e, "mkv") || !strcasecmp(e, "avi") ||
        !strcasecmp(e, "wmv") || !strcasecmp(e, "m2ts") || !strcasecmp(e, "ts")) return S_VIDEO;
    return S_NONE;
}

int covers_candidate(const char *name, int is_dir)
{
    if (is_dir) return 1;          /* checked for PS3_GAME later */
    return rom_sys(name) != S_NONE;
}

/* ---------------------------------------------------------------- ISO 9660 */

typedef struct { vfs_file_t *f; uint32_t sector, offset; } iso_t;

static int iso_sector(iso_t *iso, uint32_t lba, uint8_t *buf)
{
    if (vfs_seek(iso->f, (uint64_t)lba * iso->sector + iso->offset) != 0) return 0;
    return vfs_read(iso->f, buf, 2048) == 2048;
}

static int iso_open(iso_t *iso, const char *path)
{
    iso->f = vfs_open_read(path);
    if (!iso->f) return 0;
    static const struct { uint32_t s, o; } fmts[] = { { 2048, 0 }, { 2352, 24 }, { 2352, 16 }, { 2336, 8 } };
    uint8_t b[2048];
    for (unsigned i = 0; i < 4; i++) {
        iso->sector = fmts[i].s; iso->offset = fmts[i].o;
        if (iso_sector(iso, 16, b) && b[0] == 1 && !memcmp(b + 1, "CD001", 5)) return 1;
    }
    vfs_close(iso->f);
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* find an entry in a directory extent; returns lba/size */
static int iso_find(iso_t *iso, uint32_t dir_lba, uint32_t dir_size, const char *name, uint32_t *lba, uint32_t *size)
{
    uint8_t b[2048];
    size_t nl = strlen(name);
    for (uint32_t s = 0; s < (dir_size + 2047) / 2048 && s < 64; s++) {
        if (!iso_sector(iso, dir_lba + s, b)) return 0;
        for (uint32_t o = 0; o < 2048;) {
            uint8_t len = b[o];
            if (!len) break;
            uint8_t fl = b[o + 32];
            const char *fn = (const char *)b + o + 33;
            if (fl >= nl && !strncasecmp(fn, name, nl) && (fl == nl || fn[nl] == ';')) {
                *lba = le32(b + o + 2); *size = le32(b + o + 10);
                return 1;
            }
            o += len;
        }
    }
    return 0;
}

static int iso_read_file(iso_t *iso, const char *dir, const char *file, uint8_t *out, uint32_t max, uint32_t *got)
{
    uint8_t pvd[2048];
    if (!iso_sector(iso, 16, pvd)) return 0;
    uint32_t lba = le32(pvd + 156 + 2), size = le32(pvd + 156 + 10);
    if (dir && !iso_find(iso, lba, size, dir, &lba, &size)) return 0;
    if (!iso_find(iso, lba, size, file, &lba, &size)) return 0;
    uint32_t n = size < max ? size : max, done = 0;
    uint8_t b[2048];
    while (done < n) {
        if (!iso_sector(iso, lba + done / 2048, b)) return 0;
        uint32_t k = n - done > 2048 ? 2048 : n - done;
        memcpy(out + done, b, k);
        done += k;
    }
    *got = n;
    return 1;
}

static int sfo_title_id(const uint8_t *s, uint32_t size, char *out, int n)
{
    if (size < 0x14 || memcmp(s, "\0PSF", 4)) return 0;
    uint32_t ko = le32(s + 8), dof = le32(s + 12), cnt = le32(s + 16);
    for (uint32_t i = 0; i < cnt && 0x14 + i * 16 + 16 <= size; i++) {
        const uint8_t *e = s + 0x14 + i * 16;
        uint32_t k = ko + (e[0] | e[1] << 8), len = le32(e + 4), d = dof + le32(e + 12);
        if (k >= size || d + len > size || strcmp((const char *)s + k, "TITLE_ID")) continue;
        snprintf(out, n, "%.*s", (int)(len < 15 ? len : 15), (const char *)s + d);
        return strlen(out) == 9;
    }
    return 0;
}

/* identify a disc image: S_PS1/S_PS2 with serial, or S_PS3 with title id */
static int disc_identify(const char *path, char *id, int n)
{
    char p[1400];
    snprintf(p, sizeof(p), "%s", path);
    /* .cue -> first FILE "track.bin" next to it */
    if (!strcasecmp(ext(path), "cue")) {
        vfs_file_t *f = vfs_open_read(path);
        if (!f) return S_NONE;
        char cue[2048] = { 0 };
        vfs_read(f, cue, sizeof(cue) - 1);
        vfs_close(f);
        char *q1 = strstr(cue, "FILE \"");
        if (!q1) return S_NONE;
        q1 += 6;
        char *q2 = strchr(q1, '"');
        if (!q2) return S_NONE;
        *q2 = 0;
        char *sl = strrchr(p, '/');
        if (sl) snprintf(sl + 1, sizeof(p) - (sl + 1 - p), "%s", q1);
        else snprintf(p, sizeof(p), "%s", q1);
    }
    iso_t iso;
    if (!iso_open(&iso, p)) return S_NONE;
    uint8_t buf[4096];
    uint32_t got = 0;
    int sys = S_NONE;
    if (iso_read_file(&iso, "PS3_GAME", "PARAM.SFO", buf, sizeof(buf), &got) && sfo_title_id(buf, got, id, n)) sys = S_PS3;
    else if (iso_read_file(&iso, NULL, "SYSTEM.CNF", buf, sizeof(buf) - 1, &got)) {
        buf[got] = 0;
        char *b = (char *)buf;
        int ps2 = strstr(b, "BOOT2") != NULL;
        char *c = strstr(b, "cdrom0:\\");
        if (c) c += 8; else if ((c = strstr(b, "cdrom:\\"))) c += 7;
        if (!c) { char *c2 = strstr(b, "cdrom:"); if (c2) c = c2 + 6; }
        if (c) {
            while (*c == '\\') c++;
            char ser[32]; int k = 0;
            for (; *c && *c != ';' && *c != '\r' && *c != '\n' && k < 20; c++) {
                if (*c == '_') ser[k++] = '-';
                else if (*c != '.') ser[k++] = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
            }
            ser[k] = 0;
            if (k >= 9) { snprintf(id, n, "%s", ser); sys = ps2 ? S_PS2 : S_PS1; }
        }
    }
    vfs_close(iso.f);
    return sys;
}

/* ---------------------------------------------------------------- download */

typedef struct { uint8_t *data; size_t len, cap; } mem_t;

static size_t wr_cb(void *ptr, size_t sz, size_t nm, void *ud)
{
    mem_t *m = (mem_t *)ud;
    size_t n = sz * nm;
    if (m->len + n > 3 * 1024 * 1024) return 0;
    if (m->len + n > m->cap) {
        size_t nc = (m->len + n) * 2;
        uint8_t *d = (uint8_t *)realloc(m->data, nc);
        if (!d) return 0;
        m->data = d; m->cap = nc;
    }
    memcpy(m->data + m->len, ptr, n);
    m->len += n;
    return n;
}

static int abort_cb(void *u, double a, double b, double x, double y) { (void)u; (void)a; (void)b; (void)x; (void)y; return quit ? 1 : 0; }

static int fetch_raw(CURL *c, const char *url, mem_t *m, int need_image)
{
    if (quit) return 0;
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_PROGRESSFUNCTION, abort_cb);
    m->len = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, m);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "PS3-FileManager/1.0");
    if (curl_easy_perform(c) != CURLE_OK) return 0;
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200 || m->len < 64) return 0;
    if (!need_image) return 1;
    /* must be an image */
    return (m->data[0] == 0x89 && m->data[1] == 'P') || (m->data[0] == 0xFF && m->data[1] == 0xD8);
}

static int fetch(CURL *c, const char *url, mem_t *m) { return fetch_raw(c, url, m, 1); }

/* ---------------------------------------------------------------- movies */

/* "The.Dark.Knight.2008.1080p.BluRay.x264.mp4" -> query "The Dark Knight 2008" */

/* "[SubsPlease] Oshi no Ko - 05 (1080p) [A1B2].mkv", "Oshi.no.Ko.S02E03.mkv", "WITanime.com Oshi no Ko EP 05.mp4",
 * "One Piece 1071.mp4" -> "Oshi no Ko" / "One Piece". Movies keep their year. */
static void video_query(const char *name, char *out, int n)
{
    char t[300];
    int k = 0;
    int depth = 0;
    const char *e = strrchr(name, '.');
    int len = e ? (int)(e - name) : (int)strlen(name);
    for (int i = 0; i < len && k < (int)sizeof(t) - 1; i++) {
        char c = name[i];
        if (c == '[' || c == '(' || c == '{') { depth++; continue; }
        if (c == ']' || c == ')' || c == '}') { if (depth) depth--; t[k++] = ' '; continue; }
        if (depth) continue;
        t[k++] = (c == '.' || c == '_' || c == '+') ? ' ' : c;
    }
    t[k] = 0;
    static const char *stop[] = { "1080p", "720p", "2160p", "480p", "360p", "4k", "bluray", "blu-ray", "brrip", "bdrip", "webrip", "web-dl", "web",
        "hdrip", "dvdrip", "x264", "x265", "h264", "h265", "hevc", "aac", "hdtv", "yify", "yts", "remux", "ep", "episode", "eps", "end", "final",
        "\xd8\xa7\xd9\x84\xd8\xad\xd9\x84\xd9\x82\xd8\xa9", "\xd8\xad\xd9\x84\xd9\x82\xd8\xa9", "-", "|", NULL };
    char *words[64]; int nw = 0;
    char *save = NULL;
    for (char *w = strtok_r(t, " ", &save); w && nw < 64; w = strtok_r(NULL, " ", &save)) words[nw++] = w;
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < nw; i++) {
        char *w = words[i];
        int l = strlen(w);
        /* site tags like WITanime.com were split by the dot: drop "com"/"net"/"org" and the word before */
        if (i + 1 < nw && (!strcasecmp(words[i + 1], "com") || !strcasecmp(words[i + 1], "net") || !strcasecmp(words[i + 1], "org") || !strcasecmp(words[i + 1], "tv"))) { i++; continue; }
        if (strcasestr(w, "anime") && (strcasestr(w, "wit") || strcasestr(w, "blkom") || strcasestr(w, "4up") || strcasestr(w, "slayer") || strcasestr(w, "cloud"))) continue;
        int is_stop = 0;
        for (int j = 0; stop[j]; j++) if (!strcasecmp(w, stop[j])) is_stop = 1;
        if (is_stop) { if (o) break; else continue; }
        /* S01E05 / S2 / E05 / 05 / 05v2 / 1071 : the title ends here */
        int digits = 0; for (int q = 0; q < l; q++) if (w[q] >= '0' && w[q] <= '9') digits++;
        if ((w[0] == 'S' || w[0] == 's') && l >= 2 && w[1] >= '0' && w[1] <= '9') { if (o) break; else continue; }
        if ((w[0] == 'E' || w[0] == 'e') && l >= 2 && w[1] >= '0' && w[1] <= '9') { if (o) break; else continue; }
        int year = l == 4 && digits == 4 && ((w[0] == '1' && w[1] == '9') || (w[0] == '2' && w[1] == '0'));
        if (digits && digits >= l - 2 && !year && o) break;
        o += snprintf(out + o, n - o, "%s%s", o ? " " : "", w);
        if (year && i > 0) break;
        if (o >= n - 1) break;
    }
}

void covers_video_query(const char *name, char *out, int n) { video_query(name, out, n); }

static void norm_title(const char *in, char *out, int n)
{
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k < n - 1; p++) {
        if (*p >= 'A' && *p <= 'Z') out[k++] = *p + 32;
        else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p >= 0x80) out[k++] = *p;
    }
    out[k] = 0;
}

/* anime cover via the Jikan (MyAnimeList) API; only accepted when a title really matches the file name */
static int try_anime(CURL *c, const char *query, mem_t *m)
{
    char url[1024], nq[300];
    char *eq = curl_easy_escape(c, query, 0);
    snprintf(url, sizeof(url), "https://api.jikan.moe/v4/anime?q=%s&limit=8", eq);
    curl_free(eq);
    if (!fetch_raw(c, url, m, 0) || !m->len) return 0;
    m->data[m->len < m->cap ? m->len : m->len - 1] = 0;
    norm_title(query, nq, sizeof(nq));
    if (strlen(nq) < 2) return 0;
    char *js = (char *)m->data;
    char img[900] = "";
    /* walk results: each starts with "mal_id" */
    for (char *it = strstr(js, "{\"mal_id\":"); it && !img[0]; ) {
        char *next = strstr(it + 10, "{\"mal_id\":");
        /* titles inside this item */
        int match = 0;
        for (char *tp = strstr(it, "\"title\":\""); tp && (!next || tp < next) && !match; tp = strstr(tp + 9, "\"title\":\"")) {
            char tt[300]; int k = 0;
            for (char *q = tp + 9; *q && *q != '"' && k < 299; q++) { if (*q == '\\') { q++; if (!*q) break; } tt[k++] = *q; }
            tt[k] = 0;
            char nt[300]; norm_title(tt, nt, sizeof(nt));
            if (nt[0] && (!strcmp(nt, nq) || (strlen(nq) >= 4 && strstr(nt, nq)) || (strlen(nt) >= 4 && strstr(nq, nt)))) match = 1;
        }
        if (match) {
            char *iu = strstr(it, "\"large_image_url\":\"");
            if (!iu || (next && iu > next)) iu = strstr(it, "\"image_url\":\"");
            if (iu && (!next || iu < next)) {
                iu = strchr(iu + 2, ':') + 2;
                int k = 0;
                for (; *iu && *iu != '"' && k < (int)sizeof(img) - 1; iu++) { if (*iu == '\\') continue; img[k++] = *iu; }
                img[k] = 0;
            }
        }
        it = next;
    }
    if (strncmp(img, "https://", 8)) return 0;
    return fetch(c, img, m);
}

/* poster via Wikipedia: search "<title> film", take the page's lead image */
static int try_wikipedia(CURL *c, const char *query, mem_t *m)
{
    char q[300], url[1024];
    snprintf(q, sizeof(q), "%s film", query);
    char *eq = curl_easy_escape(c, q, 0);
    snprintf(url, sizeof(url), "https://en.wikipedia.org/w/api.php?action=query&format=json&generator=search&gsrlimit=1&gsrsearch=%s&prop=pageimages&piprop=thumbnail&pithumbsize=500", eq);
    curl_free(eq);
    if (!fetch_raw(c, url, m, 0)) return 0;
    /* find "source":"https://upload.wikimedia.org/..." */
    m->data[m->len < m->cap ? m->len : m->len - 1] = 0;
    char *src = strstr((char *)m->data, "\"source\":\"");
    if (!src) return 0;
    src += 10;
    char img[900]; int k = 0;
    for (; *src && *src != '"' && k < (int)sizeof(img) - 1; src++) { if (*src == '\\') continue; img[k++] = *src; }
    img[k] = 0;
    if (strncmp(img, "https://", 8)) return 0;
    return fetch(c, img, m);
}

/* cover art stored inside an MP4: moov/udta/meta/ilst/covr/data */
static int mp4_find(vfs_file_t *f, uint64_t start, uint64_t end, const char *type, uint64_t *pos, uint64_t *len)
{
    uint64_t p = start;
    uint8_t h[16];
    while (p + 8 <= end) {
        if (vfs_seek(f, p) != 0 || vfs_read(f, h, 8) != 8) return 0;
        uint64_t sz = ((uint32_t)h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
        uint32_t hl = 8;
        if (sz == 1) { if (vfs_read(f, h + 8, 8) != 8) return 0; sz = ((uint64_t)(((uint32_t)h[8] << 24) | (h[9] << 16) | (h[10] << 8) | h[11]) << 32) | (((uint32_t)h[12] << 24) | (h[13] << 16) | (h[14] << 8) | h[15]); hl = 16; }
        else if (sz == 0) sz = end - p;
        if (sz < hl) return 0;
        if (!memcmp(h + 4, type, 4)) { *pos = p + hl; *len = sz - hl; return 1; }
        p += sz;
    }
    return 0;
}

static int mp4_cover(const char *path, uint8_t **out, uint32_t *outlen)
{
    vfs_file_t *f = vfs_open_read(path);
    if (!f) return 0;
    uint64_t end = vfs_size(f), p, l, ok = 0;
    if (mp4_find(f, 0, end, "moov", &p, &l) && mp4_find(f, p, p + l, "udta", &p, &l) && mp4_find(f, p, p + l, "meta", &p, &l)) {
        p += 4; l -= 4;   /* meta is a full box */
        if (mp4_find(f, p, p + l, "ilst", &p, &l) && mp4_find(f, p, p + l, "covr", &p, &l) && mp4_find(f, p, p + l, "data", &p, &l) && l > 8 && l < 4 * 1024 * 1024) {
            p += 8; l -= 8;   /* type + locale */
            uint8_t *b = (uint8_t *)malloc((size_t)l);
            if (b && vfs_seek(f, p) == 0 && vfs_read(f, b, (uint32_t)l) == (int64_t)l && ((b[0] == 0xFF && b[1] == 0xD8) || (b[0] == 0x89 && b[1] == 'P'))) {
                *out = b; *outlen = (uint32_t)l; ok = 1;
            } else free(b);
        }
    }
    vfs_close(f);
    return (int)ok;
}

static void libretro_name(const char *in, char *out, int n)
{
    int o = 0;
    for (const char *p = in; *p && o < n - 1; p++) out[o++] = strchr("&*/:`<>?\\|\"", *p) ? '_' : *p;
    out[o] = 0;
}

static void strip_tags(const char *in, char *out, int n)
{
    int o = 0, depth = 0;
    for (const char *p = in; *p && o < n - 1; p++) {
        if (*p == '(' || *p == '[') { depth++; continue; }
        if (*p == ')' || *p == ']') { if (depth) depth--; continue; }
        if (!depth) out[o++] = *p;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static const char *libretro_system(int sys)
{
    switch (sys) {
        case S_NES: return "Nintendo - Nintendo Entertainment System";
        case S_SNES: return "Nintendo - Super Nintendo Entertainment System";
        case S_GB: return "Nintendo - Game Boy";
        case S_GBA: return "Nintendo - Game Boy Advance";
        case S_MD: return "Sega - Mega Drive - Genesis";
        case S_PS1: return "Sony - PlayStation";
        case S_PS2: return "Sony - PlayStation 2";
    }
    return NULL;
}

static int try_libretro(CURL *c, int sys, const char *game, mem_t *m)
{
    const char *sysname = libretro_system(sys);
    if (!sysname) return 0;
    char clean[200], base[200], name[260], url[1024];
    strip_tags(game, base, sizeof(base));
    const char *cands[6]; char buf[6][260]; int nc = 0;
    snprintf(buf[nc], 260, "%s", game); cands[nc] = buf[nc]; nc++;
    const char *regions[] = { " (USA)", " (Europe)", " (Japan)", " (World)", "" };
    for (int r = 0; r < 5 && nc < 6; r++) { snprintf(buf[nc], 260, "%s%s", base, regions[r]); cands[nc] = buf[nc]; nc++; }
    char *esys = curl_easy_escape(c, sysname, 0);
    int ok = 0;
    for (int i = 0; i < nc && !ok && !quit; i++) {
        libretro_name(cands[i], clean, sizeof(clean));
        snprintf(name, sizeof(name), "%s.png", clean);
        char *ename = curl_easy_escape(c, name, 0);
        snprintf(url, sizeof(url), "http://thumbnails.libretro.com/%s/Named_Boxarts/%s", esys, ename);
        curl_free(ename);
        ok = fetch(c, url, m);
    }
    curl_free(esys);
    return ok;
}

static int download(CURL *c, const job_t *j, mem_t *m)
{
    char url[512];
    if (j->sys == S_VIDEO) {
        if (try_anime(c, j->id, m)) return 1;
        m->len = 0;
        return try_wikipedia(c, j->id, m);
    }
    if (j->sys == S_PS2 || j->sys == S_PS1) {
        if (j->id[4] == '-') {   /* serial */
            snprintf(url, sizeof(url), "https://raw.githubusercontent.com/xlenore/%s/main/covers/default/%s.jpg",
                     j->sys == S_PS2 ? "ps2-covers" : "psx-covers", j->id);
            if (fetch(c, url, m)) return 1;
            return 0;
        }
        return try_libretro(c, j->sys, j->id, m);
    }
    if (j->sys == S_PS3) {
        const char *reg[] = { "US", "EN", "JA", "FR", "DE" };
        for (int r = 0; r < 5; r++) {
            snprintf(url, sizeof(url), "https://art.gametdb.com/ps3/cover/%s/%s.jpg", reg[r], j->id);
            if (fetch(c, url, m)) return 1;
        }
        return 0;
    }
    return try_libretro(c, j->sys, j->id, m);
}

static void save(const char *path, const void *d, size_t n)
{
    vfs_file_t *f = vfs_open_write(path);
    if (f) { vfs_write(f, d, (uint32_t)n); vfs_close(f); }
}

static void worker(void *arg)
{
    (void)arg;
    CURL *c = NULL;
    mem_t m = { NULL, 0, 0 };
    while (!quit) {
        if (!qn || !enabled) { usleep(50000); continue; }
        sysMutexLock(qlock, 0);
        job_t j = q[0];
        memmove(q, q + 1, sizeof(job_t) * (qn - 1));
        qn--;
        sysMutexUnlock(qlock);

        if (!c) {
            if (net_up() < 0) { usleep(500000); continue; }
            curl_global_init(CURL_GLOBAL_ALL);
            c = curl_easy_init();
            if (!c) continue;
        }
        char out[700];
        if (download(c, &j, &m)) {
            snprintf(out, sizeof(out), "%s/%s.%s", cache_dir, j.key, m.data[0] == 0x89 ? "png" : "jpg");
            save(out, m.data, m.len);
        } else {
            snprintf(out, sizeof(out), "%s/%s.none", cache_dir, j.key);
            save(out, "", 0);
        }
        gen++;
    }
    if (c) curl_easy_cleanup(c);
    free(m.data);
    sysThreadExit(0);
}

void covers_init(const char *dir)
{
    snprintf(cache_dir, sizeof(cache_dir), "%s", dir);
    vfs_mkdir(cache_dir);
    sys_mutex_attr_t a;
    memset(&a, 0, sizeof(a));
    a.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    a.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    a.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
    a.attr_adaptive = SYS_MUTEX_ATTR_ADAPTIVE;
    strcpy(a.name, "covq");
    sysMutexCreate(&qlock, &a);
    quit = 0;
    started = sysThreadCreate(&thr, worker, NULL, 1700, 0x20000, THREAD_JOINABLE, "covers") == 0;
}

/* for app exit: abort the current download and don't wait for the worker */
void covers_stop_async(void) { quit = 1; }

void covers_shutdown(void)
{
    if (!started) return;
    quit = 1;
    u64 rv;
    sysThreadJoin(thr, &rv);
    started = 0;
}

static void sanitize(const char *in, char *out, int n)
{
    int o = 0;
    for (const char *p = in; *p && o < n - 1; p++) {
        char ch = *p;
        out[o++] = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-') ? ch : '_';
    }
    out[o] = 0;
}

static int cached(const char *key, char *out, int n)
{
    const char *exts[] = { "jpg", "png" };
    for (int i = 0; i < 2; i++) {
        snprintf(out, n, "%s/%s.%s", cache_dir, key, exts[i]);
        if (vfs_exists(out)) return CV_CACHED;
    }
    char miss[700];
    snprintf(miss, sizeof(miss), "%s/%s.none", cache_dir, key);
    if (vfs_exists(miss)) return CV_MISSING;
    return CV_NONE;
}

static int queue(int sys, const char *id, const char *key)
{
    if (!enabled || !started) return CV_MISSING;
    sysMutexLock(qlock, 0);
    for (int i = 0; i < qn; i++) if (!strcmp(q[i].key, key)) { sysMutexUnlock(qlock); return CV_PENDING; }
    if (qn >= QMAX) { sysMutexUnlock(qlock); return CV_PENDING; }
    job_t *j = &q[qn];
    j->sys = sys;
    snprintf(j->id, sizeof(j->id), "%s", id);
    snprintf(j->key, sizeof(j->key), "%s", key);
    qn++;
    sysMutexUnlock(qlock);
    return CV_PENDING;
}

int covers_lookup(const char *full, const char *name, int is_dir, char *out, int n)
{
    char id[160], key[200], tmp[160];
    if (is_dir) {
        /* PS3 JB folder: use its own icon */
        snprintf(out, n, "%s/PS3_GAME/ICON0.PNG", full);
        return vfs_exists(out) ? CV_CACHED : CV_NONE;
    }
    int sys = rom_sys(name);
    if (sys == S_NONE) return CV_NONE;

    if (sys == S_DISC) {
        /* cache the identification by file name so the ISO isn't re-read every time */
        sanitize(name, tmp, sizeof(tmp));
        snprintf(key, sizeof(key), "f_%s", tmp);
        int st = cached(key, out, n);
        if (st != CV_NONE) return st;
        sys = disc_identify(full, id, sizeof(id));
        if (sys == S_NONE) {
            /* PS1/PS2 games without a readable serial: try by name later */
            if (strstr(full, "PS2ISO")) { sys = S_PS2; }
            else if (strstr(full, "PSXISO") || !strcasecmp(ext(name), "cue")) { sys = S_PS1; }
            else return CV_NONE;
            snprintf(id, sizeof(id), "%.*s", (int)(strrchr(name, '.') ? strrchr(name, '.') - name : (long)strlen(name)), name);
        }
        return queue(sys, id, key);
    }

    if (sys == S_VIDEO) {
        sanitize(name, tmp, sizeof(tmp));
        snprintf(key, sizeof(key), "v2_%s", tmp);
        int st = cached(key, out, n);
        if (st != CV_NONE) return st;
        /* 1) cover embedded in the file */
        const char *e = ext(name);
        if (!strcasecmp(e, "mp4") || !strcasecmp(e, "m4v") || !strcasecmp(e, "mov")) {
            uint8_t *img = NULL; uint32_t il = 0;
            if (mp4_cover(full, &img, &il)) {
                snprintf(out, n, "%s/%s.%s", cache_dir, key, img[0] == 0x89 ? "png" : "jpg");
                save(out, img, il);
                free(img);
                return CV_CACHED;
            }
        }
        /* 2) movie poster online */
        video_query(name, id, sizeof(id));
        if (!id[0]) return CV_NONE;
        return queue(S_VIDEO, id, key);
    }

    /* cartridge ROM: libretro name = file name without extension */
    snprintf(id, sizeof(id), "%.*s", (int)(strrchr(name, '.') - name), name);
    sanitize(name, tmp, sizeof(tmp));
    snprintf(key, sizeof(key), "r%d_%s", sys, tmp);
    int st = cached(key, out, n);
    if (st != CV_NONE) return st;
    return queue(sys, id, key);
}


/* Catalog artwork keyed by the catalog's PS3 title ID. Images are optional:
 * use local covers/<ID>.jpg or .png first, otherwise let the background
 * cover worker try the GameTDB service and cache the result. */
int covers_catalog_lookup(const char *title_id, char *out_path, int n)
{
    if (!out_path || n <= 0) return CV_NONE;
    out_path[0] = 0;
    if (!title_id) return CV_NONE;
    size_t len = strlen(title_id);
    if (len < 5 || len > 16) return CV_NONE;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)title_id[i];
        if (!(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') &&
            !(ch >= 'a' && ch <= 'z') && ch != '-') return CV_NONE;
    }
    char key[200];
    snprintf(key, sizeof(key), "cat_%s", title_id);
    char path[700];
    snprintf(path, sizeof(path), "%s/%s.jpg", cache_dir, title_id);
    if (vfs_exists(path)) { snprintf(out_path, n, "%s", path); return CV_CACHED; }
    snprintf(path, sizeof(path), "%s/%s.png", cache_dir, title_id);
    if (vfs_exists(path)) { snprintf(out_path, n, "%s", path); return CV_CACHED; }
    /* A user-provided image takes priority over previous .none cache entries. */
    int st = cached(key, out_path, n);
    if (st != CV_NONE) return st;
    return queue(S_PS3, title_id, key);
}

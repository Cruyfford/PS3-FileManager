/* Subtitle parsing: SRT, WebVTT, ASS/SSA; text normalized to UTF-8 (UTF-16 BOM and Windows-1256 handled) */
#include "subs.h"
#include "vfs.h"
#include "arabic.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

static const uint16_t cp1256[128] = { 0x20AC, 0x067E, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0679, 0x2039, 0x0152, 0x0686, 0x0698, 0x0688, 0x06AF, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x06A9, 0x2122, 0x0691, 0x203A, 0x0153, 0x200C, 0x200D, 0x06BA, 0x00A0, 0x060C, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x06BE, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF, 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x061B, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x061F, 0x06C1, 0x0621, 0x0622, 0x0623, 0x0624, 0x0625, 0x0626, 0x0627, 0x0628, 0x0629, 0x062A, 0x062B, 0x062C, 0x062D, 0x062E, 0x062F, 0x0630, 0x0631, 0x0632, 0x0633, 0x0634, 0x0635, 0x0636, 0x00D7, 0x0637, 0x0638, 0x0639, 0x063A, 0x0640, 0x0641, 0x0642, 0x0643, 0x00E0, 0x0644, 0x00E2, 0x0645, 0x0646, 0x0647, 0x0648, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x0649, 0x064A, 0x00EE, 0x00EF, 0x064B, 0x064C, 0x064D, 0x064E, 0x00F4, 0x064F, 0x0650, 0x00F7, 0x0651, 0x00F9, 0x0652, 0x00FB, 0x00FC, 0x200E, 0x200F, 0x06D2 };

static int valid_utf8(const unsigned char *s, int n)
{
    int i = 0, bad = 0, multi = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        int k = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || i + k >= n + 0) { if (k < 0) bad++; i++; continue; }
        int ok = 1;
        for (int j = 1; j <= k; j++) if (i + j >= n || (s[i + j] & 0xC0) != 0x80) ok = 0;
        if (!ok) bad++; else multi++;
        i += ok ? k + 1 : 1;
    }
    return bad == 0 || bad * 20 < multi;
}

static int put_utf8(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = c; return 1; }
    if (c < 0x800) { o[0] = 0xC0 | (c >> 6); o[1] = 0x80 | (c & 63); return 2; }
    if (c < 0x10000) { o[0] = 0xE0 | (c >> 12); o[1] = 0x80 | ((c >> 6) & 63); o[2] = 0x80 | (c & 63); return 3; }
    o[0] = 0xF0 | (c >> 18); o[1] = 0x80 | ((c >> 12) & 63); o[2] = 0x80 | ((c >> 6) & 63); o[3] = 0x80 | (c & 63); return 4;
}

/* returns malloc'd UTF-8 */
static char *to_utf8(const unsigned char *s, int n)
{
    char *o;
    int k = 0;
    if (n >= 2 && ((s[0] == 0xFF && s[1] == 0xFE) || (s[0] == 0xFE && s[1] == 0xFF))) {
        int le = s[0] == 0xFF;
        o = (char *)malloc(n * 2 + 4);
        if (!o) return NULL;
        for (int i = 2; i + 1 < n; i += 2) {
            uint32_t c = le ? (s[i] | (s[i + 1] << 8)) : ((s[i] << 8) | s[i + 1]);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) { uint32_t c2 = le ? (s[i + 2] | (s[i + 3] << 8)) : ((s[i + 2] << 8) | s[i + 3]); c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i += 2; }
            k += put_utf8(c, o + k);
        }
        o[k] = 0;
        return o;
    }
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { s += 3; n -= 3; }
    if (valid_utf8(s, n)) { o = (char *)malloc(n + 1); if (!o) return NULL; memcpy(o, s, n); o[n] = 0; return o; }
    o = (char *)malloc(n * 3 + 1);
    if (!o) return NULL;
    for (int i = 0; i < n; i++) k += put_utf8(s[i] < 0x80 ? s[i] : cp1256[s[i] - 0x80], o + k);
    o[k] = 0;
    return o;
}

/* strip <tags>, {\ass} overrides, convert \N \n \h */
static char *clean_text(const char *in, int ass)
{
    int n = strlen(in);
    char *o = (char *)malloc(n + 2);
    if (!o) return NULL;
    int k = 0;
    for (int i = 0; i < n; i++) {
        char c = in[i];
        if (c == '{' && (ass || in[i + 1] == '\\')) { const char *e = strchr(in + i, '}'); if (e) { i = e - in; continue; } }
        if (c == '<') { const char *e = strchr(in + i, '>'); if (e && e - (in + i) < 64) { i = e - in; continue; } }
        if (c == '\\' && (in[i + 1] == 'N' || in[i + 1] == 'n')) { o[k++] = '\n'; i++; continue; }
        if (c == '\\' && in[i + 1] == 'h') { o[k++] = ' '; i++; continue; }
        if (c == '\r') continue;
        if (!strncmp(in + i, "&amp;", 5)) { o[k++] = '&'; i += 4; continue; }
        if (!strncmp(in + i, "&lt;", 4)) { o[k++] = '<'; i += 3; continue; }
        if (!strncmp(in + i, "&gt;", 4)) { o[k++] = '>'; i += 3; continue; }
        if (!strncmp(in + i, "&nbsp;", 6)) { o[k++] = ' '; i += 5; continue; }
        o[k++] = c;
    }
    while (k && (o[k - 1] == '\n' || o[k - 1] == ' ')) k--;
    o[k] = 0;
    return o;
}

static void add_cue(sublist_t *l, double s, double e, char *text)
{
    if (!text || !text[0] || e <= s) { free(text); return; }
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 512;
        subcue_t *x = (subcue_t *)realloc(l->c, sizeof(subcue_t) * nc);
        if (!x) { free(text); return; }
        l->c = x; l->cap = nc;
    }
    l->c[l->n++] = (subcue_t){ s, e, text };
    l->sorted = 0;
}

/* "00:01:02,345" / "01:02.345" / ASS "0:01:02.34" */
static int parse_time(const char *p, double *t, const char **end)
{
    int v[4] = { 0 }, n = 0;
    const char *q = p;
    while (*q == ' ') q++;
    double frac = 0;
    while (n < 3) {
        if (!isdigit((unsigned char)*q)) return 0;
        int x = 0;
        while (isdigit((unsigned char)*q)) x = x * 10 + (*q++ - '0');
        v[n++] = x;
        if (*q == ':') { q++; continue; }
        if (*q == ',' || *q == '.') {
            q++;
            double div = 1;
            while (isdigit((unsigned char)*q)) { frac = frac * 10 + (*q++ - '0'); div *= 10; }
            frac /= div;
        }
        break;
    }
    if (n == 3) *t = v[0] * 3600 + v[1] * 60 + v[2] + frac;
    else if (n == 2) *t = v[0] * 60 + v[1] + frac;
    else return 0;
    if (end) *end = q;
    return 1;
}

static void parse_srt_vtt(sublist_t *l, char *txt)
{
    char *line = txt;
    double s = 0, e = 0;
    int in_cue = 0;
    char buf[2048]; int bl = 0;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        int len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        const char *arrow = strstr(line, "-->");
        if (arrow) {
            if (in_cue && bl) { buf[bl] = 0; add_cue(l, s, e, clean_text(buf, 0)); }
            const char *q;
            in_cue = parse_time(line, &s, &q) && parse_time(arrow + 3, &e, NULL);
            bl = 0;
        } else if (!len) {
            if (in_cue && bl) { buf[bl] = 0; add_cue(l, s, e, clean_text(buf, 0)); }
            in_cue = 0; bl = 0;
        } else if (in_cue) {
            if (bl && bl < (int)sizeof(buf) - 2) buf[bl++] = '\n';
            int k = len < (int)sizeof(buf) - bl - 1 ? len : (int)sizeof(buf) - bl - 1;
            memcpy(buf + bl, line, k); bl += k;
        }
        line = nl ? nl + 1 : NULL;
    }
    if (in_cue && bl) { buf[bl] = 0; add_cue(l, s, e, clean_text(buf, 0)); }
}

static void parse_ass(sublist_t *l, char *txt)
{
    char *line = txt;
    int textcol = 9, startcol = 1, endcol = 2;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        int len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (!strncasecmp(line, "Format:", 7)) {
            /* column positions of Start/End/Text */
            int col = 0;
            for (char *p = line + 7; *p; col++) {
                while (*p == ' ') p++;
                if (!strncasecmp(p, "Start", 5)) startcol = col;
                else if (!strncasecmp(p, "End", 3)) endcol = col;
                else if (!strncasecmp(p, "Text", 4)) textcol = col;
                char *c = strchr(p, ','); if (!c) break; p = c + 1;
            }
        } else if (!strncasecmp(line, "Dialogue:", 9)) {
            char *p = line + 9;
            double s = -1, e = -1;
            int col = 0;
            while (*p && col < textcol) {
                if (col == startcol) parse_time(p, &s, NULL);
                if (col == endcol) parse_time(p, &e, NULL);
                char *c = strchr(p, ','); if (!c) { p = NULL; break; }
                p = c + 1; col++;
            }
            if (p && s >= 0 && e > s) add_cue(l, s, e, clean_text(p, 1));
        }
        line = nl ? nl + 1 : NULL;
    }
}

static int cmp_cue(const void *a, const void *b) { double d = ((const subcue_t *)a)->start - ((const subcue_t *)b)->start; return d < 0 ? -1 : d > 0; }

int subs_load_file(const char *path, sublist_t *out)
{
    vfs_file_t *f = vfs_open_read(path);
    if (!f) return 0;
    uint64_t sz = vfs_size(f);
    if (sz == 0 || sz > 16 * 1024 * 1024) { vfs_close(f); return 0; }
    unsigned char *raw = (unsigned char *)malloc(sz + 1);
    if (!raw) { vfs_close(f); return 0; }
    int64_t r = vfs_read(f, raw, (uint32_t)sz);
    vfs_close(f);
    if (r <= 0) { free(raw); return 0; }
    char *txt = to_utf8(raw, (int)r);
    free(raw);
    if (!txt) return 0;
    if (strstr(txt, "[Events]") || strstr(txt, "Dialogue:")) parse_ass(out, txt);
    else parse_srt_vtt(out, txt);
    free(txt);
    if (out->n) { qsort(out->c, out->n, sizeof(subcue_t), cmp_cue); out->sorted = 1; }
    return out->n > 0;
}

void subs_add_packet(sublist_t *l, double start, double dur, const char *data, int len, int ass)
{
    if (len <= 0 || start < 0) return;
    for (int i = l->n - 1; i >= 0 && i >= l->n - 64; i--) if (l->c[i].start > start - 0.001 && l->c[i].start < start + 0.001) return;   /* seen */
    char *u = to_utf8((const unsigned char *)data, len);
    if (!u) return;
    const char *t = u;
    if (ass) { int commas = 0; for (const char *p = u; *p; p++) if (*p == ',' && ++commas == 8) { t = p + 1; break; } }
    char *c = clean_text(t, ass);
    free(u);
    add_cue(l, start, start + (dur > 0 ? dur : 4.0), c);
}

const char *subs_at(sublist_t *l, double t)
{
    static char out[2048];
    out[0] = 0;
    if (!l->n) return out;
    if (!l->sorted) { qsort(l->c, l->n, sizeof(subcue_t), cmp_cue); l->sorted = 1; }
    /* last cue that starts before t */
    int lo = 0, hi = l->n - 1, idx = -1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (l->c[mid].start <= t) { idx = mid; lo = mid + 1; } else hi = mid - 1; }
    int k = 0;
    for (int i = idx; i >= 0 && i > idx - 12; i--) {
        if (l->c[i].end <= t) continue;
        int n = strlen(l->c[i].text);
        if (k + n + 2 >= (int)sizeof(out)) break;
        if (k) { memmove(out + n + 1, out, k + 1); memcpy(out, l->c[i].text, n); out[n] = '\n'; k += n + 1; }
        else { memcpy(out, l->c[i].text, n + 1); k = n; }
    }
    return out;
}

void subs_free(sublist_t *l)
{
    for (int i = 0; i < l->n; i++) free(l->c[i].text);
    free(l->c);
    memset(l, 0, sizeof(*l));
}

int subs_find_external(const char *video, char out[][1024], int max)
{
    char dir[1024], base[512];
    snprintf(dir, sizeof(dir), "%s", video);
    char *sl = strrchr(dir, '/');
    if (!sl) return 0;
    snprintf(base, sizeof(base), "%s", sl + 1);
    sl[sl == dir ? 1 : 0] = 0;
    if (sl == dir) dir[1] = 0;
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    int bl = strlen(base);
    static vfs_entry_t ents[1024];
    int n = vfs_list(dir, ents, 1024);
    int k = 0;
    /* exact-name matches first, then any subtitle file in the folder */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n && k < max; i++) {
            if (ents[i].is_dir) continue;
            const char *e = strrchr(ents[i].name, '.');
            if (!e || (strcasecmp(e, ".srt") && strcasecmp(e, ".ass") && strcasecmp(e, ".ssa") && strcasecmp(e, ".vtt"))) continue;
            int match = !strncasecmp(ents[i].name, base, bl);
            if ((pass == 0) != match) continue;
            snprintf(out[k++], 1024, "%s/%s", dir, ents[i].name);
        }
    return k;
}

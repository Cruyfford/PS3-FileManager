/* Minimal Arabic shaping + bidi reordering for UI strings.
 * Contextual forms come from the Unicode Presentation Forms-B block,
 * so any font with those glyphs (DejaVu Sans) renders connected Arabic. */
#include "arabic.h"
#include <string.h>

int utf8_decode(const char *s, uint32_t *out, int max)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < max) {
        uint32_t c = *p++;
        int extra = 0;
        if (c >= 0xF0)      { c &= 0x07; extra = 3; }
        else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
        else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
        else if (c >= 0x80) { continue; }
        while (extra-- > 0) {
            if ((*p & 0xC0) != 0x80) { c = '?'; break; }
            c = (c << 6) | (*p++ & 0x3F);
        }
        out[n++] = c;
    }
    return n;
}

int utf8_encode(const uint32_t *cp, int n, char *out, int max)
{
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = cp[i];
        if (c < 0x80) { if (o + 1 >= max) break; out[o++] = (char)c; }
        else if (c < 0x800) { if (o + 2 >= max) break; out[o++] = 0xC0 | (c >> 6); out[o++] = 0x80 | (c & 0x3F); }
        else if (c < 0x10000) { if (o + 3 >= max) break; out[o++] = 0xE0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
        else { if (o + 4 >= max) break; out[o++] = 0xF0 | (c >> 18); out[o++] = 0x80 | ((c >> 12) & 0x3F); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
    }
    out[o] = 0;
    return o;
}

/* forms: isolated, final, initial, medial (0 = none). type: 'D' dual, 'R' right-joining, 'N' none */
typedef struct { uint16_t cp, iso, fin, ini, med; char type; } aform_t;

static const aform_t forms[] = {
    {0x0621,0xFE80,0,0,0,'N'},
    {0x0622,0xFE81,0xFE82,0,0,'R'}, {0x0623,0xFE83,0xFE84,0,0,'R'},
    {0x0624,0xFE85,0xFE86,0,0,'R'}, {0x0625,0xFE87,0xFE88,0,0,'R'},
    {0x0626,0xFE89,0xFE8A,0xFE8B,0xFE8C,'D'}, {0x0627,0xFE8D,0xFE8E,0,0,'R'},
    {0x0628,0xFE8F,0xFE90,0xFE91,0xFE92,'D'}, {0x0629,0xFE93,0xFE94,0,0,'R'},
    {0x062A,0xFE95,0xFE96,0xFE97,0xFE98,'D'}, {0x062B,0xFE99,0xFE9A,0xFE9B,0xFE9C,'D'},
    {0x062C,0xFE9D,0xFE9E,0xFE9F,0xFEA0,'D'}, {0x062D,0xFEA1,0xFEA2,0xFEA3,0xFEA4,'D'},
    {0x062E,0xFEA5,0xFEA6,0xFEA7,0xFEA8,'D'}, {0x062F,0xFEA9,0xFEAA,0,0,'R'},
    {0x0630,0xFEAB,0xFEAC,0,0,'R'}, {0x0631,0xFEAD,0xFEAE,0,0,'R'},
    {0x0632,0xFEAF,0xFEB0,0,0,'R'}, {0x0633,0xFEB1,0xFEB2,0xFEB3,0xFEB4,'D'},
    {0x0634,0xFEB5,0xFEB6,0xFEB7,0xFEB8,'D'}, {0x0635,0xFEB9,0xFEBA,0xFEBB,0xFEBC,'D'},
    {0x0636,0xFEBD,0xFEBE,0xFEBF,0xFEC0,'D'}, {0x0637,0xFEC1,0xFEC2,0xFEC3,0xFEC4,'D'},
    {0x0638,0xFEC5,0xFEC6,0xFEC7,0xFEC8,'D'}, {0x0639,0xFEC9,0xFECA,0xFECB,0xFECC,'D'},
    {0x063A,0xFECD,0xFECE,0xFECF,0xFED0,'D'}, {0x0640,0x0640,0x0640,0x0640,0x0640,'D'},
    {0x0641,0xFED1,0xFED2,0xFED3,0xFED4,'D'}, {0x0642,0xFED5,0xFED6,0xFED7,0xFED8,'D'},
    {0x0643,0xFED9,0xFEDA,0xFEDB,0xFEDC,'D'}, {0x0644,0xFEDD,0xFEDE,0xFEDF,0xFEE0,'D'},
    {0x0645,0xFEE1,0xFEE2,0xFEE3,0xFEE4,'D'}, {0x0646,0xFEE5,0xFEE6,0xFEE7,0xFEE8,'D'},
    {0x0647,0xFEE9,0xFEEA,0xFEEB,0xFEEC,'D'}, {0x0648,0xFEED,0xFEEE,0,0,'R'},
    {0x0649,0xFEEF,0xFEF0,0,0,'R'}, {0x064A,0xFEF1,0xFEF2,0xFEF3,0xFEF4,'D'},
    {0x067E,0xFB56,0xFB57,0xFB58,0xFB59,'D'}, {0x0686,0xFB7A,0xFB7B,0xFB7C,0xFB7D,'D'},
    {0x0698,0xFB8A,0xFB8B,0,0,'R'}, {0x06A9,0xFB8E,0xFB8F,0xFB90,0xFB91,'D'},
    {0x06AF,0xFB92,0xFB93,0xFB94,0xFB95,'D'}, {0x06CC,0xFBFC,0xFBFD,0xFBFE,0xFBFF,'D'},
};

static const aform_t *find_form(uint32_t c)
{
    for (unsigned i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
        if (forms[i].cp == c) return &forms[i];
    return 0;
}

static int is_transparent(uint32_t c)
{
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670 || (c >= 0x06D6 && c <= 0x06ED);
}

int is_rtl_cp(uint32_t c)
{
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

int has_rtl(const char *s)
{
    uint32_t cp[512];
    int n = utf8_decode(s, cp, 512);
    for (int i = 0; i < n; i++) if (is_rtl_cp(cp[i])) return 1;
    return 0;
}

static int lam_alef(uint32_t alef, int final)
{
    switch (alef) {
        case 0x0622: return final ? 0xFEF6 : 0xFEF5;
        case 0x0623: return final ? 0xFEF8 : 0xFEF7;
        case 0x0625: return final ? 0xFEFA : 0xFEF9;
        case 0x0627: return final ? 0xFEFC : 0xFEFB;
    }
    return 0;
}

static int shape(const uint32_t *in, int n, uint32_t *out)
{
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = in[i];
        if (is_transparent(c)) continue;   /* harakat are dropped: the renderer has no mark positioning */
        const aform_t *f = find_form(c);
        if (!f) { out[o++] = c; continue; }

        int jp = 0, jn = 0;
        for (int k = i - 1; k >= 0; k--) {
            if (is_transparent(in[k])) continue;
            const aform_t *pf = find_form(in[k]);
            jp = (pf && pf->type == 'D' && f->type != 'N');
            break;
        }
        int nexti = -1;
        for (int k = i + 1; k < n; k++) {
            if (is_transparent(in[k])) continue;
            nexti = k;
            const aform_t *nf = find_form(in[k]);
            jn = (nf && nf->type != 'N' && f->type == 'D');
            break;
        }

        if (c == 0x0644 && nexti == i + 1) {
            int lig = lam_alef(in[i + 1], jp);
            if (lig) { out[o++] = lig; i++; continue; }
        }

        uint32_t g = f->iso;
        if (jp && jn && f->med) g = f->med;
        else if (jp && f->fin) g = f->fin;
        else if (jn && f->ini) g = f->ini;
        out[o++] = g;
    }
    return o;
}

static int dir_of(uint32_t c)
{
    if (is_rtl_cp(c)) return 1;                 /* R */
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= 0xC0 && c < 0x590)) return 0;     /* L */
    return -1;                                  /* neutral */
}

static uint32_t mirror(uint32_t c)
{
    switch (c) {
        case '(': return ')'; case ')': return '(';
        case '[': return ']'; case ']': return '[';
        case '<': return '>'; case '>': return '<';
        case '{': return '}'; case '}': return '{';
    }
    return c;
}

int bidi_visual(const uint32_t *in, int n, uint32_t *out)
{
    static uint32_t sh[1024];
    static signed char d[1024];
    if (n > 1024) n = 1024;
    int any_rtl = 0;
    for (int i = 0; i < n; i++) if (is_rtl_cp(in[i])) { any_rtl = 1; break; }
    if (!any_rtl) { memcpy(out, in, n * sizeof(uint32_t)); return n; }

    int m = shape(in, n, sh);

    int base = 0;
    for (int i = 0; i < m; i++) { int t = dir_of(sh[i]); if (t >= 0) { base = t; break; } }

    for (int i = 0; i < m; i++) d[i] = (signed char)dir_of(sh[i]);
    for (int i = 0; i < m; i++) {
        if (d[i] >= 0) continue;
        int prev = base, next = base;
        for (int k = i - 1; k >= 0; k--) if (dir_of(sh[k]) >= 0) { prev = dir_of(sh[k]); break; }
        for (int k = i + 1; k < m; k++) if (dir_of(sh[k]) >= 0) { next = dir_of(sh[k]); break; }
        d[i] = (prev == next) ? prev : base;
    }

    /* collect runs */
    int rs[512], re[512], rc = 0;
    for (int i = 0; i < m && rc < 512; ) {
        int j = i;
        while (j < m && d[j] == d[i]) j++;
        rs[rc] = i; re[rc] = j; rc++;
        i = j;
    }

    int o = 0;
    for (int r = 0; r < rc; r++) {
        int idx = base ? (rc - 1 - r) : r;
        int s = rs[idx], e = re[idx];
        if (d[s] == 1) for (int k = e - 1; k >= s; k--) out[o++] = mirror(sh[k]);
        else for (int k = s; k < e; k++) out[o++] = sh[k];
    }
    return o;
}

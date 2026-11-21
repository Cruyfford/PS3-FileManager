/* Nova — web browser for PS3 by CRUYFFORD. Tabs, bookmarks, forms, images, background downloads. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/process.h>
#include <sysutil/sysutil.h>
#include <ya2d/ya2d.h>
#include "gfx.h"
#include "arabic.h"
#include "osk.h"
#include "vfs.h"
#include "net.h"
#include "doc.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define APP_DIR   "/dev_hdd0/game/CRBR00001/USRDIR"
#define CFG_FILE  APP_DIR "/settings.cfg"
#define BM_FILE   APP_DIR "/bookmarks.txt"
#define HOME_URL  "nova:home"
#define SEARCH    "https://html.duckduckgo.com/html/?q="

/* ============================ input */
#define B_SELECT 0x00010000
#define B_L3     0x00020000
#define B_R3     0x00040000
#define B_START  0x00080000
#define B_UP     0x00100000
#define B_RIGHT  0x00200000
#define B_DOWN   0x00400000
#define B_LEFT   0x00800000
#define B_L2     0x00000001
#define B_R2     0x00000002
#define B_L1     0x00000004
#define B_R1     0x00000008
#define B_TRI    0x00000010
#define B_CIR    0x00000020
#define B_CRO    0x00000040
#define B_SQU    0x00000080

static uint32_t in_pressed, in_active, prev_down;
static int hold_frames;
static uint32_t B_OK = B_CRO, B_CANCEL = B_CIR;
static int ok_icon = BTN_CROSS, cancel_icon = BTN_CIRCLE;
static float stick_ry;
static volatile int quit_req;

static void read_input(void)
{
    ya2d_controlsRead();
    uint32_t d = 0;
    memcpy(&d, &ya2d_paddata[0].button[2], sizeof(uint32_t));
    if (ya2d_paddata[0].ANA_L_V < 0x30) d |= B_UP;
    if (ya2d_paddata[0].ANA_L_V > 0xD0) d |= B_DOWN;
    if (ya2d_paddata[0].ANA_L_H < 0x30) d |= B_LEFT;
    if (ya2d_paddata[0].ANA_L_H > 0xD0) d |= B_RIGHT;
    int ry = (int)ya2d_paddata[0].ANA_R_V - 0x80;
    stick_ry = (ry > 24 || ry < -24) ? ry / 128.0f : 0;
    in_pressed = d & ~prev_down;
    in_active = in_pressed;
    if (d && d == prev_down) {
        hold_frames++;
        uint32_t rep = d & (B_UP | B_DOWN | B_LEFT | B_RIGHT | B_L2 | B_R2);
        if (hold_frames > 30) in_active = rep;
        else if (hold_frames > 14 && hold_frames % 3 == 0) in_active = rep;
    } else hold_frames = 0;
    prev_down = d;
}

/* ============================ strings */
static int g_lang;
enum { S_SEARCH_HINT, S_WHERE, S_NEWTAB, S_CLOSETAB, S_BACK, S_FORWARD, S_RELOAD, S_HOME, S_ADDBM, S_BOOKMARKS, S_DOWNLOADS, S_LANG, S_EXIT,
       S_OPEN, S_DOWNLOAD, S_MENU, S_TABS, S_SCROLL, S_ADDRESS, S_LOADING, S_ERROR, S_RETRY, S_SAVE_TO, S_START_DL, S_CANCEL, S_DL_STARTED,
       S_DL_DONE, S_DL_FAIL, S_NO_DL, S_REMOVE, S_STOP, S_BM_ADDED, S_BM_REMOVED, S_NO_BM, S_EDIT, S_TOGGLE, S_SUBMIT, S_CHOOSE, S_ADD,
       S_DONE, S_FAILED, S_CANCELLED, S_CLOSE, S_TABFULL, S_PKG_HINT, S_UNKNOWN_SIZE, S_COUNT };
static const char *en_s[S_COUNT] = { "Search or type a web address", "Where to today?", "New tab", "Close tab", "Back", "Forward", "Reload", "Home",
    "Add bookmark", "Bookmarks", "Downloads", "Language: English", "Exit Nova", "Open", "Download", "Menu", "Tabs", "Scroll", "Address",
    "Loading…", "Couldn't open this page", "Retry", "Save to", "Download", "Cancel", "Download started", "Download finished", "Download failed",
    "No downloads yet", "Remove", "Stop", "Bookmark added", "Bookmark removed", "No bookmarks yet — press △ on a page to add one", "Edit",
    "Toggle", "Submit", "Choose", "Add", "Done", "Failed", "Cancelled", "Close", "Close a tab first (6 max)",
    "PKG files saved to /dev_hdd0/packages appear in File Manager and XMB → Install Package Files", "size unknown" };
static const char *ar_s[S_COUNT] = { "ابحث أو اكتب عنوان موقع", "إلى أين اليوم؟", "تبويب جديد", "إغلاق التبويب", "رجوع", "تقدّم", "إعادة تحميل", "الرئيسية",
    "إضافة إشارة مرجعية", "الإشارات المرجعية", "التنزيلات", "اللغة: العربية", "خروج من Nova", "فتح", "تحميل", "القائمة", "التبويبات", "تمرير", "العنوان",
    "جارٍ التحميل…", "تعذر فتح هذه الصفحة", "إعادة المحاولة", "حفظ في", "تحميل", "إلغاء", "بدأ التحميل", "اكتمل التحميل", "فشل التحميل",
    "لا توجد تنزيلات بعد", "إزالة", "إيقاف", "أضيفت الإشارة", "حُذفت الإشارة", "لا توجد إشارات — اضغط △ في أي صفحة لإضافتها", "تعديل",
    "تبديل", "إرسال", "اختيار", "إضافة", "مكتمل", "فشل", "أُلغي", "إغلاق", "أغلق تبويباً أولاً (6 كحد أقصى)",
    "ملفات PKG المحفوظة في /dev_hdd0/packages تظهر في مدير الملفات وفي XMB ← Install Package Files", "الحجم غير معروف" };
static const char *T(int i) { return (g_lang ? ar_s : en_s)[i]; }
#define RTL() (g_lang == 1)

/* ============================ colors */
#define C_BG     RGBA8(10, 9, 24, 255)
#define C_TEXT   RGBA8(236, 235, 255, 255)
#define C_DIM    RGBA8(158, 155, 199, 255)
#define C_ACC    RGBA8(127, 119, 221, 255)
#define C_ACC_L  RGBA8(175, 169, 236, 255)
#define C_TEAL   RGBA8(93, 202, 165, 255)
#define C_GLASS  RGBA8(255, 255, 255, 18)
#define C_EDGE   RGBA8(255, 255, 255, 30)

/* ============================ settings & bookmarks */
static void load_settings(void)
{
    FILE *f = fopen(CFG_FILE, "rb");
    int l = -1;
    if (f) { if (fread(&l, sizeof(l), 1, f) != 1) l = -1; fclose(f); }
    if (l < 0) { f = fopen("/dev_hdd0/game/CRPK00001/USRDIR/settings.cfg", "rb"); if (f) { if (fread(&l, sizeof(l), 1, f) != 1) l = 0; fclose(f); } }
    g_lang = (l == 1);
}
static void save_settings(void) { FILE *f = fopen(CFG_FILE, "wb"); if (f) { fwrite(&g_lang, sizeof(g_lang), 1, f); fclose(f); } }

#define BM_MAX 48
typedef struct { char url[URL_MAX]; char title[128]; } bm_t;
static bm_t bms[BM_MAX];
static int nbm;

static void bm_save(void)
{
    FILE *f = fopen(BM_FILE, "wb");
    if (!f) return;
    for (int i = 0; i < nbm; i++) fprintf(f, "%s\t%s\n", bms[i].url, bms[i].title);
    fclose(f);
}
static void bm_load(void)
{
    FILE *f = fopen(BM_FILE, "rb");
    nbm = 0;
    if (!f) return;
    char line[URL_MAX + 200];
    while (nbm < BM_MAX && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        snprintf(bms[nbm].url, URL_MAX, "%s", line);
        snprintf(bms[nbm].title, 128, "%s", tab + 1);
        nbm++;
    }
    fclose(f);
}

/* ============================ toast */
static char toast[300];
static uint64_t toast_until;
static void show_toast(const char *s) { snprintf(toast, sizeof(toast), "%s", s); toast_until = gfx_frame() + 170; }

/* ============================ tabs */
#define TAB_MAX 6
#define HIST_MAX 24
#define VIEW_X 20
#define VIEW_Y 84
#define VIEW_W (SCR_W - 40)
#define VIEW_H (SCR_H - VIEW_Y - 44)

typedef struct { char url[URL_MAX]; float scroll; } hist_t;
typedef struct { int kind; int id; int run; } unit_t;   /* kind 0 link, 1 field */

typedef struct {
    doc_t *doc;
    fetch_t fetch;
    char url[URL_MAX];
    char req_url[URL_MAX];
    hist_t hist[HIST_MAX];
    int hn, hp;
    int push;
    float restore;
    float scroll, scroll_draw;
    unit_t *units; int nunits, capunits;
    int focus;
    char err[200];
    int home;
} tab_t;

static tab_t tabs[TAB_MAX];
static int ntabs, cur;

/* home page focus: 0 = search bar, 1.. = tiles */
static int home_sel;
typedef struct { const char *title, *url; uint32_t color; const char *glyph; } tile_t;
static const tile_t def_tiles[] = {
    { "DuckDuckGo", "https://html.duckduckgo.com/html/", RGBA8(216, 90, 48, 255), "D" },
    { "GitHub", "https://github.com", RGBA8(52, 52, 60, 255), "G" },
    { "PSX-Place", "https://www.psx-place.com", RGBA8(24, 95, 165, 255), "P" },
    { "Wikipedia", "https://ar.m.wikipedia.org", RGBA8(153, 53, 86, 255), "W" },
};
#define NDEF 4

static void build_units(tab_t *t)
{
    t->nunits = 0;
    doc_t *d = t->doc;
    if (!d) return;
    char *seen_l = calloc(d->nlinks + 1, 1), *seen_f = calloc(d->nfields + 1, 1);
    for (int i = 0; i < d->nruns; i++) {
        run_t *r = &d->runs[i];
        int kind = -1, id = -1;
        if (r->field >= 0 && (r->kind == RUN_FIELD || r->kind == RUN_BUTTON)) { kind = 1; id = r->field; if (seen_f[id]) continue; seen_f[id] = 1; }
        else if (r->link >= 0) { kind = 0; id = r->link; if (seen_l[id]) continue; seen_l[id] = 1; }
        else continue;
        if (t->nunits >= t->capunits) { int nc = t->capunits ? t->capunits * 2 : 128; unit_t *nu = realloc(t->units, sizeof(unit_t) * nc); if (!nu) break; t->units = nu; t->capunits = nc; }
        t->units[t->nunits++] = (unit_t){ kind, id, i };
    }
    free(seen_l); free(seen_f);
    if (t->focus >= t->nunits) t->focus = t->nunits - 1;
}

static void tab_layout(tab_t *t)
{
    if (!t->doc) return;
    doc_layout(t->doc, VIEW_W);
    build_units(t);
}

static void tab_go(tab_t *t, const char *url, const char *post, int push)
{
    t->err[0] = 0;
    if (!strcmp(url, HOME_URL)) {
        fetch_reset(&t->fetch);
        if (push && t->hn > 0 && !strcmp(t->hist[t->hp].url, HOME_URL)) push = 0;
        if (push || t->hn == 0) {
            if (t->hn) t->hist[t->hp].scroll = t->scroll;
            t->hp = t->hn ? t->hp + 1 : 0;
            if (t->hp >= HIST_MAX) { memmove(t->hist, t->hist + 1, sizeof(hist_t) * (HIST_MAX - 1)); t->hp = HIST_MAX - 1; }
            snprintf(t->hist[t->hp].url, URL_MAX, "%s", HOME_URL);
            t->hn = t->hp + 1;
        }
        t->home = 1;
        snprintf(t->url, URL_MAX, "%s", HOME_URL);
        home_sel = 0;
        return;
    }
    snprintf(t->req_url, URL_MAX, "%s", url);
    t->push = push;
    fetch_start(&t->fetch, url, post);
}

static void go_input(tab_t *t, const char *in)
{
    while (*in == ' ') in++;
    if (!*in) return;
    char url[URL_MAX];
    int has_space = strchr(in, ' ') != NULL;
    if (!strncasecmp(in, "http://", 7) || !strncasecmp(in, "https://", 8)) snprintf(url, sizeof(url), "%s", in);
    else if (!has_space && strchr(in, '.') && strlen(in) > 3) snprintf(url, sizeof(url), "https://%s", in);
    else { char q[1200]; url_encode(in, q, sizeof(q)); snprintf(url, sizeof(url), "%s%s", SEARCH, q); }
    tab_go(t, url, NULL, 1);
}

static tab_t *new_tab(const char *url)
{
    if (ntabs >= TAB_MAX) { show_toast(T(S_TABFULL)); return NULL; }
    tab_t *t = &tabs[ntabs];
    memset(t, 0, sizeof(*t));
    t->focus = -1;
    cur = ntabs++;
    tab_go(t, url ? url : HOME_URL, NULL, 1);
    return t;
}

static void close_tab(int i)
{
    tab_t *t = &tabs[i];
    fetch_reset(&t->fetch);
    doc_free(t->doc);
    free(t->units);
    memmove(&tabs[i], &tabs[i + 1], sizeof(tab_t) * (ntabs - i - 1));
    ntabs--;
    if (ntabs == 0) { new_tab(HOME_URL); return; }
    if (cur >= ntabs) cur = ntabs - 1;
}

static void tab_back(tab_t *t)
{
    if (t->fetch.state == F_RUNNING) { fetch_reset(&t->fetch); return; }
    if (t->hp <= 0) return;
    t->hist[t->hp].scroll = t->scroll;
    t->hp--;
    t->restore = t->hist[t->hp].scroll;
    if (!strcmp(t->hist[t->hp].url, HOME_URL)) { t->home = 1; snprintf(t->url, URL_MAX, "%s", HOME_URL); return; }
    tab_go(t, t->hist[t->hp].url, NULL, 0);
}

static void tab_forward(tab_t *t)
{
    if (t->hp + 1 >= t->hn) return;
    t->hist[t->hp].scroll = t->scroll;
    t->hp++;
    t->restore = t->hist[t->hp].scroll;
    if (!strcmp(t->hist[t->hp].url, HOME_URL)) { t->home = 1; snprintf(t->url, URL_MAX, "%s", HOME_URL); return; }
    tab_go(t, t->hist[t->hp].url, NULL, 0);
}

/* ============================ downloads prompt */
static int ui;   /* 0 browse, 1 menu, 2 dl prompt, 3 downloads list, 4 bookmarks, 5 osk */
enum { UI_BROWSE = 0, UI_MENU, UI_DLPROMPT, UI_DOWNLOADS, UI_BOOKMARKS, UI_OSK };
static int osk_purpose, osk_field;   /* 1 address, 2 page field */

static char dlp_url[URL_MAX], dlp_name[256];
typedef struct { char path[512]; char label[96]; } dest_t;
static dest_t dests[16];
static int ndest, dest_sel;
static volume_t vols[24];
static int dl_list_sel, bm_sel;

static int ends_with(const char *s, const char *e) { size_t a = strlen(s), b = strlen(e); return a >= b && !strcasecmp(s + a - b, e); }

static void open_dl_prompt(const char *url, const char *disposition)
{
    snprintf(dlp_url, sizeof(dlp_url), "%s", url);
    url_filename(url, disposition, dlp_name, sizeof(dlp_name));
    ndest = 0;
    int is_pkg = ends_with(dlp_name, ".pkg");
    snprintf(dests[ndest].path, 512, "/dev_hdd0/packages"); snprintf(dests[ndest].label, 96, "HDD · packages"); ndest++;
    snprintf(dests[ndest].path, 512, "/dev_hdd0/downloads"); snprintf(dests[ndest].label, 96, "HDD · downloads"); ndest++;
    int nv = vfs_scan(vols, 24);
    for (int i = 0; i < nv && ndest < 16; i++) {
        if (vols[i].kind != 1 || !vols[i].writable) continue;
        snprintf(dests[ndest].path, 512, "%s", vols[i].root);
        size_t l = strlen(dests[ndest].path);
        if (l && dests[ndest].path[l - 1] == '/') dests[ndest].path[l - 1] = 0;
        snprintf(dests[ndest].label, 96, "%s (%s)", vols[i].label, vols[i].fs_name);
        ndest++;
    }
    dest_sel = is_pkg ? 0 : 1;
    ui = UI_DLPROMPT;
}

static void start_download(void)
{
    dest_t *d = &dests[dest_sel];
    vfs_mkdir(d->path);
    char path[1024], base[256], ext[64] = "";
    snprintf(base, sizeof(base), "%s", dlp_name);
    char *dot = strrchr(base, '.');
    if (dot && strlen(dot) < sizeof(ext)) { snprintf(ext, sizeof(ext), "%s", dot); *dot = 0; }
    snprintf(path, sizeof(path), "%s/%s%s", d->path, base, ext);
    char name[300];
    snprintf(name, sizeof(name), "%s%s", base, ext);
    for (int k = 1; vfs_exists(path) && k < 100; k++) { snprintf(path, sizeof(path), "%s/%s (%d)%s", d->path, base, k, ext); snprintf(name, sizeof(name), "%s (%d)%s", base, k, ext); }
    if (dl_start(dlp_url, path, name) >= 0) show_toast(T(S_DL_STARTED)); else show_toast(T(S_DL_FAIL));
    ui = UI_BROWSE;
}

/* ============================ fetch completion */
static void poll_tab(tab_t *t)
{
    fetch_t *f = &t->fetch;
    if (f->state == F_RUNNING || f->state == F_IDLE) return;
    fetch_wait(f);
    if (f->state == F_DOWNLOAD) {
        open_dl_prompt(f->url, f->disposition);
        if (t->push == 0 && t->hp + 1 < t->hn) { }   /* history unchanged */
    } else if (f->state == F_DONE && f->code < 400 && f->data) {
        int plain = !strncasecmp(f->ctype, "text/plain", 10);
        doc_t *d = doc_parse(f->data, f->len, f->url, plain, f->ctype);
        if (d) {
            img_cancel_all();
            doc_free(t->doc);
            t->doc = d;
            t->home = 0;
            t->focus = -1;
            snprintf(t->url, URL_MAX, "%s", f->url);
            if (t->push) {
                if (t->hn) t->hist[t->hp].scroll = t->scroll;
                t->hp = t->hn ? t->hp + 1 : 0;
                if (t->hp >= HIST_MAX) { memmove(t->hist, t->hist + 1, sizeof(hist_t) * (HIST_MAX - 1)); t->hp = HIST_MAX - 1; }
                t->hn = t->hp + 1;
                t->restore = 0;
            }
            snprintf(t->hist[t->hp].url, URL_MAX, "%s", f->url);
            tab_layout(t);
            t->scroll = t->scroll_draw = t->restore;
            t->restore = 0;
            for (int i = 0; i < d->nimgs && i < 40; i++) img_request(d->id, i, d->imgs[i].src);
            for (int i = 40; i < d->nimgs; i++) d->imgs[i].state = 2;
        }
    } else if (f->state == F_DONE || f->state == F_ERROR) {
        if (f->state == F_ERROR) snprintf(t->err, sizeof(t->err), "%s", f->err);
        else snprintf(t->err, sizeof(t->err), "HTTP %ld", f->code);
        doc_free(t->doc); t->doc = NULL; t->home = 0;
        snprintf(t->url, URL_MAX, "%s", f->url);
        if (t->push) { t->hp = t->hn ? t->hp + 1 : 0; if (t->hp >= HIST_MAX) t->hp = HIST_MAX - 1; t->hn = t->hp + 1; snprintf(t->hist[t->hp].url, URL_MAX, "%s", f->url); }
    }
    free(f->data); f->data = NULL; f->len = f->cap = 0;
    f->state = F_IDLE;
}

static void poll_images(void)
{
    uint32_t id; int idx; void *tex;
    int changed[TAB_MAX] = { 0 };
    while (img_poll(&id, &idx, &tex)) {
        int used = 0;
        for (int i = 0; i < ntabs; i++) {
            doc_t *d = tabs[i].doc;
            if (d && d->id == id && idx < d->nimgs) { d->imgs[idx].tex = tex; d->imgs[idx].state = tex ? 1 : 2; changed[i] = 1; used = 1; break; }
        }
        if (!used && tex) gfx_free_texture((texture_t)tex);
    }
    for (int i = 0; i < ntabs; i++) if (changed[i]) {
        tab_t *t = &tabs[i];
        /* keep the focused item in place across the re-layout */
        float anchor = -1;
        if (t->focus >= 0 && t->focus < t->nunits) anchor = t->doc->runs[t->units[t->focus].run].y - t->scroll;
        int fk = t->focus >= 0 && t->focus < t->nunits ? t->units[t->focus].kind : -1, fid = fk >= 0 ? t->units[t->focus].id : -1;
        tab_layout(t);
        if (fk >= 0) for (int u = 0; u < t->nunits; u++) if (t->units[u].kind == fk && t->units[u].id == fid) { t->focus = u; if (anchor >= 0) t->scroll = t->scroll_draw = t->doc->runs[t->units[u].run].y - anchor; break; }
    }
}

/* ============================ page interaction */
static float max_scroll(tab_t *t) { float m = t->doc ? t->doc->height - VIEW_H : 0; return m > 0 ? m : 0; }

static void clamp_scroll(tab_t *t)
{
    float m = max_scroll(t);
    if (t->scroll > m) t->scroll = m;
    if (t->scroll < 0) t->scroll = 0;
}

static float unit_y(tab_t *t, int u) { return t->doc->runs[t->units[u].run].y; }

static void ensure_visible(tab_t *t)
{
    if (t->focus < 0 || t->focus >= t->nunits) return;
    run_t *r = &t->doc->runs[t->units[t->focus].run];
    if (r->y < t->scroll + 10) t->scroll = r->y - 40;
    if (r->y + r->h > t->scroll + VIEW_H - 10) t->scroll = r->y + r->h - VIEW_H + 40;
    clamp_scroll(t);
}

static void move_focus(tab_t *t, int dir)
{
    if (!t->doc) return;
    int n = t->focus + dir;
    if (t->focus < 0) {
        /* first unit at or after the current scroll */
        n = -1;
        for (int u = 0; u < t->nunits; u++) if (unit_y(t, u) >= t->scroll) { n = u; break; }
        if (dir < 0 && n > 0) n--;
    }
    if (n >= 0 && n < t->nunits) {
        float y = unit_y(t, n);
        if (y > t->scroll + VIEW_H * 1.4f && dir > 0) { t->scroll += VIEW_H * 0.7f; clamp_scroll(t); return; }
        if (y < t->scroll - VIEW_H * 0.4f && dir < 0) { t->scroll -= VIEW_H * 0.7f; clamp_scroll(t); return; }
        t->focus = n;
        ensure_visible(t);
    } else {
        t->scroll += dir * VIEW_H * 0.7f;
        clamp_scroll(t);
    }
}

static void sync_focus_to_view(tab_t *t)
{
    if (!t->doc || t->focus < 0 || t->focus >= t->nunits) return;
    float y = unit_y(t, t->focus);
    if (y >= t->scroll && y < t->scroll + VIEW_H) return;
    t->focus = -1;
    for (int u = 0; u < t->nunits; u++) { float yy = unit_y(t, u); if (yy >= t->scroll && yy < t->scroll + VIEW_H) { t->focus = u; break; } }
}

static void activate(tab_t *t)
{
    if (!t->doc || t->focus < 0 || t->focus >= t->nunits) return;
    unit_t *u = &t->units[t->focus];
    doc_t *d = t->doc;
    if (u->kind == 0) { tab_go(t, d->links[u->id].href, NULL, 1); return; }
    field_t *f = &d->fields[u->id];
    switch (f->type) {
    case FLD_CHECK: f->checked = !f->checked; break;
    case FLD_RADIO:
        for (int i = 0; i < d->nfields; i++) if (d->fields[i].type == FLD_RADIO && d->fields[i].form == f->form && !strcmp(d->fields[i].name, f->name)) d->fields[i].checked = 0;
        f->checked = 1; break;
    case FLD_SELECT:
        if (f->nopt) { f->sel = (f->sel + 1) % f->nopt; snprintf(f->value, sizeof(f->value), "%s", f->opt_val[f->sel]); }
        break;
    case FLD_SUBMIT: {
        char url[URL_MAX]; char *post = NULL;
        doc_form_submit(d, f->form, u->id, url, sizeof(url), &post);
        tab_go(t, url, post, 1);
        free(post);
        break;
    }
    default:
        osk_purpose = 2; osk_field = u->id;
        if (osk_open(f->opt_txt[0] && f->opt_txt[0][0] ? f->opt_txt[0] : (f->name[0] ? f->name : T(S_EDIT)), f->value)) ui = UI_OSK;
        break;
    }
}

/* text field committed: submit its form when it is the only text input (search boxes) */
static void field_committed(tab_t *t, int fi)
{
    doc_t *d = t->doc;
    field_t *f = &d->fields[fi];
    if (f->type == FLD_TEXTAREA) return;
    int texts = 0, submit = -1;
    for (int i = 0; i < d->nfields; i++) {
        if (d->fields[i].form != f->form) continue;
        if (d->fields[i].type == FLD_TEXT || d->fields[i].type == FLD_PASSWORD) texts++;
        if (d->fields[i].type == FLD_SUBMIT && submit < 0) submit = i;
    }
    if (texts == 1 && f->form >= 0) {
        char url[URL_MAX]; char *post = NULL;
        doc_form_submit(d, f->form, submit, url, sizeof(url), &post);
        tab_go(t, url, post, 1);
        free(post);
    }
}

static void download_focused(tab_t *t)
{
    if (!t->doc || t->focus < 0 || t->focus >= t->nunits) return;
    unit_t *u = &t->units[t->focus];
    if (u->kind != 0) return;
    open_dl_prompt(t->doc->links[u->id].href, NULL);
}

/* ============================ drawing helpers */
static float approach(float v, float t, float k) { return v + (t - v) * k; }

static void draw_bg(void)
{
    gfx_rect(0, 0, SCR_W, SCR_H, C_BG);
    float t = gfx_frame() / 60.0f;
    gfx_glow(SCR_W - 120 + sinf(t * 0.2f) * 30, 30, 300, RGBA8(60, 52, 137, 90));
    gfx_glow(90 + cosf(t * 0.17f) * 30, SCR_H - 20, 260, RGBA8(8, 80, 65, 80));
}

static void draw_lock(float x, float y, uint32_t c)
{
    gfx_rrect_border(x + 2, y, 8, 8, 4, 1.6f, c);
    gfx_rrect(x, y + 5, 12, 9, 2, c);
}

static void draw_compass(float x, float y, float s)
{
    gfx_rrect(x, y, s, s, s * 0.3f, C_ACC);
    float cx = x + s / 2, cy = y + s / 2, r = s * 0.32f;
    gfx_circle(cx, cy, r + 1.5f, RGBA8(255, 255, 255, 255));
    gfx_circle(cx, cy, r - 0.5f, C_ACC);
    gfx_triangle(cx + r * 0.7f, cy - r * 0.7f, cx - r * 0.2f, cy - r * 0.2f, cx + r * 0.2f, cy + r * 0.2f, RGBA8(255, 255, 255, 255));
    gfx_triangle(cx - r * 0.7f, cy + r * 0.7f, cx - r * 0.2f, cy - r * 0.2f, cx + r * 0.2f, cy + r * 0.2f, RGBA8(255, 255, 255, 150));
}

typedef struct { int btn; const char *label; } hint_t;
static float btn_w(int b, float s) { if (b <= BTN_TRIANGLE) return s; return gfx_text_width(b == BTN_START ? "START" : b == BTN_SELECT ? "SELECT" : b == BTN_L1 ? "L1" : "R1", 10, 1) + 12; }

static void draw_hints(const hint_t *h, int n)
{
    float y = SCR_H - 32, s = 18;
    gfx_rect(0, SCR_H - 40, SCR_W, 40, RGBA8(6, 6, 16, 235));
    gfx_rect(0, SCR_H - 40, SCR_W, 1, RGBA8(255, 255, 255, 14));
    float total = 0;
    for (int i = 0; i < n; i++) total += btn_w(h[i].btn, s) + 6 + gfx_text_width(h[i].label, 12, 0) + 22;
    float x = RTL() ? SCR_W / 2 + total / 2 : SCR_W / 2 - total / 2;
    for (int i = 0; i < n; i++) {
        float lw = gfx_text_width(h[i].label, 12, 0), bw = btn_w(h[i].btn, s);
        if (RTL()) { gfx_button_icon(x - bw, y, h[i].btn, s); gfx_text(x - bw - 6, y + 2, h[i].label, 12, 0, C_DIM, ALIGN_RIGHT); x -= bw + 6 + lw + 22; }
        else { gfx_button_icon(x, y, h[i].btn, s); gfx_text(x + bw + 6, y + 2, h[i].label, 12, 0, C_DIM, ALIGN_LEFT); x += bw + 6 + lw + 22; }
    }
}

static void fmt_size(double b, char *o, int n)
{
    if (b < 1024) snprintf(o, n, "%.0f B", b);
    else if (b < 1048576) snprintf(o, n, "%.1f KB", b / 1024);
    else if (b < 1073741824.0) snprintf(o, n, "%.1f MB", b / 1048576);
    else snprintf(o, n, "%.2f GB", b / 1073741824.0);
}

static void modal_frame(float w, float h, float *ox, float *oy)
{
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA8(3, 3, 12, 170));
    float x = (SCR_W - w) / 2, y = (SCR_H - h) / 2 - 10;
    gfx_glow(SCR_W / 2, y + h / 2, w * 0.7f, RGBA8(90, 80, 220, 45));
    gfx_rrect(x, y, w, h, 22, RGBA8(24, 22, 50, 250));
    gfx_rrect_border(x, y, w, h, 22, 1.2f, RGBA8(255, 255, 255, 40));
    *ox = x; *oy = y;
}

/* ============================ header */
static void draw_header(tab_t *t)
{
    gfx_rect(0, 0, SCR_W, VIEW_Y - 6, RGBA8(10, 9, 24, 245));
    gfx_grad_v(0, VIEW_Y - 6, SCR_W, 8, RGBA8(10, 9, 24, 245), RGBA8(10, 9, 24, 0));
    /* row 1: logo, tabs, downloads, clock */
    float y = 8;
    float lx = RTL() ? SCR_W - 40 : 18;
    draw_compass(lx, y, 22);
    gfx_text(RTL() ? lx - 8 : lx + 30, y + 3, "Nova", 14, 1, C_TEXT, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
    float tx = RTL() ? lx - 70 : lx + 78;
    for (int i = 0; i < ntabs; i++) {
        float w = 118;
        float x = RTL() ? tx - w : tx;
        int a = i == cur;
        gfx_rrect(x, y - 1, w, 24, 12, a ? RGBA8(127, 119, 221, 70) : C_GLASS);
        if (a) gfx_rrect_border(x, y - 1, w, 24, 12, 1, RGBA8(175, 169, 236, 160));
        const char *tt = tabs[i].home ? T(S_HOME) : (tabs[i].doc ? tabs[i].doc->title : tabs[i].fetch.state == F_RUNNING ? T(S_LOADING) : tabs[i].url);
        if (tabs[i].fetch.state == F_RUNNING) { float ph = gfx_frame() / 8.0f; gfx_circle(x + 12, y + 11, 3 + sinf(ph) * 1.2f, C_TEAL); }
        gfx_text_fit(x + w / 2 + (tabs[i].fetch.state == F_RUNNING ? 6 : 0), y + 4, w - 26, tt, 11, a, a ? C_TEXT : C_DIM, ALIGN_CENTER);
        tx = RTL() ? tx - w - 6 : tx + w + 6;
    }
    /* clock + downloads badge on the far side */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char clk[16];
    snprintf(clk, sizeof(clk), "%02d:%02d", lt ? lt->tm_hour : 0, lt ? lt->tm_min : 0);
    float fx = RTL() ? 18 : SCR_W - 18;
    gfx_text(fx, y + 4, clk, 12, 1, C_ACC_L, RTL() ? ALIGN_LEFT : ALIGN_RIGHT);
    int act = dl_active_count();
    if (act) {
        char b[8]; snprintf(b, sizeof(b), "%d", act);
        float bx = RTL() ? 70 : SCR_W - 92;
        gfx_rrect(bx, y, 28, 22, 11, RGBA8(15, 110, 86, 255));
        gfx_triangle(bx + 8, y + 9, bx + 14, y + 16, bx + 20, y + 9, RGBA8(255, 255, 255, 230));
        gfx_rect(bx + 12.5f, y + 4, 3, 6, RGBA8(255, 255, 255, 230));
        gfx_circle(bx + 26, y + 2, 7, C_TEAL);
        gfx_text(bx + 26, y - 4, b, 9, 1, RGBA8(4, 52, 44, 255), ALIGN_CENTER);
    }
    /* row 2: address bar */
    float ay = 40, ax = 60, aw = SCR_W - 120;
    gfx_rrect(ax, ay, aw, 32, 16, RGBA8(255, 255, 255, 20));
    gfx_rrect_border(ax, ay, aw, 32, 16, 1, RGBA8(255, 255, 255, 34));
    /* back / forward chevrons */
    uint32_t cb = t->hp > 0 ? C_TEXT : RGBA8(255, 255, 255, 50), cf = t->hp + 1 < t->hn ? C_TEXT : RGBA8(255, 255, 255, 50);
    gfx_line(36, ay + 16, 44, ay + 9, 2, cb); gfx_line(36, ay + 16, 44, ay + 23, 2, cb);
    gfx_line(SCR_W - 36, ay + 16, SCR_W - 44, ay + 9, 2, cf); gfx_line(SCR_W - 36, ay + 16, SCR_W - 44, ay + 23, 2, cf);
    const char *u = t->fetch.state == F_RUNNING ? t->req_url : t->url;
    if (t->home) gfx_text(ax + aw / 2, ay + 8, T(S_SEARCH_HINT), 13, 0, C_DIM, ALIGN_CENTER);
    else {
        float x = ax + 16;
        if (url_is_https(u)) { draw_lock(x, ay + 9, C_TEAL); x += 20; }
        char host[256];
        url_host(u, host, sizeof(host));
        const char *rest = strstr(u, "://"); rest = rest ? rest + 3 + strlen(host) : "";
        int hw = gfx_text_fit(x, ay + 8, aw - (x - ax) - 20, host, 13, 1, C_TEXT, ALIGN_LEFT);
        if (rest[0] && hw < aw - 120) gfx_text_fit(x + hw, ay + 8, aw - (x - ax) - hw - 20, rest, 13, 0, C_DIM, ALIGN_LEFT);
    }
    if (t->fetch.state == F_RUNNING) {
        float p = t->fetch.total > 0 ? (float)(t->fetch.now / t->fetch.total) : 0;
        float wv = (gfx_frame() % 90) / 90.0f;
        if (p > 0.02f) gfx_rrect(ax + 14, ay + 28, (aw - 28) * p, 3, 1.5f, C_ACC_L);
        else gfx_rrect(ax + 14 + (aw - 108) * wv, ay + 28, 80, 3, 1.5f, C_ACC_L);
    }
}

/* ============================ page */
static void draw_field(doc_t *d, run_t *r, float x, float y, int focused)
{
    field_t *f = &d->fields[r->field];
    uint32_t edge = focused ? C_ACC_L : RGBA8(255, 255, 255, 60);
    switch (f->type) {
    case FLD_SUBMIT:
        gfx_rrect(x, y, r->w, r->h, r->h / 2, focused ? C_ACC : RGBA8(127, 119, 221, 140));
        gfx_text_fit(x + r->w / 2, y + 7, r->w - 16, f->value, 14, 1, RGBA8(255, 255, 255, 255), ALIGN_CENTER);
        break;
    case FLD_CHECK: case FLD_RADIO:
        gfx_rrect(x, y, r->w, r->h, f->type == FLD_RADIO ? r->h / 2 : 5, RGBA8(255, 255, 255, 20));
        gfx_rrect_border(x, y, r->w, r->h, f->type == FLD_RADIO ? r->h / 2 : 5, 1.5f, edge);
        if (f->checked) gfx_rrect(x + 5, y + 5, r->w - 10, r->h - 10, f->type == FLD_RADIO ? (r->h - 10) / 2 : 3, C_ACC_L);
        break;
    default: {
        gfx_rrect(x, y, r->w, r->h, 9, RGBA8(255, 255, 255, 16));
        gfx_rrect_border(x, y, r->w, r->h, 9, focused ? 2 : 1, edge);
        const char *show = f->value;
        char dots[256];
        if (f->type == FLD_SELECT && f->nopt) show = f->opt_txt[f->sel];
        if (f->type == FLD_PASSWORD) { int n = strlen(f->value); if (n > 60) n = 60; for (int i = 0; i < n; i++) dots[i] = '*'; dots[n] = 0; show = dots; }
        int ph = !show[0];
        if (ph) show = f->opt_txt[0] ? f->opt_txt[0] : "";
        int rtl = has_rtl(show);
        gfx_text_fit(rtl ? x + r->w - 12 - (f->type == FLD_SELECT ? 18 : 0) : x + 12, y + 7, r->w - 24 - (f->type == FLD_SELECT ? 18 : 0), show, 14, 0, ph ? C_DIM : C_TEXT, rtl ? ALIGN_RIGHT : ALIGN_LEFT);
        if (f->type == FLD_SELECT) { float cx = x + r->w - 16, cy = y + r->h / 2; gfx_triangle(cx - 5, cy - 2, cx + 5, cy - 2, cx, cy + 4, C_DIM); }
        break; }
    }
}

static void draw_page(tab_t *t)
{
    doc_t *d = t->doc;
    t->scroll_draw = approach(t->scroll_draw, t->scroll, 0.35f);
    if (fabsf(t->scroll_draw - t->scroll) < 0.5f) t->scroll_draw = t->scroll;
    float sy = t->scroll_draw;
    int fk = -1, fid = -1;
    if (t->focus >= 0 && t->focus < t->nunits) { fk = t->units[t->focus].kind; fid = t->units[t->focus].id; }
    /* focus highlight behind the runs */
    if (fk == 0) {
        for (int i = 0; i < d->nruns; i++) {
            run_t *r = &d->runs[i];
            if (r->link != fid || r->kind == RUN_FIELD || r->kind == RUN_BUTTON) continue;
            float y = VIEW_Y + r->y - sy;
            if (y + r->h < VIEW_Y - 20 || y > SCR_H) continue;
            gfx_rrect(VIEW_X + r->x - 5, y - 3, r->w + 10, r->h + 6, 7, RGBA8(127, 119, 221, 60));
            gfx_rrect_border(VIEW_X + r->x - 5, y - 3, r->w + 10, r->h + 6, 7, 1.5f, C_ACC_L);
        }
    }
    for (int i = 0; i < d->nruns; i++) {
        run_t *r = &d->runs[i];
        float y = VIEW_Y + r->y - sy, x = VIEW_X + r->x;
        if (y + r->h < VIEW_Y - 30) continue;
        if (y > SCR_H) break;
        switch (r->kind) {
        case RUN_TEXT: case RUN_BULLET:
            gfx_text(x, y, r->text, r->size, r->bold, (fk == 0 && r->link == fid) ? RGBA8(255, 255, 255, 255) : r->color, ALIGN_LEFT);
            break;
        case RUN_RULE: gfx_rect(x, y, r->w, 1, RGBA8(255, 255, 255, 40)); break;
        case RUN_IMG: {
            img_t *im = &d->imgs[r->img];
            if (im->state == 1 && im->tex) gfx_texture(im->tex, x, y, r->w, r->h, 0xFFFFFFFF);
            else { gfx_rrect(x, y, r->w, r->h, 8, RGBA8(255, 255, 255, 12)); float ph = (gfx_frame() % 60) / 60.0f; gfx_rrect(x, y, r->w * ph, r->h, 8, RGBA8(255, 255, 255, 8)); }
            break;
        }
        case RUN_FIELD: case RUN_BUTTON: draw_field(d, r, x, y, fk == 1 && r->field == fid); break;
        }
    }
    /* scrollbar */
    if (d->height > VIEW_H) {
        float track = VIEW_H - 10, th = track * VIEW_H / d->height;
        if (th < 24) th = 24;
        float ty = VIEW_Y + 5 + (track - th) * (sy / max_scroll(t));
        float sx = RTL() ? 6 : SCR_W - 10;
        gfx_rrect(sx, VIEW_Y + 5, 4, track, 2, RGBA8(255, 255, 255, 14));
        gfx_rrect(sx, ty, 4, th, 2, RGBA8(175, 169, 236, 150));
    }
    /* focused link url preview */
    if (fk == 0) {
        const char *h = d->links[fid].href;
        float w = gfx_text_width(h, 11, 0) + 24;
        if (w > SCR_W - 60) w = SCR_W - 60;
        float x = RTL() ? SCR_W - 16 - w : 16;
        gfx_rrect(x, SCR_H - 70, w, 24, 12, RGBA8(20, 18, 44, 235));
        gfx_text_fit(x + 12, SCR_H - 65, w - 24, h, 11, 0, C_ACC_L, ALIGN_LEFT);
    }
}

static void draw_error(tab_t *t)
{
    float cx = SCR_W / 2, cy = VIEW_Y + VIEW_H / 2 - 30;
    gfx_glow(cx, cy, 90, RGBA8(216, 90, 48, 50));
    gfx_circle(cx, cy - 20, 26, RGBA8(216, 90, 48, 255));
    gfx_text(cx, cy - 34, "!", 22, 1, RGBA8(255, 255, 255, 255), ALIGN_CENTER);
    gfx_text(cx, cy + 20, T(S_ERROR), 18, 1, C_TEXT, ALIGN_CENTER);
    gfx_text_fit(cx, cy + 48, SCR_W - 100, t->err, 12, 0, C_DIM, ALIGN_CENTER);
    gfx_text_fit(cx, cy + 68, SCR_W - 100, t->url, 11, 0, C_DIM, ALIGN_CENTER);
}

/* ============================ home */
static int home_tiles(const char **titles, const char **urls, uint32_t *colors, const char **glyphs)
{
    int n = 0;
    for (int i = 0; i < NDEF; i++) { titles[n] = def_tiles[i].title; urls[n] = def_tiles[i].url; colors[n] = def_tiles[i].color; glyphs[n] = def_tiles[i].glyph; n++; }
    static const uint32_t pal[] = { RGBA8(29, 158, 117, 255), RGBA8(83, 74, 183, 255), RGBA8(186, 117, 23, 255), RGBA8(212, 83, 126, 255) };
    for (int i = 0; i < nbm && n < 12; i++) { titles[n] = bms[i].title; urls[n] = bms[i].url; colors[n] = pal[i % 4]; glyphs[n] = NULL; n++; }
    return n;
}

static void draw_home(void)
{
    const char *ti[12], *ur[12], *gl[12]; uint32_t co[12];
    int n = home_tiles(ti, ur, co, gl);
    float cx = SCR_W / 2;
    gfx_text(cx, VIEW_Y + 40, T(S_WHERE), 24, 1, C_TEXT, ALIGN_CENTER);
    float bw = 520, bx = cx - bw / 2, by = VIEW_Y + 88;
    gfx_rrect(bx, by, bw, 46, 23, RGBA8(255, 255, 255, 22));
    gfx_rrect_border(bx, by, bw, 46, 23, home_sel == 0 ? 2.0f : 1.0f, home_sel == 0 ? C_ACC : RGBA8(255, 255, 255, 40));
    float sx = RTL() ? bx + bw - 30 : bx + 22;
    gfx_circle(sx, by + 21, 7, C_ACC_L); gfx_circle(sx, by + 21, 5, RGBA8(40, 36, 80, 255));
    gfx_line(sx + 4, by + 26, sx + 9, by + 31, 2.4f, C_ACC_L);
    gfx_text(RTL() ? sx - 18 : sx + 18, by + 14, T(S_SEARCH_HINT), 14, 0, C_DIM, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
    float pw = gfx_text_width("DuckDuckGo", 10, 1) + 16;
    float px = RTL() ? bx + 14 : bx + bw - 14 - pw;
    gfx_rrect(px, by + 14, pw, 18, 9, RGBA8(60, 52, 137, 255));
    gfx_text(px + pw / 2, by + 17, "DuckDuckGo", 10, 1, C_ACC_L, ALIGN_CENTER);
    /* tiles: rows of 6 */
    float tw = 100, th = 92, gap = 14;
    int per = 6;
    for (int i = 0; i < n; i++) {
        int row = i / per, col = i % per;
        int inrow = (n - row * per) < per ? n - row * per : per;
        float rowx = cx - (inrow * tw + (inrow - 1) * gap) / 2;
        float x = RTL() ? rowx + (inrow - 1 - col) * (tw + gap) : rowx + col * (tw + gap);
        float y = by + 76 + row * (th + gap);
        int s = home_sel == i + 1;
        float lift = s ? -3 : 0;
        gfx_rrect(x, y + lift, tw, th, 18, s ? RGBA8(127, 119, 221, 50) : RGBA8(255, 255, 255, 14));
        gfx_rrect_border(x, y + lift, tw, th, 18, s ? 2 : 1, s ? C_ACC_L : RGBA8(255, 255, 255, 26));
        gfx_rrect(x + tw / 2 - 20, y + 14 + lift, 40, 40, 12, co[i]);
        char g[8];
        if (gl[i]) snprintf(g, sizeof(g), "%s", gl[i]);
        else { uint32_t cp[2]; int k = utf8_decode(ti[i], cp, 1); if (k) { if (cp[0] >= 'a' && cp[0] <= 'z') cp[0] -= 32; int bl = utf8_encode(cp, 1, g, sizeof(g) - 1); g[bl] = 0; } else snprintf(g, sizeof(g), "*"); }
        gfx_text(x + tw / 2, y + 24 + lift, g, 17, 1, RGBA8(255, 255, 255, 255), ALIGN_CENTER);
        gfx_text_fit(x + tw / 2, y + 64 + lift, tw - 12, ti[i], 11, s, s ? C_TEXT : C_DIM, ALIGN_CENTER);
    }
    if (n == NDEF) gfx_text(cx, by + 76 + th + 26, T(S_NO_BM), 11, 0, RGBA8(158, 155, 199, 160), ALIGN_CENTER);
}

static void handle_home(tab_t *t)
{
    const char *ti[12], *ur[12], *gl[12]; uint32_t co[12];
    int n = home_tiles(ti, ur, co, gl);
    uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
    if (in_active & B_DOWN) { if (home_sel == 0) home_sel = 1; else if (home_sel + 6 <= n) home_sel += 6; }
    if (in_active & B_UP) { if (home_sel > 6) home_sel -= 6; else home_sel = 0; }
    if (in_active & R) { if (home_sel > 0 && home_sel < n) home_sel++; }
    if (in_active & L) { if (home_sel > 1) home_sel--; }
    if (home_sel > n) home_sel = n;
    if (in_pressed & B_OK) {
        if (home_sel == 0) { osk_purpose = 1; if (osk_open(T(S_ADDRESS), "")) ui = UI_OSK; }
        else tab_go(t, ur[home_sel - 1], NULL, 1);
    }
}

/* ============================ menus & lists */
enum { MI_NEWTAB, MI_CLOSETAB, MI_BACK, MI_FORWARD, MI_RELOAD, MI_HOME, MI_ADDBM, MI_BOOKMARKS, MI_DOWNLOADS, MI_LANG, MI_EXIT, MI_COUNT };
static const int mi_label[MI_COUNT] = { S_NEWTAB, S_CLOSETAB, S_BACK, S_FORWARD, S_RELOAD, S_HOME, S_ADDBM, S_BOOKMARKS, S_DOWNLOADS, S_LANG, S_EXIT };
static int menu_sel;

static void run_menu(tab_t *t, int m)
{
    ui = UI_BROWSE;
    switch (m) {
    case MI_NEWTAB: new_tab(HOME_URL); break;
    case MI_CLOSETAB: close_tab(cur); break;
    case MI_BACK: tab_back(t); break;
    case MI_FORWARD: tab_forward(t); break;
    case MI_RELOAD: if (!t->home) { t->restore = t->scroll; tab_go(t, t->url, NULL, 0); } break;
    case MI_HOME: tab_go(t, HOME_URL, NULL, 1); break;
    case MI_ADDBM:
        if (!t->home && t->doc) {
            for (int i = 0; i < nbm; i++) if (!strcmp(bms[i].url, t->url)) { show_toast(T(S_BM_ADDED)); return; }
            if (nbm < BM_MAX) { snprintf(bms[nbm].url, URL_MAX, "%s", t->url); snprintf(bms[nbm].title, 128, "%s", t->doc->title); nbm++; bm_save(); show_toast(T(S_BM_ADDED)); }
        }
        break;
    case MI_BOOKMARKS: bm_sel = 0; ui = UI_BOOKMARKS; break;
    case MI_DOWNLOADS: dl_list_sel = 0; ui = UI_DOWNLOADS; break;
    case MI_LANG: g_lang = !g_lang; save_settings(); ui = UI_MENU; break;
    case MI_EXIT: quit_req = 1; break;
    }
}

static void draw_menu(void)
{
    float w = 300, h = MI_COUNT * 34 + 30, x, y;
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA8(3, 3, 12, 140));
    x = RTL() ? 20 : SCR_W - w - 20; y = VIEW_Y - 4;
    gfx_rrect(x, y, w, h, 20, RGBA8(24, 22, 50, 250));
    gfx_rrect_border(x, y, w, h, 20, 1.2f, RGBA8(255, 255, 255, 40));
    for (int i = 0; i < MI_COUNT; i++) {
        float iy = y + 15 + i * 34;
        if (i == menu_sel) gfx_rrect(x + 10, iy, w - 20, 32, 12, RGBA8(127, 119, 221, 90));
        if (i == MI_LANG || i == MI_EXIT) gfx_rect(x + 20, iy - 1, w - 40, 1, RGBA8(255, 255, 255, 20));
        gfx_text(RTL() ? x + w - 28 : x + 28, iy + 8, T(mi_label[i]), 14, i == menu_sel, i == menu_sel ? C_TEXT : C_DIM, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
    }
}

static void draw_dl_prompt(void)
{
    float w = 560, h = 150 + ndest * 36, x, y;
    modal_frame(w, h, &x, &y);
    gfx_rrect(x + 26, y + 24, 44, 44, 13, RGBA8(15, 110, 86, 255));
    gfx_triangle(x + 36, y + 42, x + 48, y + 56, x + 60, y + 42, RGBA8(255, 255, 255, 240));
    gfx_rect(x + 45, y + 32, 6, 12, RGBA8(255, 255, 255, 240));
    gfx_text_fit(x + 84, y + 26, w - 110, dlp_name, 16, 1, C_TEXT, ALIGN_LEFT);
    char host[200]; url_host(dlp_url, host, sizeof(host));
    gfx_text_fit(x + 84, y + 50, w - 110, host, 12, 0, C_DIM, ALIGN_LEFT);
    gfx_text(RTL() ? x + w - 28 : x + 28, y + 86, T(S_SAVE_TO), 12, 1, C_ACC_L, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
    for (int i = 0; i < ndest; i++) {
        float iy = y + 108 + i * 36;
        int s = i == dest_sel;
        gfx_rrect(x + 20, iy, w - 40, 32, 12, s ? RGBA8(127, 119, 221, 80) : RGBA8(255, 255, 255, 10));
        if (s) gfx_rrect_border(x + 20, iy, w - 40, 32, 12, 1.4f, C_ACC_L);
        gfx_text_fit(x + 36, iy + 8, w * 0.45f, dests[i].label, 13, s, s ? C_TEXT : C_DIM, ALIGN_LEFT);
        gfx_text_fit(x + w - 36, iy + 9, w * 0.42f, dests[i].path, 11, 0, C_DIM, ALIGN_RIGHT);
    }
    if (ends_with(dlp_name, ".pkg") && dest_sel == 0) gfx_text_fit(SCR_W / 2, y + h - 30, w - 40, T(S_PKG_HINT), 10, 0, C_TEAL, ALIGN_CENTER);
}

static void draw_downloads(void)
{
    float w = 660, h = 380, x, y;
    modal_frame(w, h, &x, &y);
    gfx_text(SCR_W / 2, y + 18, T(S_DOWNLOADS), 18, 1, C_TEXT, ALIGN_CENTER);
    int idx[DL_MAX], n = 0;
    for (int i = DL_MAX - 1; i >= 0; i--) if (dl_get(i)->state != DL_FREE) idx[n++] = i;
    if (!n) { gfx_text(SCR_W / 2, y + h / 2 - 10, T(S_NO_DL), 14, 0, C_DIM, ALIGN_CENTER); return; }
    if (dl_list_sel >= n) dl_list_sel = n - 1;
    int top = dl_list_sel > 4 ? dl_list_sel - 4 : 0;
    for (int k = top; k < n && k < top + 5; k++) {
        download_t *d = dl_get(idx[k]);
        float iy = y + 56 + (k - top) * 60;
        int s = k == dl_list_sel;
        gfx_rrect(x + 18, iy, w - 36, 54, 14, s ? RGBA8(127, 119, 221, 60) : RGBA8(255, 255, 255, 10));
        if (s) gfx_rrect_border(x + 18, iy, w - 36, 54, 14, 1.3f, C_ACC_L);
        gfx_text_fit(x + 34, iy + 7, w * 0.55f, d->name, 13, 1, C_TEXT, ALIGN_LEFT);
        char a[32], b[32], line[160];
        fmt_size(d->now, a, sizeof(a));
        if (d->total > 0) fmt_size(d->total, b, sizeof(b)); else snprintf(b, sizeof(b), "%s", T(S_UNKNOWN_SIZE));
        uint32_t sc = C_DIM;
        if (d->state == DL_RUNNING) { char sp[32]; fmt_size(d->speed, sp, sizeof(sp)); snprintf(line, sizeof(line), "%s / %s · %s/s", a, b, sp); }
        else if (d->state == DL_DONE) { snprintf(line, sizeof(line), "%s · %s", T(S_DONE), a); sc = C_TEAL; }
        else if (d->state == DL_FAILED) { snprintf(line, sizeof(line), "%s · %s", T(S_FAILED), d->err); sc = RGBA8(240, 150, 120, 255); }
        else snprintf(line, sizeof(line), "%s", T(S_CANCELLED));
        gfx_text_fit(x + w - 34, iy + 8, w * 0.38f, line, 11, 0, sc, ALIGN_RIGHT);
        float p = d->total > 0 ? (float)(d->now / d->total) : (d->state == DL_DONE ? 1 : 0);
        if (p > 1) p = 1;
        gfx_rrect(x + 34, iy + 30, w - 68, 6, 3, RGBA8(255, 255, 255, 18));
        if (p > 0.01f) gfx_rrect(x + 34, iy + 30, (w - 68) * p, 6, 3, d->state == DL_FAILED ? RGBA8(216, 90, 48, 255) : C_TEAL);
        else if (d->state == DL_RUNNING) { float wv = (gfx_frame() % 80) / 80.0f; gfx_rrect(x + 34 + (w - 128) * wv, iy + 30, 60, 6, 3, C_TEAL); }
        gfx_text_fit(x + 34, iy + 38, w - 68, d->path, 9, 0, RGBA8(158, 155, 199, 170), ALIGN_LEFT);
    }
}

static int dl_list_index(int k)
{
    int n = 0;
    for (int i = DL_MAX - 1; i >= 0; i--) if (dl_get(i)->state != DL_FREE) { if (n == k) return i; n++; }
    return -1;
}

static void draw_bookmarks(void)
{
    float w = 600, h = 380, x, y;
    modal_frame(w, h, &x, &y);
    gfx_text(SCR_W / 2, y + 18, T(S_BOOKMARKS), 18, 1, C_TEXT, ALIGN_CENTER);
    if (!nbm) { gfx_text_fit(SCR_W / 2, y + h / 2 - 10, w - 40, T(S_NO_BM), 13, 0, C_DIM, ALIGN_CENTER); return; }
    int top = bm_sel > 6 ? bm_sel - 6 : 0;
    for (int k = top; k < nbm && k < top + 7; k++) {
        float iy = y + 56 + (k - top) * 42;
        int s = k == bm_sel;
        gfx_rrect(x + 18, iy, w - 36, 38, 12, s ? RGBA8(127, 119, 221, 70) : RGBA8(255, 255, 255, 10));
        gfx_text_fit(x + 34, iy + 4, w - 68, bms[k].title, 13, s, s ? C_TEXT : C_DIM, has_rtl(bms[k].title) ? ALIGN_LEFT : ALIGN_LEFT);
        gfx_text_fit(x + 34, iy + 21, w - 68, bms[k].url, 10, 0, RGBA8(158, 155, 199, 170), ALIGN_LEFT);
    }
}

/* ============================ main */
static void sys_cb(u64 status, u64 param, void *ud) { (void)param; (void)ud; if (status == SYSUTIL_EXIT_GAME) quit_req = 1; }

int main(void)
{
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_cb, NULL);
    s32 assign = 1;
    sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN, &assign);
    if (assign == 0) { B_OK = B_CIR; B_CANCEL = B_CRO; ok_icon = BTN_CIRCLE; cancel_icon = BTN_CROSS; }
    mkdir(APP_DIR, 0777);
    load_settings();
    bm_load();
    gfx_init(APP_DIR "/fonts/DejaVuSans.ttf", APP_DIR "/fonts/DejaVuSans-Bold.ttf");
    vfs_init();
    ya2d_paddata[0].ANA_L_H = ya2d_paddata[0].ANA_L_V = 0x80;
    ya2d_paddata[0].ANA_R_H = ya2d_paddata[0].ANA_R_V = 0x80;
    int net = net_init();
    new_tab(HOME_URL);
    if (!net) show_toast("Network unavailable");

    int last_lang = g_lang;
    while (!quit_req) {
        sysUtilCheckCallback();
        read_input();
        tab_t *t = &tabs[cur];
        for (int i = 0; i < ntabs; i++) poll_tab(&tabs[i]);
        poll_images();
        if (last_lang != g_lang) { last_lang = g_lang; for (int i = 0; i < ntabs; i++) tab_layout(&tabs[i]); }
        t = &tabs[cur];

        switch (ui) {
        case UI_BROWSE:
            if (in_pressed & B_L1) { cur = (cur + ntabs - 1) % ntabs; break; }
            if (in_pressed & B_R1) { cur = (cur + 1) % ntabs; break; }
            if (in_pressed & B_TRI) { menu_sel = 0; ui = UI_MENU; break; }
            if (in_pressed & B_SELECT) { osk_purpose = 1; if (osk_open(T(S_ADDRESS), t->home ? "" : t->url)) ui = UI_OSK; break; }
            if (in_pressed & B_START) { tab_go(t, HOME_URL, NULL, 1); break; }
            if (in_pressed & B_CANCEL) { tab_back(t); break; }
            if (t->home) { handle_home(t); break; }
            if (t->err[0] && !t->doc) { if (in_pressed & B_OK) tab_go(t, t->url, NULL, 0); break; }
            if (!t->doc) break;
            if (in_active & (B_DOWN | (RTL() ? B_LEFT : B_RIGHT))) move_focus(t, 1);
            if (in_active & (B_UP | (RTL() ? B_RIGHT : B_LEFT))) move_focus(t, -1);
            if (in_active & B_R2) { t->scroll += VIEW_H * 0.85f; clamp_scroll(t); sync_focus_to_view(t); }
            if (in_active & B_L2) { t->scroll -= VIEW_H * 0.85f; clamp_scroll(t); sync_focus_to_view(t); }
            if (stick_ry != 0) { t->scroll += stick_ry * fabsf(stick_ry) * 22; clamp_scroll(t); t->scroll_draw = t->scroll; sync_focus_to_view(t); }
            if (in_pressed & B_OK) activate(t);
            if (in_pressed & B_SQU) download_focused(t);
            break;
        case UI_MENU:
            if (in_active & B_DOWN) menu_sel = (menu_sel + 1) % MI_COUNT;
            if (in_active & B_UP) menu_sel = (menu_sel + MI_COUNT - 1) % MI_COUNT;
            if (in_pressed & (B_CANCEL | B_TRI)) ui = UI_BROWSE;
            else if (in_pressed & B_OK) run_menu(t, menu_sel);
            break;
        case UI_DLPROMPT:
            if (in_active & B_DOWN) dest_sel = (dest_sel + 1) % ndest;
            if (in_active & B_UP) dest_sel = (dest_sel + ndest - 1) % ndest;
            if (in_pressed & B_CANCEL) ui = UI_BROWSE;
            else if (in_pressed & B_OK) start_download();
            break;
        case UI_DOWNLOADS: {
            int n = 0; for (int i = 0; i < DL_MAX; i++) if (dl_get(i)->state != DL_FREE) n++;
            if (n && (in_active & B_DOWN)) dl_list_sel = (dl_list_sel + 1) % n;
            if (n && (in_active & B_UP)) dl_list_sel = (dl_list_sel + n - 1) % n;
            if (n && (in_pressed & B_SQU)) { int i = dl_list_index(dl_list_sel); if (i >= 0) { if (dl_get(i)->state == DL_RUNNING) dl_cancel(i); else dl_remove(i); } }
            if (in_pressed & (B_CANCEL | B_OK)) ui = UI_BROWSE;
            break;
        }
        case UI_BOOKMARKS:
            if (nbm && (in_active & B_DOWN)) bm_sel = (bm_sel + 1) % nbm;
            if (nbm && (in_active & B_UP)) bm_sel = (bm_sel + nbm - 1) % nbm;
            if (nbm && (in_pressed & B_SQU)) { memmove(&bms[bm_sel], &bms[bm_sel + 1], sizeof(bm_t) * (nbm - bm_sel - 1)); nbm--; if (bm_sel >= nbm && bm_sel) bm_sel--; bm_save(); show_toast(T(S_BM_REMOVED)); }
            if (in_pressed & B_CANCEL) ui = UI_BROWSE;
            else if (nbm && (in_pressed & B_OK)) { ui = UI_BROWSE; tab_go(t, bms[bm_sel].url, NULL, 1); }
            break;
        case UI_OSK: {
            char buf[1024];
            int r = osk_update(buf, sizeof(buf));
            if (r == OSK_RESULT_OK) {
                ui = UI_BROWSE;
                if (osk_purpose == 1) go_input(t, buf);
                else if (osk_purpose == 2 && t->doc && osk_field < t->doc->nfields) {
                    snprintf(t->doc->fields[osk_field].value, sizeof(t->doc->fields[osk_field].value), "%s", buf);
                    field_committed(t, osk_field);
                }
            } else if (r == OSK_RESULT_CANCEL) ui = UI_BROWSE;
            break;
        }
        }

        /* download notifications */
        static int prev_state[DL_MAX];
        for (int i = 0; i < DL_MAX; i++) {
            int s = dl_get(i)->state;
            if (prev_state[i] == DL_RUNNING && s == DL_DONE) { char m[320]; snprintf(m, sizeof(m), "%s · %s", T(S_DL_DONE), dl_get(i)->name); show_toast(m); }
            if (prev_state[i] == DL_RUNNING && s == DL_FAILED) { char m[320]; snprintf(m, sizeof(m), "%s · %s", T(S_DL_FAIL), dl_get(i)->name); show_toast(m); }
            prev_state[i] = s;
        }

        t = &tabs[cur];
        gfx_begin_frame();
        draw_bg();
        if (t->home) draw_home();
        else if (t->doc) draw_page(t);
        else if (t->err[0]) draw_error(t);
        else if (t->fetch.state == F_RUNNING) {
            float tt = gfx_frame() / 60.0f;
            for (int i = 0; i < 8; i++) { float a = tt * 4 + i * 0.785f; gfx_circle(SCR_W / 2 + cosf(a) * 20, VIEW_Y + VIEW_H / 2 + sinf(a) * 20, 4, RGBA8(175, 169, 236, 40 + i * 26)); }
        }
        draw_header(t);

        if (ui == UI_MENU) draw_menu();
        else if (ui == UI_DLPROMPT) draw_dl_prompt();
        else if (ui == UI_DOWNLOADS) draw_downloads();
        else if (ui == UI_BOOKMARKS) draw_bookmarks();

        hint_t h[8]; int n = 0;
        if (ui == UI_BROWSE) {
            if (t->home) { h[n++] = (hint_t){ ok_icon, T(S_OPEN) }; }
            else {
                h[n++] = (hint_t){ ok_icon, T(S_OPEN) };
                if (t->doc && t->focus >= 0 && t->focus < t->nunits && t->units[t->focus].kind == 0) h[n++] = (hint_t){ BTN_SQUARE, T(S_DOWNLOAD) };
                h[n++] = (hint_t){ cancel_icon, T(S_BACK) };
            }
            h[n++] = (hint_t){ BTN_TRIANGLE, T(S_MENU) };
            h[n++] = (hint_t){ BTN_L1, T(S_TABS) };
            h[n++] = (hint_t){ BTN_SELECT, T(S_ADDRESS) };
            h[n++] = (hint_t){ BTN_START, T(S_HOME) };
        } else if (ui == UI_MENU) { h[n++] = (hint_t){ ok_icon, T(S_CHOOSE) }; h[n++] = (hint_t){ cancel_icon, T(S_CLOSE) }; }
        else if (ui == UI_DLPROMPT) { h[n++] = (hint_t){ ok_icon, T(S_START_DL) }; h[n++] = (hint_t){ cancel_icon, T(S_CANCEL) }; }
        else if (ui == UI_DOWNLOADS) { h[n++] = (hint_t){ BTN_SQUARE, T(S_STOP) }; h[n++] = (hint_t){ cancel_icon, T(S_CLOSE) }; }
        else if (ui == UI_BOOKMARKS) { h[n++] = (hint_t){ ok_icon, T(S_OPEN) }; h[n++] = (hint_t){ BTN_SQUARE, T(S_REMOVE) }; h[n++] = (hint_t){ cancel_icon, T(S_CLOSE) }; }
        if (ui != UI_OSK) draw_hints(h, n);

        if (gfx_frame() < toast_until && toast[0]) {
            float left = (float)(toast_until - gfx_frame());
            int a = left < 20 ? (int)(left / 20 * 255) : 255;
            float tw = gfx_text_width(toast, 13, 1) + 40;
            if (tw > SCR_W - 40) tw = SCR_W - 40;
            float ty = SCR_H - 92;
            gfx_rrect(SCR_W / 2 - tw / 2, ty, tw, 32, 16, RGBA8(30, 28, 66, a * 240 / 255));
            gfx_rrect_border(SCR_W / 2 - tw / 2, ty, tw, 32, 16, 1, RGBA8(255, 255, 255, a / 4));
            gfx_text_fit(SCR_W / 2, ty + 8, tw - 30, toast, 13, 1, WITH_ALPHA(C_TEXT, a), ALIGN_CENTER);
        }
        gfx_end_frame();
    }

    net_shutdown();
    for (int i = 0; i < ntabs; i++) { fetch_reset(&tabs[i].fetch); doc_free(tabs[i].doc); }
    vfs_shutdown();
    gfx_shutdown();
    sysProcessExit(0);
    return 0;
}

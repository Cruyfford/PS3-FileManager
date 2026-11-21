/* File Manager by CRUYFFORD — modern PS3 file manager + PKG installer (HEN & CFW).
 * Reads internal HDD, flash, Blu-ray, and USB drives in FAT32, exFAT, NTFS and ext2/3/4. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>
#include <lv2/process.h>
#include <io/pad.h>
#include <ya2d/ya2d.h>

#include "gfx.h"
#include "lang.h"
#include "pkginfo.h"
#include "installer.h"
#include "fileops.h"
#include "vfs.h"
#include "osk.h"
#include "pkgtool.h"

#define APP_DIR   "/dev_hdd0/game/CRPK00001/USRDIR"
#define CFG_FILE  APP_DIR "/settings.cfg"

extern const uint8_t ICONFILE_png[];
extern const uint32_t ICONFILE_png_size;

/* =========================================================== input */
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

static void read_input(void)
{
    ya2d_controlsRead();
    uint32_t d = 0;
    memcpy(&d, &ya2d_paddata[0].button[2], sizeof(uint32_t));
    if (ya2d_paddata[0].ANA_L_V < 0x30) d |= B_UP;
    if (ya2d_paddata[0].ANA_L_V > 0xD0) d |= B_DOWN;
    if (ya2d_paddata[0].ANA_L_H < 0x30) d |= B_LEFT;
    if (ya2d_paddata[0].ANA_L_H > 0xD0) d |= B_RIGHT;
    in_pressed = d & ~prev_down;
    in_active = in_pressed;
    if (d && d == prev_down) {
        hold_frames++;
        if (hold_frames > 18 && (hold_frames % 4) == 0) in_active = d & (B_UP | B_DOWN | B_LEFT | B_RIGHT | B_L2 | B_R2);
    } else hold_frames = 0;
    prev_down = d;
}

/* =========================================================== settings */
typedef struct { int lang; int mode; int del; } settings_t;
static settings_t cfg = { 0, MODE_XMB_QUEUE, 0 };

static void load_settings(void)
{
    FILE *f = fopen(CFG_FILE, "rb");
    if (f) {
        settings_t s;
        if (fread(&s, sizeof(s), 1, f) == 1) {
            if (s.lang >= 0 && s.lang <= 1) cfg.lang = s.lang;
            if (s.mode >= 0 && s.mode <= 1) cfg.mode = s.mode;
            if (s.del >= 0 && s.del <= 1) cfg.del = s.del;
        }
        fclose(f);
    }
    g_lang = cfg.lang;
}

static void save_settings(void)
{
    mkdir(APP_DIR, 0777);
    FILE *f = fopen(CFG_FILE, "wb");
    if (f) { fwrite(&cfg, sizeof(cfg), 1, f); fclose(f); }
}

/* =========================================================== RTL-aware layout */
static inline int RTL(void) { return g_lang == 1; }
static inline float MX(float x, float w) { return RTL() ? SCR_W - x - w : x; }
static inline float MXP(float x) { return RTL() ? SCR_W - x : x; }
static inline int MA(int a) { if (!RTL()) return a; return a == ALIGN_LEFT ? ALIGN_RIGHT : a == ALIGN_RIGHT ? ALIGN_LEFT : a; }

static void L_rrect(float x, float y, float w, float h, float r, uint32_t c) { gfx_rrect(MX(x, w), y, w, h, r, c); }
static void L_rrect_grad(float x, float y, float w, float h, float r, uint32_t a, uint32_t b) { gfx_rrect_grad(MX(x, w), y, w, h, r, a, b); }
static void L_border(float x, float y, float w, float h, float r, float t, uint32_t c) { gfx_rrect_border(MX(x, w), y, w, h, r, t, c); }
static int  L_text(float x, float y, const char *s, int size, int bold, uint32_t c, int a) { return gfx_text(MXP(x), y, s, size, bold, c, MA(a)); }
static int  L_text_fit(float x, float y, float mw, const char *s, int size, int bold, uint32_t c, int a) { return gfx_text_fit(MXP(x), y, mw, s, size, bold, c, MA(a)); }

static void L_grad_pill(float x, float y, float w, float h, float r, uint32_t a, uint32_t b)
{
    float rx = MX(x, w);
    if (RTL()) { uint32_t t = a; a = b; b = t; }
    gfx_rrect(rx, y, w, h, r, a);
    gfx_grad_h(rx + r, y, w - 2 * r, h, WITH_ALPHA(a, 0), b);
    gfx_rrect(rx + w - 2 * r, y, 2 * r, h, r, b);
}

static void draw_lines(float x, float y, const char *s, int size, int bold, uint32_t c, int align, float lh)
{
    char b[512];
    const char *p = s;
    while (*p) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        if (n > 510) n = 510;
        memcpy(b, p, n); b[n] = 0;
        gfx_text(x, y, b, size, bold, c, align);
        y += lh;
        if (!e) break;
        p = e + 1;
    }
}

static void fmt_size(uint64_t b, char *out, int n)
{
    if (b >= 1024ULL * 1024 * 1024) snprintf(out, n, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024) snprintf(out, n, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024) snprintf(out, n, "%.0f KB", b / 1024.0);
    else snprintf(out, n, "%u B", (unsigned)b);
}

static void fmt_time(uint32_t s, char *out, int n)
{
    if (s >= 3600) snprintf(out, n, "%uh %02um", s / 3600, (s % 3600) / 60);
    else if (s >= 60) snprintf(out, n, "%um %02us", s / 60, s % 60);
    else snprintf(out, n, "%us", s);
}

static float approach(float cur, float target, float k)
{
    float d = target - cur;
    if (fabsf(d) < 0.01f) return target;
    return cur + d * k;
}

/* =========================================================== paths */
static void path_join(char *out, int n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (l && dir[l - 1] == '/') snprintf(out, n, "%s%s", dir, name);
    else snprintf(out, n, "%s/%s", dir, name);
}

/* strips the last component; keeps "ntfs0:/" and "0:/" roots intact */
static int path_parent(char *p, char *child, int cn)
{
    size_t l = strlen(p);
    if (l > 1 && p[l - 1] == '/' && p[l - 2] != ':') p[--l] = 0;
    char *s = strrchr(p, '/');
    if (!s) return 0;
    if (child) snprintf(child, cn, "%s", s + 1);
    if (s > p && s[-1] == ':') s[1] = 0;
    else if (s == p) s[1] = 0;
    else *s = 0;
    return 1;
}

static const char *ext_of(const char *n)
{
    const char *d = strrchr(n, '.');
    return d ? d + 1 : "";
}

/* =========================================================== file types */
enum { FT_DIR, FT_PKG, FT_DISC, FT_IMAGE, FT_VIDEO, FT_AUDIO, FT_ARCHIVE, FT_SYSTEM, FT_TEXT, FT_OTHER };

static int file_type(const char *name, int is_dir)
{
    if (is_dir) return FT_DIR;
    const char *e = ext_of(name);
    static const struct { const char *ext; int t; } map[] = {
        {"pkg",FT_PKG},{"iso",FT_DISC},{"bin",FT_DISC},{"img",FT_DISC},{"cue",FT_DISC},{"mdf",FT_DISC},{"vob",FT_VIDEO},
        {"png",FT_IMAGE},{"jpg",FT_IMAGE},{"jpeg",FT_IMAGE},{"bmp",FT_IMAGE},{"gif",FT_IMAGE},{"gtf",FT_IMAGE},{"dds",FT_IMAGE},
        {"mp4",FT_VIDEO},{"mkv",FT_VIDEO},{"avi",FT_VIDEO},{"m2ts",FT_VIDEO},{"mts",FT_VIDEO},{"pam",FT_VIDEO},{"wmv",FT_VIDEO},
        {"mp3",FT_AUDIO},{"ogg",FT_AUDIO},{"wav",FT_AUDIO},{"flac",FT_AUDIO},{"at3",FT_AUDIO},{"aac",FT_AUDIO},{"m4a",FT_AUDIO},
        {"zip",FT_ARCHIVE},{"rar",FT_ARCHIVE},{"7z",FT_ARCHIVE},{"tar",FT_ARCHIVE},{"gz",FT_ARCHIVE},
        {"self",FT_SYSTEM},{"sprx",FT_SYSTEM},{"prx",FT_SYSTEM},{"elf",FT_SYSTEM},{"sfo",FT_SYSTEM},{"rap",FT_SYSTEM},{"edat",FT_SYSTEM},{"sdat",FT_SYSTEM},
        {"txt",FT_TEXT},{"xml",FT_TEXT},{"ini",FT_TEXT},{"log",FT_TEXT},{"cfg",FT_TEXT},{"json",FT_TEXT},{"conf",FT_TEXT},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (!strcasecmp(e, map[i].ext)) return map[i].t;
    if (!strcasecmp(name, "EBOOT.BIN")) return FT_SYSTEM;
    return FT_OTHER;
}

static void type_colors(int t, uint32_t *a, uint32_t *b)
{
    switch (t) {
        case FT_DIR:     *a = RGBA8(255, 200, 90, 255);  *b = RGBA8(240, 150, 40, 255); break;
        case FT_PKG:     *a = RGBA8(80, 140, 255, 255);  *b = RGBA8(60, 80, 230, 255); break;
        case FT_DISC:    *a = RGBA8(170, 110, 255, 255); *b = RGBA8(110, 60, 220, 255); break;
        case FT_IMAGE:   *a = RGBA8(70, 220, 150, 255);  *b = RGBA8(20, 160, 120, 255); break;
        case FT_VIDEO:   *a = RGBA8(255, 100, 110, 255); *b = RGBA8(210, 50, 80, 255); break;
        case FT_AUDIO:   *a = RGBA8(255, 120, 200, 255); *b = RGBA8(200, 70, 170, 255); break;
        case FT_ARCHIVE: *a = RGBA8(255, 170, 70, 255);  *b = RGBA8(220, 110, 40, 255); break;
        case FT_SYSTEM:  *a = RGBA8(60, 210, 240, 255);  *b = RGBA8(30, 140, 200, 255); break;
        case FT_TEXT:    *a = RGBA8(190, 196, 220, 255); *b = RGBA8(130, 136, 170, 255); break;
        default:         *a = RGBA8(150, 156, 190, 255); *b = RGBA8(100, 106, 140, 255); break;
    }
}

/* =========================================================== volumes */
#define MAX_VOL 24
static volume_t vols[MAX_VOL];
static int vol_count, vol_sel, vol_scroll;
static uint64_t vol_free;
static int native_usb_mask;

static int usb_mask(void)
{
    int m = 0;
    for (int i = 0; i < 8; i++) {
        char p[32]; struct stat sb;
        snprintf(p, sizeof(p), "/dev_usb%03d", i);
        if (stat(p, &sb) == 0) m |= 1 << i;
    }
    return m;
}

/* =========================================================== entries */
#define MAX_ENTRIES 3000
typedef struct {
    char name[256];
    int is_dir, type, marked, parsed;
    uint64_t size;
    pkg_info_t info;
    texture_t icon;
} entry_t;

static entry_t *ents;
static vfs_entry_t *raw;
static int ent_count, sel, scroll_top, list_err;
static float sel_anim, scroll_anim;
static char cwd[1024];
static int focus_list = 1;

static int ent_cmp(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void free_entries(void)
{
    for (int i = 0; i < ent_count; i++) {
        pkg_free_info(&ents[i].info);
        if (ents[i].icon) gfx_free_texture(ents[i].icon);
    }
    ent_count = 0;
}

static void open_dir(const char *path, const char *select_name)
{
    free_entries();
    snprintf(cwd, sizeof(cwd), "%s", path);
    if (!strcmp(path, PACKAGES_DIR)) vfs_mkdir(PACKAGES_DIR);
    int n = vfs_list(cwd, raw, MAX_ENTRIES);
    list_err = n < 0;
    for (int i = 0; i < n; i++) {
        entry_t *e = &ents[ent_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", raw[i].name);
        e->is_dir = raw[i].is_dir;
        e->size = raw[i].size;
        e->type = file_type(e->name, e->is_dir);
    }
    qsort(ents, ent_count, sizeof(entry_t), ent_cmp);
    sel = 0;
    if (select_name) for (int i = 0; i < ent_count; i++) if (!strcmp(ents[i].name, select_name)) sel = i;
    scroll_top = sel > 3 ? sel - 3 : 0;
    sel_anim = sel; scroll_anim = scroll_top;
}

static int at_volume_root(void) { return vol_count && !strcmp(cwd, vols[vol_sel].root); }

static void go_up(void)
{
    if (at_volume_root()) { focus_list = 0; return; }
    char p[1024], child[256] = "";
    snprintf(p, sizeof(p), "%s", cwd);
    if (!path_parent(p, child, sizeof(child))) { focus_list = 0; return; }
    open_dir(p, child);
}

static void select_volume(int i)
{
    if (!vol_count) return;
    vol_sel = (i + vol_count) % vol_count;
    vol_free = vfs_free_space(vols[vol_sel].root);
    open_dir(vols[vol_sel].root, NULL);
}

static int marked_count(void)
{
    int n = 0;
    for (int i = 0; i < ent_count; i++) if (ents[i].marked) n++;
    return n;
}

/* parse one PKG per frame near the cursor; drop textures far away */
static void lazy_parse(void)
{
    for (int dist = 0; dist < 14; dist++) {
        int c[2] = { sel + dist, sel - dist };
        for (int k = 0; k < 2; k++) {
            int i = c[k];
            if (i < 0 || i >= ent_count) continue;
            entry_t *e = &ents[i];
            if (e->type != FT_PKG || e->parsed) continue;
            char full[1400];
            path_join(full, sizeof(full), cwd, e->name);
            pkg_read_info(full, &e->info, 1);
            if (e->info.icon) e->icon = gfx_load_png_buffer(e->info.icon, e->info.icon_size);
            e->parsed = 1;
            return;
        }
    }
    for (int i = 0; i < ent_count; i++)
        if (ents[i].parsed && abs(i - sel) > 40) {
            if (ents[i].icon) gfx_free_texture(ents[i].icon);
            ents[i].icon = NULL; ents[i].parsed = 0; pkg_free_info(&ents[i].info);
        }
}

/* =========================================================== clipboard */
static char (*clip)[1024];
static int clip_n, clip_op;   /* OP_COPY / OP_MOVE */

/* =========================================================== UI state */
enum { UI_BROWSE, UI_MENU, UI_CONFIRM_INSTALL, UI_CONFIRM_DELETE, UI_PROGRESS_INSTALL, UI_PROGRESS_OPS,
       UI_RESULT_INSTALL, UI_RESULT_OPS, UI_PROPS, UI_SETTINGS, UI_OSK, UI_SCANNING, UI_SFO };
static int ui = UI_SCANNING, prev_ui = UI_BROWSE;
static int confirm_sel, result_sel, set_sel, menu_sel;
static float modal_anim, menu_anim;
static int quit_req;
static char toast[256];
static uint64_t toast_until;
static int osk_purpose;   /* 0 rename, 1 new folder, 2 sfo field */
static sfo_field_t sfo_fields[SFO_MAX_FIELDS];
static int sfo_count, sfo_sel;
static char sfo_path[1400];
static char ops_output[1400];
static char osk_old[256];

static void show_toast(const char *s) { snprintf(toast, sizeof(toast), "%s", s); toast_until = gfx_frame() + 150; }
static void open_modal(int m) { prev_ui = ui; ui = m; modal_anim = 0.6f; }

static void sys_cb(u64 status, u64 param, void *ud)
{
    (void)param; (void)ud;
    if (status == SYSUTIL_EXIT_GAME) quit_req = 1;
}

/* =========================================================== colors */
static const uint32_t C_TEXT = RGBA8(240, 242, 255, 255);
static const uint32_t C_DIM = RGBA8(160, 168, 205, 255);
static const uint32_t C_ACC1 = RGBA8(64, 120, 255, 255);
static const uint32_t C_ACC2 = RGBA8(150, 70, 255, 255);
static const uint32_t C_PANEL = RGBA8(255, 255, 255, 14);
static const uint32_t C_EDGE = RGBA8(255, 255, 255, 30);
static const uint32_t C_OKC = RGBA8(60, 220, 150, 255);
static const uint32_t C_BAD = RGBA8(255, 80, 90, 255);

/* =========================================================== drawing */
static void draw_background(float t)
{
    gfx_grad_v(0, 0, SCR_W, SCR_H, RGBA8(9, 12, 34, 255), RGBA8(20, 8, 38, 255));
    gfx_glow(180 + sinf(t * 0.21f) * 90, 140 + cosf(t * 0.17f) * 60, 300, RGBA8(50, 90, 255, 70));
    gfx_glow(690 + cosf(t * 0.15f) * 80, 380 + sinf(t * 0.19f) * 50, 320, RGBA8(140, 50, 255, 60));
    gfx_glow(470 + sinf(t * 0.11f) * 160, 40 + cosf(t * 0.13f) * 30, 220, RGBA8(0, 200, 255, 32));
    for (int w = 0; w < 3; w++) {
        float py = 0;
        for (int x = 0; x <= SCR_W; x += 16) {
            float y = 330 + w * 26 + sinf(x * 0.008f + t * (0.4f + w * 0.15f) + w) * 22;
            if (x) gfx_line(x - 16, py, x, y, 1.2f, RGBA8(140, 160, 255, 16));
            py = y;
        }
    }
}

static void draw_logo(float x, float y, float s)
{
    gfx_rrect_grad(x, y, s, s, s * 0.28f, C_ACC1, C_ACC2);
    /* folder glyph */
    gfx_rrect(x + s * 0.2f, y + s * 0.3f, s * 0.26f, s * 0.12f, s * 0.04f, C_TEXT);
    gfx_rrect(x + s * 0.2f, y + s * 0.36f, s * 0.6f, s * 0.38f, s * 0.07f, C_TEXT);
    gfx_rrect(x + s * 0.27f, y + s * 0.45f, s * 0.46f, s * 0.05f, s * 0.02f, WITH_ALPHA(C_ACC1, 200));
}

static void draw_header(void)
{
    draw_logo(MX(24, 36), 18, 36);
    L_text(70, 17, T(S_FM_TITLE), 21, 1, C_TEXT, ALIGN_LEFT);
    L_text(71, 40, T(S_SUBTITLE), 11, 0, C_DIM, ALIGN_LEFT);

    u64 sec, nsec;
    sysGetCurrentTime(&sec, &nsec);
    s32 tz = 0, dst = 0;
    sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIMEZONE, &tz);
    sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME, &dst);
    time_t lt = (time_t)sec + tz * 60 + (dst ? 3600 : 0);
    struct tm *tmv = gmtime(&lt);
    char clk[16], fr[64], sz[32];
    snprintf(clk, sizeof(clk), "%02d:%02d", tmv ? tmv->tm_hour : 0, tmv ? tmv->tm_min : 0);
    L_text(824, 16, clk, 20, 1, C_TEXT, ALIGN_RIGHT);
    if (vol_count) {
        fmt_size(vol_free, sz, sizeof(sz));
        char f2[64]; snprintf(f2, sizeof(f2), T(S_FREE_ON), sz);
        snprintf(fr, sizeof(fr), "%s  \xC2\xB7  %s", vols[vol_sel].label, f2);
        L_text(824, 41, fr, 11, 0, C_DIM, ALIGN_RIGHT);
    }
}

static void draw_vol_icon(float x, float y, int kind, uint32_t c)
{
    float rx = MX(x, 22);
    uint32_t hole = RGBA8(20, 20, 40, 255);
    switch (kind) {
    case 1: /* usb */
        gfx_rrect(rx + 4, y + 7, 14, 13, 3, c);
        gfx_rect(rx + 7, y + 2, 8, 6, c);
        gfx_rect(rx + 8.5f, y + 3.5f, 2, 2, hole);
        gfx_rect(rx + 11.5f, y + 3.5f, 2, 2, hole);
        break;
    case 2: /* flash chip */
        gfx_rrect(rx + 4, y + 4, 14, 14, 3, c);
        for (int i = 0; i < 3; i++) { gfx_rect(rx + 1, y + 6 + i * 4, 3, 2, c); gfx_rect(rx + 18, y + 6 + i * 4, 3, 2, c); }
        break;
    case 3: /* disc */
        gfx_circle(rx + 11, y + 11, 9, c);
        gfx_circle(rx + 11, y + 11, 2.5f, hole);
        break;
    case 4: /* box */
        gfx_rrect(rx + 2, y + 6, 18, 14, 3, c);
        gfx_rect(rx + 2, y + 4, 18, 4, WITH_ALPHA(c, 180));
        gfx_rect(rx + 9, y + 9, 4, 3, hole);
        break;
    default: /* hdd */
        gfx_rrect(rx + 1, y + 4, 20, 15, 3, c);
        gfx_rect(rx + 4, y + 13, 9, 2, hole);
        gfx_circle(rx + 17, y + 14, 1.8f, C_OKC);
    }
}

static void draw_sidebar(void)
{
    float x = 24, y = 78, w = 200, h = 366, ih = 40;
    L_rrect(x, y, w, h, 16, C_PANEL);
    L_border(x, y, w, h, 16, 1, C_EDGE);
    L_text(x + 18, y + 14, T(S_DEVICES), 11, 1, C_DIM, ALIGN_LEFT);

    int visible = (int)((h - 44) / ih);
    if (vol_sel < vol_scroll) vol_scroll = vol_sel;
    if (vol_sel >= vol_scroll + visible) vol_scroll = vol_sel - visible + 1;

    for (int i = vol_scroll; i < vol_count && i < vol_scroll + visible; i++) {
        float iy = y + 36 + (i - vol_scroll) * ih;
        int on = (i == vol_sel);
        if (on) {
            L_grad_pill(x + 8, iy, w - 16, ih - 4, 11, WITH_ALPHA(C_ACC1, focus_list ? 140 : 235), WITH_ALPHA(C_ACC2, focus_list ? 140 : 235));
            if (!focus_list) L_border(x + 8, iy, w - 16, ih - 4, 11, 1.4f, RGBA8(255, 255, 255, 150 + (int)(sinf(gfx_frame() * 0.1f) * 60)));
        }
        draw_vol_icon(x + 18, iy + 7, vols[i].kind, on ? C_TEXT : C_DIM);
        const char *fs = vols[i].fs_name;
        float cw = gfx_text_width(fs, 9, 1) + 12;
        L_text_fit(x + 48, iy + 10, w - 70 - cw, vols[i].label, 13, on, on ? C_TEXT : C_DIM, ALIGN_LEFT);
        uint32_t cc = !strcmp(fs, "NTFS") ? RGBA8(90, 160, 255, 255) : !strcmp(fs, "exFAT") ? RGBA8(255, 150, 60, 255) :
                      !strcmp(fs, "FAT32") ? RGBA8(60, 210, 150, 255) : !strcmp(fs, "ext") ? RGBA8(230, 90, 160, 255) : RGBA8(120, 125, 160, 255);
        L_rrect(x + w - 16 - cw, iy + 10, cw, 16, 8, WITH_ALPHA(cc, on ? 255 : 170));
        L_text(x + w - 16 - cw / 2, iy + 12, fs, 9, 1, C_TEXT, ALIGN_CENTER);
    }
}

static void draw_file_icon(float x, float y, float s, int type, const char *name)
{
    uint32_t a, b;
    type_colors(type, &a, &b);
    float rx = MX(x, s);
    if (type == FT_DIR) {
        gfx_rrect_grad(rx, y + s * 0.18f, s, s * 0.78f, s * 0.14f, a, b);
        gfx_rrect(rx, y + s * 0.08f, s * 0.45f, s * 0.24f, s * 0.1f, a);
        return;
    }
    gfx_rrect_grad(rx, y, s, s, s * 0.22f, a, b);
    char lbl[6];
    const char *e = ext_of(name);
    if (!*e) e = "FILE";
    int n = 0;
    for (; e[n] && n < 4; n++) lbl[n] = (e[n] >= 'a' && e[n] <= 'z') ? e[n] - 32 : e[n];
    lbl[n] = 0;
    gfx_text(rx + s / 2, y + s / 2 - s * 0.17f, lbl, (int)(s * (n > 3 ? 0.27f : 0.32f)), 1, C_TEXT, ALIGN_CENTER);
}

static void draw_list(void)
{
    float x = 240, y = 78, w = 584, h = 366;
    L_rrect(x, y, w, h, 16, C_PANEL);
    L_border(x, y, w, h, 16, 1, C_EDGE);

    /* breadcrumb + counter */
    L_rrect(x + 12, y + 9, w - 150, 22, 11, RGBA8(0, 0, 0, 60));
    L_text_fit(x + 24, y + 13, w - 176, cwd, 11, 0, C_TEXT, ALIGN_LEFT);
    char cnt[64];
    int mk = marked_count();
    if (mk) snprintf(cnt, sizeof(cnt), T(S_SELECTED), mk); else snprintf(cnt, sizeof(cnt), T(S_FILES), ent_count);
    L_text(x + w - 18, y + 13, cnt, 11, 1, mk ? RGBA8(120, 200, 255, 255) : C_DIM, ALIGN_RIGHT);

    float ly = y + 40, row_h = 42, gap = 5, area_h = h - 48 - (clip_n ? 30 : 0);
    int visible = (int)(area_h / (row_h + gap));
    if (visible < 1) visible = 1;

    if (ent_count == 0) {
        draw_logo(MX(x + w / 2 - 28, 56), y + 110, 56);
        gfx_text(MXP(x + w / 2), y + 182, list_err ? T(S_READONLY) : T(S_EMPTY), 17, 1, C_TEXT, ALIGN_CENTER);
    } else {
        if (sel < scroll_top) scroll_top = sel;
        if (sel >= scroll_top + visible) scroll_top = sel - visible + 1;
        scroll_anim = approach(scroll_anim, scroll_top, 0.25f);
        sel_anim = approach(sel_anim, sel, 0.35f);

        float hy = ly + (sel_anim - scroll_anim) * (row_h + gap);
        if (hy > ly - row_h && hy < ly + area_h) {
            if (focus_list) gfx_glow(MXP(x + w / 2), hy + row_h / 2, w * 0.55f, RGBA8(90, 110, 255, 26));
            L_grad_pill(x + 10, hy, w - 22, row_h, 12, WITH_ALPHA(C_ACC1, focus_list ? 210 : 80), WITH_ALPHA(C_ACC2, focus_list ? 210 : 80));
            L_border(x + 10, hy, w - 22, row_h, 12, 1.2f, RGBA8(255, 255, 255, focus_list ? 110 : 30));
        }

        int first = (int)floorf(scroll_anim);
        for (int i = first; i < ent_count && i <= first + visible; i++) {
            if (i < 0) continue;
            float ry = ly + (i - scroll_anim) * (row_h + gap);
            if (ry < ly - 4 || ry + row_h > ly + area_h + 2) continue;
            entry_t *e = &ents[i];
            int on = (i == sel);
            if (!on) L_rrect(x + 10, ry, w - 22, row_h, 12, RGBA8(255, 255, 255, e->marked ? 30 : 9));

            float ix = x + 20;
            float tx = ix + 44;
            if (e->type == FT_PKG && e->icon) {
                gfx_texture(e->icon, MX(ix, 60), ry + 5, 60, 33, RGBA8(255, 255, 255, 255));
                gfx_rrect_border(MX(ix, 60) - 1, ry + 4, 62, 35, 4, 1, RGBA8(255, 255, 255, 60));
                tx = ix + 72;
            } else draw_file_icon(ix, ry + 6, 30, e->type, e->name);

            const char *title = (e->type == FT_PKG && e->parsed && e->info.title[0]) ? e->info.title : e->name;
            float tw = w - (tx - x) - 150;
            L_text_fit(tx, ry + 6, tw, title, 14, 1, C_TEXT, ALIGN_LEFT);

            char line2[200];
            if (e->is_dir) snprintf(line2, sizeof(line2), "%s", T(S_FOLDER_TYPE));
            else if (e->type == FT_PKG && e->parsed && e->info.title[0])
                snprintf(line2, sizeof(line2), "%s  \xC2\xB7  %s%s%s", e->name, e->info.title_id, e->info.version[0] ? "  v" : "", e->info.version);
            else snprintf(line2, sizeof(line2), "%s", ext_of(e->name)[0] ? ext_of(e->name) : T(S_FILE_TYPE));
            L_text_fit(tx, ry + 25, tw, line2, 10, 0, on ? RGBA8(220, 225, 255, 255) : C_DIM, ALIGN_LEFT);

            float bx = x + w - 26;
            if (e->marked) {
                float cx = MXP(bx - 8), cy = ry + row_h / 2;
                gfx_circle(cx, cy, 10, C_OKC);
                gfx_line(cx - 4.5f, cy, cx - 1, cy + 4, 2.2f, C_TEXT);
                gfx_line(cx - 1, cy + 4, cx + 5, cy - 3.5f, 2.2f, C_TEXT);
                bx -= 26;
            }
            if (!e->is_dir) {
                char sz[32];
                fmt_size(e->size, sz, sizeof(sz));
                L_text(bx, ry + 7, sz, 12, 1, on ? C_TEXT : C_DIM, ALIGN_RIGHT);
                if (e->type == FT_PKG && e->parsed) {
                    const char *tl = pkg_type_label(e->info.content_type);
                    const char *drm = !e->info.retail ? T(S_DEBUG) : e->info.drm_type == 3 ? T(S_FREE_DRM) :
                                      (e->info.drm_type == 1 || e->info.drm_type == 2) ? T(S_NEEDS_RAP) : NULL;
                    char tag[64];
                    snprintf(tag, sizeof(tag), drm ? "%s \xC2\xB7 %s" : "%s", tl, drm ? drm : "");
                    L_text(bx, ry + 25, tag, 9, 1, RGBA8(130, 180, 255, 255), ALIGN_RIGHT);
                }
            }
        }

        if (ent_count > visible) {
            float th = area_h * visible / ent_count;
            if (th < 24) th = 24;
            float tyy = ly + (area_h - th) * (scroll_anim / (float)(ent_count - visible));
            L_rrect(x + w - 8, ly, 3, area_h, 1.5f, RGBA8(255, 255, 255, 18));
            L_rrect(x + w - 8, tyy, 3, th, 1.5f, RGBA8(255, 255, 255, 120));
        }
    }

    /* clipboard strip */
    if (clip_n) {
        float cy = y + h - 34;
        L_rrect_grad(x + 10, cy, w - 20, 26, 13, RGBA8(40, 60, 140, 200), RGBA8(30, 40, 100, 200));
        char c[96];
        snprintf(c, sizeof(c), clip_op == OP_MOVE ? T(S_CLIP_CUT) : T(S_CLIP_COPY), clip_n);
        L_text(x + 26, cy + 6, c, 11, 1, C_TEXT, ALIGN_LEFT);
        L_text(x + w - 26, cy + 6, T(S_PASTE), 11, 0, C_DIM, ALIGN_RIGHT);
    }
}

typedef struct { int btn; int label; } hint_t;

static float btn_w(int b, float s)
{
    if (b <= BTN_TRIANGLE) return s;
    const char *l = b == BTN_START ? "START" : b == BTN_SELECT ? "SELECT" : b == BTN_L1 ? "L1" : "R1";
    return gfx_text_width(l, (int)(s * 0.5f), 1) + s * 0.6f;
}

static void draw_hints(const hint_t *h, int n)
{
    float y = 460, s = 20;
    float x = RTL() ? SCR_W - 30 : 30;
    L_rrect(16, 452, SCR_W - 32, 36, 18, RGBA8(0, 0, 0, 70));
    for (int i = 0; i < n; i++) {
        float lw = gfx_text_width(T(h[i].label), 12, 0), bw = btn_w(h[i].btn, s);
        if (RTL()) {
            gfx_button_icon(x - bw, y, h[i].btn, s);
            gfx_text(x - bw - 6, y + 3, T(h[i].label), 12, 0, C_TEXT, ALIGN_RIGHT);
            x -= bw + 6 + lw + 18;
        } else {
            gfx_button_icon(x, y, h[i].btn, s);
            gfx_text(x + bw + 6, y + 3, T(h[i].label), 12, 0, C_TEXT, ALIGN_LEFT);
            x += bw + 6 + lw + 18;
        }
    }
}

static void draw_modal_frame(float w, float h, float *ox, float *oy)
{
    modal_anim = approach(modal_anim, 1.0f, 0.2f);
    float a = modal_anim;
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA8(4, 4, 14, (int)(170 * a)));
    float sc = 0.92f + 0.08f * a, mw = w * sc, mh = h * sc;
    float x = (SCR_W - mw) / 2, y = (SCR_H - mh) / 2 - 10;
    gfx_glow(SCR_W / 2, y + mh / 2, mw * 0.75f, RGBA8(80, 90, 255, (int)(50 * a)));
    gfx_rrect_grad(x, y, mw, mh, 20, RGBA8(36, 38, 78, (int)(250 * a)), RGBA8(24, 18, 52, (int)(250 * a)));
    gfx_rrect_border(x, y, mw, mh, 20, 1.2f, RGBA8(255, 255, 255, (int)(50 * a)));
    *ox = x; *oy = y;
}

static void draw_button_row(float cx, float y, const char *a, const char *b, int selected, uint32_t accent_a)
{
    float bw = 170, bh = 38, gap = 16;
    float x0 = b ? cx - bw - gap / 2 : cx - bw / 2, x1 = cx + gap / 2;
    if (RTL() && b) { float t = x0; x0 = x1; x1 = t; }
    const char *lbl[2] = { a, b };
    float xs[2] = { x0, x1 };
    for (int i = 0; i < 2; i++) {
        if (!lbl[i]) continue;
        if (i == selected) {
            gfx_rrect_grad(xs[i], y, bw, bh, 19, i == 0 ? accent_a : C_ACC1, C_ACC2);
            gfx_rrect_border(xs[i], y, bw, bh, 19, 1.2f, RGBA8(255, 255, 255, 140));
        } else gfx_rrect(xs[i], y, bw, bh, 19, RGBA8(255, 255, 255, 22));
        gfx_text(xs[i] + bw / 2, y + 10, lbl[i], 15, 1, C_TEXT, ALIGN_CENTER);
    }
}

static void draw_result_icon(float cx, float cy, int ok, uint32_t c)
{
    gfx_glow(cx, cy, 60, WITH_ALPHA(c, 90));
    gfx_circle(cx, cy, 26, c);
    if (ok) { gfx_line(cx - 11, cy, cx - 3, cy + 8, 4, C_TEXT); gfx_line(cx - 3, cy + 8, cx + 12, cy - 8, 4, C_TEXT); }
    else { gfx_line(cx - 9, cy - 9, cx + 9, cy + 9, 4, C_TEXT); gfx_line(cx - 9, cy + 9, cx + 9, cy - 9, 4, C_TEXT); }
}

static void draw_progress_bar(float bx, float by, float bw, float pct, float *shown)
{
    *shown = approach(*shown, pct, 0.2f);
    if (pct < *shown - 0.5f) *shown = pct;
    gfx_rrect(bx, by, bw, 16, 8, RGBA8(255, 255, 255, 22));
    float fw = bw * *shown;
    if (fw > 16) {
        gfx_rrect_grad(bx, by, fw, 16, 8, RGBA8(90, 170, 255, 255), RGBA8(60, 90, 255, 255));
        float sh = fmodf(gfx_frame() * 4.0f, fw + 60) - 30;
        if (sh > 0 && sh < fw - 8) gfx_glow(bx + sh, by + 8, 26, RGBA8(255, 255, 255, 90));
    }
}

/* ---------- targets (marked items, or the cursor item) ---------- */
static int collect_targets(char out[][1024], int max, int pkg_only)
{
    int n = 0, mk = marked_count();
    for (int i = 0; i < ent_count && n < max; i++) {
        entry_t *e = &ents[i];
        if (!(mk ? e->marked : i == sel)) continue;
        if (pkg_only && e->type != FT_PKG) continue;
        path_join(out[n++], 1024, cwd, e->name);
    }
    return n;
}

static int count_targets(int pkg_only, uint64_t *bytes)
{
    int n = 0, mk = marked_count();
    if (bytes) *bytes = 0;
    for (int i = 0; i < ent_count; i++) {
        entry_t *e = &ents[i];
        if (!(mk ? e->marked : i == sel)) continue;
        if (pkg_only && e->type != FT_PKG) continue;
        n++;
        if (bytes) *bytes += e->size;
    }
    return n;
}

static int is_system_path(const char *p)
{
    return !strncmp(p, "/dev_flash", 10) || !strncmp(p, "/dev_blind", 10) ||
           !strncmp(p, "/dev_hdd0/vsh", 13) || !strncmp(p, "/dev_hdd0/home", 14);
}

/* ---------- context menu ---------- */
enum { M_OPEN, M_INSTALL, M_EXTRACT, M_BUILD, M_EDITSFO, M_COPY, M_CUT, M_PASTE, M_RENAME, M_NEWDIR, M_DELETE, M_SELALL, M_PROPS, M_REFRESH, M_SETTINGS, M_COUNT };
static const int menu_label[M_COUNT] = { S_OPEN, S_INSTALL_PKG, S_EXTRACT, S_BUILD, S_EDIT_SFO, S_COPY, S_CUT, S_PASTE, S_RENAME, S_NEWFOLDER, S_DELETE_ACT, S_SELECT_ALL, S_PROPS, S_REFRESH, S_SETTINGS };
static int menu_items[M_COUNT], menu_n;

static int cur_writable(void) { return vol_count && vols[vol_sel].writable; }

/* PARAM.SFO related to the cursor item: the file itself, or <folder>/PARAM.SFO */
static int cursor_sfo(char *out, int n)
{
    if (!ent_count || marked_count()) return 0;
    entry_t *e = &ents[sel];
    char p[1400];
    path_join(p, sizeof(p), cwd, e->name);
    if (!e->is_dir) { if (strcasecmp(e->name, "PARAM.SFO")) return 0; snprintf(out, n, "%s", p); return 1; }
    char q[1400];
    path_join(q, sizeof(q), p, "PARAM.SFO");
    if (!vfs_exists(q)) return 0;
    snprintf(out, n, "%s", q);
    return 1;
}

static int cursor_is_pkg_folder(void)
{
    char t[1400];
    return ent_count && ents[sel].is_dir && cursor_sfo(t, sizeof(t));
}

/* where tool output goes: this folder if writable, otherwise the HDD */
static const char *output_dir(void) { return cur_writable() ? cwd : "/dev_hdd0/packages"; }

static int menu_enabled(int m)
{
    int has = ent_count > 0;
    switch (m) {
        case M_OPEN: return has && ents[sel].is_dir && !marked_count();
        case M_INSTALL: return count_targets(1, NULL) > 0;
        case M_EXTRACT: return has && !marked_count() && ents[sel].type == FT_PKG;
        case M_BUILD: return cursor_is_pkg_folder();
        case M_EDITSFO: { char t[1400]; return cursor_sfo(t, sizeof(t)); }
        case M_COPY: return has;
        case M_CUT: case M_DELETE: return has && cur_writable();
        case M_PASTE: return clip_n > 0 && cur_writable();
        case M_RENAME: return has && cur_writable() && marked_count() <= 1;
        case M_NEWDIR: return cur_writable();
        case M_SELALL: case M_PROPS: return has;
    }
    return 1;
}

static void build_menu(void)
{
    menu_n = 0;
    for (int m = 0; m < M_COUNT; m++) {
        if (m == M_OPEN && !(ent_count && ents[sel].is_dir)) continue;
        if (m == M_INSTALL && !count_targets(1, NULL)) continue;
        if (m == M_EXTRACT && !(ent_count && ents[sel].type == FT_PKG && !marked_count())) continue;
        if (m == M_BUILD && !cursor_is_pkg_folder()) continue;
        if (m == M_EDITSFO) { char t[1400]; if (!cursor_sfo(t, sizeof(t))) continue; }
        if (m == M_PASTE && !clip_n) continue;
        menu_items[menu_n++] = m;
    }
    menu_sel = 0;
    while (menu_sel < menu_n - 1 && !menu_enabled(menu_items[menu_sel])) menu_sel++;
}

static void draw_menu(void)
{
    menu_anim = approach(menu_anim, 1.0f, 0.25f);
    float w = 250, ih = 30, h = menu_n * ih + 24;
    if (h > 430) h = 430;
    float x = SCR_W - 24 - w + (1 - menu_anim) * 60, y = 64;
    if (RTL()) x = 24 - (1 - menu_anim) * 60;
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA8(4, 4, 14, (int)(110 * menu_anim)));
    gfx_rrect_grad(x, y, w, h, 18, RGBA8(40, 44, 92, 245), RGBA8(26, 20, 58, 245));
    gfx_rrect_border(x, y, w, h, 18, 1.2f, RGBA8(255, 255, 255, 50));
    for (int i = 0; i < menu_n; i++) {
        float iy = y + 12 + i * ih;
        int m = menu_items[i], en = menu_enabled(m);
        if (i == menu_sel) {
            gfx_rrect_grad(x + 8, iy, w - 16, ih - 3, 10, WITH_ALPHA(C_ACC1, 220), WITH_ALPHA(C_ACC2, 220));
        }
        uint32_t c = en ? C_TEXT : RGBA8(120, 124, 160, 200);
        if (m == M_DELETE && en) c = i == menu_sel ? C_TEXT : RGBA8(255, 130, 140, 255);
        float tx = RTL() ? x + w - 22 : x + 22;
        gfx_text(tx, iy + 7, T(menu_label[m]), 13, i == menu_sel, c, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
    }
}

/* ---------- dialogs ---------- */
static void draw_confirm_install(void)
{
    float x, y, w = 470, h = 250;
    draw_modal_frame(w, h, &x, &y);
    uint64_t bytes; int n = count_targets(1, &bytes);
    char title[96], sz[32], info[200];
    if (n == 1) snprintf(title, sizeof(title), "%s", T(S_CONFIRM_ONE)); else snprintf(title, sizeof(title), T(S_CONFIRM_MULTI), n);
    gfx_text(SCR_W / 2, y + 22, title, 20, 1, C_TEXT, ALIGN_CENTER);
    entry_t *e = (n == 1 && !marked_count() && sel < ent_count) ? &ents[sel] : NULL;
    if (e && e->icon) gfx_texture(e->icon, SCR_W / 2 - 80, y + 58, 160, 88, RGBA8(255, 255, 255, 255));
    else draw_file_icon(MXP(SCR_W / 2) - (RTL() ? -34 : 34), y + 66, 68, FT_PKG, "x.pkg");
    if (e) gfx_text_fit(SCR_W / 2, y + 152, w - 60, e->info.title[0] ? e->info.title : e->name, 14, 1, C_TEXT, ALIGN_CENTER);
    fmt_size(bytes, sz, sizeof(sz));
    snprintf(info, sizeof(info), "%s  \xC2\xB7  %s: %s", cfg.mode == MODE_XMB_QUEUE ? T(S_MODE_XMB) : T(S_MODE_PKGS), T(S_TOTAL), sz);
    gfx_text_fit(SCR_W / 2, y + 174, w - 40, info, 11, 0, C_DIM, ALIGN_CENTER);
    draw_button_row(SCR_W / 2, y + h - 56, T(S_CONFIRM_BTN), T(S_CANCEL), confirm_sel, C_ACC1);
}

static void draw_confirm_delete(void)
{
    float x, y, w = 470, h = 230;
    draw_modal_frame(w, h, &x, &y);
    int n = count_targets(0, NULL);
    char t[96]; snprintf(t, sizeof(t), T(S_CONFIRM_DELETE), n);
    draw_result_icon(SCR_W / 2, y + 52, 0, C_BAD);
    gfx_text(SCR_W / 2, y + 92, t, 19, 1, C_TEXT, ALIGN_CENTER);
    if (n == 1 && !marked_count()) gfx_text_fit(SCR_W / 2, y + 118, w - 60, ents[sel].name, 12, 0, C_DIM, ALIGN_CENTER);
    int sys = is_system_path(cwd);
    gfx_text(SCR_W / 2, y + 138, sys ? T(S_SYSTEM_WARN) : T(S_DELETE_HINT), 12, sys, sys ? RGBA8(255, 170, 80, 255) : C_DIM, ALIGN_CENTER);
    draw_button_row(SCR_W / 2, y + h - 56, T(S_DELETE_ACT), T(S_CANCEL), confirm_sel, C_BAD);
}

static void draw_progress_install(void)
{
    const inst_status_t *s = inst_status();
    float x, y, w = 540, h = 250;
    draw_modal_frame(w, h, &x, &y);
    gfx_text(SCR_W / 2, y + 20, T(S_INSTALLING), 20, 1, C_TEXT, ALIGN_CENTER);
    char nfo[64]; snprintf(nfo, sizeof(nfo), T(S_FILE_OF), s->current + 1, s->count);
    gfx_text(SCR_W / 2, y + 48, nfo, 12, 0, C_DIM, ALIGN_CENTER);
    gfx_text_fit(SCR_W / 2, y + 74, w - 60, s->current_name, 15, 1, C_TEXT, ALIGN_CENTER);
    float pct = s->total_bytes ? (float)s->done_bytes / (float)s->total_bytes : 0;
    static float shown = 0;
    draw_progress_bar(x + 40, y + 112, w - 80, pct, &shown);
    char pc[16]; snprintf(pc, sizeof(pc), "%d%%", (int)(pct * 100));
    gfx_text(SCR_W / 2, y + 136, pc, 26, 1, C_TEXT, ALIGN_CENTER);
    char done[32], tot[32], spd[32], eta[48] = "", line[200];
    fmt_size(s->done_bytes, done, sizeof(done)); fmt_size(s->total_bytes, tot, sizeof(tot)); fmt_size(s->speed, spd, sizeof(spd));
    if (s->speed > 0) { char t[24]; fmt_time((uint32_t)((s->all_total - s->all_done) / s->speed), t, sizeof(t)); snprintf(eta, sizeof(eta), T(S_LEFT), t); }
    snprintf(line, sizeof(line), "%s / %s   \xC2\xB7   %s/s   \xC2\xB7   %s", done, tot, spd, eta);
    gfx_text(SCR_W / 2, y + 174, line, 12, 0, C_DIM, ALIGN_CENTER);
    float hx = SCR_W / 2 - 40, bwi = gfx_button_icon(hx, y + h - 30, cancel_icon, 18);
    gfx_text(hx + bwi + 6, y + h - 29, T(S_CANCEL), 12, 0, C_DIM, ALIGN_LEFT);
}

static void draw_progress_ops(void)
{
    const ops_status_t *s = ops_status();
    float x, y, w = 540, h = 250;
    draw_modal_frame(w, h, &x, &y);
    const char *t = s->op == OP_DELETE ? T(S_DELETING) : s->op == OP_MOVE ? T(S_MOVING) :
                    s->op == OP_EXTRACT ? T(S_EXTRACTING) : s->op == OP_BUILD ? T(S_BUILDING) : T(S_COPYING);
    gfx_text(SCR_W / 2, y + 20, t, 20, 1, C_TEXT, ALIGN_CENTER);
    char nfo[64];
    if (s->op == OP_EXTRACT || s->op == OP_BUILD) snprintf(nfo, sizeof(nfo), T(S_FILES), (int)s->files_done);
    else snprintf(nfo, sizeof(nfo), T(S_FILE_OF), (int)s->files_done, (int)s->files_total);
    gfx_text(SCR_W / 2, y + 48, nfo, 12, 0, C_DIM, ALIGN_CENTER);
    gfx_text_fit(SCR_W / 2, y + 74, w - 60, s->current, 15, 1, C_TEXT, ALIGN_CENTER);
    float pct = s->op == OP_DELETE ? (s->files_total ? (float)s->files_done / s->files_total : 0)
                                   : (s->bytes_total ? (float)s->bytes_done / (float)s->bytes_total : 0);
    if (pct > 1) pct = 1;
    static float shown = 0;
    draw_progress_bar(x + 40, y + 112, w - 80, pct, &shown);
    char pc[16]; snprintf(pc, sizeof(pc), "%d%%", (int)(pct * 100));
    gfx_text(SCR_W / 2, y + 136, pc, 26, 1, C_TEXT, ALIGN_CENTER);
    if (s->op != OP_DELETE) {
        char done[32], tot[32], spd[32], eta[48] = "", line[200];
        fmt_size(s->bytes_done, done, sizeof(done)); fmt_size(s->bytes_total, tot, sizeof(tot)); fmt_size(s->speed, spd, sizeof(spd));
        if (s->speed > 0 && s->bytes_total > s->bytes_done) { char tt[24]; fmt_time((uint32_t)((s->bytes_total - s->bytes_done) / s->speed), tt, sizeof(tt)); snprintf(eta, sizeof(eta), T(S_LEFT), tt); }
        snprintf(line, sizeof(line), "%s / %s   \xC2\xB7   %s/s   \xC2\xB7   %s", done, tot, spd, eta);
        gfx_text(SCR_W / 2, y + 174, line, 12, 0, C_DIM, ALIGN_CENTER);
    }
    float hx = SCR_W / 2 - 40, bwi = gfx_button_icon(hx, y + h - 30, cancel_icon, 18);
    gfx_text(hx + bwi + 6, y + h - 29, T(S_CANCEL), 12, 0, C_DIM, ALIGN_LEFT);
}

static void draw_result_install(void)
{
    const inst_status_t *s = inst_status();
    float x, y, w = 540, h = 250;
    draw_modal_frame(w, h, &x, &y);
    float cx = SCR_W / 2;
    if (s->state == INST_DONE) {
        draw_result_icon(cx, y + 62, 1, C_OKC);
        gfx_text(cx, y + 100, T(S_DONE_TITLE), 20, 1, C_TEXT, ALIGN_CENTER);
        draw_lines(cx, y + 130, cfg.mode == MODE_XMB_QUEUE ? T(S_DONE_XMB) : T(S_DONE_PKGS), 12, 0, C_DIM, ALIGN_CENTER, 18);
        draw_button_row(cx, y + h - 56, T(S_EXIT_NOW), T(S_STAY), result_sel, C_ACC1);
    } else {
        int canc = s->state == INST_CANCELLED;
        draw_result_icon(cx, y + 62, 0, canc ? RGBA8(255, 180, 60, 255) : C_BAD);
        gfx_text(cx, y + 100, canc ? T(S_CANCELLED) : T(S_ERR_TITLE), 20, 1, C_TEXT, ALIGN_CENTER);
        if (!canc) {
            static const int map[] = { S_ERR_MEM, S_ERR_SPACE, S_ERR_MKDIR, S_ERR_OPEN_SRC, S_ERR_OPEN_DST, S_ERR_READ, S_ERR_WRITE, S_ERR_PDB, S_ERR_MEM };
            int ec = s->error_code >= 0 && s->error_code < 9 ? s->error_code : 8;
            gfx_text(cx, y + 132, T(map[ec]), 13, 0, C_DIM, ALIGN_CENTER);
            if (s->ok_count > 0) { char p[96]; snprintf(p, sizeof(p), T(S_PARTIAL), s->ok_count, s->count); gfx_text(cx, y + 152, p, 12, 0, C_DIM, ALIGN_CENTER); }
        }
        draw_button_row(cx, y + h - 56, T(S_CLOSE), NULL, 0, C_ACC1);
    }
}

static void draw_result_ops(void)
{
    const ops_status_t *s = ops_status();
    float x, y, w = 520, h = 230;
    draw_modal_frame(w, h, &x, &y);
    float cx = SCR_W / 2;
    if (s->state == OPS_DONE) {
        draw_result_icon(cx, y + 62, 1, C_OKC);
        gfx_text(cx, y + 100, T(S_OPS_DONE), 20, 1, C_TEXT, ALIGN_CENTER);
        char nfo[64]; snprintf(nfo, sizeof(nfo), T(S_FILES), (int)s->files_done);
        gfx_text(cx, y + 130, nfo, 12, 0, C_DIM, ALIGN_CENTER);
        if (s->op == OP_EXTRACT || s->op == OP_BUILD) {
            char o[1500]; snprintf(o, sizeof(o), "%s: %s", T(S_OUTPUT), ops_output);
            gfx_text_fit(cx, y + 150, w - 50, o, 11, 0, C_DIM, ALIGN_CENTER);
        }
    } else {
        int canc = s->state == OPS_CANCELLED;
        draw_result_icon(cx, y + 62, 0, canc ? RGBA8(255, 180, 60, 255) : C_BAD);
        gfx_text(cx, y + 100, canc ? T(S_CANCELLED) : T(S_OPS_FAILED), 20, 1, C_TEXT, ALIGN_CENTER);
        if (!canc) {
            static const int map[] = { S_ERR_MEM, S_E_SPACE_DST, S_ERR_OPEN_SRC, S_E_OPEN_DST, S_ERR_READ, S_ERR_WRITE, S_MKDIR_FAIL, S_E_DELETE, S_E_INTO_SELF, S_ERR_MEM };
            int ec = s->error >= 0 && s->error < 10 ? s->error : 9;
            gfx_text(cx, y + 128, T(map[ec]), 13, 0, C_DIM, ALIGN_CENTER);
            if (s->error_path[0]) gfx_text_fit(cx, y + 148, w - 50, s->error_path, 10, 0, C_DIM, ALIGN_CENTER);
        }
    }
    draw_button_row(cx, y + h - 56, T(S_CLOSE), NULL, 0, C_ACC1);
}

static void draw_props(void)
{
    float x, y, w = 520, h = 290;
    draw_modal_frame(w, h, &x, &y);
    gfx_text(SCR_W / 2, y + 18, T(S_PROPS), 19, 1, C_TEXT, ALIGN_CENTER);
    if (!ent_count) return;
    entry_t *e = &ents[sel];
    if (e->type == FT_PKG && e->icon) gfx_texture(e->icon, SCR_W / 2 - 64, y + 50, 128, 70, RGBA8(255, 255, 255, 255));
    else draw_file_icon(MXP(SCR_W / 2) - (RTL() ? -28 : 28), y + 56, 56, e->type, e->name);

    char full[1400], sz[32], cnt[64], tbuf[64];
    path_join(full, sizeof(full), cwd, e->name);
    const char *rows[6][2];
    int n = 0;
    rows[n][0] = T(S_NAME); rows[n][1] = e->name; n++;
    if (e->is_dir) {
        rows[n][0] = T(S_TYPE); rows[n][1] = T(S_FOLDER_TYPE); n++;
    } else {
        fmt_size(e->size, sz, sizeof(sz));
        rows[n][0] = T(S_SIZE); rows[n][1] = sz; n++;
        snprintf(tbuf, sizeof(tbuf), "%s", e->type == FT_PKG && e->parsed ? pkg_type_label(e->info.content_type) : (ext_of(e->name)[0] ? ext_of(e->name) : T(S_FILE_TYPE)));
        rows[n][0] = T(S_TYPE); rows[n][1] = tbuf; n++;
        if (e->type == FT_PKG && e->parsed) {
            rows[n][0] = "Content ID"; rows[n][1] = e->info.content_id; n++;
            if (e->info.title[0]) { snprintf(cnt, sizeof(cnt), "%s  v%s", e->info.title_id, e->info.version); rows[n][0] = "Title ID"; rows[n][1] = cnt; n++; }
        }
    }
    rows[n][0] = T(S_LOCATION); rows[n][1] = cwd; n++;
    for (int i = 0; i < n; i++) {
        float ry = y + 132 + i * 24;
        float lx = RTL() ? x + w - 30 : x + 30, vx = RTL() ? x + w - 150 : x + 150;
        gfx_text(lx, ry, rows[i][0], 12, 1, C_DIM, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
        gfx_text_fit(vx, ry, w - 180, rows[i][1], 12, 0, C_TEXT, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
    }
}

static void draw_sfo(void)
{
    float x, y, w = 560, h = 380;
    draw_modal_frame(w, h, &x, &y);
    gfx_text(SCR_W / 2, y + 16, T(S_SFO_TITLE), 19, 1, C_TEXT, ALIGN_CENTER);
    gfx_text_fit(SCR_W / 2, y + 42, w - 40, sfo_path, 10, 0, C_DIM, ALIGN_CENTER);
    int visible = 9, top = sfo_sel - visible + 1;
    if (top < 0) top = 0;
    for (int i = top; i < sfo_count && i < top + visible; i++) {
        float iy = y + 64 + (i - top) * 33, ix = x + 20, iw = w - 40;
        if (i == sfo_sel) gfx_rrect_grad(ix, iy, iw, 29, 10, WITH_ALPHA(C_ACC1, 210), WITH_ALPHA(C_ACC2, 210));
        else gfx_rrect(ix, iy, iw, 29, 10, RGBA8(255, 255, 255, 10));
        float lx = RTL() ? ix + iw - 14 : ix + 14, vx = RTL() ? ix + 14 : ix + iw - 14;
        gfx_text(lx, iy + 7, sfo_fields[i].key, 12, 1, sfo_fields[i].is_string ? C_TEXT : C_DIM, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
        gfx_text_fit(vx, iy + 7, iw - 200, sfo_fields[i].value, 12, 0, i == sfo_sel ? C_TEXT : C_DIM, RTL() ? ALIGN_LEFT : ALIGN_RIGHT);
    }
}

#define SET_ITEMS 5
static void draw_settings(void)
{
    float x, y, w = 520, h = 330;
    draw_modal_frame(w, h, &x, &y);
    gfx_text(SCR_W / 2, y + 20, T(S_SETTINGS), 20, 1, C_TEXT, ALIGN_CENTER);
    const int labels[SET_ITEMS] = { S_SET_LANG, S_SET_MODE, S_SET_DELETE, S_SET_ABOUT, S_SET_EXIT };
    const char *vals[SET_ITEMS] = {
        T(S_LANG_NAME), cfg.mode == MODE_XMB_QUEUE ? T(S_MODE_XMB) : T(S_MODE_PKGS),
        cfg.del ? T(S_DELETE) : T(S_KEEP), T(S_ABOUT_TXT), NULL
    };
    for (int i = 0; i < SET_ITEMS; i++) {
        float iy = y + 60 + i * 48, ix = x + 24, iw = w - 48;
        if (i == set_sel) {
            gfx_rrect_grad(ix, iy, iw, 40, 12, WITH_ALPHA(C_ACC1, 210), WITH_ALPHA(C_ACC2, 210));
            gfx_rrect_border(ix, iy, iw, 40, 12, 1.2f, RGBA8(255, 255, 255, 120));
        } else gfx_rrect(ix, iy, iw, 40, 12, RGBA8(255, 255, 255, 12));
        float lx = RTL() ? ix + iw - 16 : ix + 16, vx = RTL() ? ix + 16 : ix + iw - 16;
        gfx_text(lx, iy + 11, T(labels[i]), 14, 1, C_TEXT, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
        if (vals[i]) {
            char v[160];
            if (i < 3) snprintf(v, sizeof(v), "\xE2\x80\xB9  %s  \xE2\x80\xBA", vals[i]); else snprintf(v, sizeof(v), "%s", vals[i]);
            gfx_text(vx, iy + 12, v, 12, 0, i == set_sel ? C_TEXT : C_DIM, RTL() ? ALIGN_LEFT : ALIGN_RIGHT);
        }
    }
}

static void draw_scanning(void)
{
    float x, y;
    draw_modal_frame(360, 130, &x, &y);
    float cx = SCR_W / 2, cy = y + 46, a = gfx_frame() * 0.12f;
    for (int i = 0; i < 8; i++) {
        float aa = a + i * (float)M_PI / 4;
        gfx_circle(cx + cosf(aa) * 16, cy + sinf(aa) * 16, 2.5f + i * 0.35f, RGBA8(120, 160, 255, 60 + i * 24));
    }
    gfx_text(cx, y + 84, T(S_DEVICES_SCANNING), 14, 1, C_TEXT, ALIGN_CENTER);
}

static void draw_toast(void)
{
    if (gfx_frame() >= toast_until || !toast[0]) return;
    float left = (float)(toast_until - gfx_frame());
    int a = left < 20 ? (int)(left / 20 * 255) : 255;
    float tw = gfx_text_width(toast, 13, 1) + 40;
    gfx_rrect(SCR_W / 2 - tw / 2, 60, tw, 32, 16, RGBA8(30, 34, 70, a * 230 / 255));
    gfx_rrect_border(SCR_W / 2 - tw / 2, 60, tw, 32, 16, 1, RGBA8(255, 255, 255, a / 4));
    gfx_text(SCR_W / 2, 68, toast, 13, 1, WITH_ALPHA(C_TEXT, a), ALIGN_CENTER);
}

/* =========================================================== actions */
static void do_rescan(void)
{
    char cur_root[64] = "";
    if (vol_count) snprintf(cur_root, sizeof(cur_root), "%s", vols[vol_sel].root);
    vol_count = vfs_scan(vols, MAX_VOL);
    native_usb_mask = usb_mask();
    int idx = 0;
    for (int i = 0; i < vol_count; i++) if (!strcmp(vols[i].root, cur_root)) idx = i;
    if (!cur_root[0]) {
        /* first start: first USB drive, otherwise the packages folder */
        idx = -1;
        for (int i = 0; i < vol_count && idx < 0; i++) if (vols[i].kind == 1) idx = i;
        for (int i = 0; i < vol_count && idx < 0; i++) if (vols[i].kind == 4) idx = i;
        if (idx < 0) idx = 0;
    }
    select_volume(idx);
}

static void start_install(void)
{
    static inst_job_t jobs[MAX_JOBS];
    int n = 0, mk = marked_count();
    for (int i = 0; i < ent_count && n < MAX_JOBS; i++) {
        entry_t *e = &ents[i];
        if (e->type != FT_PKG || !(mk ? e->marked : i == sel)) continue;
        inst_job_t *j = &jobs[n++];
        memset(j, 0, sizeof(*j));
        path_join(j->path, sizeof(j->path), cwd, e->name);
        snprintf(j->name, sizeof(j->name), "%s", e->name);
        snprintf(j->title, sizeof(j->title), "%s", e->parsed && e->info.title[0] ? e->info.title : e->name);
        j->size = e->size;
        if (e->parsed && e->info.icon && e->info.icon_size) {
            j->icon = malloc(e->info.icon_size);
            if (j->icon) { memcpy(j->icon, e->info.icon, e->info.icon_size); j->icon_size = e->info.icon_size; }
        }
    }
    if (n && inst_start(jobs, n, cfg.mode, cfg.del)) open_modal(UI_PROGRESS_INSTALL);
}

static void refresh_keep(void)
{
    char keep[256] = "";
    if (sel < ent_count) snprintf(keep, sizeof(keep), "%s", ents[sel].name);
    open_dir(cwd, keep);
    if (vol_count) vol_free = vfs_free_space(vols[vol_sel].root);
}

static void run_menu_action(int m)
{
    ui = UI_BROWSE;
    switch (m) {
    case M_OPEN: { char p[1400]; path_join(p, sizeof(p), cwd, ents[sel].name); open_dir(p, NULL); break; }
    case M_INSTALL: confirm_sel = 0; open_modal(UI_CONFIRM_INSTALL); break;
    case M_EXTRACT: {
        char src[1400];
        path_join(src, sizeof(src), cwd, ents[sel].name);
        const char *od = output_dir();
        vfs_mkdir(od);
        snprintf(ops_output, sizeof(ops_output), "%s", od);
        if (ops_start_extract(src, od)) open_modal(UI_PROGRESS_OPS);
        break;
    }
    case M_BUILD: {
        char folder[1400], cid[64];
        path_join(folder, sizeof(folder), cwd, ents[sel].name);
        if (!pkgtool_content_id_for(folder, cid, sizeof(cid))) { show_toast(T(S_NO_CID)); break; }
        const char *od = output_dir();
        vfs_mkdir(od);
        char outp[1400], fn[80];
        snprintf(fn, sizeof(fn), "%s.pkg", cid);
        path_join(outp, sizeof(outp), od, fn);
        snprintf(ops_output, sizeof(ops_output), "%s", outp);
        if (ops_start_build(folder, outp, cid)) open_modal(UI_PROGRESS_OPS);
        break;
    }
    case M_EDITSFO:
        if (cursor_sfo(sfo_path, sizeof(sfo_path)) && (sfo_count = sfo_load(sfo_path, sfo_fields, SFO_MAX_FIELDS)) > 0) {
            sfo_sel = 0; open_modal(UI_SFO);
        }
        break;
    case M_COPY: case M_CUT:
        clip_n = collect_targets(clip, OPS_MAX_SRC, 0);
        clip_op = (m == M_CUT) ? OP_MOVE : OP_COPY;
        for (int i = 0; i < ent_count; i++) ents[i].marked = 0;
        { char t[96]; snprintf(t, sizeof(t), clip_op == OP_MOVE ? T(S_CLIP_CUT) : T(S_CLIP_COPY), clip_n); show_toast(t); }
        break;
    case M_PASTE:
        if (ops_start(clip_op, clip, clip_n, cwd)) open_modal(UI_PROGRESS_OPS);
        break;
    case M_RENAME:
        if (!ent_count) break;
        if (marked_count() == 1) for (int i = 0; i < ent_count; i++) if (ents[i].marked) sel = i;
        snprintf(osk_old, sizeof(osk_old), "%s", ents[sel].name);
        osk_purpose = 0;
        if (osk_open(T(S_RENAME), osk_old)) ui = UI_OSK;
        break;
    case M_NEWDIR:
        osk_purpose = 1;
        if (osk_open(T(S_NEWFOLDER), T(S_NEW_FOLDER_NAME))) ui = UI_OSK;
        break;
    case M_DELETE: confirm_sel = 1; open_modal(UI_CONFIRM_DELETE); break;
    case M_SELALL: {
        int any = marked_count();
        for (int i = 0; i < ent_count; i++) ents[i].marked = !any;
        break;
    }
    case M_PROPS: open_modal(UI_PROPS); break;
    case M_REFRESH: ui = UI_SCANNING; break;
    case M_SETTINGS: set_sel = 0; open_modal(UI_SETTINGS); break;
    }
}

static void handle_browse(void)
{
    uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;

    if (in_pressed & (B_L1 | B_R1)) select_volume(vol_sel + ((in_pressed & B_L1) ? -1 : 1));
    if (in_pressed & B_TRI) { build_menu(); menu_anim = 0; ui = UI_MENU; return; }
    if (in_pressed & B_START) { set_sel = 0; open_modal(UI_SETTINGS); return; }
    if (in_pressed & B_R3) { ui = UI_SCANNING; return; }

    if (!focus_list) {
        if (in_active & B_UP) select_volume(vol_sel - 1);
        if (in_active & B_DOWN) select_volume(vol_sel + 1);
        if (in_pressed & (R | B_OK)) focus_list = 1;
        return;
    }

    if (in_pressed & L) { go_up(); return; }
    if (in_pressed & B_CANCEL) { go_up(); return; }
    if (!ent_count) return;

    if (in_active & B_UP)   sel = sel > 0 ? sel - 1 : ent_count - 1;
    if (in_active & B_DOWN) sel = sel < ent_count - 1 ? sel + 1 : 0;
    if (in_active & B_L2)   { sel -= 7; if (sel < 0) sel = 0; }
    if (in_active & B_R2)   { sel += 7; if (sel >= ent_count) sel = ent_count - 1; }

    entry_t *e = &ents[sel];
    if (in_pressed & B_SQU) { e->marked = !e->marked; if (sel < ent_count - 1) sel++; }
    if (in_pressed & B_SELECT) { int any = marked_count(); for (int i = 0; i < ent_count; i++) ents[i].marked = !any; }
    if (in_pressed & R) {
        if (e->is_dir) { char p[1400]; path_join(p, sizeof(p), cwd, e->name); open_dir(p, NULL); }
    }
    if (in_pressed & B_OK) {
        if (e->is_dir) { char p[1400]; path_join(p, sizeof(p), cwd, e->name); open_dir(p, NULL); }
        else if (e->type == FT_PKG) { confirm_sel = 0; open_modal(UI_CONFIRM_INSTALL); }
        else if (!strcasecmp(e->name, "PARAM.SFO")) run_menu_action(M_EDITSFO);
        else open_modal(UI_PROPS);
    }
}

static void handle_settings(void)
{
    if (in_active & B_UP) set_sel = (set_sel + SET_ITEMS - 1) % SET_ITEMS;
    if (in_active & B_DOWN) set_sel = (set_sel + 1) % SET_ITEMS;
    if (in_pressed & (B_LEFT | B_RIGHT | B_OK)) {
        if (set_sel == 0) { cfg.lang = !cfg.lang; g_lang = cfg.lang; save_settings(); }
        else if (set_sel == 1) { cfg.mode = !cfg.mode; save_settings(); }
        else if (set_sel == 2) { cfg.del = !cfg.del; save_settings(); }
        else if (set_sel == 4 && (in_pressed & B_OK)) quit_req = 1;
    }
    if (in_pressed & (B_CANCEL | B_START)) ui = UI_BROWSE;
}

static void handle_osk(void)
{
    char out[512];
    int r = osk_update(out, sizeof(out));
    if (r == OSK_RESULT_NONE) return;
    if (osk_purpose == 2) {
        ui = UI_SFO; modal_anim = 1.0f;
        if (r != OSK_RESULT_OK) return;
        if (sfo_set_string(sfo_path, sfo_fields[sfo_sel].key, out) == 0) {
            show_toast(T(S_SFO_SAVED));
            sfo_count = sfo_load(sfo_path, sfo_fields, SFO_MAX_FIELDS);
        } else show_toast(T(S_SFO_TOO_LONG));
        return;
    }
    ui = UI_BROWSE;
    if (r != OSK_RESULT_OK || !out[0] || strchr(out, '/')) return;
    char a[1400], b[1400];
    path_join(b, sizeof(b), cwd, out);
    if (osk_purpose == 0) {
        if (!strcmp(out, osk_old)) return;
        path_join(a, sizeof(a), cwd, osk_old);
        if (vfs_rename(a, b) != 0) show_toast(T(S_RENAME_FAIL));
    } else if (vfs_mkdir(b) != 0) show_toast(T(S_MKDIR_FAIL));
    open_dir(cwd, out);
}

/* =========================================================== main */
int main(void)
{
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_cb, NULL);
    s32 assign = 1;
    sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN, &assign);
    if (assign == 0) { B_OK = B_CIR; B_CANCEL = B_CRO; ok_icon = BTN_CIRCLE; cancel_icon = BTN_CROSS; }

    mkdir(APP_DIR, 0777);
    load_settings();
    gfx_init(APP_DIR "/fonts/DejaVuSans.ttf", APP_DIR "/fonts/DejaVuSans-Bold.ttf");
    inst_set_default_icon(ICONFILE_png, ICONFILE_png_size);
    vfs_init();

    ya2d_paddata[0].ANA_L_H = 0x80;
    ya2d_paddata[0].ANA_L_V = 0x80;
    ents = calloc(MAX_ENTRIES, sizeof(entry_t));
    raw = calloc(MAX_ENTRIES, sizeof(vfs_entry_t));
    clip = calloc(OPS_MAX_SRC, 1024);

    int scan_frames = 0;
    uint64_t next_usb_check = 0;

    while (!quit_req) {
        sysUtilCheckCallback();
        read_input();
        float t = gfx_frame() / 60.0f;

        /* hot-plug: re-scan when the set of FAT32 USB drives changes */
        if (ui == UI_BROWSE && gfx_frame() >= next_usb_check) {
            if (usb_mask() != native_usb_mask) ui = UI_SCANNING;
            next_usb_check = gfx_frame() + 120;
        }

        switch (ui) {
        case UI_SCANNING:
            /* draw the spinner for a couple of frames, then do the (blocking) scan */
            if (++scan_frames >= 3) { scan_frames = 0; do_rescan(); ui = UI_BROWSE; }
            break;
        case UI_BROWSE: handle_browse(); break;
        case UI_MENU:
            if (in_active & (B_UP | B_DOWN)) {
                int d = (in_active & B_UP) ? menu_n - 1 : 1;
                for (int tries = 0; tries < menu_n; tries++) {
                    menu_sel = (menu_sel + d) % menu_n;
                    if (menu_enabled(menu_items[menu_sel])) break;
                }
            }
            if (in_pressed & (B_CANCEL | B_TRI)) ui = UI_BROWSE;
            else if ((in_pressed & B_OK) && menu_enabled(menu_items[menu_sel])) run_menu_action(menu_items[menu_sel]);
            break;
        case UI_CONFIRM_INSTALL: case UI_CONFIRM_DELETE: {
            uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
            if (in_pressed & (L | R)) confirm_sel = !confirm_sel;
            if (in_pressed & B_CANCEL) ui = UI_BROWSE;
            else if (in_pressed & B_OK) {
                if (confirm_sel != 0) ui = UI_BROWSE;
                else if (ui == UI_CONFIRM_INSTALL) start_install();
                else {
                    static char targets[OPS_MAX_SRC][1024];
                    int n = collect_targets(targets, OPS_MAX_SRC, 0);
                    if (ops_start(OP_DELETE, targets, n, NULL)) open_modal(UI_PROGRESS_OPS); else ui = UI_BROWSE;
                }
            }
            break;
        }
        case UI_PROGRESS_INSTALL:
            if (in_pressed & B_CANCEL) inst_cancel();
            if (inst_status()->state != INST_RUNNING) { result_sel = 0; open_modal(UI_RESULT_INSTALL); }
            break;
        case UI_PROGRESS_OPS:
            if (in_pressed & B_CANCEL) ops_cancel();
            if (ops_status()->state != OPS_RUNNING) open_modal(UI_RESULT_OPS);
            break;
        case UI_RESULT_INSTALL: {
            const inst_status_t *s = inst_status();
            uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
            if (s->state == INST_DONE && (in_pressed & (L | R))) result_sel = !result_sel;
            if (in_pressed & (B_OK | B_CANCEL)) {
                int exit_now = s->state == INST_DONE && result_sel == 0 && (in_pressed & B_OK);
                inst_reset();
                for (int i = 0; i < ent_count; i++) ents[i].marked = 0;
                if (exit_now) quit_req = 1; else refresh_keep();
                ui = UI_BROWSE;
            }
            break;
        }
        case UI_RESULT_OPS:
            if (in_pressed & (B_OK | B_CANCEL)) {
                const ops_status_t *s = ops_status();
                if (s->op == OP_MOVE && s->state == OPS_DONE) clip_n = 0;
                ops_reset();
                for (int i = 0; i < ent_count; i++) ents[i].marked = 0;
                refresh_keep();
                ui = UI_BROWSE;
            }
            break;
        case UI_PROPS: if (in_pressed & (B_OK | B_CANCEL)) ui = UI_BROWSE; break;
        case UI_SFO:
            if (in_active & B_UP) sfo_sel = (sfo_sel + sfo_count - 1) % sfo_count;
            if (in_active & B_DOWN) sfo_sel = (sfo_sel + 1) % sfo_count;
            if (in_pressed & B_CANCEL) { ui = UI_BROWSE; refresh_keep(); }
            else if ((in_pressed & B_OK) && sfo_fields[sfo_sel].is_string && cur_writable()) {
                osk_purpose = 2;
                if (osk_open(sfo_fields[sfo_sel].key, sfo_fields[sfo_sel].value)) ui = UI_OSK;
            }
            break;
        case UI_SETTINGS: handle_settings(); break;
        case UI_OSK: handle_osk(); break;
        }

        if (ui == UI_BROWSE) lazy_parse();

        gfx_begin_frame();
        draw_background(t);
        draw_header();
        draw_sidebar();
        draw_list();

        if (ui == UI_BROWSE || ui == UI_MENU) {
            hint_t h[7]; int n = 0;
            if (focus_list && ent_count) {
                entry_t *e = &ents[sel];
                h[n++] = (hint_t){ ok_icon, e->is_dir ? S_OPEN : e->type == FT_PKG ? S_INSTALL : S_PROPS };
            }
            h[n++] = (hint_t){ cancel_icon, S_BACK };
            if (focus_list && ent_count) h[n++] = (hint_t){ BTN_SQUARE, S_MARK };
            h[n++] = (hint_t){ BTN_TRIANGLE, S_MENU };
            h[n++] = (hint_t){ BTN_L1, S_SWITCH };
            h[n++] = (hint_t){ BTN_START, S_SETTINGS };
            draw_hints(h, n);
        }
        switch (ui) {
            case UI_MENU: draw_menu(); break;
            case UI_CONFIRM_INSTALL: draw_confirm_install(); break;
            case UI_CONFIRM_DELETE: draw_confirm_delete(); break;
            case UI_PROGRESS_INSTALL: draw_progress_install(); break;
            case UI_PROGRESS_OPS: draw_progress_ops(); break;
            case UI_RESULT_INSTALL: draw_result_install(); break;
            case UI_RESULT_OPS: draw_result_ops(); break;
            case UI_PROPS: draw_props(); break;
            case UI_SFO: draw_sfo(); break;
            case UI_OSK: if (osk_purpose == 2) draw_sfo(); break;
            case UI_SETTINGS: draw_settings(); break;
            case UI_SCANNING: draw_scanning(); break;
        }
        draw_toast();
        gfx_end_frame();
    }

    if (inst_status()->state == INST_RUNNING) { inst_cancel(); while (inst_status()->state == INST_RUNNING) usleep(10000); }
    if (ops_status()->state == OPS_RUNNING) { ops_cancel(); while (ops_status()->state == OPS_RUNNING) usleep(10000); }
    free_entries();
    vfs_shutdown();
    gfx_shutdown();
    sysProcessExit(0);
    return 0;
}

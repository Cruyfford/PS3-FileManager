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
#include <sys/thread.h>
#include <sysutil/sysutil.h>
#include <lv2/process.h>
#include <ppu-lv2.h>
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
#include "audio.h"
#include "video.h"
#include "games.h"
#include "ftp.h"
#include "archive.h"
#include "arabic.h"
#include "launcher.h"
#include "webman.h"
#include "covers.h"
#include "theme.h"
#include "disc.h"
#include "tools.h"
#include "catalog.h"
#include "integrity.h"
#include "save_pfd.h"
#include "subdl.h"
#include "hubapi.h"
#define SUBKEY_FILE APP_DIR "/subdl_key.txt"
#define SYS_WEBMAN 100
#define SYS_MOVIAN 101

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
        int rep_mask = d & (B_UP | B_DOWN | B_LEFT | B_RIGHT | B_L2 | B_R2);
        if (hold_frames > 40) in_active = rep_mask;                                   /* fast scroll */
        else if (hold_frames > 12 && (hold_frames % 2) == 0) in_active = rep_mask;
    } else hold_frames = 0;
    prev_down = d;
}

/* =========================================================== settings */
typedef struct { int lang; int mode; int del; int ftp; int covers; } settings_t;
static settings_t cfg = { 0, MODE_XMB_QUEUE, 0, 0, 1 };

static void load_settings(void)
{
    FILE *f = fopen(CFG_FILE, "rb");
    if (f) {
        settings_t s;
        if (fread(&s, sizeof(s), 1, f) == 1) {
            if (s.lang >= 0 && s.lang <= 1) cfg.lang = s.lang;
            if (s.mode >= 0 && s.mode <= 1) cfg.mode = s.mode;
            if (s.del >= 0 && s.del <= 1) cfg.del = s.del;
            if (s.ftp >= 0 && s.ftp <= 1) cfg.ftp = s.ftp;
            if (s.covers >= 0 && s.covers <= 1) cfg.covers = s.covers;
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
enum { FT_DIR, FT_PKG, FT_DISC, FT_IMAGE, FT_VIDEO, FT_AUDIO, FT_ARCHIVE, FT_SYSTEM, FT_TEXT, FT_OTHER, FT_THEME };

static int file_type(const char *name, int is_dir)
{
    if (is_dir) return FT_DIR;
    const char *e = ext_of(name);
    static const struct { const char *ext; int t; } map[] = {
        {"pkg",FT_PKG},{"p3t",FT_THEME},{"iso",FT_DISC},{"bin",FT_DISC},{"img",FT_DISC},{"cue",FT_DISC},{"mdf",FT_DISC},{"vob",FT_VIDEO},
        {"png",FT_IMAGE},{"jpg",FT_IMAGE},{"jpeg",FT_IMAGE},{"bmp",FT_IMAGE},{"gif",FT_IMAGE},{"gtf",FT_IMAGE},{"dds",FT_IMAGE},
        {"mp4",FT_VIDEO},{"mkv",FT_VIDEO},{"avi",FT_VIDEO},{"m2ts",FT_VIDEO},{"mts",FT_VIDEO},{"pam",FT_VIDEO},{"wmv",FT_VIDEO},
        {"webm",FT_VIDEO},{"mov",FT_VIDEO},{"m4v",FT_VIDEO},{"3gp",FT_VIDEO},{"divx",FT_VIDEO},{"xvid",FT_VIDEO},{"ts",FT_VIDEO},{"tp",FT_VIDEO},{"trp",FT_VIDEO},{"mpg",FT_VIDEO},{"mpeg",FT_VIDEO},{"m2v",FT_VIDEO},{"flv",FT_VIDEO},
        {"srt",FT_TEXT},{"ass",FT_TEXT},{"ssa",FT_TEXT},{"vtt",FT_TEXT},
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
        case FT_THEME:   *a = RGBA8(255, 110, 180, 255); *b = RGBA8(150, 70, 255, 255); break;
        default:         *a = RGBA8(150, 156, 190, 255); *b = RGBA8(100, 106, 140, 255); break;
    }
}

/* =========================================================== volumes */
#define MAX_VOL 24
static volume_t vols[MAX_VOL];
static int vol_count, vol_sel, vol_scroll;
static uint64_t vol_free;
static int native_usb_mask, usb_dev_mask;

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
    int is_dir, type, marked, parsed, cover_wait;
    uint64_t size;
    pkg_info_t info;
    texture_t icon;
} entry_t;

static entry_t *ents;
static vfs_entry_t *raw;
static int ent_count, sel, scroll_top, list_err;
static int dir_gen;
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
    dir_gen++;
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

static int games_btn;        /* sidebar focus is on the My Games button */
static int games_mode;       /* the games screen is the base view */
static void games_open(void);
static void tools_open(void);

static int marked_count(void)
{
    int n = 0;
    for (int i = 0; i < ent_count; i++) if (ents[i].marked) n++;
    return n;
}

static int thumbable(const char *n)
{
    const char *e = strrchr(n, '.');
    return e && (!strcasecmp(e, ".png") || !strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg"));
}

/* ---------- background loader: PKG info + thumbnails are read and decoded off the UI thread ---------- */
static volatile int ld_state;          /* 0 idle, 1 job queued, 2 result ready */
static struct { int gen, idx, is_img, is_dir, cover_st; uint64_t size; char name[256]; char path[1400]; texture_t tex; pkg_info_t info; } ld;
static sys_ppu_thread_t ld_thr;
static volatile int ld_quit;

static void loader_thread(void *arg)
{
    (void)arg;
    while (!ld_quit) {
        if (ld_state != 1) { usleep(3000); continue; }
        ld.tex = NULL;
        ld.cover_st = CV_NONE;
        memset(&ld.info, 0, sizeof(ld.info));
        if (ld.is_img == 3) {
            uint8_t *b = NULL; uint32_t sz = 0;
            if (p3t_preview(ld.path, &b, &sz)) {
                texture_t t = (b[0] == 0x89) ? gfx_load_png_buffer(b, sz) : gfx_load_jpg_buffer(b, sz);
                ld.tex = gfx_make_thumb(t, 128, 72);
                free(b);
            }
        } else if (ld.is_img == 2) {
            char cp[1400];
            ld.cover_st = covers_lookup(ld.path, ld.name, ld.is_dir, cp, sizeof(cp));
            if (ld.cover_st == CV_CACHED) {
                vfs_file_t *f = vfs_open_read(cp);
                if (f) {
                    uint64_t sz64 = vfs_size(f);
                    uint32_t sz = sz64 < 4 * 1024 * 1024 ? (uint32_t)sz64 : 0, got = 0;
                    uint8_t *b = sz ? (uint8_t *)malloc(sz) : NULL;
                    while (b && got < sz) { int64_t r = vfs_read(f, b + got, sz - got); if (r <= 0) break; got += (uint32_t)r; }
                    vfs_close(f);
                    if (b && got == sz && sz > 8) {
                        texture_t t = (b[0] == 0x89 && b[1] == 'P') ? gfx_load_png_buffer(b, sz) : gfx_load_jpg_buffer(b, sz);
                        ld.tex = gfx_make_thumb(t, 112, 112);
                    }
                    free(b);
                }
            }
        } else if (ld.is_img) {
            if (ld.size > 0 && ld.size <= 8 * 1024 * 1024) {
                vfs_file_t *f = vfs_open_read(ld.path);
                if (f) {
                    uint32_t sz = (uint32_t)ld.size, got = 0;
                    uint8_t *b = (uint8_t *)malloc(sz);
                    while (b && got < sz) { int64_t r = vfs_read(f, b + got, sz - got); if (r <= 0) break; got += (uint32_t)r; }
                    vfs_close(f);
                    if (b && got == sz) {
                        texture_t t = (b[0] == 0x89 && b[1] == 'P') ? gfx_load_png_buffer(b, sz) : gfx_load_jpg_buffer(b, sz);
                        ld.tex = gfx_make_thumb(t, 128, 72);
                    }
                    free(b);
                }
            }
        } else {
            pkg_read_info(ld.path, &ld.info, 1);
            if (ld.info.icon) ld.tex = gfx_make_thumb(gfx_load_png_buffer(ld.info.icon, ld.info.icon_size), 128, 72);
        }
        ld_state = 2;
    }
    sysThreadExit(0);
}

static void loader_start(void)
{
    ld_quit = 0;
    sysThreadCreate(&ld_thr, loader_thread, NULL, 1600, 0x20000, THREAD_JOINABLE, "loader");
}

static void loader_stop(void)
{
    ld_quit = 1;
    u64 rv;
    sysThreadJoin(ld_thr, &rv);
    if (ld_state == 2) { if (ld.tex) gfx_free_texture(ld.tex); pkg_free_info(&ld.info); }
    ld_state = 0;
}

static void lazy_parse(void)
{
    static int seen_gen;
    int g = covers_generation();
    if (g != seen_gen) {
        seen_gen = g;
        for (int i = 0; i < ent_count; i++) if (ents[i].cover_wait) { ents[i].cover_wait = 0; if (ents[i].parsed == 1 && !ents[i].icon) ents[i].parsed = 0; }
    }
    /* 1. collect a finished result */
    if (ld_state == 2) {
        if (ld.gen == dir_gen && ld.idx < ent_count && !strcmp(ents[ld.idx].name, ld.name)) {
            entry_t *e = &ents[ld.idx];
            e->info = ld.info;
            e->icon = ld.tex;
            e->parsed = 1;
            e->cover_wait = (ld.is_img == 2 && ld.cover_st == CV_PENDING);
        } else {
            if (ld.tex) gfx_free_texture(ld.tex);
            pkg_free_info(&ld.info);
        }
        ld_state = 0;
    }
    /* 2. queue the next item nearest to the cursor */
    if (ld_state == 0) {
        for (int dist = 0; dist < 14 && ld_state == 0; dist++) {
            int c[2] = { sel + dist, sel - dist };
            for (int k = 0; k < 2; k++) {
                int i = c[k];
                if (i < 0 || i >= ent_count) continue;
                entry_t *e = &ents[i];
                if (e->parsed) continue;
                int img = e->type == FT_IMAGE && thumbable(e->name);
                int thm = e->type == FT_THEME;
                int cov = !img && !thm && e->type != FT_PKG && cfg.covers && covers_candidate(e->name, e->is_dir);
                if (e->type != FT_PKG && !img && !cov && !thm) continue;
                e->parsed = 2;   /* pending */
                ld.gen = dir_gen; ld.idx = i; ld.is_img = thm ? 3 : cov ? 2 : img; ld.is_dir = e->is_dir; ld.size = e->size;
                snprintf(ld.name, sizeof(ld.name), "%s", e->name);
                path_join(ld.path, sizeof(ld.path), cwd, e->name);
                ld_state = 1;
                break;
            }
        }
    }
    /* 3. drop textures far from the cursor */
    for (int i = 0; i < ent_count; i++)
        if (ents[i].parsed == 1 && abs(i - sel) > 40) {
            if (ents[i].icon) gfx_free_texture(ents[i].icon);
            ents[i].icon = NULL; ents[i].parsed = 0; pkg_free_info(&ents[i].info);
        }
}

/* =========================================================== clipboard */
static char (*clip)[1024];
static int clip_n, clip_op;   /* OP_COPY / OP_MOVE */

/* =========================================================== UI state */
enum { UI_BROWSE, UI_MENU, UI_CONFIRM_INSTALL, UI_CONFIRM_DELETE, UI_PROGRESS_INSTALL, UI_PROGRESS_OPS,
       UI_RESULT_INSTALL, UI_RESULT_OPS, UI_PROPS, UI_SETTINGS, UI_OSK, UI_SCANNING, UI_SFO, UI_IMAGE, UI_AUDIO, UI_TEXT, UI_HEX, UI_CONFIRM_LAUNCH, UI_LAUNCHING, UI_THEME, UI_VIDEO, UI_GAMES, UI_TOOLS };
static int ui = UI_SCANNING, prev_ui = UI_BROWSE;
static int confirm_sel, result_sel, set_sel, menu_sel;
static float modal_anim, menu_anim;
static int quit_req;
static char toast[256];
static uint64_t toast_until;
static int osk_purpose;   /* 0 rename, 1 new folder, 2 sfo field, 3 text line, 4 hex byte */
static sfo_field_t sfo_fields[SFO_MAX_FIELDS];
static int sfo_count, sfo_sel;
static char sfo_path[1400];
static char ops_output[1400];
static int thm_installing, play_after_copy;
static char movian_self[1400];
static int launch_sys;
static char launch_core[1400], launch_core_name[64], launch_src[1400];
static char pending_self[1400], pending_arg[1400];

/* ---------- task queue, background progress, power off when done ---------- */
#define JQ_MAX 8
typedef struct { int op, n; char (*src)[1024]; char dst[1024]; } qjob_t;
static qjob_t jq[JQ_MAX];
static int jq_n, ops_bg, shutdown_after, shut_countdown, power_off;

static int jq_push(int op, char srcs[][1024], int n, const char *dst)
{
    if (jq_n >= JQ_MAX || n <= 0) return 0;
    qjob_t *j = &jq[jq_n];
    j->src = malloc((size_t)n * 1024);
    if (!j->src) return 0;
    memcpy(j->src, srcs, (size_t)n * 1024);
    j->op = op; j->n = n;
    snprintf(j->dst, sizeof(j->dst), "%s", dst ? dst : "");
    jq_n++;
    return 1;
}

static int jq_start_next(void)
{
    while (jq_n) {
        qjob_t j = jq[0];
        memmove(jq, jq + 1, sizeof(qjob_t) * (jq_n - 1));
        jq_n--;
        int ok = ops_start(j.op, j.src, j.n, j.op == OP_DELETE ? NULL : j.dst);
        free(j.src);
        if (ok) return 1;
    }
    return 0;
}

/* start now, or queue behind the running task */
static int ops_go(int op, char srcs[][1024], int n, const char *dst);
static char osk_old[256];

static void show_toast(const char *s) { snprintf(toast, sizeof(toast), "%s", s); toast_until = gfx_frame() + 150; }
static void open_modal(int m) { prev_ui = ui; ui = m; modal_anim = 0.6f; }
static void show_toast(const char *s);
static int ops_go(int op, char srcs[][1024], int n, const char *dst)
{
    if (ops_status()->state == OPS_RUNNING || ops_bg) {
        if (!jq_push(op, srcs, n, dst)) return 0;
        char m[96]; snprintf(m, sizeof(m), T(S_QUEUED), jq_n); show_toast(m);
        return 1;
    }
    if (ops_status()->state != OPS_IDLE) ops_reset();
    if (!ops_start(op, srcs, n, dst)) return 0;
    open_modal(UI_PROGRESS_OPS);
    return 1;
}

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
        float xy[2 * 56];
        int n = 0;
        for (int x = 0; x <= SCR_W + 16 && n < 56; x += 16, n++) {
            xy[n * 2] = x;
            xy[n * 2 + 1] = 330 + w * 26 + sinf(x * 0.008f + t * (0.4f + w * 0.15f) + w) * 22;
        }
        gfx_polyline(xy, n, 1.2f, RGBA8(140, 160, 255, 16));
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
    float cw = L_text(824, 16, clk, 20, 1, C_TEXT, ALIGN_RIGHT);
    if (ftp_running()) {
        char f[64];
        if (ftp_port() == 21) snprintf(f, sizeof(f), "FTP  %s", ftp_ip()); else snprintf(f, sizeof(f), "FTP  %s:%d", ftp_ip(), ftp_port());
        float fw = gfx_text_width(f, 10, 1) + 20;
        L_rrect(824 - cw - 14 - fw, 18, fw, 20, 10, ftp_clients() ? RGBA8(60, 200, 140, 230) : RGBA8(64, 120, 255, 200));
        L_text(824 - cw - 14 - fw / 2, 21, f, 10, 1, C_TEXT, ALIGN_CENTER);
    }
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

    int visible = (int)((h - 44 - 96) / ih);
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
    /* "My Games" button */
    {
        float by = y + h - 50, bh = 40;
        int on = games_btn == 1 && !focus_list;
        /* "Tools" button above it */
        {
            float ty = by - 46;
            int ton = games_btn == 2 && !focus_list;
            if (ton) {
                L_grad_pill(x + 8, ty, w - 16, bh, 12, RGBA8(40, 200, 150, 235), RGBA8(40, 120, 200, 235));
                L_border(x + 8, ty, w - 16, bh, 12, 1.4f, RGBA8(255, 255, 255, 150 + (int)(sinf(gfx_frame() * 0.1f) * 60)));
            } else {
                L_rrect(x + 8, ty, w - 16, bh, 12, RGBA8(255, 255, 255, 14));
                L_border(x + 8, ty, w - 16, bh, 12, 1, RGBA8(80, 220, 170, 90));
            }
            float gx = MXP(x + 31), gy = ty + 20;
            gfx_circle(gx, gy, 9, ton ? C_TEXT : RGBA8(110, 230, 190, 255));
            gfx_circle(gx, gy, 4, ton ? RGBA8(40, 140, 170, 255) : RGBA8(20, 18, 50, 255));
            L_text_fit(x + 50, ty + 11, w - 70, T(S_TOOLS), 14, 1, ton ? C_TEXT : RGBA8(160, 240, 210, 255), ALIGN_LEFT);
        }
        if (on) {
            L_grad_pill(x + 8, by, w - 16, bh, 12, RGBA8(255, 120, 80, 235), RGBA8(200, 60, 160, 235));
            L_border(x + 8, by, w - 16, bh, 12, 1.4f, RGBA8(255, 255, 255, 150 + (int)(sinf(gfx_frame() * 0.1f) * 60)));
        } else {
            L_rrect(x + 8, by, w - 16, bh, 12, RGBA8(255, 255, 255, 14));
            L_border(x + 8, by, w - 16, bh, 12, 1, RGBA8(255, 140, 100, 90));
        }
        /* small controller glyph */
        float gx = MX(x + 20, 22), gy = by + 13;
        gfx_rrect(gx, gy, 22, 13, 6, on ? C_TEXT : RGBA8(255, 170, 130, 255));
        gfx_rect(gx + 4, gy + 5, 6, 2, on ? RGBA8(200, 60, 120, 255) : RGBA8(20, 18, 50, 255));
        gfx_rect(gx + 6, gy + 3, 2, 6, on ? RGBA8(200, 60, 120, 255) : RGBA8(20, 18, 50, 255));
        gfx_circle(gx + 16, gy + 6, 2, on ? RGBA8(200, 60, 120, 255) : RGBA8(20, 18, 50, 255));
        L_text_fit(x + 50, by + 11, w - 70, T(S_MYGAMES), 14, 1, on ? C_TEXT : RGBA8(255, 190, 160, 255), ALIGN_LEFT);
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
        scroll_anim = approach(scroll_anim, scroll_top, 0.45f);
        sel_anim = approach(sel_anim, sel, 0.55f);

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
            if (e->icon) {
                int tw0, th0;
                gfx_texture_size(e->icon, &tw0, &th0);
                int portrait = th0 > tw0;
                float bw = portrait ? 30 : 60, bh = portrait ? 38 : 33, by = portrait ? ry + 2 : ry + 5;
                float sc = bw / tw0;
                if (bh / th0 < sc) sc = bh / th0;
                float dw = tw0 * sc, dh = th0 * sc;
                if (!portrait) gfx_rrect(MX(ix, bw), by, bw, bh, 4, RGBA8(0, 0, 0, 90));
                gfx_texture(e->icon, MX(ix, bw) + (bw - dw) / 2, by + (bh - dh) / 2, dw, dh, RGBA8(255, 255, 255, 255));
                gfx_rrect_border(MX(ix, bw) + (bw - dw) / 2 - 1, by + (bh - dh) / 2 - 1, dw + 2, dh + 2, 3, 1, RGBA8(255, 255, 255, 60));
                tx = ix + (portrait ? 42 : 72);
            } else if (e->type == FT_IMAGE && thumbable(e->name) && e->parsed != 1) {
                /* loading placeholder: pulsing tile */
                int a = 40 + (int)(sinf(gfx_frame() * 0.15f) * 25);
                gfx_rrect(MX(ix, 60), ry + 5, 60, 33, 4, RGBA8(255, 255, 255, a));
                tx = ix + 72;
            } else draw_file_icon(ix, ry + 6, 30, e->type, e->name);

            const char *title = (e->type == FT_PKG && e->parsed == 1 && e->info.title[0]) ? e->info.title : e->name;
            float tw = w - (tx - x) - 150;
            L_text_fit(tx, ry + 6, tw, title, 14, 1, C_TEXT, ALIGN_LEFT);

            char line2[200];
            if (e->is_dir) snprintf(line2, sizeof(line2), "%s", T(S_FOLDER_TYPE));
            else if (e->type == FT_PKG && e->parsed == 1 && e->info.title[0])
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
                if (e->type == FT_PKG && e->parsed == 1) {
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
enum { M_OPEN, M_WEBMAN, M_MOVIAN, M_THEME, M_INSTALL, M_EXTRACT, M_BUILD, M_EDITSFO, M_EXTRACT_ARC, M_OPEN_TEXT, M_OPEN_HEX, M_CRC32, M_COPY, M_CUT, M_PASTE, M_RENAME, M_NEWDIR, M_DELETE, M_SELALL, M_PROPS, M_REFRESH, M_UNISO, M_TOOLS, M_SETTINGS, M_COUNT };
static const int menu_label[M_COUNT] = { S_OPEN, S_WM_PLAY, S_PLAY_MOVIAN, S_THEME_INSTALL, S_INSTALL_PKG, S_EXTRACT, S_BUILD, S_EDIT_SFO, S_EXTRACT_ARC, S_OPEN_TEXT, S_OPEN_HEX, S_CRC32, S_COPY, S_CUT, S_PASTE, S_RENAME, S_NEWFOLDER, S_DELETE_ACT, S_SELECT_ALL, S_PROPS, S_REFRESH, S_T_UNISO, S_TOOLS, S_SETTINGS };
static int menu_items[M_COUNT], menu_n;
static int find_movian(void);
static void ask_launch(int idx);

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
        case M_WEBMAN: return has && !marked_count();
        case M_THEME: return strcmp(cwd, THEME_DIR) != 0;
        case M_BUILD: return cursor_is_pkg_folder();
        case M_EDITSFO: { char t[1400]; return cursor_sfo(t, sizeof(t)); }
        case M_EXTRACT_ARC: return has && !marked_count() && archive_kind(ents[sel].name) != ARC_NONE && cur_writable();
        case M_OPEN_TEXT: case M_OPEN_HEX: case M_CRC32: return has && !marked_count() && !ents[sel].is_dir;
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
        if (m == M_THEME) { int any = 0, mk = marked_count(); for (int i = 0; i < ent_count; i++) if (ents[i].type == FT_THEME && (mk ? ents[i].marked : i == sel)) any = 1; if (!any) continue; }
        if (m == M_WEBMAN) { if (!ent_count) continue; char fp[1400]; path_join(fp, sizeof(fp), cwd, ents[sel].name); if (!webman_is_game(ents[sel].name, ents[sel].is_dir, fp)) continue; }
        if (m == M_MOVIAN && !(ent_count && !marked_count() && ents[sel].type == FT_VIDEO && find_movian())) continue;
        if (m == M_BUILD && !cursor_is_pkg_folder()) continue;
        if (m == M_EDITSFO) { char t[1400]; if (!cursor_sfo(t, sizeof(t))) continue; }
        if (m == M_EXTRACT_ARC && !(ent_count && archive_kind(ents[sel].name) != ARC_NONE)) continue;
        if ((m == M_OPEN_TEXT || m == M_OPEN_HEX || m == M_CRC32) && !(ent_count && !ents[sel].is_dir)) continue;
        if (m == M_PASTE && !clip_n) continue;
        if (m == M_UNISO && !(ent_count && !marked_count() && !ents[sel].is_dir && iso_is_image(ents[sel].name))) continue;
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
    {
        char sd[96]; snprintf(sd, sizeof(sd), "%s %s", T(S_SHUTDOWN_AFTER), shutdown_after ? "\xE2\x9C\x93" : "");
        char qn[48] = ""; if (jq_n) snprintf(qn, sizeof(qn), T(S_QUEUE_N), jq_n);
        float tw = 18 + 6 + gfx_text_width(T(S_CANCEL), 12, 0) + 24 + 18 + 6 + gfx_text_width(sd, 12, 0) + 24 + 18 + 6 + gfx_text_width(T(S_BG_HIDE), 12, 0);
        float hx = SCR_W / 2 - tw / 2;
        float bwi = gfx_button_icon(hx, y + h - 30, BTN_TRIANGLE, 18);
        gfx_text(hx + bwi + 6, y + h - 29, T(S_BG_HIDE), 12, 0, C_DIM, ALIGN_LEFT);
        hx += bwi + 6 + gfx_text_width(T(S_BG_HIDE), 12, 0) + 24;
        bwi = gfx_button_icon(hx, y + h - 30, BTN_SQUARE, 18);
        gfx_text(hx + bwi + 6, y + h - 29, sd, 12, shutdown_after, shutdown_after ? C_OKC : C_DIM, ALIGN_LEFT);
        hx += bwi + 6 + gfx_text_width(sd, 12, 0) + 24;
        bwi = gfx_button_icon(hx, y + h - 30, cancel_icon, 18);
        gfx_text(hx + bwi + 6, y + h - 29, T(S_CANCEL), 12, 0, C_DIM, ALIGN_LEFT);
        if (qn[0]) gfx_text(SCR_W / 2, y + 196, qn, 11, 1, RGBA8(255, 200, 120, 255), ALIGN_CENTER);
    }
}

static void draw_progress_ops(void)
{
    const ops_status_t *s = ops_status();
    float x, y, w = 540, h = 250;
    draw_modal_frame(w, h, &x, &y);
    const char *t = s->op == OP_DELETE ? T(S_DELETING) : s->op == OP_MOVE ? T(S_MOVING) :
                    s->op == OP_EXTRACT ? T(S_EXTRACTING) : s->op == OP_BUILD ? T(S_BUILDING) :
                    s->op == OP_UNARCHIVE ? T(S_UNARCHIVING) : s->op == OP_DISC ? T(S_DUMPING) : s->op == OP_UNISO ? T(S_T_UNISO) : T(S_COPYING);
    gfx_text(SCR_W / 2, y + 20, t, 20, 1, C_TEXT, ALIGN_CENTER);
    char nfo[64];
    if (s->op == OP_EXTRACT || s->op == OP_BUILD || s->op == OP_UNARCHIVE || s->op == OP_DISC || s->op == OP_UNISO) snprintf(nfo, sizeof(nfo), T(S_FILES), (int)s->files_done);
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
        if (thm_installing && s->op == OP_COPY) {
            draw_lines(cx, y + 150, T(S_THEME_APPLY), 11, 0, C_DIM, ALIGN_CENTER, 16);
        } else if (s->op == OP_EXTRACT || s->op == OP_BUILD || s->op == OP_UNARCHIVE || s->op == OP_DISC || s->op == OP_UNISO) {
            char o[1500]; snprintf(o, sizeof(o), "%s: %s", T(S_OUTPUT), s->op == OP_DISC ? ops_out_path() : ops_output);
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
    const char *rows[8][2];
    int n = 0;
    rows[n][0] = T(S_NAME); rows[n][1] = e->name; n++;
    if (e->is_dir) {
        rows[n][0] = T(S_TYPE); rows[n][1] = T(S_FOLDER_TYPE); n++;
        if (!dirsize_done()) { rows[n][0] = T(S_SIZE); rows[n][1] = T(S_CALCULATING); n++; }
        else {
            uint64_t b; uint32_t fcount, dcount;
            dirsize_result(&b, &fcount, &dcount);
            fmt_size(b, sz, sizeof(sz));
            rows[n][0] = T(S_SIZE); rows[n][1] = sz; n++;
            snprintf(cnt, sizeof(cnt), T(S_FOLDERS_FILES), dcount, fcount);
            rows[n][0] = T(S_CONTENTS); rows[n][1] = cnt; n++;
        }
    } else {
        fmt_size(e->size, sz, sizeof(sz));
        rows[n][0] = T(S_SIZE); rows[n][1] = sz; n++;
        snprintf(tbuf, sizeof(tbuf), "%s", e->type == FT_PKG && e->parsed == 1 ? pkg_type_label(e->info.content_type) : (ext_of(e->name)[0] ? ext_of(e->name) : T(S_FILE_TYPE)));
        rows[n][0] = T(S_TYPE); rows[n][1] = tbuf; n++;
        if (e->type == FT_PKG && e->parsed == 1) {
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

#define SET_ITEMS 7
static void draw_settings(void)
{
    float x, y, w = 520, h = 422;
    draw_modal_frame(w, h, &x, &y);
    gfx_text(SCR_W / 2, y + 20, T(S_SETTINGS), 20, 1, C_TEXT, ALIGN_CENTER);
    const int labels[SET_ITEMS] = { S_SET_LANG, S_SET_MODE, S_SET_DELETE, S_FTP, S_COVERS, S_SET_ABOUT, S_SET_EXIT };
    char ftpv[96];
    if (ftp_running()) {
        if (ftp_port() == 21) snprintf(ftpv, sizeof(ftpv), "%s  ftp://%s", T(S_ON), ftp_ip());
        else snprintf(ftpv, sizeof(ftpv), "%s  ftp://%s:%d", T(S_ON), ftp_ip(), ftp_port());
    } else snprintf(ftpv, sizeof(ftpv), "%s", T(S_OFF));
    const char *vals[SET_ITEMS] = {
        T(S_LANG_NAME), cfg.mode == MODE_XMB_QUEUE ? T(S_MODE_XMB) : T(S_MODE_PKGS),
        cfg.del ? T(S_DELETE) : T(S_KEEP), ftpv, cfg.covers ? T(S_ON) : T(S_OFF), T(S_ABOUT_TXT), NULL
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
            if (i < 5) snprintf(v, sizeof(v), "\xE2\x80\xB9  %s  \xE2\x80\xBA", vals[i]); else snprintf(v, sizeof(v), "%s", vals[i]);
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
    float ty = 408;
    gfx_rrect(SCR_W / 2 - tw / 2, ty, tw, 32, 16, RGBA8(30, 34, 70, a * 240 / 255));
    gfx_rrect_border(SCR_W / 2 - tw / 2, ty, tw, 32, 16, 1, RGBA8(255, 255, 255, a / 4));
    gfx_text(SCR_W / 2, ty + 8, toast, 13, 1, WITH_ALPHA(C_TEXT, a), ALIGN_CENTER);
}

static void refresh_keep_wrapper(void);

/* =========================================================== viewers & editors */

/* ---------- image viewer ---------- */
static texture_t img_tex;
static int img_idx, img_fill, img_err;
static char img_name[256];

static int is_image(const entry_t *e) { return e->type == FT_IMAGE && (!strcasecmp(ext_of(e->name), "png") || !strcasecmp(ext_of(e->name), "jpg") || !strcasecmp(ext_of(e->name), "jpeg")); }

static void image_load(int idx)
{
    if (img_tex) { gfx_free_texture(img_tex); img_tex = NULL; }
    img_idx = idx; img_err = 0;
    snprintf(img_name, sizeof(img_name), "%s", ents[idx].name);
    if (ents[idx].size == 0 || ents[idx].size > 24 * 1024 * 1024) { img_err = 1; return; }
    char p[1400];
    path_join(p, sizeof(p), cwd, ents[idx].name);
    vfs_file_t *f = vfs_open_read(p);
    if (!f) { img_err = 1; return; }
    uint32_t sz = (uint32_t)ents[idx].size;
    uint8_t *b = (uint8_t *)malloc(sz);
    uint32_t got = 0;
    while (b && got < sz) { int64_t r = vfs_read(f, b + got, sz - got); if (r <= 0) break; got += (uint32_t)r; }
    vfs_close(f);
    if (b && got == sz) {
        if (sz > 8 && b[0] == 0x89 && b[1] == 'P') img_tex = gfx_load_png_buffer(b, sz);
        else img_tex = gfx_load_jpg_buffer(b, sz);
    }
    free(b);
    if (!img_tex) img_err = 1;
}

static void image_step(int d)
{
    for (int k = 1; k <= ent_count; k++) {
        int i = (img_idx + d * k + ent_count * 4) % ent_count;
        if (is_image(&ents[i])) { sel = i; image_load(i); return; }
    }
}

static void draw_image(void)
{
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA8(0, 0, 0, 255));
    if (img_tex) {
        int iw, ih;
        gfx_texture_size(img_tex, &iw, &ih);
        float sx = (float)SCR_W / iw, sy = (float)(SCR_H) / ih;
        float sc = img_fill ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
        float w = iw * sc, h = ih * sc;
        gfx_texture(img_tex, (SCR_W - w) / 2, (SCR_H - h) / 2, w, h, RGBA8(255, 255, 255, 255));
        char info[320];
        snprintf(info, sizeof(info), "%s  \xC2\xB7  %dx%d", img_name, iw, ih);
        gfx_grad_v(0, 0, SCR_W, 44, RGBA8(0, 0, 0, 170), RGBA8(0, 0, 0, 0));
        gfx_text_fit(SCR_W / 2, 12, SCR_W - 60, info, 13, 1, C_TEXT, ALIGN_CENTER);
    } else {
        gfx_text(SCR_W / 2, SCR_H / 2 - 10, T(S_IMG_ERR), 15, 1, C_TEXT, ALIGN_CENTER);
        gfx_text_fit(SCR_W / 2, SCR_H / 2 + 16, SCR_W - 60, img_name, 12, 0, C_DIM, ALIGN_CENTER);
    }
}

static void close_image(void) { if (img_tex) { gfx_free_texture(img_tex); img_tex = NULL; } ui = UI_BROWSE; }

static void handle_image(void)
{
    uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
    if (in_pressed & (R | B_R1)) image_step(1);
    if (in_pressed & (L | B_L1)) image_step(-1);
    if (in_pressed & B_SQU) img_fill = !img_fill;
    if (in_pressed & (B_CANCEL | B_OK)) close_image();
}

/* ---------- music player ---------- */
static int aud_idx;
static char aud_name[256];

static void audio_open(int idx)
{
    char p[1400];
    aud_idx = idx; sel = idx;
    snprintf(aud_name, sizeof(aud_name), "%s", ents[idx].name);
    path_join(p, sizeof(p), cwd, ents[idx].name);
    if (!audio_play(p)) show_toast(T(S_AUDIO_ERR));
}

static void audio_step(int d)
{
    for (int k = 1; k <= ent_count; k++) {
        int i = (aud_idx + d * k + ent_count * 4) % ent_count;
        if (!ents[i].is_dir && audio_is_supported(ents[i].name)) { audio_open(i); return; }
    }
}

static void draw_audio(void)
{
    float x, y, w = 520, h = 300;
    draw_modal_frame(w, h, &x, &y);
    float cx = SCR_W / 2, t = gfx_frame() / 60.0f;
    int st = audio_state();
    /* spinning disc */
    float a = (st == AUD_PLAYING) ? t * 2.0f : 0;
    gfx_glow(cx, y + 78, 70, RGBA8(120, 90, 255, 70));
    gfx_circle(cx, y + 78, 48, RGBA8(30, 30, 60, 255));
    for (int r = 46; r > 18; r -= 6) gfx_circle(cx, y + 78, r, RGBA8(40 + r, 40 + r / 2, 90 + r, 255));
    gfx_line(cx, y + 78, cx + cosf(a) * 40, y + 78 + sinf(a) * 40, 2, RGBA8(255, 255, 255, 120));
    gfx_circle(cx, y + 78, 12, C_ACC1);
    gfx_circle(cx, y + 78, 4, RGBA8(20, 20, 40, 255));

    gfx_text(cx, y + 138, T(S_NOW_PLAYING), 11, 1, C_DIM, ALIGN_CENTER);
    gfx_text_fit(cx, y + 156, w - 60, aud_name, 16, 1, C_TEXT, ALIGN_CENTER);

    float lv[8];
    audio_levels(lv);
    for (int i = 0; i < 8; i++) {
        float bh = 6 + lv[i] * 46;
        gfx_rrect_grad(cx - 92 + i * 24, y + 228 - bh, 16, bh, 4, RGBA8(120, 190, 255, 255), RGBA8(130, 80, 255, 255));
    }
    uint32_t pos = audio_position_ms(), dur = audio_duration_ms();
    char a1[24], a2[24], line[64];
    fmt_time(pos / 1000, a1, sizeof(a1));
    if (dur) { fmt_time(dur / 1000, a2, sizeof(a2)); snprintf(line, sizeof(line), "%s / %s", a1, a2); }
    else snprintf(line, sizeof(line), "%s", a1);
    if (dur) {
        float p = (float)pos / dur; if (p > 1) p = 1;
        gfx_rrect(x + 60, y + 240, w - 120, 6, 3, RGBA8(255, 255, 255, 25));
        if ((w - 120) * p > 6) gfx_rrect(x + 60, y + 240, (w - 120) * p, 6, 3, C_ACC1);
    }
    gfx_text(cx, y + 252, st == AUD_ERROR ? T(S_AUDIO_ERR) : line, 12, 0, C_DIM, ALIGN_CENTER);
}

static void handle_audio(void)
{
    uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
    if (in_pressed & B_OK) audio_toggle_pause();
    if (in_pressed & (R | B_R1)) audio_step(1);
    if (in_pressed & (L | B_L1)) audio_step(-1);
    if (in_pressed & B_CANCEL) { audio_stop(); ui = UI_BROWSE; }
    else if (audio_state() == AUD_ENDED) audio_step(1);
}

/* ---------- My Games ---------- */
static int gm_cat = -1;          /* -1 all, else GP_* */
static int gm_sel, gm_top;
static float gm_anim;
static int gm_idx[3000], gm_n;
#define GM_COLS 6
#define GM_ROWS 2

static void games_open(void)
{
    if (!games_count() && !games_scanning()) games_scan(vols, vol_count);
    games_mode = 1;
    ui = UI_GAMES;
    gm_anim = 0;
}

static void gm_filter(void)
{
    gm_n = 0;
    int n = games_count();
    for (int i = 0; i < n && gm_n < 3000; i++) { game_t *g = games_get(i); if (g && (gm_cat < 0 || g->plat == gm_cat)) gm_idx[gm_n++] = i; }
    if (gm_sel >= gm_n) gm_sel = gm_n ? gm_n - 1 : 0;
}

static const char *plat_name(int p) { static const char *n[GP_COUNT] = { "PS3", "PS2", "PS1", "PSP" }; return p >= 0 && p < GP_COUNT ? n[p] : ""; }
static uint32_t plat_color(int p)
{
    switch (p) {
    case GP_PS3: return RGBA8(64, 115, 242, 255);
    case GP_PS2: return RGBA8(82, 77, 205, 255);
    case GP_PS1: return RGBA8(140, 143, 160, 255);
    default: return RGBA8(205, 85, 128, 255);
    }
}

static void draw_games(void)
{
    gm_filter();
    gm_anim += (1.0f - gm_anim) * 0.2f;
    float a = gm_anim;
    /* title + count */
    L_text(28, 22, T(S_MYGAMES), 24, 1, C_TEXT, ALIGN_LEFT);
    char cnt[64];
    if (games_scanning()) snprintf(cnt, sizeof(cnt), "%s", T(S_SCANNING_GAMES)); else snprintf(cnt, sizeof(cnt), T(S_GAMES_COUNT), gm_n);
    L_text(SCR_W - 28, 30, cnt, 12, 0, C_DIM, ALIGN_RIGHT);
    if (games_scanning()) { float tt = gfx_frame() / 60.0f; for (int i = 0; i < 6; i++) { float an = tt * 5 + i * 1.05f; gfx_circle(MXP(SCR_W - 40 - gfx_text_width(cnt, 12, 0)) + cosf(an) * 6, 36 + sinf(an) * 6, 1.8f, WITH_ALPHA(C_TEXT, 60 + i * 30)); } }
    /* category tabs */
    float tx = 28;
    for (int c = -1; c < GP_COUNT; c++) {
        const char *nm = c < 0 ? T(S_ALL) : plat_name(c);
        float w = gfx_text_width(nm, 13, 1) + 28;
        int on = c == gm_cat;
        if (on) L_grad_pill(tx, 60, w, 28, 14, plat_color(c < 0 ? GP_PS3 : c), WITH_ALPHA(C_ACC2, 230));
        else L_rrect(tx, 60, w, 28, 14, RGBA8(255, 255, 255, 16));
        L_text(tx + w / 2, 66, nm, 13, 1, on ? C_TEXT : C_DIM, ALIGN_CENTER);
        tx += w + 8;
    }
    if (!gm_n) {
        if (!games_scanning()) L_text_fit(SCR_W / 2, 250, SCR_W - 80, T(S_NO_GAMES), 14, 0, C_DIM, ALIGN_CENTER);
        return;
    }
    /* grid */
    int row = gm_sel / GM_COLS;
    if (row < gm_top) gm_top = row;
    if (row >= gm_top + GM_ROWS) gm_top = row - GM_ROWS + 1;
    int from = gm_top * GM_COLS, to = from + GM_COLS * GM_ROWS - 1;
    /* covers for the visible rows plus one row each side */
    int wf = from - GM_COLS, wt = to + GM_COLS;
    if (wf < 0) wf = 0;
    if (wt >= gm_n) wt = gm_n - 1;
    if (wt >= 0) games_window(gm_idx[wf], gm_idx[wt]);
    float cw = 112, ch = 156, gx = (SCR_W - (GM_COLS * cw + (GM_COLS - 1) * 22)) / 2, gy = 104;
    for (int k = from; k <= to && k < gm_n; k++) {
        game_t *g = games_get(gm_idx[k]);
        if (!g) continue;
        int r = (k - from) / GM_COLS, c = (k - from) % GM_COLS;
        float x = gx + c * (cw + 22), y = gy + r * (ch + 46) + (1 - a) * 30;
        int on = k == gm_sel;
        float lift = on ? -6 : 0;
        float sc = on ? 1.06f : 1.0f;
        float w = cw * sc, h = ch * sc, xx = x - (w - cw) / 2, yy = y - (h - ch) / 2 + lift;
        if (on) gfx_glow(MXP(xx + w / 2), yy + h / 2, w * 0.9f, WITH_ALPHA(plat_color(g->plat), 120));
        gfx_rect(MX(xx + 4, w), yy + 6, w, h, RGBA8(0, 0, 0, 70));
        if (g->cov == GC_READY && g->tex) {
            gfx_texture(g->tex, MX(xx, w), yy, w, h, 0xFFFFFFFF);
        } else {
            gfx_rrect_grad(MX(xx, w), yy, w, h, 10, plat_color(g->plat), RGBA8(20, 18, 50, 255));
            gfx_text(MXP(xx + w / 2), yy + 18, plat_name(g->plat), 16, 1, WITH_ALPHA(C_TEXT, 200), ALIGN_CENTER);
            gfx_text_fit(MXP(xx + w / 2), yy + h / 2 - 8, w - 14, g->title, 11, 1, C_TEXT, ALIGN_CENTER);
            if (g->cov == GC_LOADING || g->cov == GC_PENDING) { float tt = gfx_frame() / 60.0f; gfx_circle(MXP(xx + w / 2) + cosf(tt * 6) * 8, yy + h - 24 + sinf(tt * 6) * 8, 2.5f, C_TEXT); }
        }
        if (on) gfx_rrect_border(MX(xx - 3, w + 6), yy - 3, w + 6, h + 6, 10, 2.5f, RGBA8(255, 255, 255, 200 + (int)(sinf(gfx_frame() * 0.12f) * 50)));
        /* platform badge */
        float bw = gfx_text_width(plat_name(g->plat), 9, 1) + 12;
        gfx_rrect(MX(xx + w - bw - 5, bw), yy + 5, bw, 15, 7, WITH_ALPHA(plat_color(g->plat), 235));
        gfx_text(MXP(xx + w - bw / 2 - 5), yy + 7, plat_name(g->plat), 9, 1, C_TEXT, ALIGN_CENTER);
        gfx_text_fit(MXP(x + cw / 2), y + ch + 8, cw + 16, g->title, 11, on, on ? C_TEXT : C_DIM, ALIGN_CENTER);
    }
    /* selected game details */
    game_t *g = games_get(gm_idx[gm_sel]);
    if (g) {
        char info[200];
        snprintf(info, sizeof(info), "%s  \xC2\xB7  %s  \xC2\xB7  %s", plat_name(g->plat), g->drive, g->name);
        L_text_fit(28, SCR_H - 66, SCR_W - 56, info, 11, 0, C_DIM, ALIGN_LEFT);
    }
    /* scrollbar */
    int rows = (gm_n + GM_COLS - 1) / GM_COLS;
    if (rows > GM_ROWS) {
        float track = 2 * (ch + 46) - 20, th = track * GM_ROWS / rows;
        float ty = gy + (track - th) * gm_top / (rows - GM_ROWS);
        gfx_rrect(MX(SCR_W - 14, 4), gy, 4, track, 2, RGBA8(255, 255, 255, 20));
        gfx_rrect(MX(SCR_W - 14, 4), ty, 4, th, 2, WITH_ALPHA(C_ACC2, 200));
    }
}

static void handle_games(void)
{
    gm_filter();
    uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
    if (in_pressed & B_CANCEL) { games_mode = 0; ui = UI_BROWSE; return; }
    if (in_pressed & B_SELECT) { games_scan(vols, vol_count); gm_sel = 0; gm_top = 0; return; }
    if (in_pressed & (B_L1 | B_R1)) {
        gm_cat += (in_pressed & B_R1) ? 1 : -1;
        if (gm_cat >= GP_COUNT) gm_cat = -1;
        if (gm_cat < -1) gm_cat = GP_COUNT - 1;
        gm_sel = 0; gm_top = 0; gm_anim = 0.4f;
        return;
    }
    if (!gm_n) return;
    if (in_active & R) { if (gm_sel + 1 < gm_n) gm_sel++; }
    if (in_active & L) { if (gm_sel > 0) gm_sel--; }
    if (in_active & B_DOWN) { if (gm_sel + GM_COLS < gm_n) gm_sel += GM_COLS; else if (gm_sel / GM_COLS < (gm_n - 1) / GM_COLS) gm_sel = gm_n - 1; }
    if (in_active & B_UP) { if (gm_sel >= GM_COLS) gm_sel -= GM_COLS; }
    if (in_active & B_R2) { gm_sel += GM_COLS * GM_ROWS; if (gm_sel >= gm_n) gm_sel = gm_n - 1; }
    if (in_active & B_L2) { gm_sel -= GM_COLS * GM_ROWS; if (gm_sel < 0) gm_sel = 0; }
    game_t *g = games_get(gm_idx[gm_sel]);
    if (!g) return;
    if (in_pressed & B_TRI) {
        /* open the game's folder in the file list */
        char dir[1024]; snprintf(dir, sizeof(dir), "%s", g->path);
        char *s = strrchr(dir, '/'); if (s) *s = 0;
        games_mode = 0; ui = UI_BROWSE; focus_list = 1; games_btn = 0;
        open_dir(dir, g->name);
        return;
    }
    if (in_pressed & B_OK) {
        snprintf(launch_src, sizeof(launch_src), "%s", g->path);
        launch_sys = SYS_WEBMAN;
        confirm_sel = 0;
        open_modal(UI_CONFIRM_LAUNCH);
    }
}

/* ---------- built-in video player ---------- */
static char vid_name[256];
static uint64_t vid_last_input;
static int vid_err_shown;
static int sub_size = 1;   /* 0 small, 1 medium, 2 large */
static char vid_path[1400];
/* player preferences (saved) */
typedef struct { int size, color, pos, bg, aspect, debug; } pprefs_t;
static pprefs_t pp = { 1, 0, 0, 0, 0, 0 };
#define PP_FILE APP_DIR "/player.cfg"
static void pp_load(void) { FILE *f = fopen(PP_FILE, "rb"); if (f) { pprefs_t t; if (fread(&t, sizeof(t), 1, f) == 1) pp = t; fclose(f); } sub_size = pp.size % 3; video_set_aspect(pp.aspect % 3); }
static void pp_save(void) { pp.size = sub_size; FILE *f = fopen(PP_FILE, "wb"); if (f) { fwrite(&pp, sizeof(pp), 1, f); fclose(f); } }
static int vid_panel;      /* 0 none, 1 settings, 2 subtitle file picker */
static int vp_sel;

static void video_start(int idx)
{
    char p[1400];
    path_join(p, sizeof(p), cwd, ents[idx].name);
    if (!video_supported(ents[idx].name)) {
        if (find_movian()) { ask_launch(idx); return; }
        show_toast(T(S_VID_FMT_NO)); return;
    }
    audio_stop();
    snprintf(vid_name, sizeof(vid_name), "%s", ents[idx].name);
    snprintf(vid_path, sizeof(vid_path), "%s", p);
    vid_panel = 0;
    pp_load();
    vid_last_input = gfx_frame();
    vid_err_shown = 0;
    ui = UI_VIDEO;
    if (!video_open(p)) {
        static const int msg[] = { S_VID_ERR_OPEN, S_VID_ERR_OPEN, S_VID_ERR_FORMAT, S_VID_ERR_CODEC, S_VID_ERR_DEC, S_VID_ERR_MEM };
        int e = video_error();
        char m[200];
        if (e == VERR_CODEC) snprintf(m, sizeof(m), T(S_VCODEC_NO), video_error_detail());
        else if (e == VERR_DECODER) snprintf(m, sizeof(m), T(S_VDEC_FAIL), video_error_detail());
        else snprintf(m, sizeof(m), "%s", T(msg[e >= 0 && e <= 5 ? e : 1]));
        show_toast(m);
        video_close();
        ui = UI_BROWSE;
        return;
    }
    if (video_audio_warning()[0]) { char m[200]; snprintf(m, sizeof(m), T(S_NO_AUDIO_FMT), video_audio_warning()); show_toast(m); }
}

/* subtitle text: word-wrapped, outlined, bottom centered; Arabic is shaped by gfx_text */
static void draw_subtitles(float bottom)
{
    const char *txt = video_subtitle_text();
    if (!txt[0]) return;
    static const int sizes[3] = { 16, 20, 25 };
    int sz = sizes[sub_size];
    float maxw = SCR_W - 90;
    char lines[12][400]; int nl = 0;
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", txt);
    char *save = NULL;
    for (char *ln = strtok_r(buf, "\n", &save); ln && nl < 12; ln = strtok_r(NULL, "\n", &save)) {
        /* wrap by words */
        char cur[400] = "";
        char *ws = NULL;
        for (char *w = strtok_r(ln, " ", &ws); w; w = strtok_r(NULL, " ", &ws)) {
            char test[400];
            snprintf(test, sizeof(test), "%s%s%s", cur, cur[0] ? " " : "", w);
            if (cur[0] && gfx_text_width(test, sz, 1) > maxw) { if (nl < 12) snprintf(lines[nl++], 400, "%s", cur); snprintf(cur, sizeof(cur), "%s", w); }
            else snprintf(cur, sizeof(cur), "%s", test);
        }
        if (cur[0] && nl < 12) snprintf(lines[nl++], 400, "%s", cur);
    }
    float lh = sz * 1.35f;
    bottom -= (pp.pos % 5) * 22;
    float y = bottom - nl * lh;
    static const uint32_t cols[3] = { RGBA8(255, 255, 255, 255), RGBA8(255, 230, 90, 255), RGBA8(120, 230, 255, 255) };
    uint32_t col = cols[pp.color % 3];
    for (int i = 0; i < nl; i++, y += lh) {
        float cx = SCR_W / 2;
        if (pp.bg == 1) {
            float tw = gfx_text_width(lines[i], sz, 1) + 24;
            gfx_rrect(cx - tw / 2, y - 3, tw, lh + 2, 6, RGBA8(0, 0, 0, 170));
        } else {
            static const float ox[8] = { -2, 2, 0, 0, -1.5f, 1.5f, -1.5f, 1.5f }, oy[8] = { 0, 0, -2, 2, -1.5f, -1.5f, 1.5f, 1.5f };
            for (int k = 0; k < 8; k++) gfx_text(cx + ox[k], y + oy[k], lines[i], sz, 1, RGBA8(0, 0, 0, 230), ALIGN_CENTER);
        }
        gfx_text(cx, y, lines[i], sz, 1, col, ALIGN_CENTER);
    }
}

static void draw_video(void)
{
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA8(0, 0, 0, 255));
    video_render(0, 0, SCR_W, SCR_H);
    {
        int ov = video_state() != VID_PLAYING || gfx_frame() - vid_last_input < 180;
        draw_subtitles(ov ? SCR_H - 128 : SCR_H - 28);
    }
    int vw, vhh, st = video_state();
    if (!video_size(&vw, &vhh) && st == VID_PLAYING) {
        float t = gfx_frame() / 60.0f;
        for (int i = 0; i < 8; i++) {
            float a = t * 4 + i * 0.785f;
            gfx_circle(SCR_W / 2 + cosf(a) * 22, SCR_H / 2 - 10 + sinf(a) * 22, 4, RGBA8(255, 255, 255, 40 + i * 25));
        }
        gfx_text(SCR_W / 2, SCR_H / 2 + 30, T(S_LOADING), 13, 0, C_DIM, ALIGN_CENTER);
    }
    int show = st != VID_PLAYING || gfx_frame() - vid_last_input < 180;
    if (!show) return;
    /* top bar */
    gfx_grad_v(0, 0, SCR_W, 70, RGBA8(0, 0, 0, 200), RGBA8(0, 0, 0, 0));
    gfx_text_fit(RTL() ? SCR_W - 40 : 40, 16, SCR_W - 80, vid_name, 16, 1, C_TEXT, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
    {
        char info[160];
        if (video_sub_delay() != 0) { char dl[48]; snprintf(dl, sizeof(dl), T(S_SUB_DELAY_L), video_sub_delay()); snprintf(info, sizeof(info), "%s  ·  %s", video_format_info(), dl); }
        else snprintf(info, sizeof(info), "%s", video_format_info());
        gfx_text_fit(RTL() ? SCR_W - 40 : 40, 40, SCR_W - 80, info, 11, 0, C_DIM, RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
        if (pp.debug) gfx_text_fit(RTL() ? SCR_W - 40 : 40, 56, SCR_W - 80, video_debug_info(), 10, 0, RGBA8(140, 150, 190, 200), RTL() ? ALIGN_RIGHT : ALIGN_LEFT);
    }
    /* bottom bar */
    gfx_grad_v(0, SCR_H - 120, SCR_W, 120, RGBA8(0, 0, 0, 0), RGBA8(0, 0, 0, 220));
    double pos = video_position(), dur = video_duration();
    if (pos > dur) pos = dur;
    float bx = 40, bw = SCR_W - 80, by = SCR_H - 70;
    float p = dur > 0 ? (float)(pos / dur) : 0;
    gfx_rrect(bx, by, bw, 6, 3, RGBA8(255, 255, 255, 40));
    if (bw * p > 6) gfx_rrect_grad(bx, by, bw * p, 6, 3, RGBA8(120, 190, 255, 255), RGBA8(130, 80, 255, 255));
    gfx_circle(bx + bw * p, by + 3, 7, RGBA8(255, 255, 255, 255));
    char a1[24], a2[24];
    fmt_time((uint32_t)pos, a1, sizeof(a1));
    fmt_time((uint32_t)dur, a2, sizeof(a2));
    gfx_text(bx, by + 16, a1, 12, 1, C_TEXT, ALIGN_LEFT);
    gfx_text(bx + bw, by + 16, a2, 12, 0, C_DIM, ALIGN_RIGHT);
    /* play/pause glyph */
    float cx = SCR_W / 2, cy = by + 26;
    if (st == VID_PAUSED) { gfx_rect(cx - 8, cy - 9, 5, 18, C_TEXT); gfx_rect(cx + 3, cy - 9, 5, 18, C_TEXT); }
    else if (st == VID_ENDED) gfx_text(cx, cy - 8, T(S_STOP), 12, 1, C_TEXT, ALIGN_CENTER);
    else gfx_circle(cx, cy, 4, RGBA8(255, 255, 255, 160));
}

/* ---------- player settings panel and subtitle file picker ---------- */
enum { VP_SUBTRACK, VP_LOADSUB, VP_DLSUB, VP_SUBKEY, VP_SUBSIZE, VP_SUBCOLOR, VP_SUBPOS, VP_SUBBG, VP_SUBDELAY, VP_AUDIO, VP_ASPECT, VP_DEBUG, VP_COUNT };
static const int vp_label[VP_COUNT] = { S_PL_SUBTRACK, S_PL_LOADSUB, S_PL_DLSUB, S_PL_SUBKEY, S_PL_SUBSIZE, S_PL_SUBCOLOR, S_PL_SUBPOS, S_PL_SUBBG, S_PL_SUBDELAY, S_PL_AUDIO, S_PL_ASPECT, S_PL_DEBUG };

static void vp_value(int i, char *o, int n)
{
    o[0] = 0;
    switch (i) {
    case VP_SUBTRACK: { int c = video_sub_current(); snprintf(o, n, "%s", c <= 0 ? T(S_SUB_OFF_L) : video_sub_label(c)); break; }
    case VP_SUBSIZE: { static const int nm[3] = { S_SMALL, S_MEDIUM, S_LARGE }; snprintf(o, n, "%s", T(nm[sub_size % 3])); break; }
    case VP_SUBCOLOR: { static const int nm[3] = { S_C_WHITE, S_C_YELLOW, S_C_CYAN }; snprintf(o, n, "%s", T(nm[pp.color % 3])); break; }
    case VP_SUBPOS: snprintf(o, n, "%d", pp.pos % 5); break;
    case VP_SUBBG: snprintf(o, n, "%s", T(pp.bg ? S_BG_BOX : S_BG_OUTLINE)); break;
    case VP_SUBDELAY: snprintf(o, n, "%+.1f s", video_sub_delay()); break;
    case VP_AUDIO: { int c = video_audio_current(); snprintf(o, n, "%s", c < 0 ? "—" : video_audio_label(c)); break; }
    case VP_ASPECT: { static const int nm[3] = { S_ASP_FIT, S_ASP_FILL, S_ASP_STRETCH }; snprintf(o, n, "%s", T(nm[pp.aspect % 3])); break; }
    case VP_DEBUG: snprintf(o, n, "%s", T(pp.debug ? S_ON : S_OFF)); break;
    case VP_SUBKEY: snprintf(o, n, "%s", T(subdl_key()[0] ? S_SET : S_NOT_SET)); break;
    }
}

static void vp_change(int i, int d)
{
    switch (i) {
    case VP_SUBTRACK: { int n = video_sub_count(); if (n > 1) video_sub_select((video_sub_current() + d + n) % n); break; }
    case VP_SUBSIZE: sub_size = (sub_size + d + 3) % 3; break;
    case VP_SUBCOLOR: pp.color = (pp.color + d + 3) % 3; break;
    case VP_SUBPOS: pp.pos = (pp.pos + d + 5) % 5; break;
    case VP_SUBBG: pp.bg = !pp.bg; break;
    case VP_SUBDELAY: video_sub_delay_add(d * 0.1); break;
    case VP_AUDIO: { int n = video_audio_count(); if (n > 1) video_audio_select((video_audio_current() + d + n) % n); break; }
    case VP_ASPECT: pp.aspect = (pp.aspect + d + 3) % 3; video_set_aspect(pp.aspect); break;
    case VP_DEBUG: pp.debug = !pp.debug; break;
    }
    pp_save();
}

/* picker: "" = list of drives, otherwise a folder */
static char pk_dir[1400];
static vfs_entry_t *pk_ents;
static int pk_n, pk_sel, pk_top;

static int is_sub_name(const char *n)
{
    const char *e = strrchr(n, '.');
    return e && (!strcasecmp(e, ".srt") || !strcasecmp(e, ".ass") || !strcasecmp(e, ".ssa") || !strcasecmp(e, ".vtt"));
}

static int pk_cmp(const void *a, const void *b)
{
    const vfs_entry_t *x = (const vfs_entry_t *)a, *y = (const vfs_entry_t *)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void pk_open(const char *dir)
{
    if (!pk_ents) pk_ents = (vfs_entry_t *)calloc(2048, sizeof(vfs_entry_t));
    if (!pk_ents) return;
    snprintf(pk_dir, sizeof(pk_dir), "%s", dir);
    pk_n = 0; pk_sel = 0; pk_top = 0;
    if (!dir[0]) {
        for (int i = 0; i < vol_count && pk_n < 2048; i++) {
            snprintf(pk_ents[pk_n].name, sizeof(pk_ents[pk_n].name), "%s", vols[i].label);
            pk_ents[pk_n].is_dir = 1; pk_ents[pk_n].size = i; pk_n++;
        }
        return;
    }
    static vfs_entry_t tmp[2048];
    int n = vfs_list(dir, tmp, 2048);
    for (int i = 0; i < n; i++) if (tmp[i].is_dir || is_sub_name(tmp[i].name)) pk_ents[pk_n++] = tmp[i];
    qsort(pk_ents, pk_n, sizeof(vfs_entry_t), pk_cmp);
}

static void pk_up(void)
{
    if (!pk_dir[0]) { vid_panel = 1; return; }
    for (int i = 0; i < vol_count; i++) if (!strcmp(pk_dir, vols[i].root)) { pk_open(""); return; }
    char p[1400];
    snprintf(p, sizeof(p), "%s", pk_dir);
    size_t l = strlen(p);
    if (l > 1 && p[l - 1] == '/') p[--l] = 0;
    char *s = strrchr(p, '/');
    if (!s) { pk_open(""); return; }
    if (s == p || s[-1] == ':') s[1] = 0; else *s = 0;
    pk_open(p);
}

static void draw_vid_panel(void)
{
    float w = 420, x = RTL() ? 0 : SCR_W - w, y = 0, h = SCR_H;
    gfx_rect(x, y, w, h, RGBA8(10, 12, 30, 235));
    gfx_rect(RTL() ? x + w - 1 : x, y, 1, h, RGBA8(255, 255, 255, 30));
    float tx = RTL() ? x + w - 24 : x + 24;
    int al = RTL() ? ALIGN_RIGHT : ALIGN_LEFT, ar = RTL() ? ALIGN_LEFT : ALIGN_RIGHT;
    float vx = RTL() ? x + 24 : x + w - 24;
    if (vid_panel == 1) {
        gfx_text(tx, 22, T(S_PL_SETTINGS), 18, 1, C_TEXT, al);
        for (int i = 0; i < VP_COUNT; i++) {
            float iy = 56 + i * 37;
            int s = i == vp_sel;
            if (s) gfx_rrect(x + 10, iy - 4, w - 20, 34, 10, RGBA8(110, 100, 230, 90));
            gfx_text_fit(tx, iy + 4, w * 0.5f, T(vp_label[i]), 13, s, s ? C_TEXT : C_DIM, al);
            char v[160]; vp_value(i, v, sizeof(v));
            if (v[0]) {
                gfx_text_fit(vx, iy + 4, w * 0.42f, v, 13, 1, s ? RGBA8(175, 169, 236, 255) : C_TEXT, ar);
            }
        }
    } else if (vid_panel == 3) {
        gfx_text(tx, 22, T(S_SUBS_ONLINE), 18, 1, C_TEXT, al);
        gfx_text_fit(tx, 48, w - 48, subdl_query(), 11, 0, C_DIM, al);
        int stt = subdl_state();
        if (stt == SUBDL_SEARCHING || stt == SUBDL_DOWNLOADING) {
            float tt = gfx_frame() / 60.0f, cx = x + w / 2;
            for (int i = 0; i < 8; i++) { float an = tt * 5 + i * 0.785f; gfx_circle(cx + cosf(an) * 12, 150 + sinf(an) * 12, 2.4f, WITH_ALPHA(C_TEXT, 40 + i * 26)); }
            gfx_text(cx, 180, T(stt == SUBDL_SEARCHING ? S_SUBS_SEARCHING : S_SUBS_DOWNLOADING), 13, 0, C_DIM, ALIGN_CENTER);
        } else if (stt == SUBDL_ERROR && !subdl_count()) {
            gfx_text_fit(x + w / 2, 130, w - 40, T(S_SUBS_NONE), 14, 1, C_TEXT, ALIGN_CENTER);
            if (strcmp(subdl_error(), "0")) gfx_text_fit(x + w / 2, 156, w - 40, subdl_error(), 11, 0, C_DIM, ALIGN_CENTER);
        }
        if (!subdl_key()[0]) gfx_text_fit(x + w / 2, SCR_H - 70, w - 30, T(S_SUBS_KEYHINT), 10, 0, RGBA8(255, 200, 120, 255), ALIGN_CENTER);
        if (stt == SUBDL_RESULTS || (stt == SUBDL_ERROR && subdl_count())) {
            int n = subdl_count(), rows = 10;
            if (pk_sel >= n) pk_sel = n - 1;
            if (pk_sel < pk_top) pk_top = pk_sel;
            if (pk_sel >= pk_top + rows) pk_top = pk_sel - rows + 1;
            for (int k = pk_top; k < n && k < pk_top + rows; k++) {
                const subres_t *r = subdl_get(k);
                float iy = 74 + (k - pk_top) * 36;
                int sl = k == pk_sel;
                if (sl) gfx_rrect(x + 10, iy - 4, w - 20, 32, 10, RGBA8(110, 100, 230, 90));
                uint32_t lc = !strcasecmp(r->lang, "ar") ? RGBA8(60, 200, 140, 255) : RGBA8(90, 140, 255, 255);
                gfx_rrect(RTL() ? x + w - 50 : x + 18, iy + 1, 32, 18, 9, lc);
                gfx_text(RTL() ? x + w - 34 : x + 34, iy + 3, r->lang, 10, 1, C_TEXT, ALIGN_CENTER);
                gfx_text_fit(RTL() ? x + w - 60 : x + 58, iy + 3, w - 90, r->title, 12, sl, sl ? C_TEXT : C_DIM, al);
            }
        }
    } else {
        gfx_text(tx, 22, T(S_PICK_SUB), 18, 1, C_TEXT, al);
        gfx_text_fit(tx, 48, w - 48, pk_dir[0] ? pk_dir : T(S_DRIVES), 11, 0, C_DIM, al);
        if (!pk_n) gfx_text_fit(x + w / 2, 120, w - 40, T(S_NO_SUB_FILES), 13, 0, C_DIM, ALIGN_CENTER);
        int rows = 10;
        if (pk_sel < pk_top) pk_top = pk_sel;
        if (pk_sel >= pk_top + rows) pk_top = pk_sel - rows + 1;
        for (int k = pk_top; k < pk_n && k < pk_top + rows; k++) {
            float iy = 74 + (k - pk_top) * 36;
            int s = k == pk_sel;
            if (s) gfx_rrect(x + 10, iy - 4, w - 20, 32, 10, RGBA8(110, 100, 230, 90));
            draw_file_icon(RTL() ? x + w - 44 : x + 20, iy - 1, 22, pk_ents[k].is_dir ? FT_DIR : FT_TEXT, pk_ents[k].name);
            gfx_text_fit(RTL() ? x + w - 54 : x + 52, iy + 3, w - 80, pk_ents[k].name, 13, s, s ? C_TEXT : C_DIM, al);
        }
    }
}

static void handle_vid_panel(void)
{
    uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
    if (vid_panel == 1) {
        if (in_active & B_DOWN) vp_sel = (vp_sel + 1) % VP_COUNT;
        if (in_active & B_UP) vp_sel = (vp_sel + VP_COUNT - 1) % VP_COUNT;
        if (in_active & R) vp_change(vp_sel, 1);
        if (in_active & L) vp_change(vp_sel, -1);
        if (in_pressed & B_OK) {
            if (vp_sel == VP_LOADSUB) {
                /* start in the video's folder */
                char d[1400]; snprintf(d, sizeof(d), "%s", vid_path);
                char *s = strrchr(d, '/'); if (s) *s = 0;
                pk_open(d);
                if (!pk_n && pk_dir[0]) pk_open("");
                vid_panel = 2;
            } else if (vp_sel == VP_DLSUB) {
                if (video_state() == VID_PLAYING) video_toggle_pause();
                const char *bn = strrchr(vid_path, '/');
                subdl_search(bn ? bn + 1 : vid_path);
                pk_sel = 0; pk_top = 0;
                vid_panel = 3;
            } else if (vp_sel == VP_SUBKEY) {
                if (video_state() == VID_PLAYING) video_toggle_pause();
                osk_purpose = 8;
                if (osk_open(T(S_PL_SUBKEY), subdl_key())) ui = UI_OSK;
            } else vp_change(vp_sel, 1);
        }
        if (in_pressed & (B_CANCEL | B_START)) vid_panel = 0;
        return;
    }
    if (vid_panel == 3) {
        int n = subdl_count(), stt = subdl_state();
        if (stt == SUBDL_READY) {
            if (video_add_subtitle_file(subdl_result())) { show_toast(T(S_SUB_LOADED)); vid_panel = 0; if (video_state() == VID_PAUSED) video_toggle_pause(); }
            else show_toast(T(S_SUB_LOAD_FAIL));
            subdl_ack();
            return;
        }
        if (stt == SUBDL_ERROR && n) { show_toast(subdl_error()[0] ? subdl_error() : T(S_SUB_LOAD_FAIL)); subdl_ack(); }
        if (in_pressed & B_CANCEL) { vid_panel = 1; return; }
        if (in_pressed & B_START) { vid_panel = 0; return; }
        if (stt == SUBDL_SEARCHING || stt == SUBDL_DOWNLOADING || !n) return;
        if (in_active & B_DOWN) pk_sel = (pk_sel + 1) % n;
        if (in_active & B_UP) pk_sel = (pk_sel + n - 1) % n;
        if (in_pressed & B_OK) {
            char base[1400]; snprintf(base, sizeof(base), "%s", vid_path);
            char *dot = strrchr(base, '.'), *sl = strrchr(base, '/');
            if (dot && (!sl || dot > sl)) *dot = 0;
            subdl_download(pk_sel, base);
        }
        return;
    }
    /* picker */
    if (pk_n && (in_active & B_DOWN)) pk_sel = (pk_sel + 1) % pk_n;
    if (pk_n && (in_active & B_UP)) pk_sel = (pk_sel + pk_n - 1) % pk_n;
    if (in_pressed & B_CANCEL) { pk_up(); return; }
    if (in_pressed & B_START) { vid_panel = 0; return; }
    if (pk_n && (in_pressed & B_OK)) {
        vfs_entry_t *e = &pk_ents[pk_sel];
        if (!pk_dir[0]) { pk_open(vols[(int)e->size].root); return; }
        char p[1400];
        path_join(p, sizeof(p), pk_dir, e->name);
        if (e->is_dir) { pk_open(p); return; }
        if (video_add_subtitle_file(p)) { show_toast(T(S_SUB_LOADED)); vid_panel = 0; }
        else show_toast(T(S_SUB_LOAD_FAIL));
    }
}

static void handle_video(void)
{
    if (vid_panel) { if (in_pressed) vid_last_input = gfx_frame(); handle_vid_panel(); return; }
    if (in_pressed & B_START) { vid_panel = 1; vp_sel = 0; vid_last_input = gfx_frame(); return; }
    uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
    if (in_pressed) vid_last_input = gfx_frame();
    if (in_pressed & B_CANCEL) { video_close(); ui = UI_BROWSE; return; }
    if (in_pressed & B_OK) {
        if (video_state() == VID_ENDED) { video_seek(-(int)video_duration() - 5); }
        else video_toggle_pause();
    }
    if (in_pressed & R) video_seek(10);
    if (in_pressed & L) video_seek(-10);
    if (in_pressed & B_R1) video_seek(60);
    if (in_pressed & B_L1) video_seek(-60);
    if (in_pressed & B_SQU) {
        char lb[120], m[200];
        if (video_cycle_subtitle(lb, sizeof(lb))) { snprintf(m, sizeof(m), T(S_SUB_LABEL), strcmp(lb, "Off") ? lb : T(S_SUB_OFF_L)); show_toast(m); }
        else show_toast(T(S_NO_SUBS));
    }
    if (in_pressed & B_TRI) {
        char lb[120], m[200];
        if (video_cycle_audio(lb, sizeof(lb))) { snprintf(m, sizeof(m), T(S_AUD_LABEL), lb); show_toast(m); }
        else show_toast(T(S_ONE_AUDIO));
    }
    if (in_active & (B_R2 | B_L2)) {
        video_sub_delay_add((in_active & B_R2) ? 0.1 : -0.1);
        char m[80]; snprintf(m, sizeof(m), T(S_SUB_DELAY_L), video_sub_delay()); show_toast(m);
    }
    if (in_pressed & B_SELECT) {
        sub_size = (sub_size + 1) % 3;
        pp_save();
        static const int nm[3] = { S_SMALL, S_MEDIUM, S_LARGE };
        char m[80]; snprintf(m, sizeof(m), T(S_SUB_SIZE_L), T(nm[sub_size])); show_toast(m);
    }
    if (video_state() == VID_ERROR && !vid_err_shown) { vid_err_shown = 1; show_toast(T(S_VID_ERR_DEC)); video_close(); ui = UI_BROWSE; }
}

/* ---------- text editor ---------- */
#define TXT_MAX_LINES 20000
static char **txt_lines;
static int txt_count, txt_sel, txt_top, txt_dirty, txt_crlf;
static char txt_path[1400];
static uint64_t discard_until;

static void text_free(void)
{
    for (int i = 0; i < txt_count; i++) free(txt_lines[i]);
    txt_count = 0;
}

static int text_open(const char *path, uint64_t size)
{
    if (size > 1024 * 1024) { show_toast(T(S_TOO_BIG)); return 0; }
    vfs_file_t *f = vfs_open_read(path);
    if (!f) return 0;
    char *b = (char *)malloc((size_t)size + 1);
    uint32_t got = 0;
    while (b && got < size) { int64_t r = vfs_read(f, b + got, (uint32_t)(size - got)); if (r <= 0) break; got += (uint32_t)r; }
    vfs_close(f);
    if (!b) return 0;
    b[got] = 0;
    if (!txt_lines) txt_lines = (char **)calloc(TXT_MAX_LINES, sizeof(char *));
    text_free();
    txt_crlf = strstr(b, "\r\n") != NULL;
    char *p = b;
    while (txt_count < TXT_MAX_LINES) {
        char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        txt_lines[txt_count] = (char *)malloc(n + 1);
        memcpy(txt_lines[txt_count], p, n); txt_lines[txt_count][n] = 0;
        for (char *q = txt_lines[txt_count]; *q; q++) if (*q == '\t') *q = ' ';
        txt_count++;
        if (!e) break;
        p = e + 1;
    }
    free(b);
    snprintf(txt_path, sizeof(txt_path), "%s", path);
    txt_sel = txt_top = txt_dirty = 0;
    return 1;
}

static int text_save(void)
{
    vfs_file_t *f = vfs_open_write(txt_path);
    if (!f) return 0;
    const char *eol = txt_crlf ? "\r\n" : "\n";
    int ok = 1;
    for (int i = 0; i < txt_count && ok; i++) {
        uint32_t n = (uint32_t)strlen(txt_lines[i]);
        if (n && vfs_write(f, txt_lines[i], n) != n) ok = 0;
        if (i < txt_count - 1 && vfs_write(f, eol, (uint32_t)strlen(eol)) != (int64_t)strlen(eol)) ok = 0;
    }
    vfs_close(f);
    if (ok) txt_dirty = 0;
    return ok;
}

static void draw_text_editor(void)
{
    gfx_grad_v(0, 0, SCR_W, SCR_H, RGBA8(12, 14, 30, 255), RGBA8(18, 10, 34, 255));
    char head[1600];
    snprintf(head, sizeof(head), "%s%s", txt_path, txt_dirty ? "  \xE2\x80\xA2" : "");
    gfx_rrect(16, 12, SCR_W - 32, 30, 15, RGBA8(255, 255, 255, 16));
    gfx_text_fit(30, 18, SCR_W - 200, head, 12, 1, txt_dirty ? RGBA8(255, 200, 120, 255) : C_TEXT, ALIGN_LEFT);
    char pos[48]; snprintf(pos, sizeof(pos), "%s %d / %d", T(S_LINE), txt_sel + 1, txt_count);
    gfx_text(SCR_W - 30, 18, pos, 12, 0, C_DIM, ALIGN_RIGHT);

    int visible = 18;
    if (txt_sel < txt_top) txt_top = txt_sel;
    if (txt_sel >= txt_top + visible) txt_top = txt_sel - visible + 1;
    for (int i = txt_top; i < txt_count && i < txt_top + visible; i++) {
        float ly = 52 + (i - txt_top) * 21.5f;
        if (i == txt_sel) gfx_rrect_grad(16, ly - 2, SCR_W - 32, 21, 6, WITH_ALPHA(C_ACC1, 150), WITH_ALPHA(C_ACC2, 150));
        char num[16]; snprintf(num, sizeof(num), "%d", i + 1);
        gfx_text(62, ly + 2, num, 11, 0, C_DIM, ALIGN_RIGHT);
        int rtl = has_rtl(txt_lines[i]);
        gfx_text_fit(rtl ? SCR_W - 28 : 74, ly + 1, SCR_W - 110, txt_lines[i], 12, 0, C_TEXT, rtl ? ALIGN_RIGHT : ALIGN_LEFT);
    }
}

static int confirm_discard(int dirty)
{
    if (!dirty) return 1;
    if (gfx_frame() < discard_until) return 1;
    discard_until = gfx_frame() + 120;
    show_toast(T(S_DISCARD));
    return 0;
}

static void handle_text(void)
{
    if (in_active & B_UP) { if (txt_sel > 0) txt_sel--; }
    if (in_active & B_DOWN) { if (txt_sel < txt_count - 1) txt_sel++; }
    if (in_active & B_L2) { txt_sel -= 17; if (txt_sel < 0) txt_sel = 0; }
    if (in_active & B_R2) { txt_sel += 17; if (txt_sel >= txt_count) txt_sel = txt_count - 1; }
    if (in_pressed & B_OK) {
        if (strlen(txt_lines[txt_sel]) > 255) show_toast(T(S_LINE_LONG));
        else { osk_purpose = 3; if (osk_open(T(S_EDIT_LINE), txt_lines[txt_sel])) ui = UI_OSK; }
    }
    if ((in_pressed & B_SQU) && txt_count < TXT_MAX_LINES) {
        memmove(&txt_lines[txt_sel + 2], &txt_lines[txt_sel + 1], sizeof(char *) * (txt_count - txt_sel - 1));
        txt_lines[txt_sel + 1] = (char *)calloc(1, 1);
        txt_count++; txt_sel++; txt_dirty = 1;
    }
    if ((in_pressed & B_SELECT) && txt_count > 1) {
        free(txt_lines[txt_sel]);
        memmove(&txt_lines[txt_sel], &txt_lines[txt_sel + 1], sizeof(char *) * (txt_count - txt_sel - 1));
        txt_count--; if (txt_sel >= txt_count) txt_sel = txt_count - 1; txt_dirty = 1;
    }
    if (in_pressed & B_START) {
        if (!cur_writable()) show_toast(T(S_READONLY_FILE));
        else show_toast(text_save() ? T(S_SAVED) : T(S_SAVE_FAIL));
    }
    if ((in_pressed & B_CANCEL) && confirm_discard(txt_dirty)) { text_free(); ui = UI_BROWSE; refresh_keep_wrapper(); }
}

/* ---------- hex editor ---------- */
#define HEX_MAX_EDITS 8192
static char hex_path[1400];
static uint64_t hex_size, hex_cur, hex_top, hex_cache_off = ~0ULL;
static uint8_t hex_cache[512];
static uint32_t hex_cache_len;
static uint64_t hex_edit_off[HEX_MAX_EDITS];
static uint8_t hex_edit_val[HEX_MAX_EDITS];
static int hex_edits;

static int hex_find_edit(uint64_t off) { for (int i = hex_edits - 1; i >= 0; i--) if (hex_edit_off[i] == off) return i; return -1; }

static void hex_fill_cache(void)
{
    if (hex_cache_off == hex_top) return;
    hex_cache_off = hex_top; hex_cache_len = 0;
    vfs_file_t *f = vfs_open_read(hex_path);
    if (!f) return;
    if (vfs_seek(f, hex_top) == 0) { int64_t r = vfs_read(f, hex_cache, sizeof(hex_cache)); hex_cache_len = r > 0 ? (uint32_t)r : 0; }
    vfs_close(f);
}

static int hex_byte(uint64_t off, int *edited)
{
    int e = hex_find_edit(off);
    if (edited) *edited = e >= 0;
    if (e >= 0) return hex_edit_val[e];
    if (off >= hex_cache_off && off < hex_cache_off + hex_cache_len) return hex_cache[off - hex_cache_off];
    return -1;
}

static void hex_open(const char *path, uint64_t size)
{
    snprintf(hex_path, sizeof(hex_path), "%s", path);
    hex_size = size; hex_cur = hex_top = 0; hex_cache_off = ~0ULL; hex_edits = 0;
}

static int hex_save(void)
{
    vfs_file_t *f = vfs_open_rw(hex_path);
    if (!f) return 0;
    int ok = 1;
    for (int i = 0; i < hex_edits && ok; i++) {
        if (vfs_seek(f, hex_edit_off[i]) != 0 || vfs_write(f, &hex_edit_val[i], 1) != 1) ok = 0;
    }
    vfs_close(f);
    if (ok) { hex_edits = 0; hex_cache_off = ~0ULL; }
    return ok;
}

#define HEX_ROWS 18
static int tbl_n; static char tbl_name[256];
static void tbl_row_text(uint64_t ro, char *out, int on);
static void tbl_cycle(void);
static void draw_hex(void)
{
    gfx_grad_v(0, 0, SCR_W, SCR_H, RGBA8(12, 14, 30, 255), RGBA8(18, 10, 34, 255));
    if (hex_cur < hex_top) hex_top = hex_cur & ~15ULL;
    if (hex_cur >= hex_top + HEX_ROWS * 16) hex_top = (hex_cur & ~15ULL) - (HEX_ROWS - 1) * 16;
    hex_fill_cache();
    char head[1600], sz[32];
    fmt_size(hex_size, sz, sizeof(sz));
    snprintf(head, sizeof(head), "%s  (%s)%s%s%s", hex_path, sz, tbl_n ? "  \xC2\xB7  TBL: " : "", tbl_n ? tbl_name : "", hex_edits ? "  \xE2\x80\xA2" : "");
    gfx_rrect(16, 12, SCR_W - 32, 30, 15, RGBA8(255, 255, 255, 16));
    gfx_text_fit(30, 18, SCR_W - 220, head, 12, 1, hex_edits ? RGBA8(255, 200, 120, 255) : C_TEXT, ALIGN_LEFT);
    char pos[48]; snprintf(pos, sizeof(pos), "%s 0x%08llX", T(S_OFFSET), (unsigned long long)hex_cur);
    gfx_text(SCR_W - 30, 18, pos, 12, 0, C_DIM, ALIGN_RIGHT);

    for (int r = 0; r < HEX_ROWS; r++) {
        uint64_t ro = hex_top + r * 16;
        if (ro >= hex_size) break;
        float ly = 52 + r * 21.5f;
        char off[24]; snprintf(off, sizeof(off), "%08llX", (unsigned long long)ro);
        gfx_text(24, ly, off, 11, 0, C_DIM, ALIGN_LEFT);
        char ascii[17];
        for (int c = 0; c < 16; c++) {
            uint64_t o = ro + c;
            ascii[c] = ' ';
            if (o >= hex_size) continue;
            int ed = 0, v = hex_byte(o, &ed);
            float bx = 110 + c * 32 + (c >= 8 ? 8 : 0);
            if (o == hex_cur) gfx_rrect(bx - 4, ly - 2, 28, 20, 5, C_ACC1);
            char hx[4]; snprintf(hx, sizeof(hx), v < 0 ? "??" : "%02X", v);
            gfx_text(bx, ly, hx, 12, ed, ed ? RGBA8(255, 200, 120, 255) : C_TEXT, ALIGN_LEFT);
            ascii[c] = (v >= 32 && v < 127) ? (char)v : '.';
        }
        ascii[16] = 0;
        if (tbl_n) { char tx[512]; tbl_row_text(ro, tx, sizeof(tx)); gfx_text_fit(650, ly, SCR_W - 670, tx, 12, 0, RGBA8(170, 220, 255, 255), ALIGN_LEFT); }
        else gfx_text(650, ly, ascii, 12, 0, C_DIM, ALIGN_LEFT);
    }
}

static void handle_hex(void)
{
    uint64_t last = hex_size ? hex_size - 1 : 0;
    if (in_active & B_UP && hex_cur >= 16) hex_cur -= 16;
    if (in_active & B_DOWN && hex_cur + 16 <= last) hex_cur += 16;
    if (in_active & B_LEFT && hex_cur > 0) hex_cur--;
    if (in_active & B_RIGHT && hex_cur < last) hex_cur++;
    if (in_active & B_L2) hex_cur = hex_cur >= 256 ? hex_cur - 256 : 0;
    if (in_active & B_R2) hex_cur = hex_cur + 256 <= last ? hex_cur + 256 : last;
    if (in_active & B_L1) hex_cur = hex_cur >= 65536 ? hex_cur - 65536 : 0;
    if (in_active & B_R1) hex_cur = hex_cur + 65536 <= last ? hex_cur + 65536 : last;
    if ((in_pressed & B_OK) && hex_size) {
        char cur[8]; int v = hex_byte(hex_cur, NULL);
        snprintf(cur, sizeof(cur), "%02X", v < 0 ? 0 : v);
        osk_purpose = 4;
        if (osk_open(T(S_EDIT_BYTE), cur)) ui = UI_OSK;
    }
    if (in_pressed & B_START) {
        if (!cur_writable()) show_toast(T(S_READONLY_FILE));
        else show_toast(hex_save() ? T(S_SAVED) : T(S_SAVE_FAIL));
    }
    if (in_pressed & B_SELECT) tbl_cycle();
    if ((in_pressed & B_SQU) && hex_size) { osk_purpose = 6; if (osk_open(T(S_TBL_FIND), "")) ui = UI_OSK; }
    if ((in_pressed & B_TRI) && hex_size) { osk_purpose = 7; if (osk_open(T(S_TBL_WRITE), "")) ui = UI_OSK; }
    if ((in_pressed & B_CANCEL) && confirm_discard(hex_edits > 0)) { ui = UI_BROWSE; refresh_keep_wrapper(); }
}

/* ---------- theme viewer ---------- */
static texture_t thm_tex;
static char thm_path[1400], thm_name[256];

static int theme_installed(const char *name)
{
    char p[1400];
    snprintf(p, sizeof(p), "%s/%s", THEME_DIR, name);
    return vfs_exists(p);
}

static void theme_open(int idx)
{
    if (thm_tex) { gfx_free_texture(thm_tex); thm_tex = NULL; }
    path_join(thm_path, sizeof(thm_path), cwd, ents[idx].name);
    snprintf(thm_name, sizeof(thm_name), "%s", ents[idx].name);
    uint8_t *b = NULL; uint32_t sz = 0;
    if (p3t_preview(thm_path, &b, &sz)) {
        texture_t t = (b[0] == 0x89) ? gfx_load_png_buffer(b, sz) : gfx_load_jpg_buffer(b, sz);
        thm_tex = gfx_make_thumb(t, 1280, 720);
        free(b);
    }
    ui = UI_THEME;
}

static void theme_install_paths(char srcs[][1024], int n)
{
    vfs_mkdir(THEME_DIR);
    thm_installing = 1;
    snprintf(ops_output, sizeof(ops_output), "%s", THEME_DIR);
    if (ops_start(OP_COPY, srcs, n, THEME_DIR)) open_modal(UI_PROGRESS_OPS);
}

static void draw_theme(void)
{
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA8(0, 0, 0, 255));
    if (thm_tex) {
        int iw, ih;
        gfx_texture_size(thm_tex, &iw, &ih);
        float sc = (float)SCR_W / iw;
        if ((float)SCR_H / ih > sc) sc = (float)SCR_H / ih;
        gfx_texture(thm_tex, (SCR_W - iw * sc) / 2, (SCR_H - ih * sc) / 2, iw * sc, ih * sc, RGBA8(255, 255, 255, 255));
    } else gfx_text(SCR_W / 2, SCR_H / 2 - 10, T(S_THEME_NO_PREVIEW), 15, 1, C_TEXT, ALIGN_CENTER);
    gfx_grad_v(0, 0, SCR_W, 70, RGBA8(0, 0, 0, 190), RGBA8(0, 0, 0, 0));
    gfx_text(MXP(28), 14, T(S_THEME), 11, 1, C_DIM, MA(ALIGN_LEFT));
    gfx_text_fit(MXP(28), 30, SCR_W - 200, thm_name, 16, 1, C_TEXT, MA(ALIGN_LEFT));
    if (theme_installed(thm_name)) {
        float w = gfx_text_width(T(S_THEME_INSTALLED), 11, 1) + 24;
        L_rrect(SCR_W - 28 - w, 22, w, 24, 12, C_OKC);
        L_text(SCR_W - 28 - w / 2, 27, T(S_THEME_INSTALLED), 11, 1, C_TEXT, ALIGN_CENTER);
    }
}

static void handle_theme(void)
{
    if (in_pressed & B_CANCEL) { if (thm_tex) { gfx_free_texture(thm_tex); thm_tex = NULL; } ui = UI_BROWSE; return; }
    if ((in_pressed & B_OK) && !theme_installed(thm_name)) {
        static char one[1][1024];
        snprintf(one[0], sizeof(one[0]), "%s", thm_path);
        if (thm_tex) { gfx_free_texture(thm_tex); thm_tex = NULL; }
        ui = UI_BROWSE;
        theme_install_paths(one, 1);
    }
}

static void open_props(void)
{
    if (ent_count && ents[sel].is_dir) { char p[1400]; path_join(p, sizeof(p), cwd, ents[sel].name); dirsize_start(p); }
    open_modal(UI_PROPS);
}

static int find_movian(void)
{
    static const char *ids[] = { "HTSS00003", "MOVIAN000" };
    static const char *bins[] = { "movian.self", "showtime.self", "EBOOT.BIN" };
    for (unsigned i = 0; i < 2; i++)
        for (unsigned b = 0; b < 3; b++) {
            snprintf(movian_self, sizeof(movian_self), "/dev_hdd0/game/%s/USRDIR/%s", ids[i], bins[b]);
            if (vfs_exists(movian_self)) return 1;
        }
    movian_self[0] = 0;
    return 0;
}

static void ask_launch(int idx)
{
    entry_t *e = &ents[idx];
    launch_sys = launch_kind(e->name);
    path_join(launch_src, sizeof(launch_src), cwd, e->name);
    if (webman_is_game(e->name, e->is_dir, launch_src)) launch_sys = SYS_WEBMAN;
    if (e->type == FT_VIDEO) {
        if (!find_movian()) { show_toast(T(S_NO_MOVIAN)); return; }
        launch_sys = SYS_MOVIAN;
    }
    if (launch_sys != SYS_SELF && launch_sys != SYS_WEBMAN && launch_sys != SYS_MOVIAN) {
        char m[128];
        if (!launch_retroarch_installed()) { snprintf(m, sizeof(m), T(S_NO_RA), launch_system_name(launch_sys)); show_toast(m); return; }
        if (!launch_find_core(launch_sys, launch_core, sizeof(launch_core), launch_core_name, sizeof(launch_core_name))) {
            snprintf(m, sizeof(m), T(S_NO_CORE), launch_system_name(launch_sys)); show_toast(m); return;
        }
    }
    confirm_sel = 0;
    open_modal(UI_CONFIRM_LAUNCH);
}

static void draw_confirm_launch(void)
{
    float x, y, w = 480, h = 240;
    draw_modal_frame(w, h, &x, &y);
    const char *bn = strrchr(launch_src, '/');
    bn = bn ? bn + 1 : launch_src;
    gfx_text(SCR_W / 2, y + 22, launch_sys == SYS_SELF ? T(S_LAUNCH_Q) : launch_sys == SYS_WEBMAN ? T(S_WM_Q) :
             launch_sys == SYS_MOVIAN ? T(S_MOVIAN_Q) : T(S_PLAY_GAME), 20, 1, C_TEXT, ALIGN_CENTER);
    draw_file_icon(MXP(SCR_W / 2) - (RTL() ? -28 : 28), y + 60, 56, launch_sys == SYS_SELF ? FT_SYSTEM : launch_sys == SYS_MOVIAN ? FT_VIDEO : FT_DISC, bn);
    gfx_text_fit(SCR_W / 2, y + 126, w - 60, bn, 14, 1, C_TEXT, ALIGN_CENTER);
    if (launch_sys == SYS_MOVIAN) {
        if (vfs_backend(launch_src) != VFS_NATIVE) gfx_text_fit(SCR_W / 2, y + 150, w - 40, T(S_MOVIAN_COPY), 11, 0, RGBA8(255, 200, 120, 255), ALIGN_CENTER);
    } else if (launch_sys == SYS_WEBMAN) {
        gfx_text_fit(SCR_W / 2, y + 150, w - 60, T(S_WM_HINT), 11, 0, C_DIM, ALIGN_CENTER);
    } else if (launch_sys != SYS_SELF) {
        char m[160]; snprintf(m, sizeof(m), T(S_PLAY_WITH), launch_core_name);
        gfx_text_fit(SCR_W / 2, y + 150, w - 60, m, 11, 0, C_DIM, ALIGN_CENTER);
    }
    draw_button_row(SCR_W / 2, y + h - 56, T(S_LAUNCH), T(S_CANCEL), confirm_sel, C_OKC);
}

static void do_launch(void)
{
    char native[1400];
    if (launch_sys == SYS_MOVIAN) {
        ui = UI_BROWSE;
        if (vfs_backend(launch_src) == VFS_NATIVE) {
            snprintf(pending_self, sizeof(pending_self), "%s", movian_self);
            snprintf(pending_arg, sizeof(pending_arg), "file://%s", launch_src);
            quit_req = 1;
            return;
        }
        /* copy to the HDD with progress, then play */
        static char one[1][1024];
        snprintf(one[0], sizeof(one[0]), "%s", launch_src);
        vfs_mkdir("/dev_hdd0/tmp");
        vfs_mkdir("/dev_hdd0/tmp/fm_video");
        play_after_copy = 1;
        if (ops_start(OP_COPY, one, 1, "/dev_hdd0/tmp/fm_video")) open_modal(UI_PROGRESS_OPS); else play_after_copy = 0;
        return;
    }
    if (launch_sys == SYS_WEBMAN) {
        ui = UI_BROWSE;
        int r = webman_resolve(launch_src, native, sizeof(native));
        if (r == WM_OK) r = webman_mount(native);
        if (r == WM_OK) { quit_req = 1; return; }      /* back to XMB: the game is on the disc icon */
        show_toast(r == WM_NOT_RUNNING ? T(S_WM_NOT_RUNNING) : r == WM_NO_CACHE ? T(S_WM_NO_CACHE) : T(S_WM_ERR));
        return;
    }
    if (!launch_prepare(launch_src, native, sizeof(native))) { show_toast(T(S_LAUNCH_FAIL)); ui = UI_BROWSE; return; }
    if (launch_sys == SYS_SELF) { snprintf(pending_self, sizeof(pending_self), "%s", native); pending_arg[0] = 0; }
    else { snprintf(pending_self, sizeof(pending_self), "%s", launch_core); snprintf(pending_arg, sizeof(pending_arg), "%s", native); }
    quit_req = 1;
}

/* open a file with the right viewer */
static void open_entry(int idx)
{
    entry_t *e = &ents[idx];
    char p[1400];
    path_join(p, sizeof(p), cwd, e->name);
    if (e->type == FT_THEME) { theme_open(idx); return; }
    if (is_image(e)) { image_load(idx); ui = UI_IMAGE; return; }
    if (audio_is_supported(e->name)) { audio_open(idx); ui = UI_AUDIO; modal_anim = 0.6f; return; }
    if (e->type == FT_VIDEO) { show_toast(T(S_VIDEO_NO)); return; }
    if (e->type == FT_TEXT) { if (text_open(p, e->size)) ui = UI_TEXT; return; }
    open_props();
}

/* =========================================================== actions */
static void do_rescan(void)
{
    char cur_root[64] = "";
    if (vol_count) snprintf(cur_root, sizeof(cur_root), "%s", vols[vol_sel].root);
    vol_count = vfs_scan(vols, MAX_VOL);
    native_usb_mask = usb_mask();
    usb_dev_mask = vfs_usb_device_mask();
    ftp_set_volumes(vols, vol_count);
    hub_set_volumes(vols, vol_count);
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
        snprintf(j->title, sizeof(j->title), "%s", e->parsed == 1 && e->info.title[0] ? e->info.title : e->name);
        j->size = e->size;
        if (e->parsed == 1 && e->info.icon && e->info.icon_size) {
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

static void refresh_keep_wrapper(void) { refresh_keep(); }

static void run_menu_action(int m)
{
    ui = UI_BROWSE;
    switch (m) {
    case M_OPEN: { char p[1400]; path_join(p, sizeof(p), cwd, ents[sel].name); open_dir(p, NULL); break; }
    case M_MOVIAN: ask_launch(sel); break;
    case M_INSTALL: confirm_sel = 0; open_modal(UI_CONFIRM_INSTALL); break;
    case M_WEBMAN: ask_launch(sel); break;
    case M_THEME: {
        static char srcs[OPS_MAX_SRC][1024];
        int n = 0, mk = marked_count();
        for (int i = 0; i < ent_count && n < OPS_MAX_SRC; i++)
            if (ents[i].type == FT_THEME && (mk ? ents[i].marked : i == sel)) path_join(srcs[n++], 1024, cwd, ents[i].name);
        for (int i = 0; i < ent_count; i++) ents[i].marked = 0;
        if (n) theme_install_paths(srcs, n);
        break;
    }
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
    case M_EXTRACT_ARC: {
        char src[1400], dst[1400], base[256];
        path_join(src, sizeof(src), cwd, ents[sel].name);
        snprintf(base, sizeof(base), "%s", ents[sel].name);
        char *dot = strrchr(base, '.'); if (dot) *dot = 0;
        path_join(dst, sizeof(dst), cwd, base);
        snprintf(ops_output, sizeof(ops_output), "%s", dst);
        if (ops_start_unarchive(src, dst)) open_modal(UI_PROGRESS_OPS);
        break;
    }
    case M_CRC32: {
        char p[1400];
        path_join(p, sizeof(p), cwd, ents[sel].name);
        if (integrity_start(p)) show_toast(T(S_CRC32_RUNNING));
        else show_toast(T(S_CRC32_BUSY));
        break;
    }
    case M_OPEN_TEXT: case M_OPEN_HEX: {
        char p[1400];
        path_join(p, sizeof(p), cwd, ents[sel].name);
        if (m == M_OPEN_TEXT) { if (text_open(p, ents[sel].size)) ui = UI_TEXT; }
        else { hex_open(p, ents[sel].size); ui = UI_HEX; }
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
        ops_go(clip_op, clip, clip_n, cwd);
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
    case M_PROPS: open_props(); break;
    case M_REFRESH: ui = UI_SCANNING; break;
    case M_TOOLS: tools_open(); break;
    case M_UNISO: {
        char src[1400], dst[1400], base[256];
        path_join(src, sizeof(src), cwd, ents[sel].name);
        snprintf(base, sizeof(base), "%s", ents[sel].name);
        size_t bl = strlen(base);
        if (bl > 6 && !strcasecmp(base + bl - 6, ".iso.0")) base[bl - 6] = 0;
        else { char *dot = strrchr(base, '.'); if (dot) *dot = 0; }
        path_join(dst, sizeof(dst), cur_writable() ? cwd : "/dev_hdd0/tmp", base);
        if (!cur_writable()) vfs_mkdir("/dev_hdd0/tmp");
        snprintf(ops_output, sizeof(ops_output), "%s", dst);
        if (ops_start_uniso(src, dst)) open_modal(UI_PROGRESS_OPS);
        break;
    }
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

    if (in_pressed & B_L3) { games_open(); return; }
    if (!focus_list) {
        if (games_btn) {
            if (in_active & B_UP) games_btn = games_btn == 1 ? 2 : 0;
            else if (in_active & B_DOWN) games_btn = 1;
            if (in_pressed & B_OK) { if (games_btn == 1) games_open(); else tools_open(); return; }
            if (in_pressed & R) { games_btn = 0; focus_list = 1; }
            return;
        }
        if (in_active & B_UP) select_volume(vol_sel - 1);
        if (in_active & B_DOWN) { if (vol_sel == vol_count - 1) games_btn = 2; else select_volume(vol_sel + 1); }
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
        else if (archive_kind(e->name) != ARC_NONE && cur_writable()) run_menu_action(M_EXTRACT_ARC);
        else if (e->type == FT_VIDEO) video_start(sel);
        else if (launch_kind(e->name) != SYS_NONE) ask_launch(sel);
        else if (!e->is_dir) { char fp[1400]; path_join(fp, sizeof(fp), cwd, e->name); if (webman_is_game(e->name, 0, fp)) ask_launch(sel); else open_entry(sel); }
        else open_entry(sel);
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
        else if (set_sel == 3) {
            cfg.ftp = !cfg.ftp; save_settings();
            if (cfg.ftp) hub_start(); else hub_stop();
            if (cfg.ftp) {
                if (!ftp_start()) {
                    char m[160]; snprintf(m, sizeof(m), "%s  [%s]", T(S_FTP_FAIL), ftp_error());
                    show_toast(m); cfg.ftp = 0; save_settings();
                }
            }
            else ftp_stop();
        }
        else if (set_sel == 4) { cfg.covers = !cfg.covers; covers_enable(cfg.covers); save_settings(); refresh_keep(); }
        else if (set_sel == 6 && (in_pressed & B_OK)) quit_req = 1;
    }
    if (in_pressed & (B_CANCEL | B_START)) ui = UI_BROWSE;
}

#include "tools_ui.inc"

static void handle_osk(void)
{
    char out[512];
    int r = osk_update(out, sizeof(out));
    if (r == OSK_RESULT_NONE) return;
    if (osk_purpose == 3) {
        ui = UI_TEXT;
        if (r == OSK_RESULT_OK && strcmp(out, txt_lines[txt_sel])) {
            free(txt_lines[txt_sel]);
            txt_lines[txt_sel] = strdup(out);
            txt_dirty = 1;
        }
        return;
    }
    if (osk_purpose == 4) {
        ui = UI_HEX;
        unsigned v;
        if (r == OSK_RESULT_OK && sscanf(out, "%x", &v) == 1 && v < 256) {
            int e = hex_find_edit(hex_cur);
            if (e < 0 && hex_edits < HEX_MAX_EDITS) { e = hex_edits++; hex_edit_off[e] = hex_cur; }
            if (e >= 0) hex_edit_val[e] = (uint8_t)v;
            if (hex_cur + 1 < hex_size) hex_cur++;
        }
        return;
    }
    if (osk_purpose == 5) {
        ui = UI_TOOLS;
        if (r == OSK_RESULT_OK && out[0]) { snprintf(sr_q, sizeof(sr_q), "%s", out); sr_run(); }
        else if (!sr_q[0] || !search_count()) tl_scr = TS_HUB;
        return;
    }
    if (osk_purpose == 9) {
        ui = UI_TOOLS;
        if (r == OSK_RESULT_OK) {
            snprintf(arc_query,sizeof(arc_query),"%s",out);
            arc_page=0;arc_confirm=0;
            arc_scan();
        }
        return;
    }
    if (osk_purpose == 8) {
        ui = UI_VIDEO;
        if (r == OSK_RESULT_OK) subdl_save_key(SUBKEY_FILE, out);
        return;
    }
    if (osk_purpose == 6 || osk_purpose == 7) {
        ui = UI_HEX;
        if (r == OSK_RESULT_OK && out[0]) { if (osk_purpose == 6) hex_find_text(out); else hex_write_text(out); }
        return;
    }
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

    if (cfg.ftp && !ftp_start()) cfg.ftp = 0;
    if (cfg.ftp) hub_start();

    ya2d_paddata[0].ANA_L_H = 0x80;
    ya2d_paddata[0].ANA_L_V = 0x80;
    ents = calloc(MAX_ENTRIES, sizeof(entry_t));
    covers_init(APP_DIR "/covers");
    subdl_load_key(SUBKEY_FILE);
    covers_enable(cfg.covers);
    loader_start();
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
            if (usb_mask() != native_usb_mask || vfs_usb_device_mask() != usb_dev_mask) ui = UI_SCANNING;
            next_usb_check = gfx_frame() + 120;
        }

        /* phone link: install requests and status */
        {
            char ip[1024];
            if (inst_status()->state != INST_RUNNING && ops_status()->state != OPS_RUNNING && (ui == UI_BROWSE || ui == UI_GAMES || ui == UI_TOOLS) && hub_take_install(ip, sizeof(ip))) {
                static inst_job_t hj[1];
                memset(hj, 0, sizeof(hj));
                snprintf(hj[0].path, sizeof(hj[0].path), "%s", ip);
                const char *bn = strrchr(ip, '/'); bn = bn ? bn + 1 : ip;
                snprintf(hj[0].name, sizeof(hj[0].name), "%s", bn);
                snprintf(hj[0].title, sizeof(hj[0].title), "%s", bn);
                int d = 0; uint64_t sz = 0; vfs_stat(ip, &d, &sz); hj[0].size = sz;
                inst_reset();
                if (inst_start(hj, 1, cfg.mode, 0)) { games_mode = 0; open_modal(UI_PROGRESS_INSTALL); }
            }
            if ((gfx_frame() & 31) == 0 && hub_running()) {
                char js[600];
                const inst_status_t *is = inst_status(); const ops_status_t *os = ops_status();
                if (is->state == INST_RUNNING) snprintf(js, sizeof(js), "{\"busy\":true,\"task\":\"install\",\"name\":\"%.100s\",\"done\":%llu,\"total\":%llu}", is->current_name, (unsigned long long)is->all_done, (unsigned long long)is->all_total);
                else if (os->state == OPS_RUNNING) snprintf(js, sizeof(js), "{\"busy\":true,\"task\":\"copy\",\"name\":\"%.100s\",\"done\":%llu,\"total\":%llu,\"queue\":%d}", os->current, (unsigned long long)os->bytes_done, (unsigned long long)os->bytes_total, jq_n);
                else snprintf(js, sizeof(js), "{\"busy\":false}");
                for (char *c = js; *c; c++) if (*c == '\\') *c = '/';
                hub_set_status(js);
            }
        }
        /* background task finished? */
        if (ops_bg && ops_status()->state != OPS_RUNNING) {
            const ops_status_t *os = ops_status();
            if (os->state == OPS_DONE && jq_n) { ops_reset(); jq_start_next(); }
            else if (os->state == OPS_DONE) {
                ops_bg = 0; ops_reset(); show_toast(T(S_ALL_DONE));
                if (ui == UI_BROWSE) refresh_keep();
                if (shutdown_after) shut_countdown = 600;
            } else if (ui == UI_BROWSE) { ops_bg = 0; open_modal(UI_RESULT_OPS); }
        }
        if (shut_countdown) {
            if (in_pressed & B_CANCEL) { shut_countdown = 0; shutdown_after = 0; in_pressed = 0; }
            else if (--shut_countdown == 0) { power_off = 1; quit_req = 1; }
            else in_pressed = in_active = 0;
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
                    ui = UI_BROWSE;
                    ops_go(OP_DELETE, targets, n, NULL);
                }
            }
            break;
        }
        case UI_PROGRESS_INSTALL:
            if (in_pressed & B_CANCEL) inst_cancel();
            if (in_pressed & B_SQU) shutdown_after = !shutdown_after;
            if (inst_status()->state == INST_DONE && shutdown_after) { inst_reset(); ui = UI_BROWSE; refresh_keep(); shut_countdown = 600; break; }
            if (inst_status()->state != INST_RUNNING) { result_sel = 0; open_modal(UI_RESULT_INSTALL); }
            break;
        case UI_PROGRESS_OPS:
            if (in_pressed & B_CANCEL) { ops_cancel(); for (int i = 0; i < jq_n; i++) free(jq[i].src); jq_n = 0; }
            if (in_pressed & B_SQU) shutdown_after = !shutdown_after;
            if ((in_pressed & B_TRI) && !ops_ret_tools && !play_after_copy && !thm_installing) { ops_bg = 1; ui = UI_BROWSE; break; }
            if (ops_status()->state == OPS_DONE && jq_n && !play_after_copy) { ops_reset(); jq_start_next(); break; }
            if (ops_status()->state == OPS_DONE && shutdown_after && !play_after_copy) { ops_reset(); ui = UI_BROWSE; refresh_keep(); shut_countdown = 600; break; }
            if (ops_status()->state != OPS_RUNNING) {
                if (play_after_copy && ops_status()->state == OPS_DONE) {
                    const char *bn = strrchr(launch_src, '/');
                    snprintf(pending_self, sizeof(pending_self), "%s", movian_self);
                    snprintf(pending_arg, sizeof(pending_arg), "file:///dev_hdd0/tmp/fm_video/%s", bn ? bn + 1 : launch_src);
                    ops_reset();
                    play_after_copy = 0;
                    quit_req = 1;
                } else open_modal(UI_RESULT_OPS);
            }
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
                thm_installing = 0;
                if (play_after_copy && s->state == OPS_DONE) {
                    const char *bn = strrchr(launch_src, '/');
                    snprintf(pending_self, sizeof(pending_self), "%s", movian_self);
                    snprintf(pending_arg, sizeof(pending_arg), "file:///dev_hdd0/tmp/fm_video/%s", bn ? bn + 1 : launch_src);
                    quit_req = 1;
                }
                play_after_copy = 0;
                if (sv_work_pending) sv_work_completed = (s->state == OPS_DONE);
                ops_reset();
                if (jq_n && jq_start_next()) { open_modal(UI_PROGRESS_OPS); break; }
                if (ops_ret_tools) { ops_ret_tools = 0; ui = UI_TOOLS; tools_after_ops(); break; }
                for (int i = 0; i < ent_count; i++) ents[i].marked = 0;
                refresh_keep();
                ui = UI_BROWSE;
            }
            break;
        case UI_PROPS: if (in_pressed & (B_OK | B_CANCEL)) ui = UI_BROWSE; break;
        case UI_IMAGE: handle_image(); break;
        case UI_THEME: handle_theme(); break;
        case UI_CONFIRM_LAUNCH: {
            uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
            if (in_pressed & (L | R)) confirm_sel = !confirm_sel;
            if (in_pressed & B_CANCEL) ui = games_mode ? UI_GAMES : UI_BROWSE;
            else if (in_pressed & B_OK) { if (confirm_sel == 0) { ui = UI_LAUNCHING; scan_frames = 0; } else ui = games_mode ? UI_GAMES : UI_BROWSE; }
            break;
        }
        case UI_LAUNCHING:
            if (++scan_frames >= 3) { scan_frames = 0; do_launch(); }
            break;
        case UI_AUDIO: handle_audio(); break;
        case UI_VIDEO: handle_video(); break;
        case UI_GAMES: handle_games(); break;
        case UI_TOOLS: handle_tools(); break;
        case UI_TEXT: handle_text(); break;
        case UI_HEX: handle_hex(); break;
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

        {
            uint32_t crc = 0;
            int ck = integrity_poll(&crc);
            if (ck) {
                char msg[128];
                if (ck > 0) snprintf(msg, sizeof(msg), "CRC32: %08X", (unsigned)crc);
                else snprintf(msg, sizeof(msg), "%s", T(S_CRC32_FAIL));
                show_toast(msg);
            }
        }
        if (ui == UI_BROWSE) lazy_parse();

        gfx_begin_frame();
        if (ui == UI_VIDEO) {
            draw_video();
            if (vid_panel) {
                draw_vid_panel();
                if (vid_panel == 1) { hint_t h[3] = { { ok_icon, S_CHANGE }, { cancel_icon, S_CLOSE } }; draw_hints(h, 2); }
                else { hint_t h[3] = { { ok_icon, S_SELECT_L }, { cancel_icon, S_UP_DIR }, { BTN_START, S_CLOSE } }; draw_hints(h, 3); }
            } else if (gfx_frame() - vid_last_input < 180 || video_state() != VID_PLAYING) {
                hint_t h[6] = { { ok_icon, S_PAUSE }, { BTN_SQUARE, S_SUBTITLES }, { BTN_TRIANGLE, S_AUDIO_TRK }, { BTN_L1, S_SEEK60 }, { BTN_START, S_SETTINGS }, { cancel_icon, S_STOP } };
                draw_hints(h, 6);
            }
            draw_toast();
            gfx_end_frame();
            continue;
        }
        if (games_mode && (ui == UI_GAMES || ui == UI_CONFIRM_LAUNCH || ui == UI_LAUNCHING)) {
            draw_background(t);
            draw_games();
            if (ui == UI_GAMES) {
                hint_t h[6] = { { ok_icon, S_PLAY }, { BTN_L1, S_CATEGORY }, { BTN_TRIANGLE, S_OPEN_FOLDER }, { BTN_SELECT, S_RESCAN }, { cancel_icon, S_BACK } };
                draw_hints(h, 5);
            } else if (ui == UI_CONFIRM_LAUNCH) draw_confirm_launch();
            else { float x2, y2; draw_modal_frame(360, 110, &x2, &y2); gfx_text(SCR_W / 2, y2 + 44, T(S_PREPARING), 15, 1, C_TEXT, ALIGN_CENTER); }
            draw_toast();
            gfx_end_frame();
            continue;
        }
        if (ui == UI_TOOLS || (ui == UI_OSK && (osk_purpose == 5 || osk_purpose == 9)) || (ops_ret_tools && (ui == UI_PROGRESS_OPS || ui == UI_RESULT_OPS))) {
            draw_background(t);
            /* Never read USB save metadata or decode ICON0 while a background
             * restore worker is accessing the same FAT/exFAT filesystem.
             * FatFs file system calls may not be re-entrant on PS3. */
            if (ui == UI_TOOLS || ui == UI_OSK) draw_tools();
            if (ui == UI_TOOLS) draw_tools_hints();
            else if (ui == UI_PROGRESS_OPS) draw_progress_ops();
            else if (ui == UI_RESULT_OPS) draw_result_ops();
            draw_toast();
            gfx_end_frame();
            continue;
        }
        draw_background(t);
        draw_header();
        draw_sidebar();
        draw_list();

        if (ui == UI_BROWSE || ui == UI_MENU) {
            hint_t h[7]; int n = 0;
            if (focus_list && ent_count) {
                entry_t *e = &ents[sel];
                int lbl = e->is_dir ? S_OPEN : e->type == FT_PKG ? S_INSTALL : e->type == FT_THEME ? S_VIEW : e->type == FT_VIDEO ? S_PLAY : (e->type == FT_IMAGE || e->type == FT_TEXT) ? S_VIEW :
                          audio_is_supported(e->name) ? S_PLAY : archive_kind(e->name) != ARC_NONE ? S_EXTRACT_ARC :
                          launch_kind(e->name) == SYS_SELF ? S_LAUNCH : launch_kind(e->name) != SYS_NONE ? S_PLAY :
                          !strcasecmp(ext_of(e->name), "iso") ? S_PLAY : S_PROPS;
                h[n++] = (hint_t){ ok_icon, lbl };
            }
            h[n++] = (hint_t){ cancel_icon, S_BACK };
            if (focus_list && ent_count) h[n++] = (hint_t){ BTN_SQUARE, S_MARK };
            h[n++] = (hint_t){ BTN_TRIANGLE, S_MENU };
            h[n++] = (hint_t){ BTN_L1, S_SWITCH };
            h[n++] = (hint_t){ BTN_START, S_SETTINGS };
            draw_hints(h, n);
        }
        if (ui == UI_TEXT || (ui == UI_OSK && osk_purpose == 3)) {
            draw_text_editor();
            hint_t h[6] = { { ok_icon, S_EDIT_LINE }, { BTN_SQUARE, S_NEW_LINE }, { BTN_SELECT, S_DEL_LINE }, { BTN_START, S_SAVE }, { cancel_icon, S_BACK } };
            draw_hints(h, 5);
        } else if (ui == UI_HEX || (ui == UI_OSK && osk_purpose >= 4 && osk_purpose <= 7 && osk_purpose != 5)) {
            draw_hex();
            hint_t h[7] = { { ok_icon, S_EDIT_BYTE }, { BTN_SQUARE, S_TBL_FIND }, { BTN_TRIANGLE, S_TBL_WRITE }, { BTN_SELECT, S_TBL_LOAD }, { BTN_L1, S_JUMP }, { BTN_START, S_SAVE }, { cancel_icon, S_BACK } };
            draw_hints(h, 7);
        } else if (ui == UI_THEME) {
            draw_theme();
            hint_t h[2]; int n = 0;
            if (!theme_installed(thm_name)) h[n++] = (hint_t){ ok_icon, S_THEME_INSTALL };
            h[n++] = (hint_t){ cancel_icon, S_BACK };
            draw_hints(h, n);
        } else if (ui == UI_IMAGE) {
            draw_image();
            hint_t h[4] = { { BTN_L1, S_PREV }, { BTN_R1, S_NEXT }, { BTN_SQUARE, S_FIT }, { cancel_icon, S_BACK } };
            draw_hints(h, 4);
        } else if (ui == UI_AUDIO) {
            draw_audio();
            hint_t h[4] = { { ok_icon, S_PAUSE }, { BTN_L1, S_PREV }, { BTN_R1, S_NEXT }, { cancel_icon, S_STOP } };
            draw_hints(h, 4);
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
            case UI_CONFIRM_LAUNCH: draw_confirm_launch(); break;
            case UI_LAUNCHING: { float x2, y2; draw_modal_frame(360, 110, &x2, &y2); gfx_text(SCR_W / 2, y2 + 44, T(S_PREPARING), 15, 1, C_TEXT, ALIGN_CENTER); break; }
            case UI_OSK: if (osk_purpose == 2) draw_sfo(); break;
            case UI_SETTINGS: draw_settings(); break;
            case UI_SCANNING: draw_scanning(); break;
        }
        if (ops_bg && ops_status()->state == OPS_RUNNING) {
            const ops_status_t *os = ops_status();
            int pc = os->op == OP_DELETE ? (os->files_total ? (int)(os->files_done * 100 / os->files_total) : 0) : (os->bytes_total ? (int)(os->bytes_done * 100 / os->bytes_total) : 0);
            char m[160], q[48] = "";
            if (jq_n) { snprintf(q, sizeof(q), "   \xC2\xB7   "); size_t l = strlen(q); snprintf(q + l, sizeof(q) - l, T(S_QUEUE_N), jq_n); }
            snprintf(m, sizeof(m), "%s %d%%%s%s", os->op == OP_DELETE ? T(S_DELETING) : os->op == OP_MOVE ? T(S_MOVING) : T(S_COPYING), pc, q, shutdown_after ? "   \xE2\x8F\xBB" : "");
            float mw = gfx_text_width(m, 11, 1) + 30, mx = SCR_W / 2 - mw / 2, my = 50;
            gfx_rrect(mx, my, mw, 22, 11, RGBA8(30, 34, 80, 235));
            gfx_rrect(mx, my, mw * pc / 100.0f, 22, 11, WITH_ALPHA(C_ACC1, 160));
            gfx_text(SCR_W / 2, my + 5, m, 11, 1, C_TEXT, ALIGN_CENTER);
        }
        if (shut_countdown) {
            float x2, y2; draw_modal_frame(420, 140, &x2, &y2);
            char m[96]; snprintf(m, sizeof(m), T(S_SHUTDOWN_IN), (shut_countdown + 59) / 60);
            gfx_text(SCR_W / 2, y2 + 40, m, 17, 1, C_TEXT, ALIGN_CENTER);
            float hx = SCR_W / 2 - 40, bwi = gfx_button_icon(hx, y2 + 90, cancel_icon, 18);
            gfx_text(hx + bwi + 6, y2 + 91, T(S_CANCEL), 12, 0, C_DIM, ALIGN_LEFT);
        }
        draw_toast();
        gfx_end_frame();
    }

    if (inst_status()->state == INST_RUNNING) { inst_cancel(); while (inst_status()->state == INST_RUNNING) usleep(10000); }
    if (ops_status()->state == OPS_RUNNING) { ops_cancel(); while (ops_status()->state == OPS_RUNNING) usleep(10000); }
    catalog_shutdown();
    integrity_shutdown();
    video_close();
    audio_stop();
    /* exiting: background workers (FTP, covers, thumbnails) are told to stop but not waited for —
     * a network request or a connected FTP client could otherwise hold the exit for many seconds */
    ftp_stop_async();
    hub_stop_async();
    covers_stop_async();
    loader_stop();          /* local files only: finishes quickly */
    games_stop();
    free_entries();
    vfs_shutdown();
    gfx_shutdown();
    if (power_off) { lv2syscall4(379, 0x1100, 0, 0, 0); }
    if (pending_self[0]) launch_exec(pending_self, pending_arg[0] ? pending_arg : NULL);
    sysProcessExit(0);
    return 0;
}

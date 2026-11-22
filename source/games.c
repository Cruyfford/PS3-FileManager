/* "My Games": PS3ISO / GAMES / GAMEZ / PS2ISO / PSXISO / PSPISO on every drive, with covers */
#include "games.h"
#include "covers.h"
#include "pkgtool.h"
#include <sys/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define GMAX 3000

static game_t *list;
static volatile int count, scanning, quit, want_from, want_to = -1;
static volume_t vq[24]; static int nvq;
static sys_ppu_thread_t th; static int th_on;
static volatile int rescan_req;

int games_scanning(void) { return scanning; }
int games_count(void) { return count; }
game_t *games_get(int i) { return (i >= 0 && i < count) ? &list[i] : NULL; }

static const char *ext_of(const char *n) { const char *e = strrchr(n, '.'); return e ? e + 1 : ""; }

static void clean_title(const char *name, char *out, int n)
{
    char t[256];
    snprintf(t, sizeof(t), "%s", name);
    /* strip extensions (incl. split .iso.0) */
    for (int k = 0; k < 2; k++) {
        char *d = strrchr(t, '.');
        if (!d) break;
        if (!strcasecmp(d, ".iso") || !strcasecmp(d, ".bin") || !strcasecmp(d, ".cue") || !strcasecmp(d, ".img") || !strcasecmp(d, ".cso") ||
            !strcasecmp(d, ".mdf") || !strcmp(d, ".0")) *d = 0;
        else break;
    }
    /* drop trailing [BLUS12345] / (USA) tags */
    for (;;) {
        int l = strlen(t);
        while (l && t[l - 1] == ' ') t[--l] = 0;
        if (l && (t[l - 1] == ']' || t[l - 1] == ')')) {
            char open = t[l - 1] == ']' ? '[' : '(';
            char *o = strrchr(t, open);
            if (o && o != t) { *o = 0; continue; }
        }
        break;
    }
    for (char *p = t; *p; p++) if (*p == '_') *p = ' ';
    snprintf(out, n, "%s", t[0] ? t : name);
}

static void add_game(const char *dir, const char *name, int is_dir, int plat, const char *drive)
{
    if (count >= GMAX) return;
    game_t *g = &list[count];
    memset(g, 0, sizeof(*g));
    snprintf(g->name, sizeof(g->name), "%s", name);
    size_t dl = strlen(dir);
    snprintf(g->path, sizeof(g->path), "%s%s%s", dir, dl && dir[dl - 1] == '/' ? "" : "/", name);
    snprintf(g->drive, sizeof(g->drive), "%s", drive);
    g->is_dir = is_dir; g->plat = plat;
    clean_title(name, g->title, sizeof(g->title));
    if (is_dir) {
        char sfo[1100];
        snprintf(sfo, sizeof(sfo), "%s/PS3_GAME/PARAM.SFO", g->path);
        sfo_field_t f[SFO_MAX_FIELDS];
        int nf = sfo_load(sfo, f, SFO_MAX_FIELDS);
        for (int i = 0; i < nf; i++) if (!strcmp(f[i].key, "TITLE") && f[i].value[0]) snprintf(g->title, sizeof(g->title), "%s", f[i].value);
    }
    __sync_synchronize();
    count++;
}

static int cmp_title(const void *a, const void *b)
{
    const game_t *x = (const game_t *)a, *y = (const game_t *)b;
    if (x->plat != y->plat) return x->plat - y->plat;
    return strcasecmp(x->title, y->title);
}

static void scan_folder(const char *root, const char *sub, int plat, const char *drive, vfs_entry_t *e)
{
    char dir[600];
    size_t rl = strlen(root);
    snprintf(dir, sizeof(dir), "%s%s%s", root, rl && root[rl - 1] == '/' ? "" : "/", sub);
    int n = vfs_list(dir, e, 2048);
    for (int i = 0; i < n && !quit; i++) {
        const char *nm = e[i].name, *x = ext_of(nm);
        if (nm[0] == '.') continue;
        if (!strcmp(sub, "GAMES") || !strcmp(sub, "GAMEZ")) {
            if (e[i].is_dir) {
                char p[1100];
                snprintf(p, sizeof(p), "%s/%s/PS3_GAME/PARAM.SFO", dir, nm);
                if (vfs_exists(p)) add_game(dir, nm, 1, GP_PS3, drive);
            }
            continue;
        }
        if (e[i].is_dir) continue;
        int ok = 0;
        int split0 = !strcmp(x, "0") && strlen(nm) > 6 && !strncasecmp(nm + strlen(nm) - 6, ".iso.0", 6);
        if (plat == GP_PS3) ok = !strcasecmp(x, "iso") || split0;
        else if (plat == GP_PS2) ok = !strcasecmp(x, "iso") || !strcasecmp(x, "bin") || !strcasecmp(x, "img") || split0;
        else if (plat == GP_PSP) ok = !strcasecmp(x, "iso") || !strcasecmp(x, "cso");
        else if (plat == GP_PS1) {
            if (!strcasecmp(x, "cue") || !strcasecmp(x, "iso") || !strcasecmp(x, "img") || !strcasecmp(x, "mdf")) ok = 1;
            else if (!strcasecmp(x, "bin")) {
                /* a .bin with a matching .cue is listed once, as the .cue */
                char cue[300]; snprintf(cue, sizeof(cue), "%.*s.cue", (int)(x - nm - 1), nm);
                ok = 1;
                for (int k = 0; k < n; k++) if (!strcasecmp(e[k].name, cue)) { ok = 0; break; }
            }
        }
        if (ok) add_game(dir, nm, 0, plat, drive);
    }
}

static void load_cover(game_t *g)
{
    char cp[1400];
    int r = covers_lookup(g->path, g->name, g->is_dir, cp, sizeof(cp));
    if (r == CV_PENDING) { g->cov_gen = covers_generation(); g->cov = GC_PENDING; return; }
    if (r != CV_CACHED) { g->cov = GC_MISSING; return; }
    texture_t t = NULL;
    vfs_file_t *f = vfs_open_read(cp);
    if (f) {
        uint64_t sz64 = vfs_size(f);
        uint32_t sz = sz64 < 4 * 1024 * 1024 ? (uint32_t)sz64 : 0, got = 0;
        uint8_t *b = sz ? (uint8_t *)malloc(sz) : NULL;
        while (b && got < sz) { int64_t k = vfs_read(f, b + got, sz - got); if (k <= 0) break; got += (uint32_t)k; }
        vfs_close(f);
        if (b && got == sz && sz > 8) {
            texture_t s = (b[0] == 0x89 && b[1] == 'P') ? gfx_load_png_buffer(b, sz) : gfx_load_jpg_buffer(b, sz);
            if (s) t = gfx_make_thumb(s, 150, 210);
        }
        free(b);
    }
    g->tex = t;
    __sync_synchronize();
    g->cov = t ? GC_READY : GC_MISSING;
}

static void worker(void *arg)
{
    (void)arg;
    vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * 2048);
    static const struct { const char *sub; int plat; } dirs[] = {
        { "PS3ISO", GP_PS3 }, { "GAMES", GP_PS3 }, { "GAMEZ", GP_PS3 }, { "PS2ISO", GP_PS2 }, { "PSXISO", GP_PS1 }, { "PSXGAMES", GP_PS1 }, { "PSPISO", GP_PSP }, { "ISO", GP_PSP },
    };
    while (!quit) {
        if (rescan_req && e) {
            rescan_req = 0;
            scanning = 1;
            count = 0;
            for (int v = 0; v < nvq && !quit; v++) {
                if (vq[v].kind != 0 && vq[v].kind != 1) continue;
                for (unsigned d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && !quit; d++) scan_folder(vq[v].root, dirs[d].sub, dirs[d].plat, vq[v].label, e);
            }
            int n = count;
            count = 0;
            __sync_synchronize();
            qsort(list, n, sizeof(game_t), cmp_title);
            count = n;
            scanning = 0;
        }
        /* covers for the visible window, nearest first */
        int did = 0;
        int gen = covers_generation();
        for (int i = want_from; i <= want_to && i < count && !quit && !rescan_req; i++) {
            if (i < 0) continue;
            game_t *g = &list[i];
            if (g->cov == GC_NONE || (g->cov == GC_PENDING && g->cov_gen != gen)) { g->cov = GC_LOADING; load_cover(g); did = 1; break; }
        }
        if (!did) usleep(20000);
    }
    free(e);
    sysThreadExit(0);
}

void games_scan(const volume_t *vols, int n)
{
    if (!list) list = (game_t *)calloc(GMAX, sizeof(game_t));
    if (!list) return;
    /* free covers of the previous scan (main thread) */
    for (int i = 0; i < count; i++) if (list[i].cov == GC_READY && list[i].tex) { gfx_free_texture(list[i].tex); list[i].tex = NULL; list[i].cov = GC_NONE; }
    nvq = n > 24 ? 24 : n;
    memcpy(vq, vols, sizeof(volume_t) * nvq);
    rescan_req = 1;
    scanning = 1;
    if (!th_on) {
        quit = 0;
        if (sysThreadCreate(&th, worker, NULL, 1650, 0x20000, THREAD_JOINABLE, "games") == 0) th_on = 1;
    }
}

void games_window(int from, int to)
{
    want_from = from; want_to = to;
    /* drop textures far from the window (main thread owns freeing) */
    for (int i = 0; i < count; i++) {
        if (i >= from - 24 && i <= to + 24) continue;
        game_t *g = &list[i];
        if (g->cov == GC_READY && g->tex) { texture_t t = g->tex; g->tex = NULL; g->cov = GC_NONE; gfx_free_texture(t); }
    }
}

void games_stop(void)
{
    if (!th_on) return;
    quit = 1;
    u64 rv;
    sysThreadJoin(th, &rv);
    th_on = 0;
}

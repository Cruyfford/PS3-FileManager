/* background search and folder-size analysis */
#include "tools.h"
#include "vfs.h"
#include <sys/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define LIST_MAX 4096

/* ================= search ================= */
typedef struct { char path[1024]; int is_dir; uint64_t size; } hit_t;
static hit_t *hits;
static volatile int nhits, s_running, s_cancel;
static char s_roots[16][64]; static int s_nroots;
static char s_query[256];
static char s_where[1024];
static sys_ppu_thread_t s_th; static int s_th_on;

static int ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 0;
    for (const char *p = hay; *p; p++) if (!strncasecmp(p, needle, n)) return 1;
    return 0;
}

/* collects the sub-folder names of a listing into one compact buffer so the big listing can be freed before recursing */
static char *subdirs(vfs_entry_t *e, int n, int *count)
{
    size_t tot = 1; int k = 0;
    for (int i = 0; i < n; i++) if (e[i].is_dir) { tot += strlen(e[i].name) + 1; k++; }
    char *b = (char *)malloc(tot), *p = b;
    if (!b) { *count = 0; return NULL; }
    for (int i = 0; i < n; i++) if (e[i].is_dir) { size_t l = strlen(e[i].name) + 1; memcpy(p, e[i].name, l); p += l; }
    *count = k;
    return b;
}

static void s_walk(const char *dir, int depth, vfs_entry_t *unused)
{
    (void)unused;
    if (s_cancel || depth > 24 || nhits >= SEARCH_MAX) return;
    snprintf(s_where, sizeof(s_where), "%s", dir);
    vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * LIST_MAX);
    if (!e) return;
    int n = vfs_list(dir, e, LIST_MAX);
    char child[1024];
    size_t dl = strlen(dir);
    for (int i = 0; i < n && !s_cancel && nhits < SEARCH_MAX; i++) {
        if (ci_contains(e[i].name, s_query)) {
            hit_t *h = &hits[nhits];
            snprintf(h->path, sizeof(h->path), "%s%s%s", dir, dl && dir[dl - 1] == '/' ? "" : "/", e[i].name);
            h->is_dir = e[i].is_dir; h->size = e[i].size;
            __sync_synchronize();
            nhits++;
        }
    }
    int nd = 0;
    char *subs = subdirs(e, n < 0 ? 0 : n, &nd);
    free(e);
    char *p = subs;
    for (int i = 0; i < nd && !s_cancel && nhits < SEARCH_MAX; i++) {
        snprintf(child, sizeof(child), "%s%s%s", dir, dl && dir[dl - 1] == '/' ? "" : "/", p);
        s_walk(child, depth + 1, NULL);
        p += strlen(p) + 1;
    }
    free(subs);
}

static void s_thread(void *a)
{
    (void)a;
    for (int r = 0; r < s_nroots && !s_cancel; r++) s_walk(s_roots[r], 0, NULL);
    s_running = 0;
    sysThreadExit(0);
}

void search_cancel(void)
{
    if (!s_th_on) return;
    s_cancel = 1;
    u64 rv; sysThreadJoin(s_th, &rv);
    s_th_on = 0; s_running = 0;
}

void search_start(char roots[][64], int nroots, const char *query)
{
    search_cancel();
    if (!hits) hits = (hit_t *)malloc(sizeof(hit_t) * SEARCH_MAX);
    if (!hits) return;
    nhits = 0; s_cancel = 0;
    s_nroots = nroots > 16 ? 16 : nroots;
    for (int i = 0; i < s_nroots; i++) snprintf(s_roots[i], 64, "%s", roots[i]);
    snprintf(s_query, sizeof(s_query), "%s", query);
    s_running = 1;
    if (sysThreadCreate(&s_th, s_thread, NULL, 1700, 0x40000, THREAD_JOINABLE, "search") == 0) s_th_on = 1; else s_running = 0;
}

int search_running(void) { return s_running; }
int search_count(void) { return nhits; }
const char *search_path(int i) { return (i >= 0 && i < nhits) ? hits[i].path : ""; }
int search_is_dir(int i) { return (i >= 0 && i < nhits) ? hits[i].is_dir : 0; }
uint64_t search_size(int i) { return (i >= 0 && i < nhits) ? hits[i].size : 0; }
const char *search_where(void) { return s_where; }

/* ================= storage analyzer ================= */
static space_item_t *items;
static volatile int nitems, sp_running, sp_cancel;
static char sp_dir[1024];
static sys_ppu_thread_t sp_th; static int sp_th_on;
static uint64_t sp_total;

static void measure(const char *path, uint64_t *bytes, uint32_t *files, int depth)
{
    if (sp_cancel || depth > 32) return;
    vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * LIST_MAX);
    if (!e) return;
    int n = vfs_list(path, e, LIST_MAX);
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) if (!e[i].is_dir) { *bytes += e[i].size; (*files)++; }
    int nd = 0;
    char *subs = subdirs(e, n, &nd);
    free(e);
    char child[1024];
    size_t pl = strlen(path);
    char *p = subs;
    for (int i = 0; i < nd && !sp_cancel; i++) {
        snprintf(child, sizeof(child), "%s%s%s", path, pl && path[pl - 1] == '/' ? "" : "/", p);
        measure(child, bytes, files, depth + 1);
        p += strlen(p) + 1;
    }
    free(subs);
}

static int cmp_size(const void *a, const void *b)
{
    const space_item_t *x = (const space_item_t *)a, *y = (const space_item_t *)b;
    return x->size < y->size ? 1 : x->size > y->size ? -1 : 0;
}

static void sp_thread(void *a)
{
    (void)a;
    vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * LIST_MAX);
    int n = e ? vfs_list(sp_dir, e, LIST_MAX) : 0;
    if (n > LIST_MAX) n = LIST_MAX;
    /* files first (instant), then folders one by one */
    for (int i = 0; i < n; i++) {
        space_item_t *it = &items[nitems];
        memset(it, 0, sizeof(*it));
        snprintf(it->name, sizeof(it->name), "%s", e[i].name);
        it->is_dir = e[i].is_dir;
        if (!it->is_dir) { it->size = e[i].size; it->files = 1; it->done = 1; sp_total += it->size; }
        __sync_synchronize();
        nitems++;
    }
    char child[1024];
    size_t dl = strlen(sp_dir);
    for (int i = 0; i < nitems && !sp_cancel; i++) {
        space_item_t *it = &items[i];
        if (!it->is_dir) continue;
        snprintf(child, sizeof(child), "%s%s%s", sp_dir, dl && sp_dir[dl - 1] == '/' ? "" : "/", it->name);
        uint64_t b = 0; uint32_t f = 0;
        measure(child, &b, &f, 0);
        it->size = b; it->files = f; it->done = 1;
        sp_total += b;
    }
    free(e);
    if (!sp_cancel) {
        int k = nitems;
        nitems = 0;
        __sync_synchronize();
        qsort(items, k, sizeof(space_item_t), cmp_size);
        nitems = k;
    }
    sp_running = 0;
    sysThreadExit(0);
}

void space_cancel(void)
{
    if (!sp_th_on) return;
    sp_cancel = 1;
    u64 rv; sysThreadJoin(sp_th, &rv);
    sp_th_on = 0; sp_running = 0;
}

void space_start(const char *dir)
{
    space_cancel();
    if (!items) items = (space_item_t *)malloc(sizeof(space_item_t) * LIST_MAX);
    if (!items) return;
    nitems = 0; sp_total = 0; sp_cancel = 0;
    snprintf(sp_dir, sizeof(sp_dir), "%s", dir);
    sp_running = 1;
    if (sysThreadCreate(&sp_th, sp_thread, NULL, 1700, 0x40000, THREAD_JOINABLE, "space") == 0) sp_th_on = 1; else sp_running = 0;
}

int space_running(void) { return sp_running; }
int space_count(void) { return nitems; }
space_item_t *space_item(int i) { return (i >= 0 && i < nitems) ? &items[i] : NULL; }
uint64_t space_total(void) { return sp_total; }

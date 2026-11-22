#pragma once
#include "gfx.h"
#include "vfs.h"

/* "My Games": scans the webMAN game folders on every drive and loads covers in the background */
enum { GP_PS3 = 0, GP_PS2, GP_PS1, GP_PSP, GP_COUNT };
enum { GC_NONE = 0, GC_LOADING, GC_READY, GC_MISSING, GC_PENDING };

typedef struct {
    char title[128];
    char name[256];
    char path[1024];
    char drive[32];
    int is_dir;
    int plat;
    volatile int cov;          /* GC_* */
    volatile texture_t tex;
    int cov_gen;
} game_t;

void games_scan(const volume_t *vols, int n);     /* (re)starts the background scan */
int  games_scanning(void);
int  games_count(void);
game_t *games_get(int i);
/* indices whose covers should be loaded (visible rows); textures far outside are freed */
void games_window(int from, int to);
void games_stop(void);

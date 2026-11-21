/* Launch SELF files, and ROMs through installed RetroArch cores (core receives the ROM path as argv[1]). */
#include "launcher.h"
#include "vfs.h"
#include <lv2/process.h>
#include <sys/process.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

static const char *RA_DIRS[] = { "/dev_hdd0/game/SSNE10000/USRDIR/cores", "/dev_hdd0/game/SSNE10001/USRDIR/cores", "/dev_hdd0/game/RETROARCH/USRDIR/cores" };

static const char *ext(const char *n) { const char *d = strrchr(n, '.'); return d ? d + 1 : ""; }

int launch_kind(const char *name)
{
    const char *e = ext(name);
    if (!strcasecmp(e, "self") || !strcasecmp(name, "EBOOT.BIN") || !strcasecmp(e, "elf")) return SYS_SELF;
    if (!strcasecmp(e, "nes") || !strcasecmp(e, "fds") || !strcasecmp(e, "unf") || !strcasecmp(e, "unif")) return SYS_NES;
    if (!strcasecmp(e, "smc") || !strcasecmp(e, "sfc") || !strcasecmp(e, "fig") || !strcasecmp(e, "swc")) return SYS_SNES;
    if (!strcasecmp(e, "cue") || !strcasecmp(e, "pbp") || !strcasecmp(e, "chd")) return SYS_PS1;
    if (!strcasecmp(e, "gb") || !strcasecmp(e, "gbc")) return SYS_GB;
    if (!strcasecmp(e, "gba")) return SYS_GBA;
    if (!strcasecmp(e, "md") || !strcasecmp(e, "gen") || !strcasecmp(e, "smd")) return SYS_MD;
    return SYS_NONE;
}

const char *launch_system_name(int s)
{
    switch (s) {
        case SYS_NES: return "NES"; case SYS_SNES: return "SNES"; case SYS_PS1: return "PS1";
        case SYS_GB: return "Game Boy"; case SYS_GBA: return "GBA"; case SYS_MD: return "Mega Drive";
    }
    return "";
}

static const char *const *core_prefixes(int s)
{
    static const char *nes[] = { "fceumm", "nestopia", "quicknes", NULL };
    static const char *snes[] = { "snes9x2010", "snes9x2005_plus", "snes9x2005", "snes9x", "snes9x2002", NULL };
    static const char *ps1[] = { "pcsx_rearmed", "mednafen_psx", "pcsx", NULL };
    static const char *gb[] = { "gambatte", "sameboy", "gearboy", "mgba", NULL };
    static const char *gba[] = { "mgba", "vba_next", "gpsp", NULL };
    static const char *md[] = { "genesis_plus_gx", "picodrive", NULL };
    switch (s) { case SYS_NES: return nes; case SYS_SNES: return snes; case SYS_PS1: return ps1; case SYS_GB: return gb; case SYS_GBA: return gba; case SYS_MD: return md; }
    return NULL;
}

int launch_retroarch_installed(void)
{
    for (unsigned i = 0; i < sizeof(RA_DIRS) / sizeof(RA_DIRS[0]); i++) if (vfs_exists(RA_DIRS[i])) return 1;
    return 0;
}

int launch_find_core(int sys, char *out, int n, char *name, int nn)
{
    const char *const *pre = core_prefixes(sys);
    if (!pre) return 0;
    vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * 512);
    if (!e) return 0;
    int found = 0;
    for (int p = 0; pre[p] && !found; p++) {
        size_t pl = strlen(pre[p]);
        for (unsigned d = 0; d < sizeof(RA_DIRS) / sizeof(RA_DIRS[0]) && !found; d++) {
            int cnt = vfs_list(RA_DIRS[d], e, 512);
            for (int i = 0; i < cnt; i++) {
                /* <core>_libretro_ps3.SELF ; avoid matching snes9x2010 when looking for "snes9x" exactly */
                if (strncasecmp(e[i].name, pre[p], pl) || e[i].name[pl] != '_') continue;
                if (strcasecmp(ext(e[i].name), "SELF")) continue;
                snprintf(out, n, "%s/%s", RA_DIRS[d], e[i].name);
                snprintf(name, nn, "%s", pre[p]);
                found = 1;
                break;
            }
        }
    }
    free(e);
    return found;
}

static int copy_any(const char *src, const char *dst)
{
    vfs_file_t *in = vfs_open_read(src);
    if (!in) return 0;
    vfs_file_t *out = vfs_open_write(dst);
    if (!out) { vfs_close(in); return 0; }
    uint8_t *b = (uint8_t *)malloc(1024 * 1024);
    int ok = b != NULL;
    while (ok) {
        int64_t r = vfs_read(in, b, 1024 * 1024);
        if (r < 0) { ok = 0; break; }
        if (r == 0) break;
        if (vfs_write(out, b, (uint32_t)r) != r) ok = 0;
    }
    free(b);
    vfs_close(in); vfs_close(out);
    return ok;
}

int launch_prepare(const char *path, char *native, int n)
{
    if (vfs_backend(path) == VFS_NATIVE) { snprintf(native, n, "%s", path); return 1; }
    const char *tmp = "/dev_hdd0/tmp/fm_launch";
    vfs_mkdir("/dev_hdd0/tmp");
    vfs_mkdir(tmp);
    const char *bn = strrchr(path, '/');
    bn = bn ? bn + 1 : path;
    snprintf(native, n, "%s/%s", tmp, bn);
    if (!copy_any(path, native)) return 0;

    /* .cue: also copy the track files that share its base name */
    if (!strcasecmp(ext(bn), "cue")) {
        char dir[1024], base[256];
        snprintf(dir, sizeof(dir), "%s", path);
        char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
        snprintf(base, sizeof(base), "%s", bn);
        char *dot = strrchr(base, '.'); if (dot) *dot = 0;
        vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * 512);
        int cnt = e ? vfs_list(dir, e, 512) : 0;
        for (int i = 0; i < cnt; i++) {
            if (e[i].is_dir || strncasecmp(e[i].name, base, strlen(base)) || !strcasecmp(e[i].name, bn)) continue;
            char s1[1400], s2[1400];
            snprintf(s1, sizeof(s1), "%s/%s", dir, e[i].name);
            snprintf(s2, sizeof(s2), "%s/%s", tmp, e[i].name);
            copy_any(s1, s2);
        }
        free(e);
    }
    return 1;
}

void launch_exec(const char *self_path, const char *arg)
{
    const char *argv[2] = { arg, NULL };
    sysProcessExitSpawn2(self_path, arg ? argv : NULL, NULL, NULL, 0, 1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);
}

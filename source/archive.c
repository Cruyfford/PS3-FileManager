/* ZIP (libzip), RAR (libunrar) and 7z (libun7zip) extraction. Paths here are native lv2 paths. */
#include "archive.h"
#include "fileops.h"
#include <zip.h>
#include <unrar.h>
#include <un7zip.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static pt_cb_fn g_cb;

int archive_kind(const char *name)
{
    const char *e = strrchr(name, '.');
    if (!e) return ARC_NONE;
    if (!strcasecmp(e, ".zip")) return ARC_ZIP;
    if (!strcasecmp(e, ".rar")) return ARC_RAR;
    if (!strcasecmp(e, ".7z")) return ARC_7Z;
    return ARC_NONE;
}

static void mkdirs(const char *path)
{
    char t[1100];
    snprintf(t, sizeof(t), "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0777); *p = '/'; }
    mkdir(t, 0777);
}

static int zip_extract(const char *arc, const char *dest, volatile int *cancel, volatile uint64_t *total)
{
    int err = 0;
    struct zip *z = zip_open(arc, 0, &err);
    if (!z) return OPE_OPEN_SRC;
    int n = zip_get_num_files(z);
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) { struct zip_stat st; if (zip_stat_index(z, i, 0, &st) == 0) sum += (uint64_t)st.size; }
    *total = sum;
    uint8_t *buf = (uint8_t *)malloc(1024 * 1024);
    int rc = OPE_NONE;
    for (int i = 0; i < n && !*cancel && buf && !rc; i++) {
        struct zip_stat st;
        if (zip_stat_index(z, i, 0, &st) != 0 || !st.name) continue;
        if (strstr(st.name, "..")) continue;
        char out[1400];
        snprintf(out, sizeof(out), "%s/%s", dest, st.name);
        size_t l = strlen(st.name);
        if (l && st.name[l - 1] == '/') { mkdirs(out); continue; }
        char parent[1400];
        snprintf(parent, sizeof(parent), "%s", out);
        char *sl = strrchr(parent, '/'); if (sl) { *sl = 0; mkdirs(parent); }
        const char *bn = strrchr(st.name, '/');
        if (g_cb) g_cb(0, bn ? bn + 1 : st.name);
        struct zip_file *zf = zip_fopen_index(z, i, 0);
        if (!zf) { rc = OPE_READ; break; }
        FILE *f = fopen(out, "wb");
        if (!f) { zip_fclose(zf); rc = OPE_OPEN_DST; break; }
        for (;;) {
            ssize_t r = zip_fread(zf, buf, 1024 * 1024);
            if (r < 0) { rc = OPE_READ; break; }
            if (r == 0) break;
            if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { rc = OPE_WRITE; break; }
            if (g_cb) g_cb((uint64_t)r, NULL);
            if (*cancel) break;
        }
        fclose(f);
        zip_fclose(zf);
    }
    free(buf);
    zip_close(z);
    return buf ? rc : OPE_MEM;
}

static int rar_extract(const char *arc, const char *dest, volatile int *cancel, volatile uint64_t *total)
{
    /* first pass: total size */
    struct RAROpenArchiveDataEx od;
    struct RARHeaderDataEx hd;
    uint64_t sum = 0;
    memset(&od, 0, sizeof(od));
    od.ArcName = (char *)arc;
    od.OpenMode = RAR_OM_LIST;
    HANDLE h = RAROpenArchiveEx(&od);
    if (!h || od.OpenResult) return OPE_OPEN_SRC;
    memset(&hd, 0, sizeof(hd));
    while (RARReadHeaderEx(h, &hd) == 0) { sum += ((uint64_t)hd.UnpSizeHigh << 32) | hd.UnpSize; RARProcessFile(h, RAR_SKIP, NULL, NULL); }
    RARCloseArchive(h);
    *total = sum;

    memset(&od, 0, sizeof(od));
    od.ArcName = (char *)arc;
    od.OpenMode = RAR_OM_EXTRACT;
    h = RAROpenArchiveEx(&od);
    if (!h || od.OpenResult) return OPE_OPEN_SRC;
    int rc = OPE_NONE;
    memset(&hd, 0, sizeof(hd));
    while (!*cancel && RARReadHeaderEx(h, &hd) == 0) {
        const char *bn = strrchr(hd.FileName, '/');
        if (g_cb) g_cb(0, bn ? bn + 1 : hd.FileName);
        int r = RARProcessFile(h, RAR_EXTRACT, (char *)dest, NULL);
        if (r != 0) { rc = (r == ERAR_ECREATE || r == ERAR_EWRITE) ? OPE_WRITE : OPE_READ; break; }
        if (g_cb) g_cb(((uint64_t)hd.UnpSizeHigh << 32) | hd.UnpSize, NULL);
    }
    RARCloseArchive(h);
    return rc;
}

static void cb7z(const char *name, unsigned long size, unsigned num, unsigned count)
{
    (void)num; (void)count;
    if (g_cb) { g_cb(0, name); g_cb(size, NULL); }
}

static int sevenz_extract(const char *arc, const char *dest, volatile uint64_t *total)
{
    struct stat sb;
    *total = stat(arc, &sb) == 0 ? (uint64_t)sb.st_size * 2 : 0;   /* unknown until listed; rough estimate */
    return Extract7zFileEx(arc, dest, cb7z, DEFAULT_IN_BUF_SIZE) == 0 ? OPE_NONE : OPE_READ;
}

int archive_extract(const char *arc, const char *dest, volatile int *cancel, pt_cb_fn cb, volatile uint64_t *total)
{
    g_cb = cb;
    mkdirs(dest);
    switch (archive_kind(arc)) {
        case ARC_ZIP: return zip_extract(arc, dest, cancel, total);
        case ARC_RAR: return rar_extract(arc, dest, cancel, total);
        case ARC_7Z:  return sevenz_extract(arc, dest, total);
    }
    return OPE_OPEN_SRC;
}

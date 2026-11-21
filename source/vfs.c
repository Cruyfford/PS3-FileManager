/* VFS: lv2 native + libntfs_ext (NTFS / ext2/3/4) + FatFs (exFAT/FAT on unmounted USB ports). */
#include <ppu-types.h>
#include <lv2/sysfs.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntfs.h"
#include "ff.h"
#include "fflib.h"
#include "vfs.h"

#define USB_MASS_STORAGE_1(n) (0x10300000000000AULL + (n))
#define USB_MASS_STORAGE_2(n) (0x10300000000001FULL + ((n) - 6))
#define USB_MASS_STORAGE(n)   (((n) < 6) ? USB_MASS_STORAGE_1(n) : USB_MASS_STORAGE_2(n))

#define FF_SLOTS 8

struct vfs_file {
    int be;
    s32 fd;      /* native / ntfs */
    FIL fil;     /* exFAT */
};

static ntfs_md *ntfs_mounts;
static int ntfs_count;
static FATFS *ff_fs[FF_SLOTS];
static int ff_port[FF_SLOTS];
static int ff_inited;

int vfs_backend(const char *p)
{
    if (!p || !p[0]) return VFS_NATIVE;
    if (p[0] == '/') return VFS_NATIVE;
    if (p[0] >= '0' && p[0] <= '9' && p[1] == ':') return VFS_FAT;
    return VFS_NTFS;
}

static int native_is_dir(const char *p) { struct stat sb; return stat(p, &sb) == 0 && S_ISDIR(sb.st_mode); }

void vfs_init(void)
{
    for (int i = 0; i < FF_SLOTS; i++) ff_port[i] = -1;
}

static void unmount_all(void)
{
    for (int i = 0; i < ntfs_count; i++) ntfsUnmount(ntfs_mounts[i].name, 1);
    if (ntfs_mounts) free(ntfs_mounts);
    ntfs_mounts = NULL; ntfs_count = 0;
    for (int i = 0; i < FF_SLOTS; i++) {
        if (ff_fs[i]) {
            char drn[8]; snprintf(drn, sizeof(drn), "%d:/", i);
            f_mount(NULL, drn, 0);
            free(ff_fs[i]); ff_fs[i] = NULL;
        }
        if (ff_port[i] >= 0) { fflib_detach(i); ff_port[i] = -1; }
    }
}

void vfs_shutdown(void) { unmount_all(); }

static void add_vol(volume_t *out, int *n, int max, const char *root, const char *label, const char *fs, int kind, int be, int port, int w)
{
    if (*n >= max) return;
    volume_t *v = &out[(*n)++];
    memset(v, 0, sizeof(*v));
    snprintf(v->root, sizeof(v->root), "%s", root);
    snprintf(v->label, sizeof(v->label), "%s", label);
    snprintf(v->fs_name, sizeof(v->fs_name), "%s", fs);
    v->kind = kind; v->fs = be; v->port = port; v->writable = w;
}

int vfs_scan(volume_t *out, int max)
{
    int n = 0;
    char lbl[64], path[32];

    unmount_all();

    /* native USB (FAT32) */
    int native_usb[8] = { 0 };
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev_usb%03d", i);
        if (native_is_dir(path)) {
            native_usb[i] = 1;
            snprintf(lbl, sizeof(lbl), "USB %d", i);
            add_vol(out, &n, max, path, lbl, "FAT32", 1, VFS_NATIVE, i, 1);
        }
    }

    /* NTFS / ext2/3/4 partitions on any USB drive */
    ntfs_count = ntfsMountAll(&ntfs_mounts, NTFS_DEFAULT | NTFS_RECOVER);
    if (ntfs_count < 0) ntfs_count = 0;
    for (int i = 0; i < ntfs_count; i++) {
        char root[40];
        snprintf(root, sizeof(root), "%s:/", ntfs_mounts[i].name);
        const char *fs = !strncmp(ntfs_mounts[i].name, "ext", 3) ? "ext" : "NTFS";
        snprintf(lbl, sizeof(lbl), "USB %s", ntfs_mounts[i].name);
        add_vol(out, &n, max, root, lbl, fs, 1, VFS_NTFS, -1, 1);
    }

    /* exFAT (and FAT the system didn't mount) on ports the PS3 can't read natively */
    if (!ff_inited) { fflib_init(); ff_inited = 1; }
    int slot = 0;
    for (int port = 0; port < 8 && slot < FF_SLOTS; port++) {
        if (native_usb[port]) continue;
        if (fflib_attach(slot, USB_MASS_STORAGE(port), 1) != FR_OK) { fflib_detach(slot); continue; }
        char drn[8]; snprintf(drn, sizeof(drn), "%d:/", slot);
        FATFS *fs = (FATFS *)calloc(1, sizeof(FATFS));
        if (!fs) { fflib_detach(slot); continue; }
        if (f_mount(fs, drn, 1) != FR_OK) { free(fs); fflib_detach(slot); continue; }
        ff_fs[slot] = fs; ff_port[slot] = port;
        snprintf(lbl, sizeof(lbl), "USB %d", port);
        add_vol(out, &n, max, drn, lbl, fs->fs_type == FS_EXFAT ? "exFAT" : "FAT32", 1, VFS_FAT, port, 1);
        slot++;
    }

    add_vol(out, &n, max, "/dev_hdd0", "HDD", "PS3", 0, VFS_NATIVE, -1, 1);
    add_vol(out, &n, max, "/dev_hdd0/packages", "packages", "PS3", 4, VFS_NATIVE, -1, 1);
    if (native_is_dir("/dev_hdd1")) add_vol(out, &n, max, "/dev_hdd1", "HDD1 (cache)", "FAT", 0, VFS_NATIVE, -1, 1);
    if (native_is_dir("/dev_bdvd")) add_vol(out, &n, max, "/dev_bdvd", "Blu-ray", "BD", 3, VFS_NATIVE, -1, 0);
    if (native_is_dir("/app_home")) add_vol(out, &n, max, "/app_home", "app_home", "PS3", 3, VFS_NATIVE, -1, 0);
    add_vol(out, &n, max, "/dev_flash", "dev_flash", "FLASH", 2, VFS_NATIVE, -1, 0);
    if (native_is_dir("/dev_flash2")) add_vol(out, &n, max, "/dev_flash2", "dev_flash2", "FLASH", 2, VFS_NATIVE, -1, 1);
    if (native_is_dir("/dev_flash3")) add_vol(out, &n, max, "/dev_flash3", "dev_flash3", "FLASH", 2, VFS_NATIVE, -1, 0);
    if (native_is_dir("/dev_blind")) add_vol(out, &n, max, "/dev_blind", "dev_blind", "FLASH", 2, VFS_NATIVE, -1, 1);
    return n;
}

/* ---------- directory listing ---------- */

int vfs_list(const char *path, vfs_entry_t *out, int max)
{
    int n = 0;
    int be = vfs_backend(path);
    if (be == VFS_NATIVE) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *de;
        char full[1400];
        while ((de = readdir(d)) && n < max) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
            struct stat sb;
            vfs_entry_t *e = &out[n];
            snprintf(e->name, sizeof(e->name), "%s", de->d_name);
            if (stat(full, &sb) == 0) { e->is_dir = S_ISDIR(sb.st_mode); e->size = e->is_dir ? 0 : (uint64_t)sb.st_size; }
            else { e->is_dir = 0; e->size = 0; }
            n++;
        }
        closedir(d);
    } else if (be == VFS_NTFS) {
        DIR_ITER *d = ps3ntfs_diropen(path);
        if (!d) return -1;
        char name[256];
        struct stat st;
        while (n < max && ps3ntfs_dirnext(d, name, &st) == 0) {
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            vfs_entry_t *e = &out[n++];
            snprintf(e->name, sizeof(e->name), "%s", name);
            e->is_dir = S_ISDIR(st.st_mode);
            e->size = e->is_dir ? 0 : (uint64_t)st.st_size;
        }
        ps3ntfs_dirclose(d);
    } else {
        FDIR dir;
        static FILINFO fno;
        if (f_opendir(&dir, path) != FR_OK) return -1;
        while (n < max) {
            if (f_readdir(&dir, &fno) != FR_OK || !fno.fname[0]) break;
            vfs_entry_t *e = &out[n++];
            snprintf(e->name, sizeof(e->name), "%s", fno.fname);
            e->is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
            e->size = e->is_dir ? 0 : (uint64_t)fno.fsize;
        }
        f_closedir(&dir);
    }
    return n;
}

int vfs_stat(const char *path, int *is_dir, uint64_t *size)
{
    int be = vfs_backend(path);
    if (be == VFS_NATIVE) {
        struct stat sb;
        if (stat(path, &sb) != 0) return -1;
        if (is_dir) *is_dir = S_ISDIR(sb.st_mode);
        if (size) *size = (uint64_t)sb.st_size;
        return 0;
    }
    if (be == VFS_NTFS) {
        struct stat sb;
        if (ps3ntfs_stat(path, &sb) != 0) return -1;
        if (is_dir) *is_dir = S_ISDIR(sb.st_mode);
        if (size) *size = (uint64_t)sb.st_size;
        return 0;
    }
    /* FatFs can't stat a drive root */
    if (strlen(path) <= 3) { if (is_dir) *is_dir = 1; if (size) *size = 0; return 0; }
    static FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return -1;
    if (is_dir) *is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
    if (size) *size = (uint64_t)fno.fsize;
    return 0;
}

int vfs_exists(const char *path) { return vfs_stat(path, NULL, NULL) == 0; }

int vfs_mkdir(const char *path)
{
    switch (vfs_backend(path)) {
    case VFS_NATIVE: return mkdir(path, 0777) == 0 || native_is_dir(path) ? 0 : -1;
    case VFS_NTFS:   return ps3ntfs_mkdir(path, 0777) == 0 ? 0 : -1;
    default:        { FRESULT r = f_mkdir(path); return (r == FR_OK || r == FR_EXIST) ? 0 : -1; }
    }
}

int vfs_unlink(const char *path)
{
    switch (vfs_backend(path)) {
    case VFS_NATIVE: return sysFsUnlink(path) == 0 ? 0 : -1;
    case VFS_NTFS:   return ps3ntfs_unlink(path) == 0 ? 0 : -1;
    default:        return f_unlink(path) == FR_OK ? 0 : -1;
    }
}

int vfs_rmdir(const char *path)
{
    switch (vfs_backend(path)) {
    case VFS_NATIVE: return sysFsRmdir(path) == 0 ? 0 : -1;
    case VFS_NTFS:   return ps3ntfs_unlink(path) == 0 ? 0 : -1;
    default:        return f_unlink(path) == FR_OK ? 0 : -1;
    }
}

int vfs_rename(const char *from, const char *to)
{
    int be = vfs_backend(from);
    if (be != vfs_backend(to)) return -1;
    switch (be) {
    case VFS_NATIVE: return rename(from, to) == 0 ? 0 : -1;
    case VFS_NTFS:   return ps3ntfs_rename(from, to) == 0 ? 0 : -1;
    default: {
        /* FatFs: destination must not carry the drive prefix */
        const char *t = (to[1] == ':') ? to + 2 : to;
        return f_rename(from, t) == FR_OK ? 0 : -1;
    }
    }
}

uint64_t vfs_free_space(const char *path)
{
    int be = vfs_backend(path);
    if (be == VFS_NATIVE) {
        char dev[32] = { 0 };
        const char *s = strchr(path + 1, '/');
        int l = s ? (int)(s - path) : (int)strlen(path);
        if (l > 30) l = 30;
        memcpy(dev, path, l); strcat(dev, "/");
        u32 bs = 0; u64 blocks = 0;
        if (sysFsGetFreeSize(dev, &bs, &blocks) != 0) return 0;
        return (uint64_t)bs * blocks;
    }
    if (be == VFS_NTFS) {
        struct statvfs sv;
        char dev[40] = { 0 };
        const char *c = strchr(path, ':');
        int l = c ? (int)(c - path) + 1 : 0;
        memcpy(dev, path, l < 38 ? l : 38); strcat(dev, "/");
        if (ps3ntfs_statvfs(dev, &sv) != 0) return 0;
        return (uint64_t)sv.f_bsize * sv.f_bfree;
    }
    DWORD clst = 0; FATFS *fs = NULL;
    char drn[4] = { path[0], ':', '/', 0 };
    if (f_getfree(drn, &clst, &fs) != FR_OK || !fs) return 0;
    return (uint64_t)clst * fs->csize * (uint64_t)fflib_ss_get(path[0] - '0');
}

/* ---------- file I/O ---------- */

static vfs_file_t *open_any(const char *path, int write)
{
    vfs_file_t *f = (vfs_file_t *)calloc(1, sizeof(vfs_file_t));
    if (!f) return NULL;
    f->be = vfs_backend(path);
    int ok = 0;
    if (f->be == VFS_NATIVE) {
        s32 flags = write ? (SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC) : SYS_O_RDONLY;
        ok = sysFsOpen(path, flags, &f->fd, NULL, 0) == 0;
        if (ok && write) sysFsChmod(path, 0666);
    } else if (f->be == VFS_NTFS) {
        f->fd = ps3ntfs_open(path, write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0666);
        ok = f->fd >= 0;
    } else {
        ok = f_open(&f->fil, path, write ? (FA_WRITE | FA_CREATE_ALWAYS) : FA_READ) == FR_OK;
    }
    if (!ok) { free(f); return NULL; }
    return f;
}

vfs_file_t *vfs_open_read(const char *path)  { return open_any(path, 0); }
vfs_file_t *vfs_open_write(const char *path) { return open_any(path, 1); }

int64_t vfs_read(vfs_file_t *f, void *buf, uint32_t len)
{
    if (f->be == VFS_NATIVE) { u64 rd = 0; if (sysFsRead(f->fd, buf, len, &rd) != 0) return -1; return (int64_t)rd; }
    if (f->be == VFS_NTFS) { int r = ps3ntfs_read(f->fd, (char *)buf, len); return r; }
    UINT br = 0; if (f_read(&f->fil, buf, len, &br) != FR_OK) return -1; return br;
}

int64_t vfs_write(vfs_file_t *f, const void *buf, uint32_t len)
{
    if (f->be == VFS_NATIVE) { u64 wr = 0; if (sysFsWrite(f->fd, buf, len, &wr) != 0) return -1; return (int64_t)wr; }
    if (f->be == VFS_NTFS) { int r = ps3ntfs_write(f->fd, (const char *)buf, len); return r; }
    UINT bw = 0; if (f_write(&f->fil, buf, len, &bw) != FR_OK) return -1; return bw;
}

int vfs_seek(vfs_file_t *f, uint64_t off)
{
    if (f->be == VFS_NATIVE) { u64 pos; return sysFsLseek(f->fd, (s64)off, SEEK_SET, &pos) == 0 ? 0 : -1; }
    if (f->be == VFS_NTFS) return ps3ntfs_seek64(f->fd, (s64)off, SEEK_SET) >= 0 ? 0 : -1;
    return f_lseek(&f->fil, off) == FR_OK ? 0 : -1;
}

void vfs_close(vfs_file_t *f)
{
    if (!f) return;
    if (f->be == VFS_NATIVE) sysFsClose(f->fd);
    else if (f->be == VFS_NTFS) ps3ntfs_close(f->fd);
    else f_close(&f->fil);
    free(f);
}

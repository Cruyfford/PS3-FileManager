#pragma once
#include <stdint.h>

/* Unified file access across PS3 file systems:
 *   native  : /dev_hdd0, /dev_usbNNN (FAT32), /dev_flash, /dev_bdvd ...   (lv2 sysFs)
 *   NTFS/ext: ntfs0:/ ext0:/ ...  (libntfs_ext)
 *   exFAT   : 0:/ .. 7:/          (FatFs, also reads FAT32 on raw ports)  */

enum { VFS_NATIVE = 0, VFS_NTFS = 1, VFS_FAT = 2 };

typedef struct {
    char root[64];       /* path to open the volume */
    char label[64];      /* display label */
    char fs_name[12];    /* "FAT32", "NTFS", "exFAT", "ext4", "PS3" ... */
    int  kind;           /* 0 hdd, 1 usb, 2 flash/system, 3 disc, 4 packages shortcut */
    int  fs;             /* FS_* backend */
    int  port;           /* usb port, -1 otherwise */
    int  writable;
} volume_t;

typedef struct {
    char name[256];
    int is_dir;
    uint64_t size;
} vfs_entry_t;

typedef struct vfs_file vfs_file_t;

void vfs_init(void);
void vfs_shutdown(void);
/* re-scan USB ports; returns number of volumes */
int  vfs_scan(volume_t *out, int max);

int  vfs_backend(const char *path);
int  vfs_list(const char *path, vfs_entry_t *out, int max);          /* returns count or -1 */
int  vfs_stat(const char *path, int *is_dir, uint64_t *size);        /* 0 = ok */
int  vfs_exists(const char *path);
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *from, const char *to);                  /* same backend only */
uint64_t vfs_free_space(const char *path);

vfs_file_t *vfs_open_read(const char *path);
vfs_file_t *vfs_open_write(const char *path);
int64_t vfs_read(vfs_file_t *f, void *buf, uint32_t len);
int64_t vfs_write(vfs_file_t *f, const void *buf, uint32_t len);
int  vfs_seek(vfs_file_t *f, uint64_t off);
void vfs_close(vfs_file_t *f);

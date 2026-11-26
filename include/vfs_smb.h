#pragma once
#include <stdint.h>
#include "vfs.h"
/* SMB2/3 network shares through libsmb2 (optional: built when HAVE_SMB2 is defined).
 * Paths look like  smb://SERVER/SHARE/dir/file  ; credentials come from the share table. */

typedef struct { char label[48]; char server[64]; char share[64]; char user[64]; char pass[64]; } smb_share_t;

#define SMB_MAX_SHARES 8
int  smb_available(void);                       /* 1 when compiled with libsmb2 */
void smb_set_shares(const smb_share_t *s, int n);
int  smb_share_count(void);
const smb_share_t *smb_share(int i);
void smb_shares_load(const char *file);
void smb_shares_save(const char *file);
int  smb_share_add(const smb_share_t *s);
void smb_share_remove(int i);
const char *smb_last_error(void);
void smb_disconnect_all(void);

int  smbfs_list(const char *path, vfs_entry_t *out, int max);
int  smbfs_stat(const char *path, int *is_dir, uint64_t *size);
int  smbfs_mkdir(const char *path);
int  smbfs_unlink(const char *path);
int  smbfs_rmdir(const char *path);
int  smbfs_rename(const char *from, const char *to);
uint64_t smbfs_free(const char *path);
void *smbfs_open(const char *path, int write);          /* 0 read, 1 create/truncate, 2 rw */
int64_t smbfs_read(void *h, void *buf, uint32_t len);
int64_t smbfs_write(void *h, const void *buf, uint32_t len);
int  smbfs_seek(void *h, uint64_t off);
uint64_t smbfs_size(void *h);
void smbfs_close(void *h);

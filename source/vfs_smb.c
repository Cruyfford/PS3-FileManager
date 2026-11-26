/* SMB network shares for the VFS (libsmb2, LGPL-2.1). One connection per share, guarded by a mutex. */
#include "vfs_smb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static smb_share_t shares[SMB_MAX_SHARES];
static int nshares;
static char lasterr[160];

int smb_share_count(void) { return nshares; }
const smb_share_t *smb_share(int i) { return i >= 0 && i < nshares ? &shares[i] : NULL; }
const char *smb_last_error(void) { return lasterr; }
void smb_set_shares(const smb_share_t *s, int n) { if (n > SMB_MAX_SHARES) n = SMB_MAX_SHARES; memcpy(shares, s, sizeof(*s) * n); nshares = n; }

void smb_shares_load(const char *file)
{
    nshares = 0;
    FILE *f = fopen(file, "rb");
    if (!f) return;
    char line[512];
    while (nshares < SMB_MAX_SHARES && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *fld[5] = { 0 }; int k = 0; char *p = line;
        while (k < 5) { fld[k++] = p; char *bar = strchr(p, '|'); if (!bar) break; *bar = 0; p = bar + 1; }
        if (k < 3 || !fld[1][0] || !fld[2][0]) continue;
        smb_share_t *s = &shares[nshares++];
        memset(s, 0, sizeof(*s));
        snprintf(s->label, sizeof(s->label), "%s", fld[0]);
        snprintf(s->server, sizeof(s->server), "%s", fld[1]);
        snprintf(s->share, sizeof(s->share), "%s", fld[2]);
        if (k > 3) snprintf(s->user, sizeof(s->user), "%s", fld[3]);
        if (k > 4) snprintf(s->pass, sizeof(s->pass), "%s", fld[4]);
    }
    fclose(f);
}

void smb_shares_save(const char *file)
{
    FILE *f = fopen(file, "wb");
    if (!f) return;
    for (int i = 0; i < nshares; i++) fprintf(f, "%s|%s|%s|%s|%s\n", shares[i].label, shares[i].server, shares[i].share, shares[i].user, shares[i].pass);
    fclose(f);
}

int smb_share_add(const smb_share_t *s)
{
    if (nshares >= SMB_MAX_SHARES) return 0;
    shares[nshares++] = *s;
    return 1;
}

void smb_share_remove(int i)
{
    if (i < 0 || i >= nshares) return;
    memmove(&shares[i], &shares[i + 1], sizeof(smb_share_t) * (nshares - i - 1));
    nshares--;
}

#ifdef HAVE_SMB2
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <sys/mutex.h>
#include <fcntl.h>
#include "ftp.h"

typedef struct { char server[64], share[64]; struct smb2_context *ctx; } conn_t;
static conn_t conns[SMB_MAX_SHARES];
static sys_mutex_t mtx; static int mtx_ok;
static void LK(void) { if (!mtx_ok) { sys_mutex_attr_t a; memset(&a, 0, sizeof(a)); a.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO; a.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE; a.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED; a.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE; strcpy(a.name, "smb"); mtx_ok = sysMutexCreate(&mtx, &a) == 0; } if (mtx_ok) sysMutexLock(mtx, 0); }
static void UL(void) { if (mtx_ok) sysMutexUnlock(mtx); }

int smb_available(void) { return 1; }

/* "smb://server/share/a/b" -> server, share, "a/b" */
static int split(const char *path, char *server, char *share, char *rest, int rn)
{
    if (strncmp(path, "smb://", 6)) return -1;
    const char *p = path + 6, *s = strchr(p, '/');
    if (!s) return -1;
    int l = s - p; if (l >= 64) return -1;
    memcpy(server, p, l); server[l] = 0;
    p = s + 1; s = strchr(p, '/');
    l = s ? (int)(s - p) : (int)strlen(p); if (!l || l >= 64) return -1;
    memcpy(share, p, l); share[l] = 0;
    const char *r = s ? s + 1 : "";
    snprintf(rest, rn, "%s", r);
    int rl = strlen(rest);
    while (rl && rest[rl - 1] == '/') rest[--rl] = 0;
    return 0;
}

static struct smb2_context *get(const char *path, char *rest, int rn)
{
    char server[64], share[64];
    if (split(path, server, share, rest, rn)) { snprintf(lasterr, sizeof(lasterr), "bad path"); return NULL; }
    for (int i = 0; i < SMB_MAX_SHARES; i++)
        if (conns[i].ctx && !strcmp(conns[i].server, server) && !strcmp(conns[i].share, share)) return conns[i].ctx;
    const smb_share_t *cfg = NULL;
    for (int i = 0; i < nshares; i++) if (!strcmp(shares[i].server, server) && !strcmp(shares[i].share, share)) cfg = &shares[i];
    if (net_up() < 0) { snprintf(lasterr, sizeof(lasterr), "network"); return NULL; }
    struct smb2_context *c = smb2_init_context();
    if (!c) return NULL;
    smb2_set_security_mode(c, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(c, 15);
    const char *user = cfg && cfg->user[0] ? cfg->user : "guest";
    smb2_set_user(c, user);
    if (cfg && cfg->pass[0]) smb2_set_password(c, cfg->pass); else smb2_set_password(c, "");
    if (smb2_connect_share(c, server, share, user) < 0) {
        snprintf(lasterr, sizeof(lasterr), "%s", smb2_get_error(c));
        smb2_destroy_context(c);
        return NULL;
    }
    for (int i = 0; i < SMB_MAX_SHARES; i++) if (!conns[i].ctx) {
        snprintf(conns[i].server, 64, "%s", server); snprintf(conns[i].share, 64, "%s", share); conns[i].ctx = c; return c;
    }
    smb2_disconnect_share(c); smb2_destroy_context(c);
    return NULL;
}

void smb_disconnect_all(void)
{
    LK();
    for (int i = 0; i < SMB_MAX_SHARES; i++) if (conns[i].ctx) { smb2_disconnect_share(conns[i].ctx); smb2_destroy_context(conns[i].ctx); conns[i].ctx = NULL; }
    UL();
}

int smbfs_list(const char *path, vfs_entry_t *out, int max)
{
    char rest[1024]; int n = 0;
    LK();
    struct smb2_context *c = get(path, rest, sizeof(rest));
    struct smb2dir *d = c ? smb2_opendir(c, rest) : NULL;
    if (!d) { if (c) snprintf(lasterr, sizeof(lasterr), "%s", smb2_get_error(c)); UL(); return -1; }
    struct smb2dirent *e;
    while (n < max && (e = smb2_readdir(c, d))) {
        if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->name);
        out[n].is_dir = e->st.smb2_type == SMB2_TYPE_DIRECTORY;
        out[n].size = out[n].is_dir ? 0 : e->st.smb2_size;
        n++;
    }
    smb2_closedir(c, d);
    UL();
    return n;
}

int smbfs_stat(const char *path, int *is_dir, uint64_t *size)
{
    char rest[1024];
    LK();
    struct smb2_context *c = get(path, rest, sizeof(rest));
    struct smb2_stat_64 st;
    int r = -1;
    if (c) {
        if (!rest[0]) { if (is_dir) *is_dir = 1; if (size) *size = 0; r = 0; }
        else if (smb2_stat(c, rest, &st) == 0) { if (is_dir) *is_dir = st.smb2_type == SMB2_TYPE_DIRECTORY; if (size) *size = st.smb2_size; r = 0; }
    }
    UL();
    return r;
}

#define SIMPLE(fn, call) int fn(const char *path) { char rest[1024]; LK(); struct smb2_context *c = get(path, rest, sizeof(rest)); int r = c ? (call(c, rest) == 0 ? 0 : -1) : -1; UL(); return r; }
SIMPLE(smbfs_unlink, smb2_unlink)
SIMPLE(smbfs_rmdir, smb2_rmdir)
int smbfs_mkdir(const char *path)
{
    char rest[1024]; LK();
    struct smb2_context *c = get(path, rest, sizeof(rest));
    int r = -1;
    if (c) { struct smb2_stat_64 st; r = smb2_mkdir(c, rest) == 0 || (smb2_stat(c, rest, &st) == 0 && st.smb2_type == SMB2_TYPE_DIRECTORY) ? 0 : -1; }
    UL();
    return r;
}

int smbfs_rename(const char *from, const char *to)
{
    char a[1024], b[1024], s1[64], s2[64], h1[64], h2[64];
    if (split(from, h1, s1, a, sizeof(a)) || split(to, h2, s2, b, sizeof(b)) || strcmp(h1, h2) || strcmp(s1, s2)) return -1;
    LK();
    struct smb2_context *c = get(from, a, sizeof(a));
    int r = c ? (smb2_rename(c, a, b) == 0 ? 0 : -1) : -1;
    UL();
    return r;
}

uint64_t smbfs_free(const char *path)
{
    char rest[1024]; LK();
    struct smb2_context *c = get(path, rest, sizeof(rest));
    struct smb2_statvfs sv; uint64_t r = 0;
    if (c && smb2_statvfs(c, rest, &sv) == 0) r = (uint64_t)sv.f_bsize * sv.f_bavail;
    UL();
    return r;
}

typedef struct { struct smb2_context *c; struct smb2fh *fh; uint32_t maxr, maxw; } sf_t;

void *smbfs_open(const char *path, int write)
{
    char rest[1024]; LK();
    struct smb2_context *c = get(path, rest, sizeof(rest));
    struct smb2fh *fh = c ? smb2_open(c, rest, write == 1 ? (O_WRONLY | O_CREAT | O_TRUNC) : write == 2 ? O_RDWR : O_RDONLY) : NULL;
    sf_t *f = NULL;
    if (fh && (f = calloc(1, sizeof(*f)))) { f->c = c; f->fh = fh; f->maxr = smb2_get_max_read_size(c); f->maxw = smb2_get_max_write_size(c); if (!f->maxr) f->maxr = 65536; if (!f->maxw) f->maxw = 65536; }
    else if (fh) smb2_close(c, fh);
    if (!fh && c) snprintf(lasterr, sizeof(lasterr), "%s", smb2_get_error(c));
    UL();
    return f;
}

int64_t smbfs_read(void *h, void *buf, uint32_t len)
{
    sf_t *f = h; uint8_t *p = buf; int64_t got = 0;
    LK();
    while (len) {
        uint32_t k = len > f->maxr ? f->maxr : len;
        int r = smb2_read(f->c, f->fh, p, k);
        if (r < 0) { UL(); return got ? got : -1; }
        if (r == 0) break;
        got += r; p += r; len -= r;
        if ((uint32_t)r < k) break;
    }
    UL();
    return got;
}

int64_t smbfs_write(void *h, const void *buf, uint32_t len)
{
    sf_t *f = h; const uint8_t *p = buf; int64_t put = 0;
    LK();
    while (len) {
        uint32_t k = len > f->maxw ? f->maxw : len;
        int r = smb2_write(f->c, f->fh, p, k);
        if (r <= 0) { UL(); return put ? put : -1; }
        put += r; p += r; len -= r;
    }
    UL();
    return put;
}

int smbfs_seek(void *h, uint64_t off) { sf_t *f = h; LK(); uint64_t cur; int r = smb2_lseek(f->c, f->fh, (int64_t)off, SEEK_SET, &cur) < 0 ? -1 : 0; UL(); return r; }
uint64_t smbfs_size(void *h) { sf_t *f = h; struct smb2_stat_64 st; LK(); uint64_t s = smb2_fstat(f->c, f->fh, &st) == 0 ? st.smb2_size : 0; UL(); return s; }
void smbfs_close(void *h) { sf_t *f = h; if (!f) return; LK(); smb2_close(f->c, f->fh); UL(); free(f); }

#else
int smb_available(void) { return 0; }
void smb_disconnect_all(void) {}
int smbfs_list(const char *p, vfs_entry_t *o, int m) { (void)p; (void)o; (void)m; snprintf(lasterr, sizeof(lasterr), "SMB not built"); return -1; }
int smbfs_stat(const char *p, int *d, uint64_t *s) { (void)p; (void)d; (void)s; return -1; }
int smbfs_mkdir(const char *p) { (void)p; return -1; }
int smbfs_unlink(const char *p) { (void)p; return -1; }
int smbfs_rmdir(const char *p) { (void)p; return -1; }
int smbfs_rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
uint64_t smbfs_free(const char *p) { (void)p; return 0; }
void *smbfs_open(const char *p, int w) { (void)p; (void)w; return NULL; }
int64_t smbfs_read(void *h, void *b, uint32_t l) { (void)h; (void)b; (void)l; return -1; }
int64_t smbfs_write(void *h, const void *b, uint32_t l) { (void)h; (void)b; (void)l; return -1; }
int smbfs_seek(void *h, uint64_t o) { (void)h; (void)o; return -1; }
uint64_t smbfs_size(void *h) { (void)h; return 0; }
void smbfs_close(void *h) { (void)h; }
#endif

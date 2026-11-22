/* Background copy / move / delete between any mounted file system (lv2, NTFS/ext, exFAT/FAT). */
#include "fileops.h"
#include "vfs.h"
#include "pkgtool.h"
#include "archive.h"
#include "disc.h"
#include "save_pfd.h"
#include "pfd.h"
#include <sys/thread.h>
#include <sys/systime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BUF_SIZE (3 * 1024 * 1024)
#define MAX_LIST 2048

static ops_status_t st;
static char (*q_src)[1024];
static int q_n, q_op;
static char q_dst[1024];
static char q_cid[64];
static char q_out[1024];
static volatile int cancel_flag;
static sys_ppu_thread_t thr;
static int thr_alive;
static uint8_t *buf;
static uint64_t last_t, last_b;

uint64_t ops_now_ms(void)
{
    u64 s, ns;
    sysGetCurrentTime(&s, &ns);
    return s * 1000ULL + ns / 1000000ULL;
}

static void join(char *out, int n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (l && dir[l - 1] == '/') snprintf(out, n, "%s%s", dir, name);
    else snprintf(out, n, "%s/%s", dir, name);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    if (s && s[1]) return s + 1;
    return p;
}

static void tick(uint64_t add)
{
    st.bytes_done += add;
    uint64_t t = ops_now_ms();
    if (t - last_t >= 500) {
        st.speed = (uint32_t)((st.bytes_done - last_b) * 1000ULL / (t - last_t));
        last_t = t; last_b = st.bytes_done;
    }
}

/* size + file count of a tree */
static void measure(const char *path, int depth)
{
    int is_dir = 0; uint64_t sz = 0;
    if (vfs_stat(path, &is_dir, &sz) != 0) return;
    if (!is_dir) { st.bytes_total += sz; st.files_total++; return; }
    st.files_total++;
    if (depth > 32) return;
    vfs_entry_t *ents = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * MAX_LIST);
    if (!ents) return;
    int n = vfs_list(path, ents, MAX_LIST);
    char child[1024];
    for (int i = 0; i < n && !cancel_flag; i++) {
        join(child, sizeof(child), path, ents[i].name);
        if (ents[i].is_dir) measure(child, depth + 1);
        else { st.bytes_total += ents[i].size; st.files_total++; }
    }
    free(ents);
}

/* ---- large files and FAT32 ----
 * Copying a file of 4 GB or more to a FAT32 drive splits it the way webMAN/multiMAN read it:
 *   game.iso  -> game.iso.0, game.iso.1, ...        other files -> name.66600, name.66601, ...
 * Copying such parts to a drive without the limit (HDD, NTFS, exFAT) joins them back into one file. */
#define PART_SIZE 0xFFFF0000ULL   /* just under 4 GB, a multiple of 64 KB (and of 2048-byte sectors) */

static int ends_with_ci(const char *s, const char *e) { size_t a = strlen(s), b = strlen(e); return a >= b && !strcasecmp(s + a - b, e); }

/* part index of "x.iso.N" or "x.666NN", else -1; base receives the name without the suffix */
static int part_index(const char *path, char *base, int n)
{
    const char *dot = strrchr(path, '.');
    if (!dot || !dot[1]) return -1;
    const char *num = dot + 1;
    for (const char *p = num; *p; p++) if (*p < '0' || *p > '9') return -1;
    int len = (int)(dot - path);
    if (strlen(num) == 5 && !strncmp(num, "666", 3)) {
        if (base) { snprintf(base, n, "%.*s", len, path); }
        return atoi(num + 3);
    }
    if (strlen(num) <= 2 && len >= 4 && !strncasecmp(dot - 4, ".iso", 4)) {
        if (base) { snprintf(base, n, "%.*s", len, path); }
        return atoi(num);
    }
    return -1;
}

static void part_name(const char *base, int i, int iso, char *out, int n)
{
    if (iso) snprintf(out, n, "%s.%d", base, i);
    else snprintf(out, n, "%s.666%02d", base, i);
}

static int pump(vfs_file_t *in, vfs_file_t *out, uint64_t limit, uint64_t *moved)
{
    *moved = 0;
    while (!cancel_flag && *moved < limit) {
        uint64_t want = limit - *moved;
        if (want > BUF_SIZE) want = BUF_SIZE;
        int64_t r = vfs_read(in, buf, (uint32_t)want);
        if (r < 0) return OPE_READ;
        if (r == 0) break;
        if (vfs_write(out, buf, (uint32_t)r) != r) return OPE_WRITE;
        *moved += (uint64_t)r;
        tick((uint64_t)r);
    }
    return OPE_NONE;
}

static int copy_plain(const char *src, const char *dst)
{
    vfs_file_t *in = vfs_open_read(src);
    if (!in) { snprintf(st.error_path, sizeof(st.error_path), "%s", src); return OPE_OPEN_SRC; }
    vfs_file_t *out = vfs_open_write(dst);
    if (!out) { vfs_close(in); snprintf(st.error_path, sizeof(st.error_path), "%s", dst); return OPE_OPEN_DST; }
    uint64_t moved;
    int err = pump(in, out, ~0ULL, &moved);
    vfs_close(in);
    vfs_close(out);
    if (err || cancel_flag) vfs_unlink(dst);
    if (err) snprintf(st.error_path, sizeof(st.error_path), "%s", src);
    return err;
}

static int copy_split(const char *src, const char *dst)
{
    vfs_file_t *in = vfs_open_read(src);
    if (!in) { snprintf(st.error_path, sizeof(st.error_path), "%s", src); return OPE_OPEN_SRC; }
    int iso = ends_with_ci(dst, ".iso");
    int err = OPE_NONE;
    char pn[1100];
    for (int i = 0; i < 100 && !cancel_flag; i++) {
        part_name(dst, i, iso, pn, sizeof(pn));
        vfs_file_t *out = vfs_open_write(pn);
        if (!out) { err = OPE_OPEN_DST; snprintf(st.error_path, sizeof(st.error_path), "%s", pn); break; }
        uint64_t moved;
        err = pump(in, out, PART_SIZE, &moved);
        vfs_close(out);
        if (err) { snprintf(st.error_path, sizeof(st.error_path), "%s", pn); break; }
        if (moved == 0) { vfs_unlink(pn); break; }     /* exact multiple: no empty last part */
        if (moved < PART_SIZE) break;
    }
    vfs_close(in);
    if (err || cancel_flag) for (int i = 0; i < 100; i++) { part_name(dst, i, iso, pn, sizeof(pn)); if (!vfs_exists(pn)) break; vfs_unlink(pn); }
    return err;
}

static int copy_join(const char *base_src, const char *dst, int iso)
{
    vfs_file_t *out = vfs_open_write(dst);
    if (!out) { snprintf(st.error_path, sizeof(st.error_path), "%s", dst); return OPE_OPEN_DST; }
    int err = OPE_NONE;
    char pn[1100];
    for (int i = 0; i < 100 && !cancel_flag; i++) {
        part_name(base_src, i, iso, pn, sizeof(pn));
        if (!vfs_exists(pn)) break;
        vfs_file_t *in = vfs_open_read(pn);
        if (!in) { err = OPE_OPEN_SRC; snprintf(st.error_path, sizeof(st.error_path), "%s", pn); break; }
        uint64_t moved;
        err = pump(in, out, ~0ULL, &moved);
        vfs_close(in);
        if (err) { snprintf(st.error_path, sizeof(st.error_path), "%s", pn); break; }
    }
    vfs_close(out);
    if (err || cancel_flag) vfs_unlink(dst);
    return err;
}

static int copy_file(const char *src, const char *dst)
{
    snprintf(st.current, sizeof(st.current), "%s", base_name(src));
    int err;
    char sbase[1024], dbase[1024];
    int pi = part_index(src, sbase, sizeof(sbase));
    int dst_limited = vfs_4gb_limit(dst);
    if (pi >= 0 && !dst_limited) {
        /* parts of a split file going to a drive that can hold the whole thing */
        int iso = ends_with_ci(sbase, ".iso");
        char first[1100];
        part_name(sbase, 0, iso, first, sizeof(first));
        if (pi > 0 && vfs_exists(first)) { st.files_done++; return OPE_NONE; }   /* joined with part 0 */
        if (pi == 0) {
            part_index(dst, dbase, sizeof(dbase));
            snprintf(st.current, sizeof(st.current), "%s", base_name(dbase));
            err = copy_join(sbase, dbase, iso);
            st.files_done++;
            return err;
        }
    }
    uint64_t sz = 0; int d = 0;
    vfs_stat(src, &d, &sz);
    if (dst_limited && sz >= 0xFFFFFFFFULL) err = copy_split(src, dst);
    else err = copy_plain(src, dst);
    st.files_done++;
    return err;
}

static int delete_tree(const char *path, int depth)
{
    int is_dir = 0;
    if (vfs_stat(path, &is_dir, NULL) != 0) return OPE_NONE;
    snprintf(st.current, sizeof(st.current), "%s", base_name(path));
    if (is_dir && depth < 40) {
        vfs_entry_t *ents = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * MAX_LIST);
        if (!ents) return OPE_MEM;
        int n;
        char child[1024];
        /* re-list until empty: deleting while iterating is unsafe on some backends */
        while (!cancel_flag && (n = vfs_list(path, ents, MAX_LIST)) > 0) {
            int progressed = 0;
            for (int i = 0; i < n && !cancel_flag; i++) {
                join(child, sizeof(child), path, ents[i].name);
                if (delete_tree(child, depth + 1) == OPE_NONE) progressed = 1;
            }
            if (!progressed) break;
        }
        free(ents);
        if (cancel_flag) return OPE_NONE;
        if (vfs_rmdir(path) != 0) { snprintf(st.error_path, sizeof(st.error_path), "%s", path); return OPE_DELETE; }
    } else {
        if (vfs_unlink(path) != 0) { snprintf(st.error_path, sizeof(st.error_path), "%s", path); return OPE_DELETE; }
    }
    st.files_done++;
    return OPE_NONE;
}

static int copy_tree(const char *src, const char *dst, int depth)
{
    int is_dir = 0; uint64_t sz = 0;
    if (vfs_stat(src, &is_dir, &sz) != 0) { snprintf(st.error_path, sizeof(st.error_path), "%s", src); return OPE_OPEN_SRC; }
    if (!is_dir) return copy_file(src, dst);

    if (vfs_mkdir(dst) != 0 && !vfs_exists(dst)) { snprintf(st.error_path, sizeof(st.error_path), "%s", dst); return OPE_MKDIR; }
    st.files_done++;
    if (depth > 32) return OPE_NONE;
    vfs_entry_t *ents = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * MAX_LIST);
    if (!ents) return OPE_MEM;
    int n = vfs_list(src, ents, MAX_LIST);
    if (n < 0) {
        free(ents);
        snprintf(st.error_path, sizeof(st.error_path), "%s", src);
        return OPE_READ; /* unreadable USB folder must not be reported as an empty save */
    }
    int err = OPE_NONE;
    char cs[1024], cd[1024];
    for (int i = 0; i < n && !cancel_flag && !err; i++) {
        join(cs, sizeof(cs), src, ents[i].name);
        join(cd, sizeof(cd), dst, ents[i].name);
        err = copy_tree(cs, cd, depth + 1);
    }
    free(ents);
    return err;
}

static void pt_cb(uint64_t add, const char *cur)
{
    if (cur) { snprintf(st.current, sizeof(st.current), "%s", cur); st.files_done++; }
    if (add) tick(add);
}

static void pt_cb_fwd(uint64_t add, const char *cur) { pt_cb(add, cur); }

/* Verify every staged file has the same size as its USB source before
 * touching the existing save. This detects premature EOF and disconnects. */
static int verify_staged_tree(const char *src, const char *dst, int depth)
{
    if (depth > 32) return OPE_READ;
    int sdir=0, ddir=0; uint64_t ssize=0, dsize=0;
    if (vfs_stat(src,&sdir,&ssize)!=0 || vfs_stat(dst,&ddir,&dsize)!=0 ||
        sdir!=ddir) {
        snprintf(st.error_path,sizeof(st.error_path),"%s",src);
        return OPE_READ;
    }
    if (!sdir) {
        if (ssize!=dsize) {
            snprintf(st.error_path,sizeof(st.error_path),"%s",src);
            return OPE_READ;
        }
        return OPE_NONE;
    }
    vfs_entry_t *entries=malloc(sizeof(vfs_entry_t)*MAX_LIST);
    if (!entries) return OPE_MEM;
    int n=vfs_list(src,entries,MAX_LIST),err=OPE_NONE;
    if (n<0 || n==MAX_LIST) {
        snprintf(st.error_path,sizeof(st.error_path),"%s",src);
        err=OPE_READ;
    }
    char s[1024],d[1024];
    for (int i=0; !err && i<n && !cancel_flag; i++) {
        join(s,sizeof(s),src,entries[i].name);
        join(d,sizeof(d),dst,entries[i].name);
        err=verify_staged_tree(s,d,depth+1);
    }
    free(entries);
    return err;
}

/* Safe restore from USB to the native PS3 save directory.
 * Copy to a private staging folder FIRST. An existing save is moved out of
 * savedata as an untouched backup, and is never overwritten in place.
 * All renames happen on the native HDD after the copy has succeeded. */
static int restore_save_one(const char *src, const char *dest_root, uint64_t stamp, int idx)
{
    const char *name = base_name(src);
    int src_is_dir = 0;
    if (!*name || strchr(name, '/') ||
        vfs_stat(src, &src_is_dir, NULL) != 0 || !src_is_dir) {
        snprintf(st.error_path,sizeof(st.error_path),"%s",src);
        return OPE_OPEN_SRC;
    }
    /* Restore only to the validated on-console home/NNNNNNNN/savedata path. */
    const char *prefix="/dev_hdd0/home/";
    const size_t prelen=strlen(prefix);
    if (strncmp(dest_root,prefix,prelen) || strlen(dest_root)!=prelen+8+9 ||
        strcmp(dest_root+prelen+8,"/savedata")) {
        snprintf(st.error_path,sizeof(st.error_path),"%s",dest_root);
        return OPE_OPEN_DST;
    }
    for (int j=0;j<8;j++) {
        char ch=dest_root[prelen+j];
        if (ch<'0' || ch>'9') return OPE_OPEN_DST;
    }

    const char *tmp_root="/dev_hdd0/CRUYFFORD_RESTORE_STAGING";
    const char *bak_root="/dev_hdd0/CRUYFFORD_SAVE_RESTORE_BACKUPS";
    char uid[9], user_bak[1024], stage[1024], target[1024], backup[1024], sfo[1024];
    memcpy(uid,dest_root+prelen,8); uid[8]=0;
    if (vfs_mkdir(tmp_root)!=0 || vfs_mkdir(bak_root)!=0) {
        snprintf(st.error_path,sizeof(st.error_path),"%s",tmp_root);
        return OPE_MKDIR;
    }
    join(user_bak,sizeof(user_bak),bak_root,uid);
    if (vfs_mkdir(user_bak)!=0) {
        snprintf(st.error_path,sizeof(st.error_path),"%s",user_bak);
        return OPE_MKDIR;
    }
    snprintf(stage,sizeof(stage),"%s/fm_%llu_%d_%s",tmp_root,
             (unsigned long long)stamp,idx,name);
    snprintf(backup,sizeof(backup),"%s/fm_%llu_%d_%s",user_bak,
             (unsigned long long)stamp,idx,name);
    join(target,sizeof(target),dest_root,name);
    if (vfs_exists(stage) || vfs_exists(backup)) {
        snprintf(st.error_path,sizeof(st.error_path),"%s",stage);
        return OPE_OPEN_DST; /* never reuse a staging/backup directory */
    }

    int error=copy_tree(src,stage,0);
    if (error || cancel_flag) return error; /* original live save remains unchanged */
    error=verify_staged_tree(src,stage,0);
    if (error || cancel_flag) return error;
    join(sfo,sizeof(sfo),stage,"PARAM.SFO");
    if (!vfs_exists(sfo)) {
        snprintf(st.error_path,sizeof(st.error_path),"%s",sfo);
        return OPE_READ;
    }
    if (cancel_flag) return OPE_NONE;

    const int previous=vfs_exists(target);
    if (previous && vfs_rename(target,backup)!=0) {
        snprintf(st.error_path,sizeof(st.error_path),"%s",target);
        return OPE_WRITE;
    }
    if (vfs_rename(stage,target)!=0) {
        if (previous) {
            /* Best-effort rollback: never delete the original backup. */
            if (vfs_rename(backup,target)!=0)
                snprintf(st.error_path,sizeof(st.error_path),
                         "ROLLBACK REQUIRED: %s",backup);
            else snprintf(st.error_path,sizeof(st.error_path),"%s",stage);
        } else snprintf(st.error_path,sizeof(st.error_path),"%s",stage);
        return OPE_WRITE;
    }
    return OPE_NONE;
}

static void worker(void *arg)
{
    (void)arg;
    int err = OPE_NONE;
    last_t = ops_now_ms(); last_b = 0;

    if (q_op == OP_UNARCHIVE) {
        const char *tmp = "/dev_hdd0/tmp/fm_arc";
        char nsrc[1024], ndst[1024];
        int src_native = vfs_backend(q_src[0]) == VFS_NATIVE, dst_native = vfs_backend(q_dst) == VFS_NATIVE;
        vfs_mkdir("/dev_hdd0/tmp");
        vfs_mkdir(tmp);
        buf = (uint8_t *)malloc(BUF_SIZE);
        if (!buf) err = OPE_MEM;
        if (!err && !src_native) {
            uint64_t sz = 0; int d = 0;
            vfs_stat(q_src[0], &d, &sz);
            st.bytes_total = sz;
            join(nsrc, sizeof(nsrc), tmp, base_name(q_src[0]));
            err = copy_file(q_src[0], nsrc);
            st.bytes_done = 0; last_b = 0;
        } else snprintf(nsrc, sizeof(nsrc), "%s", q_src[0]);
        if (dst_native) snprintf(ndst, sizeof(ndst), "%s", q_dst);
        else { join(ndst, sizeof(ndst), tmp, "out"); delete_tree(ndst, 0); }
        if (!err && !cancel_flag) err = archive_extract(nsrc, ndst, &cancel_flag, pt_cb_fwd, &st.bytes_total);
        if (!err && !cancel_flag && !dst_native) {
            st.bytes_done = 0; st.bytes_total = 0; st.files_total = 0; last_b = 0;
            measure(ndst, 0);
            err = copy_tree(ndst, q_dst, 0);
        }
        if (!dst_native) delete_tree(ndst, 0);
        if (!src_native) vfs_unlink(nsrc);
        free(buf); buf = NULL;
        st.error = err;
        st.state = cancel_flag ? OPS_CANCELLED : err ? OPS_ERROR : OPS_DONE;
        sysThreadExit(0);
        return;
    }

    if (q_op == OP_PFD_RESIGN_WORK) {
        /* Only PFD files with a single PARAM.SFO entry are supported.
         * Never overwrite a live save; resign a verified HDD working copy. */
        char dst[1024]={0};
        if (q_n!=1 || !pfd_probe_sfo_only(q_src[0]) ||
            vfs_backend(q_dst)!=VFS_NATIVE || !vfs_exists(q_dst)) {
            snprintf(st.error_path,sizeof(st.error_path),"%s",q_src[0]);
            err=OPE_OPEN_SRC;
        }
        if (!err) {
            join(dst,sizeof(dst),q_dst,base_name(q_src[0]));
            if (vfs_exists(dst)) {
                snprintf(st.error_path,sizeof(st.error_path),"%s",dst);
                err=OPE_OPEN_DST;
            }
        }
        if (!err) {
            measure(q_src[0],0);
            buf=(uint8_t *)malloc(BUF_SIZE);
            if (!buf) err=OPE_MEM;
        }
        if (!err) err=copy_tree(q_src[0],dst,0);
        if (!err && !cancel_flag) {
            savesync_pfd_options_t opts;
            savesync_pfd_default_options(&opts);
            /* Portable profile keys, only safe for metadata-only PFD
             * with no encrypted game data or secure-file ID entries. */
            if (savesync_pfd_self_test()!=0 ||
                savesync_pfd_resign(dst,&opts)!=0 ||
                savesync_pfd_validate(dst)!=0) {
                snprintf(st.error_path,sizeof(st.error_path),"%s/PARAM.PFD",dst);
                err=OPE_WRITE;
            }
        }
        free(buf);buf=NULL;
        st.error=err;
        st.state=cancel_flag?OPS_CANCELLED:err?OPS_ERROR:OPS_DONE;
        sysThreadExit(0);
        return;
    }

    if (q_op == OP_SAVE_TRASH) {
        /* Delete a selected save by moving its entire folder to a recoverable
         * trash location ON THE SAME FILESYSTEM; never copy+delete on error. */
        st.files_total=(uint32_t)q_n;
        if (!vfs_exists(q_dst)) {
            snprintf(st.error_path,sizeof(st.error_path),"%s",q_dst);
            err=OPE_OPEN_DST;
        }
        const uint64_t stamp=ops_now_ms();
        for (int i=0; i<q_n && !cancel_flag && !err; i++) {
            const char *src=q_src[i];
            const char *name=base_name(src);
            char sfo[1024], moved[1024];
            int is_dir=0;
            size_t plen=strlen(q_dst);
            int valid_source = strstr(src,"/PS3/SAVEDATA/") ||
                               strstr(src,"/savedata/");
            if (!valid_source || !name[0] || strlen(name)>180 ||
                vfs_stat(src,&is_dir,NULL)!=0 || !is_dir ||
                vfs_backend(src)!=vfs_backend(q_dst)) {
                snprintf(st.error_path,sizeof(st.error_path),"%s",src);
                err=OPE_OPEN_SRC;
                break;
            }
            join(sfo,sizeof(sfo),src,"PARAM.SFO");
            if (!vfs_exists(sfo)) {
                snprintf(st.error_path,sizeof(st.error_path),"%s",sfo);
                err=OPE_OPEN_SRC;
                break;
            }
            if (plen+strlen(name)+28 >= sizeof(moved)) {
                err=OPE_OPEN_DST;
                snprintf(st.error_path,sizeof(st.error_path),"%s",q_dst);
                break;
            }
            snprintf(moved,sizeof(moved),"%s/%llu_%02d_%s",
                     q_dst,(unsigned long long)stamp,i,name);
            if (vfs_exists(moved) || vfs_rename(src,moved)!=0) {
                snprintf(st.error_path,sizeof(st.error_path),"%s",src);
                err=OPE_DELETE;
                break;
            }
            snprintf(st.current,sizeof(st.current),"%s",name);
            st.files_done++;
        }
        st.error=err;
        st.state=cancel_flag ? OPS_CANCELLED : err ? OPS_ERROR : OPS_DONE;
        sysThreadExit(0);
        return;
    }

    if (q_op == OP_DISC || q_op == OP_UNISO) {
        if (q_op == OP_DISC) err = disc_dump(q_cid[0] == '1', q_dst, &cancel_flag, pt_cb, &st.bytes_total, st.error_path, sizeof(st.error_path), q_out, sizeof(q_out));
        else { snprintf(q_out, sizeof(q_out), "%s", q_dst); err = iso_extract(q_src[0], q_dst, &cancel_flag, pt_cb, &st.bytes_total, st.error_path, sizeof(st.error_path)); }
        st.error = err;
        st.state = cancel_flag ? OPS_CANCELLED : err ? OPS_ERROR : OPS_DONE;
        sysThreadExit(0);
        return;
    }

    if (q_op == OP_EXTRACT || q_op == OP_BUILD) {
        if (q_op == OP_EXTRACT)
            err = pkgtool_extract(q_src[0], q_dst, &cancel_flag, pt_cb, &st.bytes_total, st.error_path, sizeof(st.error_path));
        else
            err = pkgtool_build(q_src[0], q_dst, q_cid, &cancel_flag, pt_cb, &st.bytes_total, st.error_path, sizeof(st.error_path));
        st.error = err;
        st.state = cancel_flag ? OPS_CANCELLED : err ? OPS_ERROR : OPS_DONE;
        sysThreadExit(0);
        return;
    }

    for (int i = 0; i < q_n && !cancel_flag; i++) measure(q_src[i], 0);

    if (q_op != OP_DELETE) {
        buf = (uint8_t *)malloc(BUF_SIZE);
        if (!buf) err = OPE_MEM;
        /* space check only when data actually has to be copied */
        int need_copy = 0;
        for (int i = 0; i < q_n; i++) if (vfs_backend(q_src[i]) != vfs_backend(q_dst) || q_op == OP_COPY) need_copy = 1;
        if (!err && need_copy) {
            uint64_t fr = vfs_free_space(q_dst);
            if (fr && fr < st.bytes_total) err = OPE_SPACE;
        }
    }

    if (q_op == OP_SAVE_RESTORE && !err) {
        /* Destination must already be a real PS3 account savedata folder. */
        if (!vfs_exists(q_dst)) {
            snprintf(st.error_path,sizeof(st.error_path),"%s",q_dst);
            err=OPE_OPEN_DST;
        }
        const uint64_t stamp=ops_now_ms();
        for (int i=0; i<q_n && !cancel_flag && !err; i++)
            err=restore_save_one(q_src[i],q_dst,stamp,i);
        free(buf); buf=NULL;
        st.error=err;
        st.state=cancel_flag ? OPS_CANCELLED : err ? OPS_ERROR : OPS_DONE;
        sysThreadExit(0);
        return;
    }

    for (int i = 0; i < q_n && !cancel_flag && !err; i++) {
        const char *src = q_src[i];
        if (q_op == OP_DELETE) { err = delete_tree(src, 0); continue; }

        char dst[1024];
        join(dst, sizeof(dst), q_dst, base_name(src));
        size_t sl = strlen(src);
        if (!strncmp(q_dst, src, sl) && (q_dst[sl] == '/' || q_dst[sl] == 0)) { err = OPE_INTO_SELF; break; }
        if (!strcmp(src, dst)) continue;

        if (q_op == OP_MOVE && vfs_backend(src) == vfs_backend(q_dst) && vfs_rename(src, dst) == 0) {
            uint64_t sz = 0; int d = 0;
            vfs_stat(dst, &d, &sz);
            tick(d ? 0 : sz);
            st.files_done++;
            continue;
        }
        err = copy_tree(src, dst, 0);
        if (!err && !cancel_flag && q_op == OP_MOVE) err = delete_tree(src, 0);
    }

    free(buf); buf = NULL;
    st.error = err;
    st.state = cancel_flag ? OPS_CANCELLED : err ? OPS_ERROR : OPS_DONE;
    sysThreadExit(0);
}

int ops_start(int op, char srcs[][1024], int n, const char *dst_dir)
{
    if (st.state == OPS_RUNNING || n <= 0) return 0;
    /* Previous job may be complete but its joinable thread has not been
     * reaped. Reap it before reusing the global queue or thread handle. */
    if (thr_alive) {
        u64 rv;
        sysThreadJoin(thr,&rv);
        thr_alive=0;
    }
    if (!q_src) q_src = malloc(sizeof(*q_src) * OPS_MAX_SRC);
    if (!q_src) return 0;
    if (n > OPS_MAX_SRC) n = OPS_MAX_SRC;
    memset(&st, 0, sizeof(st));
    memcpy(q_src, srcs, sizeof(*q_src) * n);
    q_n = n; q_op = op;
    snprintf(q_dst, sizeof(q_dst), "%s", dst_dir ? dst_dir : "");
    cancel_flag = 0;
    st.op = op;
    st.state = OPS_RUNNING;
    if (sysThreadCreate(&thr, worker, NULL, 1200, 0x100000, THREAD_JOINABLE, "fileops") != 0) {
        st.state = OPS_ERROR; st.error = OPE_MEM; thr_alive = 0;
        return 0;
    }
    thr_alive = 1;
    return 1;
}

static int start_tool(int op, const char *src, const char *dst, const char *cid)
{
    static char one[1][1024];
    snprintf(one[0], sizeof(one[0]), "%s", src);
    snprintf(q_cid, sizeof(q_cid), "%s", cid ? cid : "");
    return ops_start(op, one, 1, dst);
}

int ops_start_extract(const char *pkg_path, const char *out_dir) { return start_tool(OP_EXTRACT, pkg_path, out_dir, NULL); }
int ops_start_build(const char *folder, const char *out_pkg, const char *cid) { return start_tool(OP_BUILD, folder, out_pkg, cid); }
int ops_start_unarchive(const char *arc, const char *dest) { return start_tool(OP_UNARCHIVE, arc, dest, NULL); }
int ops_start_disc(int mode, const char *dst) { q_out[0] = 0; return start_tool(OP_DISC, "/dev_bdvd", dst, mode ? "1" : "0"); }
int ops_start_uniso(const char *img, const char *dst) { q_out[0] = 0; return start_tool(OP_UNISO, img, dst, NULL); }
const char *ops_out_path(void) { return q_out; }

/* ---------- folder size ---------- */
static volatile int ds_done = 1;
static volatile uint64_t ds_bytes;
static volatile uint32_t ds_files, ds_dirs;
static char ds_path[1024];
static sys_ppu_thread_t ds_thr;
static int ds_alive;

static void ds_walk(const char *path, int depth)
{
    vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * MAX_LIST);
    if (!e) return;
    int n = vfs_list(path, e, MAX_LIST);
    char child[1024];
    for (int i = 0; i < n; i++) {
        if (e[i].is_dir) { ds_dirs++; if (depth < 32) { join(child, sizeof(child), path, e[i].name); ds_walk(child, depth + 1); } }
        else { ds_files++; ds_bytes += e[i].size; }
    }
    free(e);
}

static void ds_thread(void *arg) { (void)arg; ds_walk(ds_path, 0); ds_done = 1; sysThreadExit(0); }

void dirsize_start(const char *path)
{
    if (ds_alive) { u64 rv; sysThreadJoin(ds_thr, &rv); ds_alive = 0; }
    snprintf(ds_path, sizeof(ds_path), "%s", path);
    ds_bytes = 0; ds_files = 0; ds_dirs = 0; ds_done = 0;
    if (sysThreadCreate(&ds_thr, ds_thread, NULL, 1500, 0x40000, THREAD_JOINABLE, "dirsize") == 0) ds_alive = 1;
    else ds_done = 1;
}
int dirsize_done(void) { return ds_done; }
void dirsize_result(uint64_t *b, uint32_t *f, uint32_t *d) { *b = ds_bytes; *f = ds_files; *d = ds_dirs; }

void ops_cancel(void) { cancel_flag = 1; }
const ops_status_t *ops_status(void) { return &st; }

void ops_reset(void)
{
    if (st.state == OPS_RUNNING) return;
    if (thr_alive) { u64 rv; sysThreadJoin(thr, &rv); thr_alive = 0; }
    memset(&st, 0, sizeof(st));
}

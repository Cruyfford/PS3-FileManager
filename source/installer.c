/* Background installer.
 * XMB queue mode: copies the PKG into /dev_hdd0/vsh/game_pkg/<id>/ together with the
 * d0/d1/f0 .pdb records and ICON_FILE; the XMB installs it automatically when you
 * return to it (same mechanism used by PKGi PS3 "direct" installs, works on HEN and CFW).
 * Packages-folder mode: copies the PKG to /dev_hdd0/packages for "Install Package Files". */
#include "installer.h"
#include "vfs.h"
#include <lv2/sysfs.h>
#include <sys/thread.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define COPY_BUF (4 * 1024 * 1024)
#define FIRST_TASK_ID 80000002

static inst_status_t st;
static inst_job_t jobs_q[MAX_JOBS];
static int q_mode, q_delete;
static volatile int cancel_flag;
static const uint8_t *def_icon;
static uint32_t def_icon_size;
static sys_ppu_thread_t thr;

/* PDB records (see https://www.psdevwiki.com/ps3/Project_Database_(PDB)), layout as used by PKGi PS3 */
static const unsigned char install_data_pdb[123] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x64,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x65,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
    0x00,0x00,0x00,0x00,0x6B,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
    0x03,0x00,0x00,0x00,0x68,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x6C,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xDA,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x01,0x00,0x00,
    0x00,0xCD,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x01
};
static const unsigned char pdb_end_data[110] = {
    0x00,0x00,0x00,0xF0,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,
    0x00,0x00,0x00,0x05,0x00,0x00,0x00,0xF1,0x00,0x00,0x00,0x01,
    0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0xF2,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0xF4,0x00,0x00,
    0x00,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x19,0x62,0x00,0x00,
    0x00,0xDA,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x01,0x00,
    0x00,0x00,0xCD,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,
    0x00,0x00,0x00,0xEC,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
    0x01,0x00,0x00,0x00,0xFC,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x01,0x00
};

#define PDB_HDR_FILENAME "\x00\x00\x00\xCB"
#define PDB_HDR_ICON     "\x00\x00\x00\x6A"
#define PDB_HDR_TITLE    "\x00\x00\x00\x69"
#define PDB_HDR_SIZE     "\x00\x00\x00\xCE"
#define PDB_HDR_UNUSED   "\x00\x00\x00\x00"
#define PDB_HDR_DLSIZE   "\x00\x00\x00\xD0"

static void pdb_str(FILE *f, const char *hdr, const char *s)
{
    uint32_t len = (uint32_t)strlen(s) + 1;   /* big-endian host: native write is BE */
    fwrite(hdr, 1, 4, f);
    fwrite(&len, 4, 1, f);
    fwrite(&len, 4, 1, f);
    fwrite(s, 1, len, f);
}

static void pdb_u64(FILE *f, const char *hdr, uint64_t v)
{
    fwrite(hdr, 1, 4, f);
    fwrite("\x00\x00\x00\x08\x00\x00\x00\x08", 1, 8, f);
    fwrite(&v, 8, 1, f);
}

static int write_pdb(const char *dir, const char *filename, const char *title, uint64_t size)
{
    char p0[1100], p1[1100], icon[1100], disp[300];
    snprintf(p0, sizeof(p0), "%s/d0.pdb", dir);
    snprintf(p1, sizeof(p1), "%s/d1.pdb", dir);
    snprintf(icon, sizeof(icon), "%s/ICON_FILE", dir);
    snprintf(disp, sizeof(disp), "\xE2\x98\x85 %s", title && title[0] ? title : filename);

    FILE *f0 = fopen(p0, "wb");
    FILE *f1 = fopen(p1, "wb");
    if (!f0 || !f1) { if (f0) fclose(f0); if (f1) fclose(f1); return 0; }

    FILE *fs[2] = { f0, f1 };
    for (int i = 0; i < 2; i++) {
        FILE *f = fs[i];
        fwrite(install_data_pdb, 1, sizeof(install_data_pdb), f);
        pdb_u64(f, PDB_HDR_DLSIZE, size);
        pdb_u64(f, PDB_HDR_SIZE, size);
        pdb_str(f, PDB_HDR_TITLE, disp);
        pdb_str(f, PDB_HDR_FILENAME, filename);
        if (i == 1) pdb_str(f, PDB_HDR_UNUSED, icon);
        pdb_str(f, PDB_HDR_ICON, icon);
        fwrite(pdb_end_data, 1, sizeof(pdb_end_data), f);
        fclose(f);
    }

    char pf[1100];
    snprintf(pf, sizeof(pf), "%s/f0.pdb", dir);
    FILE *ff = fopen(pf, "wb");
    if (ff) fclose(ff);
    return 1;
}

static int dir_exists(const char *p)
{
    struct stat sb;
    return stat(p, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static void mkdirs(const char *path)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; if (!dir_exists(tmp)) mkdir(tmp, 0777); *p = '/'; }
    }
    if (!dir_exists(tmp)) mkdir(tmp, 0777);
}

uint64_t hdd_free_bytes(void)
{
    u32 bs = 0; u64 blocks = 0;
    if (sysFsGetFreeSize("/dev_hdd0/", &bs, &blocks) != 0) return 0;
    return (uint64_t)bs * blocks;
}

static uint64_t now_ms(void)
{
    u64 sec, nsec;
    sysGetCurrentTime(&sec, &nsec);
    return sec * 1000ULL + nsec / 1000000ULL;
}

static int same_device_hdd(const char *src)
{
    return strncmp(src, "/dev_hdd0/", 10) == 0;
}

static int copy_file(const char *src, const char *dst, uint8_t *buf)
{
    vfs_file_t *in = vfs_open_read(src);
    if (!in) return ERR_OPEN_SRC;
    s32 out;
    if (sysFsOpen(dst, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &out, NULL, 0) != 0) { vfs_close(in); return ERR_OPEN_DST; }
    sysFsChmod(dst, 0666);

    uint64_t t0 = now_ms(), last_bytes = 0, last_t = t0;
    int err = ERR_NONE;
    for (;;) {
        if (cancel_flag) break;
        int64_t rd = vfs_read(in, buf, COPY_BUF);
        if (rd < 0) { err = ERR_READ; break; }
        if (rd == 0) break;
        u64 wr = 0;
        if (sysFsWrite(out, buf, (u64)rd, &wr) != 0 || wr != (u64)rd) { err = ERR_WRITE; break; }
        st.done_bytes += rd;
        st.all_done += rd;
        uint64_t t = now_ms();
        if (t - last_t >= 500) {
            st.speed = (uint32_t)((st.done_bytes - last_bytes) * 1000ULL / (t - last_t));
            last_t = t; last_bytes = st.done_bytes;
        }
    }
    vfs_close(in);
    sysFsClose(out);
    if (cancel_flag || err) sysFsUnlink(dst);
    return err;
}

static int write_icon(const char *dir, const inst_job_t *j)
{
    char p[1100];
    snprintf(p, sizeof(p), "%s/ICON_FILE", dir);
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    if (j->icon && j->icon_size) fwrite(j->icon, 1, j->icon_size, f);
    else if (def_icon) fwrite(def_icon, 1, def_icon_size, f);
    fclose(f);
    return 1;
}

static void rm_task_dir(const char *dir)
{
    const char *files[] = { "d0.pdb", "d1.pdb", "f0.pdb", "ICON_FILE" };
    char p[1400];
    for (unsigned i = 0; i < 4; i++) { snprintf(p, sizeof(p), "%s/%s", dir, files[i]); sysFsUnlink(p); }
    sysFsRmdir(dir);
}

static int install_one(inst_job_t *j, uint8_t *buf)
{
    char dir[1100], dst[1400];
    int move = q_delete && same_device_hdd(j->path);

    if (!move && hdd_free_bytes() < j->size + 64ULL * 1024 * 1024) return ERR_SPACE;

    if (q_mode == MODE_PACKAGES_FOLDER) {
        mkdirs(PACKAGES_DIR);
        snprintf(dst, sizeof(dst), "%s/%s", PACKAGES_DIR, j->name);
        if (strcmp(dst, j->path) == 0) { st.all_done += j->size; return ERR_NONE; }
        if (move && rename(j->path, dst) == 0) { st.done_bytes = st.total_bytes; st.all_done += j->size; return ERR_NONE; }
        int e = copy_file(j->path, dst, buf);
        if (!e && q_delete && !cancel_flag) vfs_unlink(j->path);
        return e;
    }

    mkdirs(INSTALL_DIR);
    uint32_t tid = FIRST_TASK_ID;
    for (;; tid++) { snprintf(dir, sizeof(dir), "%s/%u", INSTALL_DIR, tid); if (!dir_exists(dir)) break; }
    if (mkdir(dir, 0777) != 0 && !dir_exists(dir)) return ERR_MKDIR;

    snprintf(dst, sizeof(dst), "%s/%s", dir, j->name);
    int e = ERR_NONE;
    if (move && rename(j->path, dst) == 0) { st.done_bytes = st.total_bytes; st.all_done += j->size; }
    else {
        e = copy_file(j->path, dst, buf);
        if (!e && q_delete && !cancel_flag) vfs_unlink(j->path);
    }
    if (e || cancel_flag) { rm_task_dir(dir); return e; }

    if (!write_icon(dir, j) || !write_pdb(dir, j->name, j->title, j->size)) {
        sysFsUnlink(dst); rm_task_dir(dir);
        return ERR_PDB;
    }
    return ERR_NONE;
}

static void worker(void *arg)
{
    (void)arg;
    uint8_t *buf = (uint8_t *)malloc(COPY_BUF);
    if (!buf) { st.error_code = ERR_MEM; st.state = INST_ERROR; sysThreadExit(0); return; }

    for (int i = 0; i < st.count; i++) {
        if (cancel_flag) break;
        inst_job_t *j = &jobs_q[i];
        st.current = i;
        snprintf(st.current_name, sizeof(st.current_name), "%s", j->title[0] ? j->title : j->name);
        st.done_bytes = 0;
        st.total_bytes = j->size;
        st.speed = 0;
        int e = install_one(j, buf);
        if (e) { st.error_code = e; break; }
        st.ok_count++;
    }
    free(buf);
    for (int i = 0; i < st.count; i++) { free(jobs_q[i].icon); jobs_q[i].icon = NULL; }

    if (cancel_flag) st.state = INST_CANCELLED;
    else if (st.error_code) st.state = INST_ERROR;
    else st.state = INST_DONE;
    sysThreadExit(0);
}

void inst_set_default_icon(const uint8_t *png, uint32_t size) { def_icon = png; def_icon_size = size; }

int inst_start(inst_job_t *jobs, int n, int mode, int delete_source)
{
    if (st.state == INST_RUNNING || n <= 0) return 0;
    if (n > MAX_JOBS) n = MAX_JOBS;
    memset(&st, 0, sizeof(st));
    memcpy(jobs_q, jobs, sizeof(inst_job_t) * n);
    st.count = n;
    for (int i = 0; i < n; i++) st.all_total += jobs[i].size;
    q_mode = mode; q_delete = delete_source;
    cancel_flag = 0;
    st.state = INST_RUNNING;
    if (sysThreadCreate(&thr, worker, NULL, 1200, 0x20000, THREAD_JOINABLE, "installer") != 0) {
        st.state = INST_ERROR; st.error_code = ERR_MEM; st.count = 0;
        return 0;
    }
    return 1;
}

void inst_cancel(void) { cancel_flag = 1; }
const inst_status_t *inst_status(void) { return &st; }

void inst_reset(void)
{
    if (st.state != INST_RUNNING) {
        u64 rv;
        if (st.count) sysThreadJoin(thr, &rv);
        memset(&st, 0, sizeof(st));
    }
}

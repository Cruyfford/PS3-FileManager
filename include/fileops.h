#pragma once
#include <stdint.h>

#define OPS_MAX_SRC 256

enum { OP_COPY = 0, OP_MOVE, OP_DELETE, OP_EXTRACT, OP_BUILD, OP_UNARCHIVE, OP_DISC, OP_UNISO, OP_SAVE_RESTORE, OP_HASH, OP_PATCH, OP_MKPATCH, OP_TIMX };
enum { OPS_IDLE = 0, OPS_RUNNING, OPS_DONE, OPS_ERROR, OPS_CANCELLED };
enum { OPE_NONE = 0, OPE_SPACE, OPE_OPEN_SRC, OPE_OPEN_DST, OPE_READ, OPE_WRITE, OPE_MKDIR, OPE_DELETE, OPE_INTO_SELF, OPE_MEM };

typedef struct {
    volatile int state;
    volatile int op;
    volatile int error;
    volatile uint64_t bytes_done, bytes_total;
    volatile uint32_t files_done, files_total;
    volatile uint32_t speed;
    char current[256];
    char error_path[512];
} ops_status_t;

/* srcs: full paths; dst_dir: destination folder (ignored for delete) */
int  ops_start(int op, char srcs[][1024], int n, const char *dst_dir);
/* PKG tools: extract pkg_path into out_dir/<CONTENT_ID>/ ; build folder into out_pkg */
int  ops_start_extract(const char *pkg_path, const char *out_dir);
int  ops_start_build(const char *folder, const char *out_pkg, const char *content_id);
int  ops_start_unarchive(const char *archive, const char *dest_dir);
/* disc dump (mode 0 image, 1 PS3 folder) into dst_dir; ISO/BIN image extraction */
int  ops_start_disc(int mode, const char *dst_dir);
int  ops_start_uniso(const char *image, const char *dst_dir);
const char *ops_out_path(void);
const char *ops_out_extra(void);                    /* OP_TIMX: number of images */
int  ops_start_hash(const char *file);              /* result text in ops_out_path() */
int  ops_start_timx(const char *file, const char *out_dir);
/* create=0: a = patch, b = source file; create=1: a = original, b = modified. out = new file */
int  ops_start_patch(int create, const char *a, const char *b, const char *out);
void ops_cancel(void);

/* background folder size for the Properties screen */
void dirsize_start(const char *path);
int  dirsize_done(void);
void dirsize_result(uint64_t *bytes, uint32_t *files, uint32_t *dirs);
const ops_status_t *ops_status(void);
void ops_reset(void);

/* synchronous helpers (used by the installer thread) */
uint64_t ops_now_ms(void);

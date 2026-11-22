#pragma once
#include <stdint.h>

typedef void (*pt_progress_fn)(uint64_t add_bytes, const char *current);

/* Extract a PS3 PKG (retail or debug) into out_dir/<CONTENT_ID>/.
   *total is set to the number of bytes that will be reported through cb. Returns OPE_* code. */
int pkgtool_extract(const char *pkg_path, const char *out_dir, volatile int *cancel,
                    pt_progress_fn cb, volatile uint64_t *total, char *err_path, int err_len);

/* Build a retail (finalized) PKG from a folder (PARAM.SFO, ICON0.PNG, USRDIR/...).
   content_id: 36 chars "UP0001-ABCD12345_00-0000000000000000".
   Data is read twice (hash pass + write pass); *total covers both. Returns OPE_* code. */
int pkgtool_build(const char *folder, const char *out_pkg, const char *content_id, volatile int *cancel,
                  pt_progress_fn cb, volatile uint64_t *total, char *err_path, int err_len);

/* Content ID for a folder: the folder name if it already is one, else UP0001-<TITLE_ID>_00-0000000000000000
   from its PARAM.SFO. Returns 1 on success. */
int pkgtool_content_id_for(const char *folder, char *out, int n);

/* ---------- PARAM.SFO editing ---------- */
#define SFO_MAX_FIELDS 32
typedef struct {
    char key[32];
    char value[512];
    uint32_t max_len;
    int is_string;
    uint32_t int_value;
} sfo_field_t;

int sfo_load(const char *path, sfo_field_t *fields, int max);          /* returns count, -1 on error */
/* rewrites one string field in place; fails if the new value exceeds max_len */
int sfo_set_string(const char *path, const char *key, const char *value);

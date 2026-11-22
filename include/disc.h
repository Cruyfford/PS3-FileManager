#pragma once
#include <stdint.h>

/* Game disc dumping (PS1/PS2 CD -> BIN/CUE, PS2 DVD -> ISO, PS3 -> ISO or folder)
 * and ISO/BIN image extraction. */

enum { DISC_NONE = 0, DISC_PS3, DISC_PS2, DISC_PS1, DISC_DATA };

typedef struct {
    int type;
    int is_cd;              /* CD media (raw 2352 dump) */
    int ntracks;
    char id[16];            /* BLUS12345 / SLUS_123.45 */
    char title[128];
    uint64_t sectors;       /* 2048-byte sectors */
    uint64_t bytes;         /* estimated dump size */
} disc_info_t;

typedef void (*disc_cb)(uint64_t add, const char *cur);

int disc_probe(disc_info_t *d);                 /* 0 = disc found */
const char *disc_type_name(int t);

/* mode: 0 image (ISO / BIN+CUE), 1 folder (PS3 only). dst_dir: destination folder.
 * out receives the main output path. Returns OPE_* code. */
int disc_dump(int mode, const char *dst_dir, volatile int *cancel, disc_cb cb, volatile uint64_t *total,
              char *errp, int en, char *out, int on);

/* extract an .iso/.bin/.img/.cue (or split .iso.0) into dst_dir */
int iso_extract(const char *img, const char *dst_dir, volatile int *cancel, disc_cb cb, volatile uint64_t *total,
                char *errp, int en);
int iso_is_image(const char *name);

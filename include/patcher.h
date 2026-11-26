#pragma once
#include <stdint.h>
/* ROM / disc-image patches: apply IPS, BPS, PPF (1/2/3) and xdelta3 (VCDIFF); create IPS (< 16 MB) or PPF3.
 * Output is always written to a new file. Returns 0 or an OPE_* style error (>0); message in err. */
enum { PATCH_NONE = 0, PATCH_IPS, PATCH_BPS, PATCH_PPF, PATCH_XDELTA };
int patch_kind(const char *name);
typedef void (*patch_cb)(uint64_t add, const char *cur);
int patch_apply(const char *patch, const char *src, const char *dst, volatile int *cancel, patch_cb cb, volatile uint64_t *total, char *err, int en);
int patch_create(const char *orig, const char *mod, const char *out, volatile int *cancel, patch_cb cb, volatile uint64_t *total, char *err, int en);

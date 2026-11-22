#pragma once
#include <stdint.h>

enum { ARC_NONE = 0, ARC_ZIP, ARC_RAR, ARC_7Z };
typedef void (*pt_cb_fn)(uint64_t add_bytes, const char *current);

int archive_kind(const char *name);
/* arc and dest must be native (lv2) paths; returns OPE_* */
int archive_extract(const char *arc, const char *dest, volatile int *cancel, pt_cb_fn cb, volatile uint64_t *total);

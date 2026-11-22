#pragma once
/* Read-only detection of PS3 PARAM.PFD v3/v4. Never modifies savedata. */
typedef struct {
    unsigned version;
    unsigned entries;
    unsigned reserved;
} pfd_probe_info_t;
/* Returns 1 for recognized format, 0 if absent, -1 for unreadable/unknown. */
int pfd_probe_save(const char *savedir, pfd_probe_info_t *out);

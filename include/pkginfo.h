#pragma once
#include <stdint.h>

typedef struct {
    int      valid;
    int      retail;            /* 1 = finalized/retail, 0 = debug */
    uint64_t total_size;
    uint32_t content_type;      /* metadata id 2 */
    uint32_t drm_type;          /* metadata id 1 */
    char     content_id[48];
    char     title[128];
    char     title_id[16];
    char     version[16];
    char     category[8];
    uint8_t *icon;              /* ICON0.PNG bytes (malloc'd) or NULL */
    uint32_t icon_size;
} pkg_info_t;

/* Reads header + metadata; if deep, also decrypts the item table to pull PARAM.SFO and ICON0.PNG. */
int  pkg_read_info(const char *path, pkg_info_t *info, int deep);
void pkg_free_info(pkg_info_t *info);
const char *pkg_type_label(uint32_t content_type);

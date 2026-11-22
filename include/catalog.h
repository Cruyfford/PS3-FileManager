#pragma once
#include <stdint.h>

/* ARCADIA catalog is user-supplied data, not a license or an installer.
 * The catalog is streamed and paged to avoid loading thousands of records. */
#define CAT_PAGE_SIZE 80
enum { CAT_ALL=0, CAT_PS3=1, CAT_PS2=2, CAT_PSP=3, CAT_PS1=4, CAT_APPS=5, CAT_THEMES=6, CAT_DLC=7, CAT_CATEGORY_COUNT=8 };
enum { CAT_DL_IDLE=0, CAT_DL_RUNNING=1, CAT_DL_DONE=2, CAT_DL_FAILED=3, CAT_DL_CANCELLED=4 };

typedef struct {
    char category[12], title[160], size[32], title_id[24], kind[24], url[256];
} catalog_item_t;

/* All paths use the VFS, including NTFS/exFAT USB drives. */
int catalog_set_source(const char *path);
int catalog_scan(int category, const char *query, int page);
int catalog_result_count(void);
int catalog_total_matches(void);
const catalog_item_t *catalog_result(int n);
const char *catalog_source(void);

/* Only recognized direct .pkg URLs may be downloaded.
 * No DRM, license bypass, RAP, automatic installation or page scraping. */
int catalog_is_direct_pkg(const catalog_item_t *item); /* supported .pkg or .p3t direct file */
int catalog_download_start(const catalog_item_t *item);
int catalog_download_state(void);
void catalog_download_progress(uint64_t *done, uint64_t *total);
const char *catalog_download_message(void);
const char *catalog_download_name(void);
void catalog_download_cancel(void);
void catalog_shutdown(void);

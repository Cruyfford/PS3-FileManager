#pragma once
#include <stdint.h>

/* ---- file search across drives (background) ---- */
#define SEARCH_MAX 600
void search_start(char roots[][64], int nroots, const char *query);
void search_cancel(void);
int  search_running(void);
int  search_count(void);
const char *search_path(int i);       /* full path */
int  search_is_dir(int i);
uint64_t search_size(int i);
const char *search_where(void);       /* folder being scanned */

/* ---- storage analyzer: sizes of a folder's children (background) ---- */
typedef struct { char name[256]; uint64_t size; uint32_t files; int is_dir; int done; } space_item_t;
void space_start(const char *dir);
void space_cancel(void);
int  space_running(void);
int  space_count(void);
space_item_t *space_item(int i);      /* sorted largest first once finished */
uint64_t space_total(void);

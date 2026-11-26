#pragma once
#include <stdint.h>
/* Background download manager: queue, resume (HTTP Range), retries, saved across restarts. */
enum { DL_QUEUED = 0, DL_RUNNING, DL_PAUSED, DL_DONE, DL_FAILED };
typedef struct { char url[512]; char path[1024]; char name[160]; volatile int state; volatile uint64_t done, total; volatile uint32_t speed; char err[96]; } dl_item_t;
#define DL_MAX 24
void dl_init(const char *state_file);
int  dl_add(const char *url, const char *dir);      /* index or -1 */
int  dl_count(void);
const dl_item_t *dl_get(int i);
void dl_pause(int i);
void dl_resume(int i);
void dl_remove(int i);                               /* also deletes an unfinished .part */
int  dl_active(void);                                /* 1 while something downloads */
void dl_shutdown(void);

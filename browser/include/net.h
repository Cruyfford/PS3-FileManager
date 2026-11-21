#pragma once
#include <stdint.h>
#include <stddef.h>

#define URL_MAX 2048

int  net_init(void);                 /* sysmodules, sockets, curl, shared cookies */
void net_shutdown(void);

/* ---- page fetch (one per tab) ---- */
enum { F_IDLE = 0, F_RUNNING, F_DONE, F_ERROR, F_DOWNLOAD, F_CANCELLED };

typedef struct {
    volatile int state;
    volatile int cancel;
    char url[URL_MAX];        /* requested, then effective (after redirects) */
    char *post;               /* urlencoded body or NULL (owned) */
    char ctype[128];
    char disposition[512];
    long code;
    char err[160];
    char *data; size_t len, cap;
    volatile double now, total;
    int thread_on;
    uint64_t thread;
} fetch_t;

int  fetch_start(fetch_t *f, const char *url, const char *post);
void fetch_wait(fetch_t *f);          /* joins the worker; safe to call when idle */
void fetch_reset(fetch_t *f);         /* frees data */

/* ---- images (one worker) ---- */
void img_request(uint32_t doc_id, int index, const char *url);
/* returns 1 and fills when a result is ready; tex may be NULL on failure */
int  img_poll(uint32_t *doc_id, int *index, void **tex);
void img_cancel_all(void);

/* ---- downloads ---- */
#define DL_MAX 16
enum { DL_FREE = 0, DL_RUNNING, DL_DONE, DL_FAILED, DL_CANCELLED };
typedef struct {
    volatile int state;
    volatile int cancel;
    char url[URL_MAX];
    char path[1024];          /* destination file */
    char name[256];
    volatile double now, total, speed;
    char err[160];
} download_t;

int  dl_start(const char *url, const char *dest_path, const char *name);   /* index or -1 */
download_t *dl_get(int i);
void dl_cancel(int i);
void dl_remove(int i);
int  dl_active_count(void);

/* ---- url helpers ---- */
void url_join(const char *base, const char *rel, char *out, int n);
void url_encode(const char *in, char *out, int n);
int  url_is_https(const char *u);
void url_host(const char *u, char *out, int n);
/* file name for a download: Content-Disposition filename or the last path segment */
void url_filename(const char *url, const char *disposition, char *out, int n);

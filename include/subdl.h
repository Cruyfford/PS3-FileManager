#pragma once
/* Online subtitle search + download for the video player.
 * Sources: SubDL (needs a free API key from subdl.com, saved in subdl_key.txt) and Podnapisi (no key). */

enum { SUBDL_IDLE = 0, SUBDL_SEARCHING, SUBDL_RESULTS, SUBDL_DOWNLOADING, SUBDL_READY, SUBDL_ERROR };

typedef struct { char title[200]; char url[400]; char lang[4]; int src; } subres_t;

void subdl_load_key(const char *path);
void subdl_save_key(const char *path, const char *key);
const char *subdl_key(void);
/* video file name (for title / season / episode) */
void subdl_search(const char *video_name);
void subdl_download(int i, const char *out_path_noext);   /* writes out_path_noext.<lang>.srt (or .ass) */
int  subdl_state(void);
int  subdl_count(void);
const subres_t *subdl_get(int i);
const char *subdl_result(void);      /* path of the downloaded file */
const char *subdl_error(void);
const char *subdl_query(void);
void subdl_ack(void);                /* READY/ERROR seen by the UI */

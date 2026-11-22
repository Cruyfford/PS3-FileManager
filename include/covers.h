#pragma once

enum { CV_NONE = 0, CV_CACHED = 1, CV_PENDING = 2, CV_MISSING = 3 };

void covers_init(const char *cache_dir);
void covers_shutdown(void);
void covers_enable(int on);

/* Cheap check by name only: could this entry be a game that has a cover? */
int  covers_candidate(const char *name, int is_dir);

/* Called from the loader thread (may read the ISO header). Fills out_path with a local
   image (cache file or the game's own ICON0.PNG) and returns CV_CACHED; CV_PENDING when a
   download was queued; CV_MISSING / CV_NONE otherwise. */
int  covers_lookup(const char *full_path, const char *name, int is_dir, char *out_path, int n);

/* increments whenever a queued download finishes */
int  covers_generation(void);
void covers_stop_async(void);
/* cleaned search title from a video file name ("The.Matrix.1999.1080p.mkv" -> "The Matrix 1999") */
void covers_video_query(const char *name, char *out, int n);

/* Catalog covers from title IDs, cached locally or fetched asynchronously. */
int covers_catalog_lookup(const char *title_id, char *out_path, int n);

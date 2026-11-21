#pragma once
#include <stdint.h>

enum { VID_IDLE = 0, VID_PLAYING, VID_PAUSED, VID_ENDED, VID_ERROR };
enum { VERR_NONE = 0, VERR_OPEN, VERR_FORMAT, VERR_CODEC, VERR_DECODER, VERR_MEM };

int  video_supported(const char *name);       /* MP4 / M4V / MOV */
int  video_open(const char *path);            /* 1 = playing; else see video_error() */
void video_close(void);
void video_toggle_pause(void);
void video_seek(int delta_seconds);
int  video_state(void);
int  video_error(void);
double video_position(void);
double video_duration(void);
int  video_size(int *w, int *h);
/* call once per frame: fetches due pictures and draws the current frame fitted into the rect */
void video_render(float x, float y, float w, float h);

#pragma once
#include <stdint.h>

enum { VID_IDLE = 0, VID_PLAYING, VID_PAUSED, VID_ENDED, VID_ERROR };
enum { VERR_NONE = 0, VERR_OPEN, VERR_FORMAT, VERR_CODEC, VERR_DECODER, VERR_MEM };

int  video_supported(const char *name);       /* MP4 MOV M4V MKV WEBM AVI DIVX TS M2TS MTS MPG MPEG VOB */
int  video_open(const char *path);            /* 1 = playing; else see video_error() */
void video_close(void);
void video_toggle_pause(void);
void video_seek(int delta_seconds);
int  video_state(void);
int  video_error(void);
const char *video_error_detail(void);         /* e.g. codec name that isn't supported */
const char *video_audio_warning(void);        /* non-empty when the audio track can't be decoded */
double video_position(void);
double video_duration(void);
int  video_size(int *w, int *h);
const char *video_format_info(void);          /* "MKV · H.264 · AAC" */
/* call once per frame: fetches due pictures and draws the current frame fitted into the rect */
void video_render(float x, float y, float w, float h);

/* subtitles and audio tracks */
const char *video_subtitle_text(void);        /* "" when nothing to show */
int  video_cycle_subtitle(char *label, int n);    /* switches to the next option, writes its label */
int  video_cycle_audio(char *label, int n);       /* 0 = only one track */
void video_sub_delay_add(double d);
double video_sub_delay(void);
const char *video_debug_info(void);

#pragma once
#include <stdint.h>

enum { AUD_STOPPED = 0, AUD_PLAYING, AUD_PAUSED, AUD_ENDED, AUD_ERROR };

int  audio_play(const char *path);     /* MP3, OGG, WAV — from any file system */
void audio_stop(void);
void audio_toggle_pause(void);
int  audio_state(void);
uint32_t audio_position_ms(void);
uint32_t audio_duration_ms(void);      /* 0 if unknown */
void audio_levels(float out[8]);       /* rough spectrum-like bars for the UI */
int  audio_is_supported(const char *name);

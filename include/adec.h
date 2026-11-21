#pragma once
#include <stdint.h>
#include "demux.h"

/* Audio decoders for the player. Output is always 16-bit interleaved, mono or stereo (multichannel is downmixed). */
typedef void (*pcm_sink_t)(const int16_t *pcm, int frames, int channels, int rate, void *user);

typedef struct adec adec_t;
adec_t *adec_open(const stream_t *s);           /* NULL = unsupported */
void    adec_decode(adec_t *a, const uint8_t *data, int size, pcm_sink_t sink, void *user);
void    adec_reset(adec_t *a);                   /* after a seek */
void    adec_close(adec_t *a);

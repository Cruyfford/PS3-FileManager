#pragma once
#include <stdint.h>
#include "vfs.h"

/* Container demuxers for the built-in player: MP4/MOV, MKV/WebM, AVI, MPEG-TS/M2TS, MPEG-PS/VOB */

#define MAX_STREAMS 24
enum { ST_VIDEO = 1, ST_AUDIO, ST_SUB };
enum { VC_NONE = 0, VC_H264, VC_MPEG2, VC_MPEG4, VC_HEVC, VC_VP9, VC_AV1, VC_OTHER };
enum { AC_NONE = 0, AC_AAC, AC_MP3, AC_AC3, AC_EAC3, AC_VORBIS, AC_PCM_S16LE, AC_PCM_S16BE, AC_PCM_U8, AC_PCM_S24LE, AC_DTS, AC_OPUS, AC_FLAC, AC_TRUEHD, AC_LPCM, AC_OTHER };
enum { SC_NONE = 0, SC_SRT, SC_ASS, SC_OTHER };

typedef struct {
    int type, codec;
    uint8_t *extra; int extra_len;
    int w, h;
    double fps;              /* 0 = unknown */
    int rate, channels, bits, block_align;
    int nal_len;             /* H.264: NAL length size for length-prefixed samples (0 = Annex B) */
    int es;                  /* 1 = raw elementary stream (packets don't align to frames) */
    char lang[8], name[64];
    int id;                  /* container track id */
} stream_t;

typedef struct {
    int stream;
    uint8_t *data; int size;  /* valid until the next read */
    double pts;               /* seconds from start of file, < 0 = unknown */
    int key;                  /* keyframe flag (when the container knows) */
} packet_t;

/* buffered reader over vfs (all demuxers use it) */
typedef struct {
    vfs_file_t *f;
    uint64_t size, pos;       /* logical position */
    uint8_t *buf; uint32_t blen, boff; uint64_t bstart;
} bio_t;

int      bio_open(bio_t *b, const char *path);
void     bio_close(bio_t *b);
int      bio_read(bio_t *b, void *dst, uint32_t n);    /* returns bytes read */
int      bio_seek(bio_t *b, uint64_t pos);
int      bio_skip(bio_t *b, uint64_t n);
int      bio_getc(bio_t *b);                           /* -1 at EOF */
uint32_t bio_r8(bio_t *b);
uint32_t bio_rb16(bio_t *b), bio_rb24(bio_t *b), bio_rb32(bio_t *b), bio_rl16(bio_t *b), bio_rl32(bio_t *b);
uint64_t bio_rb64(bio_t *b);
static inline uint64_t bio_tell(bio_t *b) { return b->pos; }
static inline int bio_eof(bio_t *b) { return b->pos >= b->size; }

typedef struct demux demux_t;
struct demux {
    const char *fmt;
    stream_t st[MAX_STREAMS]; int nst;
    double duration;
    int frame_count_ts;       /* 1 = video packets carry no usable pts: count frames at stream fps */
    bio_t io;
    void *priv;
    uint8_t *pbuf; int pcap;  /* packet buffer helper */
    int (*read)(demux_t *, packet_t *);      /* 1 packet, 0 EOF, -1 error */
    int (*seek)(demux_t *, double t);        /* position at a keyframe at or before t; returns 1 on success */
    void (*close)(demux_t *);
};

demux_t *demux_open(const char *path);
void     demux_close(demux_t *d);
uint8_t *demux_buf(demux_t *d, int need);   /* grows d->pbuf */

/* per-container openers (return 1 on success) */
int demux_mp4_open(demux_t *d, const char *path);
int demux_mkv_open(demux_t *d);
int demux_avi_open(demux_t *d);
int demux_ts_open(demux_t *d);
int demux_ps_open(demux_t *d);

const char *codec_name(const stream_t *s);
int  video_codec_playable(int vc);
int  audio_codec_playable(int ac);

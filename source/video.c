/* Built-in video player.
 * Containers: MP4/MOV, MKV/WebM, AVI, TS/M2TS, VOB/MPG (demux*.c)
 * Video: H.264, MPEG-2, MPEG-4 ASP (DivX/Xvid) decoded by the firmware vdec on SPU
 * Audio: AAC, MP3, AC3, Vorbis, PCM (adec.c) -> 48 kHz stereo libaudio port; audio drives the clock
 * Subtitles: external SRT/ASS/SSA/VTT and embedded MKV text tracks (subs.c) */
#include "video.h"
#include "demux.h"
#include "adec.h"
#include "subs.h"
#include "vfs.h"
#include "gfx.h"
#include "audio.h"
#include <codec/vdec.h>
#include <ppu-asm.h>
#include <audio/audio.h>
#include <sysmodule/sysmodule.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <math.h>

#define OUT_RATE   48000
#define RING_FR    (OUT_RATE * 2)
#define AU_SLOTS   10
#define AU_MAX     (2 * 1024 * 1024)

#define VDEC_CODEC_TYPE_DIVX 5
#define VDEC_DIVX_HD_1080    14

static volatile int st = VID_IDLE, verr, stop_demux, demux_done, paused;
static char err_detail[64], audio_warn[96], fmt_info[96];
static sys_ppu_thread_t th_demux, th_audio;
static int th_demux_on, th_audio_on;

static char cur_path[1024];
static demux_t *dmx;
static int vsi = -1, asi = -1;
static stream_t *vs;
static adec_t *ad;
static double duration;

/* subtitles */
typedef struct { int kind; int stream; char path[1024]; char label[96]; } subopt_t;   /* kind 0 off, 1 embedded, 2 external */
static subopt_t subopts[24];
static int nsubopts, cursub;
static sublist_t subs;
static sys_mutex_t sub_mtx; static int sub_mtx_ok;
static double sub_delay;

/* vdec */
static uint32_t vh;
static void *vmem;
static uint8_t *au_buf[AU_SLOTS];
static volatile uint32_t au_sent, au_done, pics_ready;
static volatile int seq_done;
static int need_headers;
static uint8_t sps_pps[1024]; static int sps_pps_len;

/* timing */
static volatile double audio_base; static volatile int audio_started; static volatile int audio_port_ok;
static double wall_base; static uint64_t wall_t0, wall_pause_acc, wall_pause_start; static int wall_started;
static double frame_dur = 1.0 / 25;
static double last_pic_pts = -1;
static int64_t out_count;
static double seq_base;        /* frame_count_ts: time of first picture after start/seek */
static volatile double video_start_pts;

/* audio ring */
static float *ring;
static volatile uint32_t ring_w, ring_r;
static volatile uint64_t played;

/* frame upload */
static texture_t tex[2];
static int tex_w, tex_h, cur_tex = -1, frame_w, frame_h;
static uint8_t *frame; static int frame_cap;

/* ES splitter (raw MPEG-2 / H.264 / MPEG-4 streams from TS/PS) */
typedef struct { int off; double pts; } ptsmark_t;
static uint8_t *es_buf; static int es_len, es_cap, es_scan;
static int es_seen_pic, es_seen_slice;
static ptsmark_t es_pts[64]; static int es_npts;

static uint64_t now_us(void) { u64 s, ns; sysGetCurrentTime(&s, &ns); return s * 1000000ULL + ns / 1000; }

int video_supported(const char *n)
{
    const char *e = strrchr(n, '.');
    if (!e) return 0;
    static const char *ext[] = { ".mp4", ".m4v", ".mov", ".3gp", ".mkv", ".webm", ".avi", ".divx", ".xvid", ".ts", ".m2ts", ".mts", ".tp", ".trp", ".mpg", ".mpeg", ".vob", ".m2v", NULL };
    for (int i = 0; ext[i]; i++) if (!strcasecmp(e, ext[i])) return 1;
    return 0;
}

int video_state(void) { return st; }
int video_error(void) { return verr; }
const char *video_error_detail(void) { return err_detail; }
const char *video_audio_warning(void) { return audio_warn; }
const char *video_format_info(void) { return fmt_info; }

/* small diagnostics line shown in the player overlay (helps when testing on the console) */
const char *video_debug_info(void)
{
    static char b[160];
    snprintf(b, sizeof(b), "au %u/%u  pic %u  audio %s%s  ring %u  frame %dx%d  done %d",
             (unsigned)au_done, (unsigned)au_sent, (unsigned)pics_ready, asi < 0 ? "none" : audio_started ? "on" : "wait",
             asi >= 0 && !audio_port_ok ? " (port off)" : "", (unsigned)(ring ? ring_used() : 0), frame_w, frame_h, demux_done);
    return b;
}
double video_duration(void) { return duration; }
int video_size(int *w, int *h) { *w = frame_w; *h = frame_h; return frame_w > 0; }
double video_sub_delay(void) { return sub_delay; }
void video_sub_delay_add(double d) { sub_delay += d; if (fabs(sub_delay) < 0.001) sub_delay = 0; }

static void lock_subs(void) { if (sub_mtx_ok) sysMutexLock(sub_mtx, 0); }
static void unlock_subs(void) { if (sub_mtx_ok) sysMutexUnlock(sub_mtx); }

double video_position(void)
{
    if (asi >= 0 && audio_started && audio_port_ok) {
        double t = audio_base + (double)played / OUT_RATE - (8.0 * AUDIO_BLOCK_SAMPLES) / OUT_RATE;
        return t < audio_base ? audio_base : t;
    }
    if (!wall_started) return video_start_pts > 0 ? video_start_pts : 0;
    uint64_t n = paused ? wall_pause_start : now_us();
    return wall_base + (double)(n - wall_t0 - wall_pause_acc) / 1e6;
}

/* ======================= vdec ======================= */

static u32 vdec_cb(u32 handle, u32 msg, u32 data, u32 arg)
{
    (void)handle; (void)data; (void)arg;
    if (msg == VDEC_CALLBACK_AUDONE) au_done++;
    else if (msg == VDEC_CALLBACK_PICOUT) pics_ready++;
    else if (msg == VDEC_CALLBACK_SEQDONE) seq_done = 1;
    return 0;
}

static int vdec_start(int codec)
{
    vdecType type;
    sysModuleLoad(SYSMODULE_VDEC);
    if (codec == VC_H264) { sysModuleLoad(SYSMODULE_VDEC_H264); type.codec_type = VDEC_CODEC_TYPE_H264; type.profile_level = 42; }
    else if (codec == VC_MPEG2) { sysModuleLoad(SYSMODULE_VDEC_MPEG2); type.codec_type = VDEC_CODEC_TYPE_MPEG2; type.profile_level = VDEC_MPEG2_MP_HL; }
    else if (codec == VC_MPEG4) { sysModuleLoad(SYSMODULE_VDEC_DIVX); type.codec_type = VDEC_CODEC_TYPE_DIVX; type.profile_level = VDEC_DIVX_HD_1080; }
    else return 0;
    vdecAttr attr;
    if (vdecQueryAttr(&type, &attr) != 0) return 0;
    vmem = memalign(1024 * 1024, attr.mem_size);
    if (!vmem) return 0;
    vdecConfig cfg = { (u32)(u64)vmem, attr.mem_size, 1000, 0x8000, 250, 1 };
    /* the decoder PRX calls back through a 32-bit function descriptor */
    vdecClosure cl = { (u32)__get_opd32(vdec_cb), 0 };
    if (vdecOpen(&type, &cfg, &cl, &vh) != 0) { vh = 0; return 0; }
    if (vdecStartSequence(vh) != 0) return 0;
    for (int i = 0; i < AU_SLOTS; i++) if (!(au_buf[i] = (uint8_t *)memalign(128, AU_MAX))) return 0;
    frame_cap = 1920 * 1088 * 4;
    if (!(frame = (uint8_t *)memalign(128, frame_cap))) { frame_cap = 0; return 0; }
    return 1;
}

static void build_sps_pps(void)
{
    sps_pps_len = 0;
    if (!vs || vs->codec != VC_H264 || !vs->nal_len || vs->extra_len < 7) return;
    const uint8_t *c = vs->extra; int cl = vs->extra_len;
    int o = 6, nsps = c[5] & 31;
    for (int k = 0; k < 2; k++) {
        int cnt = k == 0 ? nsps : (o < cl ? c[o++] : 0);
        for (int i = 0; i < cnt && o + 2 <= cl; i++) {
            int n = (c[o] << 8) | c[o + 1]; o += 2;
            if (o + n > cl) return;
            if (sps_pps_len + n + 4 <= (int)sizeof(sps_pps)) { memcpy(sps_pps + sps_pps_len, "\0\0\0\1", 4); memcpy(sps_pps + sps_pps_len + 4, c + o, n); sps_pps_len += n + 4; }
            o += n;
        }
    }
}

/* key detection on an access unit: Annex B (nal_len 0) or length-prefixed */
static int au_is_key(int codec, const uint8_t *b, int n, int nal_len)
{
    if (codec == VC_H264) {
        if (nal_len) {
            for (int k = 0; k + nal_len <= n;) {
                uint32_t l = 0;
                for (int i = 0; i < nal_len; i++) l = (l << 8) | b[k + i];
                k += nal_len;
                if (k >= n) break;
                if ((b[k] & 31) == 5) return 1;
                k += l;
            }
            return 0;
        }
        for (int i = 0; i + 3 < n; i++) if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) { int t = b[i + 3] & 31; if (t == 5) return 1; }
        return 0;
    }
    if (codec == VC_MPEG2) {
        for (int i = 0; i + 5 < n; i++) if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (b[i + 3] == 0xB3) return 1;
            if (b[i + 3] == 0x00) return ((b[i + 5] >> 3) & 7) == 1;
        }
        return 0;
    }
    if (codec == VC_MPEG4) {
        for (int i = 0; i + 4 < n; i++) if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 0xB6) return (b[i + 4] >> 6) == 0;
        return 0;
    }
    return 1;
}

static int submit_au(const uint8_t *data, int n, double pts)
{
    while (!stop_demux && (au_sent - au_done >= AU_SLOTS - 1 || pics_ready > 6)) usleep(2000);
    if (stop_demux) return 0;
    uint8_t *slot = au_buf[au_sent % AU_SLOTS];
    int o = 0;
    if (vs->codec == VC_H264 && vs->nal_len) {
        int idr = au_is_key(VC_H264, data, n, vs->nal_len);
        if ((idr || need_headers) && sps_pps_len) { memcpy(slot, sps_pps, sps_pps_len); o = sps_pps_len; }
        for (int i = 0; i + vs->nal_len <= n;) {
            uint32_t l = 0;
            for (int b = 0; b < vs->nal_len; b++) l = (l << 8) | data[i + b];
            i += vs->nal_len;
            if (i + (int)l > n || o + 4 + (int)l > AU_MAX) break;
            memcpy(slot + o, "\0\0\0\1", 4); o += 4;
            memcpy(slot + o, data + i, l); o += l;
            i += l;
        }
    } else {
        if (need_headers && vs->extra_len && vs->codec != VC_H264 && vs->extra_len < 4096) { memcpy(slot, vs->extra, vs->extra_len); o = vs->extra_len; }
        if (o + n > AU_MAX) n = AU_MAX - o;
        memcpy(slot + o, data, n); o += n;
    }
    need_headers = 0;
    if (o <= 0) return 0;
    vdecAU au;
    memset(&au, 0, sizeof(au));
    au.packet_addr = (u32)(u64)slot; au.packet_size = o;
    if (pts >= 0 && !dmx->frame_count_ts) { uint64_t p90 = (uint64_t)(pts * 90000.0); au.pts.low = (u32)p90; au.pts.hi = (u32)(p90 >> 32); }
    else { au.pts.low = VDEC_TS_INVALID; au.pts.hi = VDEC_TS_INVALID; }
    au.dts.low = VDEC_TS_INVALID; au.dts.hi = VDEC_TS_INVALID;
    s32 r;
    while ((r = vdecDecodeAu(vh, VDEC_DECODER_MODE_NORMAL, &au)) == (s32)VDEC_ERROR_BUSY && !stop_demux) usleep(2000);
    if (r == 0) au_sent++;
    return r == 0;
}

/* end the decoding sequence: release every pending picture and wait (bounded) for SEQDONE */
static void vdec_end_and_drain(void)
{
    if (!vh) return;
    seq_done = 0;
    vdecEndSequence(vh);
    vdecPictureFormat f = { VDEC_PICFMT_ARGB32, VDEC_COLOR_MATRIX_BT709, 0xFF };
    for (int i = 0; i < 400 && !seq_done; i++) {          /* up to ~2 s */
        u32 a;
        while (vdecGetPicItem(vh, &a) == 0 && a) { if (frame) vdecGetPicture(vh, &f, frame); else break; }
        usleep(5000);
    }
    u32 a;
    while (vdecGetPicItem(vh, &a) == 0 && a && frame) vdecGetPicture(vh, &f, frame);
    pics_ready = 0;
}

/* ======================= ES splitter ======================= */

static void es_reset(void) { es_len = es_scan = 0; es_seen_pic = es_seen_slice = 0; es_npts = 0; }

static void es_emit(int end, int *waiting_key, double *first_key_pts)
{
    double pts = -1;
    for (int i = 0; i < es_npts; i++) if (es_pts[i].off < end) { pts = es_pts[i].pts; break; }
    /* drop marks consumed by this AU */
    int k = 0;
    for (int i = 0; i < es_npts; i++) if (es_pts[i].off >= end) { es_pts[k] = es_pts[i]; es_pts[k].off -= end; k++; }
    es_npts = k;
    static int waited;
    if (!*waiting_key) waited = 0;
    /* broadcast H.264 may never send IDR pictures: give up waiting after a few seconds of frames */
    if (*waiting_key && (au_is_key(vs->codec, es_buf, end, 0) || ++waited > 150)) { *waiting_key = 0; waited = 0; if (pts >= 0) *first_key_pts = pts; }
    if (!*waiting_key) submit_au(es_buf, end, pts);
    memmove(es_buf, es_buf + end, es_len - end);
    es_len -= end;
    es_scan = 0;
    es_seen_pic = es_seen_slice = 0;
}

static void es_feed(const uint8_t *d, int n, double pts, int *waiting_key, double *first_key_pts)
{
    if (es_len + n > es_cap) {
        int nc = (es_len + n) * 2;
        if (nc > 8 * 1024 * 1024) { es_reset(); if (n > 4 * 1024 * 1024) return; nc = n * 2 + 65536; }
        uint8_t *x = (uint8_t *)realloc(es_buf, nc);
        if (!x) return;
        es_buf = x; es_cap = nc;
    }
    if (pts >= 0 && es_npts < 64) { es_pts[es_npts].off = es_len; es_pts[es_npts].pts = pts; es_npts++; }
    memcpy(es_buf + es_len, d, n);
    es_len += n;
    int codec = vs->codec;
    while (es_scan + 4 < es_len && !stop_demux) {
        int i = es_scan;
        if (!(es_buf[i] == 0 && es_buf[i + 1] == 0 && es_buf[i + 2] == 1)) { es_scan++; continue; }
        int sc = es_buf[i + 3];
        int boundary = 0;
        int start = (i > 0 && es_buf[i - 1] == 0) ? i - 1 : i;
        if (codec == VC_MPEG2) {
            if (sc == 0xB3 && i + 7 < es_len) { int w = (es_buf[i + 4] << 4) | (es_buf[i + 5] >> 4), h = ((es_buf[i + 5] & 15) << 8) | es_buf[i + 6]; if (w && h) { vs->w = w; vs->h = h; } }
            if ((sc == 0xB3 || sc == 0xB8 || sc == 0x00) && es_seen_pic && es_seen_slice) boundary = 1;
            if (sc == 0x00) es_seen_pic = 1;
            if (sc >= 0x01 && sc <= 0xAF && es_seen_pic) es_seen_slice = 1;
        } else if (codec == VC_H264) {
            int t = sc & 31;
            if (t == 9 || t == 7 || t == 8 || t == 6) { if (es_seen_slice) boundary = 1; }
            else if (t == 1 || t == 5) {
                if (i + 4 >= es_len) break;
                int first_mb_zero = (es_buf[i + 4] & 0x80) != 0;
                if (es_seen_slice && first_mb_zero) boundary = 1;
                es_seen_slice = 1;
            }
            if (boundary) es_seen_slice = 0;
            if ((t == 1 || t == 5) && boundary) es_seen_slice = 1;
        } else if (codec == VC_MPEG4) {
            if ((sc == 0xB6 || sc == 0xB3 || sc == 0xB0 || sc <= 0x2F) && es_seen_pic) boundary = 1;
            if (sc == 0xB6) es_seen_pic = 1;
        }
        if (boundary && start > 0) {
            int keep_slice = (codec == VC_H264) && es_seen_slice;
            es_emit(start, waiting_key, first_key_pts);
            if (codec == VC_MPEG2 && sc == 0x00) es_seen_pic = 1;
            if (codec == VC_MPEG4 && sc == 0xB6) es_seen_pic = 1;
            if (keep_slice) es_seen_slice = 1;
            es_scan = 4;
            continue;
        }
        es_scan = i + 4;
    }
}

/* ======================= audio ======================= */

static uint32_t ring_used(void) { return ring_w - ring_r; }

static double rs_frac; static int16_t rs_last_l, rs_last_r;

static void push_pcm(const int16_t *pcm, int frames, int ch, int rate, void *user)
{
    (void)user;
    if (rate <= 0) rate = 48000;
    double step = (double)rate / OUT_RATE;
    int i = 0;
    while (i < frames && !stop_demux) {
        while (ring_used() >= RING_FR - 4 && !stop_demux) usleep(2000);
        int16_t l = pcm[i * ch], r = ch > 1 ? pcm[i * ch + 1] : l;
        float t = (float)rs_frac;
        uint32_t w = ring_w % RING_FR;
        ring[w * 2] = (rs_last_l * (1 - t) + l * t) / 32768.0f;
        ring[w * 2 + 1] = (rs_last_r * (1 - t) + r * t) / 32768.0f;
        ring_w++;
        rs_frac += step;
        while (rs_frac >= 1.0) { rs_frac -= 1.0; rs_last_l = l; rs_last_r = r; i++; if (i >= frames) break; l = pcm[i * ch]; r = ch > 1 ? pcm[i * ch + 1] : l; }
    }
}

static void audio_thread(void *arg)
{
    (void)arg;
    if (!audio_sys_init()) { sysThreadExit(0); return; }
    audioPortParam prm = { AUDIO_PORT_2CH, AUDIO_BLOCK_8, AUDIO_PORT_INITLEVEL, 1.0f };
    audioPortConfig cfg;
    u32 port;
    if (audioPortOpen(&prm, &port) != 0) { audio_port_ok = 0; sysThreadExit(0); return; }
    audioGetPortConfig(port, &cfg);
    audioPortStart(port);
    volatile u64 *ri = (volatile u64 *)(u64)cfg.readIndex;
    float *base = (float *)(u64)cfg.audioDataStart;
    u32 next = 1;
    audio_port_ok = 1;
    while (!stop_demux) {
        if (next == (u32)*ri) { usleep(1000); continue; }
        float *blk = base + 2 * AUDIO_BLOCK_SAMPLES * next;
        if (paused || ring_used() < AUDIO_BLOCK_SAMPLES || !audio_started) memset(blk, 0, sizeof(float) * 2 * AUDIO_BLOCK_SAMPLES);
        else {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { uint32_t r = ring_r % RING_FR; blk[i * 2] = ring[r * 2]; blk[i * 2 + 1] = ring[r * 2 + 1]; ring_r++; }
            played += AUDIO_BLOCK_SAMPLES;
        }
        next = (next + 1) % AUDIO_BLOCK_8;
    }
    audioPortStop(port);
    audioPortClose(port);
    audio_port_ok = 0;
    sysThreadExit(0);
}

/* ======================= demux thread ======================= */

static void demux_thread(void *arg)
{
    (void)arg;
    int waiting_key = 1;
    double first_key_pts = -1;
    int video_aus = 0;
    double audio_gate = -1;     /* drop audio before the first video keyframe time */
    int sub_stream = (cursub < nsubopts && subopts[cursub].kind == 1) ? subopts[cursub].stream : -1;
    packet_t pk;
    while (!stop_demux) {
        int r = dmx->read(dmx, &pk);
        if (r <= 0) break;
        if (pk.stream == vsi) {
            if (vs->es) { es_feed(pk.data, pk.size, pk.pts, &waiting_key, &first_key_pts); }
            else {
                if (waiting_key) {
                    int key = pk.key >= 0 ? pk.key : au_is_key(vs->codec, pk.data, pk.size, vs->nal_len);
                    if (!key && ++video_aus < 300) continue;
                    waiting_key = 0;
                    first_key_pts = pk.pts;
                }
                submit_au(pk.data, pk.size, pk.pts);
            }
            if (!waiting_key && first_key_pts >= 0 && audio_gate < 0) { audio_gate = first_key_pts; video_start_pts = first_key_pts; }
        } else if (pk.stream == asi && ad) {
            if (waiting_key && vsi >= 0) continue;
            if (audio_gate >= 0 && pk.pts >= 0 && pk.pts < audio_gate - 0.03) continue;
            if (!audio_started) {
                audio_base = pk.pts >= 0 ? pk.pts : (first_key_pts >= 0 ? first_key_pts : 0);
                audio_started = 1;
            }
            if (th_audio_on && !audio_port_ok && audio_started && ring_used() > RING_FR / 2) continue;   /* port failed: drop audio */
            adec_decode(ad, pk.data, pk.size, push_pcm, NULL);
        } else if (pk.stream == sub_stream && sub_stream >= 0) {
            stream_t *s = &dmx->st[pk.stream];
            if (s->codec == SC_SRT || s->codec == SC_ASS) {
                lock_subs();
                subs_add_packet(&subs, pk.pts, 0, (const char *)pk.data, pk.size, s->codec == SC_ASS);
                unlock_subs();
            }
        }
    }
    if (vs && vs->es && es_len && !stop_demux) es_emit(es_len, &waiting_key, &first_key_pts);
    demux_done = 1;
    sysThreadExit(0);
}

static void start_threads(void)
{
    stop_demux = 0; demux_done = 0;
    if (sysThreadCreate(&th_demux, demux_thread, NULL, 1100, 0x40000, THREAD_JOINABLE, "vdemux") == 0) th_demux_on = 1;
    if (asi >= 0 && sysThreadCreate(&th_audio, audio_thread, NULL, 900, 0x10000, THREAD_JOINABLE, "vaudio") == 0) th_audio_on = 1;
}

static void stop_threads(void)
{
    stop_demux = 1;
    u64 rv;
    if (th_demux_on) { sysThreadJoin(th_demux, &rv); th_demux_on = 0; }
    if (th_audio_on) { sysThreadJoin(th_audio, &rv); th_audio_on = 0; }
}

static void reset_pipeline_state(void)
{
    au_sent = au_done = pics_ready = 0;
    ring_w = ring_r = 0; played = 0;
    audio_started = 0; audio_base = 0;
    wall_started = 0; wall_pause_acc = 0;
    last_pic_pts = -1; out_count = 0; seq_base = -1;
    video_start_pts = -1;
    need_headers = 1;
    rs_frac = 0; rs_last_l = rs_last_r = 0;
    es_reset();
}

/* ======================= tracks ======================= */

static int is_arabic_lang(const char *l, const char *name)
{
    return !strcasecmp(l, "ara") || !strcasecmp(l, "ar") || strcasestr(name, "arab") || strstr(name, "\xD8\xB9\xD8\xB1\xD8\xA8");
}

static void build_sub_options(void)
{
    nsubopts = 0;
    subopts[nsubopts].kind = 0; snprintf(subopts[nsubopts].label, 96, "Off"); nsubopts++;
    char ext[12][1024];
    int ne = subs_find_external(cur_path, ext, 12);
    for (int i = 0; i < ne && nsubopts < 24; i++) {
        subopts[nsubopts].kind = 2;
        snprintf(subopts[nsubopts].path, 1024, "%s", ext[i]);
        const char *b = strrchr(ext[i], '/');
        snprintf(subopts[nsubopts].label, 96, "%s", b ? b + 1 : ext[i]);
        nsubopts++;
    }
    for (int i = 0; i < dmx->nst && nsubopts < 24; i++) {
        stream_t *s = &dmx->st[i];
        if (s->type != ST_SUB || (s->codec != SC_SRT && s->codec != SC_ASS)) continue;
        subopts[nsubopts].kind = 1; subopts[nsubopts].stream = i;
        snprintf(subopts[nsubopts].label, 96, "%s%s%s", s->name[0] ? s->name : "Track", s->lang[0] ? " · " : "", s->lang);
        nsubopts++;
    }
}

static int pick_default_sub(void)
{
    char base[512];
    const char *b = strrchr(cur_path, '/');
    snprintf(base, sizeof(base), "%s", b ? b + 1 : cur_path);
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
    for (int i = 1; i < nsubopts; i++) if (subopts[i].kind == 2 && !strncasecmp(subopts[i].label, base, strlen(base))) return i;
    for (int i = 1; i < nsubopts; i++) if (subopts[i].kind == 1 && is_arabic_lang(dmx->st[subopts[i].stream].lang, dmx->st[subopts[i].stream].name)) return i;
    for (int i = 1; i < nsubopts; i++) if (subopts[i].kind == 1) return i;
    return 0;
}

static void apply_sub(int i)
{
    lock_subs();
    subs_free(&subs);
    if (i > 0 && i < nsubopts && subopts[i].kind == 2) subs_load_file(subopts[i].path, &subs);
    unlock_subs();
    cursub = i;
}

const char *video_subtitle_text(void)
{
    static char out[2048];
    out[0] = 0;
    if (cursub <= 0) return out;
    lock_subs();
    const char *t = subs_at(&subs, video_position() - sub_delay);
    snprintf(out, sizeof(out), "%s", t);
    unlock_subs();
    return out;
}

static void set_fmt_info(void)
{
    char a[48] = "";
    if (asi >= 0) snprintf(a, sizeof(a), " · %s", codec_name(&dmx->st[asi]));
    snprintf(fmt_info, sizeof(fmt_info), "%s · %s%s", dmx->fmt, codec_name(vs), a);
}

/* ======================= public ======================= */

int video_open(const char *path)
{
    video_close();
    verr = VERR_NONE; err_detail[0] = audio_warn[0] = fmt_info[0] = 0;
    sub_delay = 0;
    snprintf(cur_path, sizeof(cur_path), "%s", path);
    if (!sub_mtx_ok) {
        sys_mutex_attr_t at; memset(&at, 0, sizeof(at));
        at.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO; at.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
        at.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED; at.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
        if (sysMutexCreate(&sub_mtx, &at) == 0) sub_mtx_ok = 1;
    }
    dmx = demux_open(path);
    if (!dmx) { verr = vfs_exists(path) ? VERR_FORMAT : VERR_OPEN; st = VID_ERROR; return 0; }
    vsi = asi = -1;
    int unsup = -1;
    for (int i = 0; i < dmx->nst; i++) if (dmx->st[i].type == ST_VIDEO) {
        if (video_codec_playable(dmx->st[i].codec)) { vsi = i; break; }
        if (unsup < 0) unsup = i;
    }
    if (vsi < 0) {
        verr = VERR_CODEC;
        snprintf(err_detail, sizeof(err_detail), "%s", unsup >= 0 ? codec_name(&dmx->st[unsup]) : "?");
        video_close(); st = VID_ERROR; return 0;
    }
    vs = &dmx->st[vsi];
    int aunsup = -1;
    for (int i = 0; i < dmx->nst; i++) if (dmx->st[i].type == ST_AUDIO) {
        if (audio_codec_playable(dmx->st[i].codec)) { asi = i; break; }
        if (aunsup < 0) aunsup = i;
    }
    if (asi >= 0) { ad = adec_open(&dmx->st[asi]); if (!ad) { aunsup = asi; asi = -1; } }
    if (asi < 0 && aunsup >= 0) snprintf(audio_warn, sizeof(audio_warn), "%s", codec_name(&dmx->st[aunsup]));
    if (asi >= 0) { ring = (float *)malloc(sizeof(float) * 2 * RING_FR); if (!ring) { adec_close(ad); ad = NULL; asi = -1; } }
    duration = dmx->duration;
    frame_dur = vs->fps > 1 && vs->fps < 121 ? 1.0 / vs->fps : 1.0 / 25;
    build_sps_pps();
    build_sub_options();
    apply_sub(pick_default_sub());
    set_fmt_info();
    if (!vdec_start(vs->codec)) { verr = VERR_DECODER; snprintf(err_detail, sizeof(err_detail), "%s", codec_name(vs)); video_close(); st = VID_ERROR; return 0; }
    reset_pipeline_state();
    paused = 0;
    cur_tex = -1; frame_w = frame_h = 0;
    st = VID_PLAYING;
    start_threads();
    return 1;
}

static void free_textures(void)
{
    for (int b = 0; b < 2; b++) { if (tex[b]) gfx_free_texture(tex[b]); tex[b] = NULL; }
    tex_w = tex_h = 0;
}

void video_close(void)
{
    stop_threads();
    int leak = 0;
    if (vh) {
        vdec_end_and_drain();
        /* closing a decoder that never finished its sequence hangs the console: leave it and keep its memory */
        if (seq_done) vdecClose(vh); else leak = 1;
        vh = 0;
    }
    if (!leak) { free(vmem); for (int i = 0; i < AU_SLOTS; i++) free(au_buf[i]); }
    vmem = NULL;
    for (int i = 0; i < AU_SLOTS; i++) au_buf[i] = NULL;
    if (ad) { adec_close(ad); ad = NULL; }
    if (dmx) { demux_close(dmx); dmx = NULL; }
    vs = NULL; vsi = asi = -1;
    free(ring); ring = NULL;
    free(frame); frame = NULL; frame_cap = 0;
    free(es_buf); es_buf = NULL; es_cap = 0; es_reset();
    lock_subs(); subs_free(&subs); unlock_subs();
    nsubopts = cursub = 0;
    free_textures();
    st = VID_IDLE;
}

void video_toggle_pause(void)
{
    if (st == VID_PLAYING) { paused = 1; st = VID_PAUSED; wall_pause_start = now_us(); }
    else if (st == VID_PAUSED) { paused = 0; st = VID_PLAYING; wall_pause_acc += now_us() - wall_pause_start; }
}

static void restart_at(double target)
{
    if (target < 0) target = 0;
    if (duration > 1 && target > duration - 1) target = duration - 1;
    stop_threads();
    vdec_end_and_drain();
    vdecStartSequence(vh);
    dmx->seek(dmx, target);
    if (ad) adec_reset(ad);
    reset_pipeline_state();
    video_start_pts = target;
    if (st == VID_ENDED) { st = VID_PLAYING; paused = 0; }
    if (paused) wall_pause_start = now_us();
    start_threads();
}

void video_seek(int delta)
{
    if (st != VID_PLAYING && st != VID_PAUSED && st != VID_ENDED) return;
    restart_at(video_position() + delta);
}

int video_cycle_subtitle(char *label, int n)
{
    if (!dmx || nsubopts <= 1) { snprintf(label, n, "Off"); return 0; }
    int next = (cursub + 1) % nsubopts;
    int was_embedded = cursub > 0 && subopts[cursub].kind == 1;
    apply_sub(next);
    snprintf(label, n, "%s", subopts[next].label);
    /* embedded tracks are collected while demuxing: restart from here so the current line shows */
    if (subopts[next].kind == 1 || was_embedded) restart_at(video_position());
    return 1;
}

int video_cycle_audio(char *label, int n)
{
    if (!dmx) return 0;
    int list[MAX_STREAMS], cnt = 0, cur = -1;
    for (int i = 0; i < dmx->nst; i++) if (dmx->st[i].type == ST_AUDIO && audio_codec_playable(dmx->st[i].codec)) { if (i == asi) cur = cnt; list[cnt++] = i; }
    if (cnt <= 1) return 0;
    int ni = list[(cur + 1) % cnt];
    double pos = video_position();
    stop_threads();
    if (ad) { adec_close(ad); ad = NULL; }
    ad = adec_open(&dmx->st[ni]);
    asi = ad ? ni : -1;
    if (asi >= 0 && !ring) ring = (float *)malloc(sizeof(float) * 2 * RING_FR);
    stream_t *s = &dmx->st[ni];
    snprintf(label, n, "%s%s%s · %s", s->name[0] ? s->name : "Audio", s->lang[0] ? " · " : "", s->lang, codec_name(s));
    set_fmt_info();
    restart_at(pos);
    return 1;
}

static void ensure_textures(int w, int h)
{
    if (tex_w == w && tex_h == h) return;
    free_textures();
    for (int b = 0; b < 2; b++) tex[b] = gfx_create_argb(w, h);
    tex_w = w; tex_h = h;
}

static void pic_dims(vdecPicture *pic, int *w, int *h)
{
    *w = *h = 0;
    if (pic->codec_specific_addr) {
        if (vs->codec == VC_H264) { vdecH264Info *i = (vdecH264Info *)(u64)pic->codec_specific_addr; *w = i->width; *h = i->height; }
        else if (vs->codec == VC_MPEG2) { vdecMPEG2Info *i = (vdecMPEG2Info *)(u64)pic->codec_specific_addr; *w = i->width; *h = i->height; }
    }
    if (*w <= 0 || *h <= 0) { *w = vs->w; *h = vs->h; }
    *w &= ~1; *h &= ~1;
}

static void fetch_pictures(void)
{
    if (paused || !vh || !vs) return;
    double now = video_position();
    for (int guard = 0; guard < 4; guard++) {
        u32 addr = 0;
        if (vdecGetPicItem(vh, &addr) != 0 || !addr) break;
        vdecPicture *pic = (vdecPicture *)(u64)addr;
        double pts;
        if (dmx->frame_count_ts) {
            if (seq_base < 0) seq_base = video_start_pts >= 0 ? video_start_pts : 0;
            pts = seq_base + out_count * frame_dur;
        } else if (pic->pts[0].low != VDEC_TS_INVALID || pic->pts[0].hi != VDEC_TS_INVALID) {
            uint64_t p90 = ((uint64_t)pic->pts[0].hi << 32) | pic->pts[0].low;
            pts = (double)p90 / 90000.0;
            if (last_pic_pts >= 0) { double d = pts - last_pic_pts; if (d > 0.005 && d < 0.2 && d < frame_dur * 1.5 && d > frame_dur * 0.4) frame_dur = frame_dur * 0.9 + d * 0.1; }
        } else pts = last_pic_pts >= 0 ? last_pic_pts + frame_dur : (video_start_pts >= 0 ? video_start_pts : 0);
        if (!wall_started) { wall_started = 1; wall_base = pts; wall_t0 = now_us(); wall_pause_acc = 0; if (paused) wall_pause_start = wall_t0; now = video_position(); }
        if (pts > now + 0.010 && cur_tex >= 0) break;
        int w, h;
        pic_dims(pic, &w, &h);
        vdecPictureFormat fmt = { VDEC_PICFMT_ARGB32, h >= 720 ? VDEC_COLOR_MATRIX_BT709 : VDEC_COLOR_MATRIX_BT601, 0xFF };
        if (vdecGetPicture(vh, &fmt, frame) != 0) break;
        if (pics_ready > 0) pics_ready--;
        out_count++;
        last_pic_pts = pts;
        if (w <= 0 || h <= 0 || w > 1920 || h > 1088) continue;
        if (pts < now - 0.080 && pics_ready > 1) continue;
        ensure_textures(w, h);
        int b = (cur_tex + 1) & 1;
        if (!tex[b]) break;
        memcpy(gfx_texture_data(tex[b]), frame, (size_t)w * h * 4);
        cur_tex = b; frame_w = w; frame_h = h;
        break;
    }
    if (demux_done && pics_ready == 0 && au_sent == au_done && (asi >= 0 && audio_started ? ring_used() < AUDIO_BLOCK_SAMPLES : 1)) st = VID_ENDED;
}

void video_render(float x, float y, float w, float h)
{
    if (st == VID_PLAYING) fetch_pictures();
    if (cur_tex < 0 || !frame_w) return;
    float sc = w / frame_w;
    if (h / frame_h < sc) sc = h / frame_h;
    float dw = frame_w * sc, dh = frame_h * sc;
    gfx_texture(tex[cur_tex], x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, 0xFFFFFFFF);
}

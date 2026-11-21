/* Built-in video player: MP4/M4V/MOV with H.264 video (firmware vdec on SPU) and AAC audio (faad).
 * Demux: mp4ff sample tables + our own avcC/hdlr parsing. Audio drives the clock. */
#include "video.h"
#include "vfs.h"
#include "gfx.h"
#include "audio.h"
#include <codec/vdec.h>
#include <audio/audio.h>
#include <sysmodule/sysmodule.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <mp4ff.h>
#include <neaacdec.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

#define OUT_RATE   48000
#define RING_FR    (OUT_RATE * 2)          /* 2 s of stereo float audio */
#define AU_SLOTS   8
#define AU_MAX     (1536 * 1024)

static volatile int st = VID_IDLE, verr, stop_demux, demux_done, paused;
static sys_ppu_thread_t th_demux, th_audio;
static int th_demux_on, th_audio_on;

/* demux */
static vfs_file_t *src;
static mp4ff_callback_t cb;
static mp4ff_t *mp4;
static int vtrack = -1, atrack = -1, vcount, acount;
static int32_t vscale = 1, ascale = 1;
static int64_t *vdts;                 /* dts per video sample (track timescale) */
static int vnext, anext;
static int64_t anext_t;
static double duration;

/* H.264 */
static uint8_t sps[512], pps[512];
static int sps_len, pps_len, nal_len_size = 4;
static uint32_t vh;                   /* vdec handle */
static void *vmem;
static uint8_t *au_buf[AU_SLOTS];
static volatile uint32_t au_sent, au_done, pics_ready;
static int need_headers;

/* audio */
static NeAACDecHandle aac;
static unsigned long arate = 44100;
static unsigned char achan = 2;
static float *ring;
static volatile uint32_t ring_w, ring_r;
static volatile uint64_t played;      /* output frames consumed by the port */
static double clock_base;             /* seconds at the last seek */
static uint64_t wall_t0, wall_pause_acc, wall_pause_start;

/* frame upload */
static texture_t tex[2][3];
static int tex_w, tex_h, cur_tex = -1, frame_w, frame_h;
static uint8_t *frame;
static int frame_cap;

static uint64_t now_us(void) { u64 s, ns; sysGetCurrentTime(&s, &ns); return s * 1000000ULL + ns / 1000; }

int video_supported(const char *n)
{
    const char *e = strrchr(n, '.');
    return e && (!strcasecmp(e, ".mp4") || !strcasecmp(e, ".m4v") || !strcasecmp(e, ".mov"));
}

int video_state(void) { return st; }
int video_error(void) { return verr; }
double video_duration(void) { return duration; }

double video_position(void)
{
    if (atrack >= 0) {
        double t = clock_base + (double)played / OUT_RATE - (8.0 * AUDIO_BLOCK_SAMPLES) / OUT_RATE;
        return t < clock_base ? clock_base : t;
    }
    uint64_t n = paused ? wall_pause_start : now_us();
    return clock_base + (double)(n - wall_t0 - wall_pause_acc) / 1e6;
}

int video_size(int *w, int *h) { *w = frame_w; *h = frame_h; return frame_w > 0; }

/* ---------------- mp4 parsing helpers ---------------- */

static uint32_t cb_read(void *ud, void *buf, uint32_t len) { int64_t r = vfs_read((vfs_file_t *)ud, buf, len); return r > 0 ? (uint32_t)r : 0; }
static uint32_t cb_seek(void *ud, uint64_t pos) { return vfs_seek((vfs_file_t *)ud, pos) == 0 ? 0 : (uint32_t)-1; }
static uint32_t cb_write(void *ud, void *b, uint32_t l) { (void)ud; (void)b; (void)l; return 0; }
static uint32_t cb_trunc(void *ud) { (void)ud; return 0; }

static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int atom_find(uint64_t start, uint64_t end, const char *type, uint64_t *pos, uint64_t *len, int nth)
{
    uint64_t p = start;
    uint8_t h[16];
    int seen = 0;
    while (p + 8 <= end) {
        if (vfs_seek(src, p) != 0 || vfs_read(src, h, 8) != 8) return 0;
        uint64_t sz = rd32(h);
        uint32_t hl = 8;
        if (sz == 1) { if (vfs_read(src, h + 8, 8) != 8) return 0; sz = ((uint64_t)rd32(h + 8) << 32) | rd32(h + 12); hl = 16; }
        else if (sz == 0) sz = end - p;
        if (sz < hl) return 0;
        if (!memcmp(h + 4, type, 4)) { if (seen++ == nth) { *pos = p + hl; *len = sz - hl; return 1; } }
        p += sz;
    }
    return 0;
}

/* walk the traks: track index order matches mp4ff's */
static void scan_tracks(void)
{
    uint64_t mp, ml, end = vfs_size(src);
    if (!atom_find(0, end, "moov", &mp, &ml, 0)) return;
    for (int t = 0; t < 32; t++) {
        uint64_t tp, tl, p, l;
        if (!atom_find(mp, mp + ml, "trak", &tp, &tl, t)) break;
        if (!atom_find(tp, tp + tl, "mdia", &p, &l, 0)) continue;
        uint64_t mdp = p, mdl = l;
        char handler[5] = { 0 };
        if (atom_find(mdp, mdp + mdl, "hdlr", &p, &l, 0) && l >= 12) { vfs_seek(src, p + 8); vfs_read(src, handler, 4); }
        if (!strcmp(handler, "soun") && atrack < 0) atrack = t;
        if (strcmp(handler, "vide") || vtrack >= 0) continue;
        if (!atom_find(mdp, mdp + mdl, "minf", &p, &l, 0) || !atom_find(p, p + l, "stbl", &p, &l, 0) || !atom_find(p, p + l, "stsd", &p, &l, 0)) continue;
        /* stsd: version/flags(4) count(4), then the sample entry */
        uint64_t ep, el;
        if (!atom_find(p + 8, p + l, "avc1", &ep, &el, 0) && !atom_find(p + 8, p + l, "avc3", &ep, &el, 0)) continue;
        uint64_t cp, cl;
        if (!atom_find(ep + 78, ep + el, "avcC", &cp, &cl, 0) || cl < 8 || cl > 4096) continue;
        uint8_t c[4096];
        vfs_seek(src, cp);
        if (vfs_read(src, c, (uint32_t)cl) != (int64_t)cl) continue;
        nal_len_size = (c[4] & 3) + 1;
        int nsps = c[5] & 31, o = 6;
        for (int i = 0; i < nsps && o + 2 <= (int)cl; i++) {
            int n = (c[o] << 8) | c[o + 1]; o += 2;
            if (i == 0 && n + 4 <= (int)sizeof(sps) && o + n <= (int)cl) { memcpy(sps, "\0\0\0\1", 4); memcpy(sps + 4, c + o, n); sps_len = n + 4; }
            o += n;
        }
        int npps = o < (int)cl ? c[o++] : 0;
        for (int i = 0; i < npps && o + 2 <= (int)cl; i++) {
            int n = (c[o] << 8) | c[o + 1]; o += 2;
            if (i == 0 && n + 4 <= (int)sizeof(pps) && o + n <= (int)cl) { memcpy(pps, "\0\0\0\1", 4); memcpy(pps + 4, c + o, n); pps_len = n + 4; }
            o += n;
        }
        if (sps_len && pps_len) vtrack = t;
    }
}

/* ---------------- vdec ---------------- */

static u32 vdec_cb(u32 handle, u32 msg, u32 data, u32 arg)
{
    (void)handle; (void)data; (void)arg;
    if (msg == VDEC_CALLBACK_AUDONE) au_done++;
    else if (msg == VDEC_CALLBACK_PICOUT) pics_ready++;
    return 0;
}

static int vdec_start(void)
{
    sysModuleLoad(SYSMODULE_VDEC);
    sysModuleLoad(SYSMODULE_VDEC_H264);
    vdecType type = { VDEC_CODEC_TYPE_H264, 42 };   /* level 4.2 */
    vdecAttr attr;
    if (vdecQueryAttr(&type, &attr) != 0) return 0;
    vmem = memalign(1024 * 1024, attr.mem_size);
    if (!vmem) return 0;
    vdecConfig cfg = { (u32)(u64)vmem, attr.mem_size, 1000, 0x8000, 250, 1 };
    vdecClosure cl = { (u32)(u64)vdec_cb, 0 };
    if (vdecOpen(&type, &cfg, &cl, &vh) != 0) return 0;
    if (vdecStartSequence(vh) != 0) return 0;
    for (int i = 0; i < AU_SLOTS; i++) if (!(au_buf[i] = (uint8_t *)memalign(128, AU_MAX))) return 0;
    frame_cap = 1920 * 1088 * 3 / 2;
    if (!(frame = (uint8_t *)memalign(128, frame_cap))) { frame_cap = 0; return 0; }
    return 1;
}

/* length-prefixed NALs -> Annex B, with SPS/PPS before the first AU and every IDR */
static int build_au(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap)
{
    uint32_t o = 0, i = 0;
    int idr = 0;
    for (uint32_t k = 0; k + nal_len_size <= n;) {
        uint32_t l = 0;
        for (int b = 0; b < nal_len_size; b++) l = (l << 8) | in[k + b];
        k += nal_len_size;
        if (k + l > n) break;
        if ((in[k] & 31) == 5) idr = 1;
        k += l;
    }
    if (idr || need_headers) {
        if ((uint32_t)(sps_len + pps_len) > cap) return 0;
        memcpy(out, sps, sps_len); o += sps_len;
        memcpy(out + o, pps, pps_len); o += pps_len;
        need_headers = 0;
    }
    while (i + nal_len_size <= n) {
        uint32_t l = 0;
        for (int b = 0; b < nal_len_size; b++) l = (l << 8) | in[i + b];
        i += nal_len_size;
        if (i + l > n || o + 4 + l > cap) break;
        memcpy(out + o, "\0\0\0\1", 4); o += 4;
        memcpy(out + o, in + i, l); o += l;
        i += l;
    }
    return (int)o;
}

static int sample_is_idr(const uint8_t *in, uint32_t n)
{
    for (uint32_t k = 0; k + nal_len_size <= n;) {
        uint32_t l = 0;
        for (int b = 0; b < nal_len_size; b++) l = (l << 8) | in[k + b];
        k += nal_len_size;
        if (k >= n) break;
        if ((in[k] & 31) == 5) return 1;
        k += l;
    }
    return 0;
}

/* ---------------- audio ---------------- */

static uint32_t ring_used(void) { return ring_w - ring_r; }

static void push_pcm(const int16_t *pcm, uint32_t frames, int ch, unsigned long rate)
{
    static double frac;
    static int16_t last_l, last_r;
    double step = (double)rate / OUT_RATE;
    uint32_t i = 0;
    while (i < frames && !stop_demux) {
        while (ring_used() >= RING_FR - 4 && !stop_demux) usleep(2000);
        int16_t l = pcm[i * ch], r = ch > 1 ? pcm[i * ch + 1] : l;
        float t = (float)frac;
        float fl = (last_l * (1 - t) + l * t) / 32768.0f, fr = (last_r * (1 - t) + r * t) / 32768.0f;
        uint32_t w = ring_w % RING_FR;
        ring[w * 2] = fl; ring[w * 2 + 1] = fr;
        ring_w++;
        frac += step;
        while (frac >= 1.0) { frac -= 1.0; last_l = l; last_r = r; i++; if (i >= frames) break; l = pcm[i * ch]; r = ch > 1 ? pcm[i * ch + 1] : l; }
    }
}

static void audio_thread(void *arg)
{
    (void)arg;
    if (!audio_sys_init()) { sysThreadExit(0); return; }
    audioPortParam prm = { AUDIO_PORT_2CH, AUDIO_BLOCK_8, AUDIO_PORT_INITLEVEL, 1.0f };
    audioPortConfig cfg;
    u32 port;
    if (audioPortOpen(&prm, &port) != 0) { sysThreadExit(0); return; }
    audioGetPortConfig(port, &cfg);
    audioPortStart(port);
    volatile u64 *ri = (volatile u64 *)(u64)cfg.readIndex;
    float *base = (float *)(u64)cfg.audioDataStart;
    u32 next = 1;
    while (!stop_demux) {
        if (next == (u32)*ri) { usleep(1000); continue; }
        float *blk = base + 2 * AUDIO_BLOCK_SAMPLES * next;
        if (paused || ring_used() < AUDIO_BLOCK_SAMPLES) {
            memset(blk, 0, sizeof(float) * 2 * AUDIO_BLOCK_SAMPLES);
        } else {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                uint32_t r = ring_r % RING_FR;
                blk[i * 2] = ring[r * 2]; blk[i * 2 + 1] = ring[r * 2 + 1];
                ring_r++;
            }
            played += AUDIO_BLOCK_SAMPLES;
        }
        next = (next + 1) % AUDIO_BLOCK_8;
    }
    audioPortStop(port);
    audioPortClose(port);
    sysThreadExit(0);
}

/* ---------------- demux thread ---------------- */

static void demux_thread(void *arg)
{
    (void)arg;
    uint8_t *sbuf = NULL;
    uint32_t scap = 0;
    while (!stop_demux) {
        int do_video = vtrack >= 0 && vnext < vcount;
        int do_audio = atrack >= 0 && anext < acount;
        if (!do_video && !do_audio) break;
        if (do_video && do_audio) {
            double vt = (double)vdts[vnext] / vscale, at = (double)anext_t / ascale;
            if (at < vt) do_video = 0; else do_audio = 0;
        }
        int track = do_video ? vtrack : atrack, idx = do_video ? vnext : anext;
        uint32_t need = (uint32_t)mp4ff_read_sample_getsize(mp4, track, idx);
        if (need > scap) { free(sbuf); scap = need + 4096; sbuf = (uint8_t *)malloc(scap); if (!sbuf) { verr = VERR_MEM; break; } }
        int32_t n = need ? mp4ff_read_sample_v2(mp4, track, idx, sbuf) : 0;

        if (do_video) {
            vnext++;
            if (n <= 0) continue;
            /* wait for a free AU slot and for the display to keep up */
            while (!stop_demux && (au_sent - au_done >= AU_SLOTS - 1 || pics_ready > 6)) usleep(2000);
            if (stop_demux) break;
            uint8_t *slot = au_buf[au_sent % AU_SLOTS];
            int len = build_au(sbuf, (uint32_t)n, slot, AU_MAX);
            if (len <= 0) continue;
            int64_t pts = vdts[idx] + mp4ff_get_sample_offset(mp4, vtrack, idx);
            uint64_t p90 = (uint64_t)((double)pts * 90000.0 / vscale);
            vdecAU au;
            memset(&au, 0, sizeof(au));
            au.packet_addr = (u32)(u64)slot; au.packet_size = len;
            au.pts.low = (u32)p90; au.pts.hi = (u32)(p90 >> 32);
            au.dts.low = VDEC_TS_INVALID; au.dts.hi = VDEC_TS_INVALID;
            au.userdata = p90;
            s32 r;
            while ((r = vdecDecodeAu(vh, VDEC_DECODER_MODE_NORMAL, &au)) == (s32)VDEC_ERROR_BUSY && !stop_demux) usleep(2000);
            if (r == 0) au_sent++;
        } else {
            anext_t += mp4ff_get_sample_duration(mp4, atrack, anext);
            anext++;
            if (n <= 0 || !aac) continue;
            NeAACDecFrameInfo fi;
            int16_t *pcm = (int16_t *)NeAACDecDecode(aac, &fi, sbuf, (unsigned long)n);
            if (!pcm || fi.error || !fi.samples || !fi.channels) continue;
            push_pcm(pcm, (uint32_t)(fi.samples / fi.channels), fi.channels, fi.samplerate ? fi.samplerate : arate);
        }
    }
    free(sbuf);
    demux_done = 1;
    sysThreadExit(0);
}

static void start_threads(void)
{
    stop_demux = 0; demux_done = 0;
    if (sysThreadCreate(&th_demux, demux_thread, NULL, 1100, 0x40000, THREAD_JOINABLE, "vdemux") == 0) th_demux_on = 1;
    if (atrack >= 0 && sysThreadCreate(&th_audio, audio_thread, NULL, 900, 0x10000, THREAD_JOINABLE, "vaudio") == 0) th_audio_on = 1;
}

static void stop_threads(void)
{
    stop_demux = 1;
    u64 rv;
    if (th_demux_on) { sysThreadJoin(th_demux, &rv); th_demux_on = 0; }
    if (th_audio_on) { sysThreadJoin(th_audio, &rv); th_audio_on = 0; }
}

/* ---------------- public ---------------- */

int video_open(const char *path)
{
    video_close();
    verr = VERR_NONE;
    vtrack = atrack = -1; sps_len = pps_len = 0;
    src = vfs_open_read(path);
    if (!src) { verr = VERR_OPEN; st = VID_ERROR; return 0; }
    scan_tracks();
    cb.read = cb_read; cb.write = cb_write; cb.seek = cb_seek; cb.truncate = cb_trunc; cb.user_data = src;
    vfs_seek(src, 0);
    mp4 = mp4ff_open_read(&cb);
    if (!mp4) { verr = VERR_FORMAT; video_close(); st = VID_ERROR; return 0; }
    if (vtrack < 0) { verr = VERR_CODEC; video_close(); st = VID_ERROR; return 0; }

    vcount = mp4ff_num_samples(mp4, vtrack);
    vscale = mp4ff_time_scale(mp4, vtrack);
    if (vscale <= 0) vscale = 1;
    vdts = (int64_t *)malloc(sizeof(int64_t) * (vcount + 1));
    if (!vdts) { verr = VERR_MEM; video_close(); st = VID_ERROR; return 0; }
    int64_t t = 0;
    for (int i = 0; i < vcount; i++) { vdts[i] = t; t += mp4ff_get_sample_duration(mp4, vtrack, i); }
    vdts[vcount] = t;
    duration = (double)t / vscale;

    if (atrack >= 0) {
        acount = mp4ff_num_samples(mp4, atrack);
        ascale = mp4ff_time_scale(mp4, atrack);
        if (ascale <= 0) ascale = 1;
        unsigned char *cfg = NULL; unsigned int cfglen = 0;
        mp4ff_get_decoder_config(mp4, atrack, &cfg, &cfglen);
        aac = NeAACDecOpen();
        if (aac) {
            NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(aac);
            c->outputFormat = FAAD_FMT_16BIT;
            NeAACDecSetConfiguration(aac, c);
            if (!cfg || NeAACDecInit2(aac, cfg, cfglen, &arate, &achan) < 0) { NeAACDecClose(aac); aac = NULL; atrack = -1; }
        } else atrack = -1;
        free(cfg);
        ring = (float *)malloc(sizeof(float) * 2 * RING_FR);
        if (!ring) atrack = -1;
    }

    if (!vdec_start()) { verr = VERR_DECODER; video_close(); st = VID_ERROR; return 0; }
    vnext = anext = 0; anext_t = 0;
    au_sent = au_done = pics_ready = 0;
    ring_w = ring_r = 0; played = 0;
    clock_base = 0; paused = 0;
    wall_t0 = now_us(); wall_pause_acc = 0;
    need_headers = 1;
    cur_tex = -1; frame_w = frame_h = 0;
    st = VID_PLAYING;
    start_threads();
    return 1;
}

static void free_textures(void)
{
    for (int b = 0; b < 2; b++) for (int p = 0; p < 3; p++) { if (tex[b][p]) gfx_free_texture(tex[b][p]); tex[b][p] = NULL; }
    tex_w = tex_h = 0;
}

void video_close(void)
{
    stop_threads();
    if (vh) { vdecEndSequence(vh); usleep(20000); vdecClose(vh); vh = 0; }
    free(vmem); vmem = NULL;
    for (int i = 0; i < AU_SLOTS; i++) { free(au_buf[i]); au_buf[i] = NULL; }
    if (aac) { NeAACDecClose(aac); aac = NULL; }
    if (mp4) { mp4ff_close(mp4); mp4 = NULL; }
    if (src) { vfs_close(src); src = NULL; }
    free(vdts); vdts = NULL;
    free(ring); ring = NULL;
    free(frame); frame = NULL; frame_cap = 0;
    free_textures();
    st = VID_IDLE;
}

void video_toggle_pause(void)
{
    if (st == VID_PLAYING) { paused = 1; st = VID_PAUSED; wall_pause_start = now_us(); }
    else if (st == VID_PAUSED) { paused = 0; st = VID_PLAYING; wall_pause_acc += now_us() - wall_pause_start; }
}

void video_seek(int delta)
{
    if (st != VID_PLAYING && st != VID_PAUSED && st != VID_ENDED) return;
    double target = video_position() + delta;
    if (target < 0) target = 0;
    if (target > duration - 1) target = duration - 1 > 0 ? duration - 1 : 0;
    stop_threads();
    /* flush the decoder */
    vdecEndSequence(vh);
    for (int i = 0; i < 100 && pics_ready; i++) { u32 a; if (vdecGetPicItem(vh, &a) == 0) { vdecPictureFormat f = { VDEC_PICFMT_YUV420P, VDEC_COLOR_MATRIX_BT709, 0 }; if (frame) vdecGetPicture(vh, &f, frame); } pics_ready--; usleep(1000); }
    vdecStartSequence(vh);
    au_sent = au_done = pics_ready = 0;

    /* video: last sample at or before target, then forward to the next IDR */
    int64_t tt = (int64_t)(target * vscale);
    int lo = 0, hi = vcount - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (vdts[mid] <= tt) lo = mid; else hi = mid - 1; }
    int idx = lo;
    for (int k = idx; k >= 0 && k > idx - 600; k--) {
        uint32_t need = (uint32_t)mp4ff_read_sample_getsize(mp4, vtrack, k);
        uint8_t *b = need ? (uint8_t *)malloc(need) : NULL;
        int ok = b && mp4ff_read_sample_v2(mp4, vtrack, k, b) > 0 && sample_is_idr(b, need);
        free(b);
        if (ok) { idx = k; break; }
    }
    vnext = idx;
    double vtime = (double)vdts[idx] / vscale;
    need_headers = 1;

    if (atrack >= 0) {
        int64_t at = 0; int i = 0;
        int64_t want = (int64_t)(vtime * ascale);
        for (; i < acount; i++) { int32_t d = mp4ff_get_sample_duration(mp4, atrack, i); if (at + d > want) break; at += d; }
        anext = i; anext_t = at;
        NeAACDecPostSeekReset(aac, i);
    }
    ring_w = ring_r = 0; played = 0;
    clock_base = vtime;
    wall_t0 = now_us(); wall_pause_acc = 0; if (paused) wall_pause_start = wall_t0;
    if (st == VID_ENDED) { st = VID_PLAYING; paused = 0; }
    start_threads();
}

static void ensure_textures(int w, int h)
{
    int aw = (w + 63) & ~63, ah = (h + 1) & ~1;
    if (tex_w == aw && tex_h == ah) return;
    free_textures();
    for (int b = 0; b < 2; b++) {
        tex[b][0] = gfx_create_l8(aw, ah);
        tex[b][1] = gfx_create_l8(aw / 2, ah / 2);
        tex[b][2] = gfx_create_l8(aw / 2, ah / 2);
    }
    tex_w = aw; tex_h = ah;
}

static void fetch_pictures(void)
{
    if (paused || !vh) return;
    double now = video_position();
    for (int guard = 0; guard < 4 && pics_ready > 0; guard++) {
        u32 addr = 0;
        if (vdecGetPicItem(vh, &addr) != 0 || !addr) break;
        vdecPicture *pic = (vdecPicture *)(u64)addr;
        uint64_t p90 = ((uint64_t)pic->pts[0].hi << 32) | pic->pts[0].low;
        double pts = (double)p90 / 90000.0;
        if (pts > now + 0.010 && cur_tex >= 0) break;        /* not due yet */
        vdecH264Info *info = (vdecH264Info *)(u64)pic->codec_specific_addr;
        int w = info ? info->width : 0, h = info ? info->height : 0;
        if (w <= 0 || h <= 0 || w > 1920 || h > 1088) { vdecPictureFormat f = { VDEC_PICFMT_YUV420P, VDEC_COLOR_MATRIX_BT709, 0 }; if (frame) vdecGetPicture(vh, &f, frame); pics_ready--; continue; }
        int need = w * h * 3 / 2;
        if (need > frame_cap) { free(frame); frame = (uint8_t *)memalign(128, need); frame_cap = frame ? need : 0; }
        if (!frame) break;
        vdecPictureFormat fmt = { VDEC_PICFMT_YUV420P, h >= 720 ? VDEC_COLOR_MATRIX_BT709 : VDEC_COLOR_MATRIX_BT601, 0xFF };
        if (vdecGetPicture(vh, &fmt, frame) != 0) break;
        pics_ready--;
        /* late frames are dropped except the most recent */
        if (pts < now - 0.080 && pics_ready > 0) continue;
        ensure_textures(w, h);
        int b = (cur_tex + 1) & 1;
        if (!tex[b][0] || !tex[b][1] || !tex[b][2]) break;
        uint8_t *dy = gfx_texture_data(tex[b][0]), *du = gfx_texture_data(tex[b][1]), *dv = gfx_texture_data(tex[b][2]);
        const uint8_t *sy = frame, *su = frame + w * h, *sv = su + (w / 2) * (h / 2);
        for (int y = 0; y < h; y++) memcpy(dy + y * tex_w, sy + y * w, w);
        for (int y = 0; y < h / 2; y++) { memcpy(du + y * (tex_w / 2), su + y * (w / 2), w / 2); memcpy(dv + y * (tex_w / 2), sv + y * (w / 2), w / 2); }
        cur_tex = b; frame_w = w; frame_h = h;
        break;
    }
    if (demux_done && pics_ready == 0 && au_sent == au_done && (atrack >= 0 ? ring_used() < AUDIO_BLOCK_SAMPLES : video_position() >= duration - 0.2)) st = VID_ENDED;
}

void video_render(float x, float y, float w, float h)
{
    if (st == VID_PLAYING) fetch_pictures();
    if (cur_tex < 0 || !frame_w) return;
    float sc = w / frame_w;
    if (h / frame_h < sc) sc = h / frame_h;
    float dw = frame_w * sc, dh = frame_h * sc;
    gfx_draw_yuv(tex[cur_tex][0], tex[cur_tex][1], tex[cur_tex][2], x + (w - dw) / 2, y + (h - dh) / 2, dw, dh,
                 (float)frame_w / tex_w, (float)frame_h / tex_h);
}

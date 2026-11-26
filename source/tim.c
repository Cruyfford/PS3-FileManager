#include "tim.h"
#include "pngw.h"
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

static void c15(uint16_t c, uint8_t *o, int semi_ok)
{
    uint8_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    o[0] = r << 3 | r >> 2; o[1] = g << 3 | g >> 2; o[2] = b << 3 | b >> 2;
    o[3] = (c == 0) ? 0 : 255;          /* 0x0000 is transparent on the PS1 */
    (void)semi_ok;
}

uint32_t tim_decode(const uint8_t *d, uint32_t len, int clut_row, tim_image_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 20 || le32(d) != 0x10) return 0;
    uint32_t flags = le32(d + 4);
    int pmode = flags & 7, has_clut = (flags >> 3) & 1;
    if (flags & ~0xFu || pmode > 3) return 0;
    uint32_t p = 8;
    const uint8_t *clut = NULL; int cw = 0, ch = 0;
    if (has_clut) {
        if (p + 12 > len) return 0;
        uint32_t bl = le32(d + p);
        cw = le16(d + p + 8); ch = le16(d + p + 10);
        if (bl < 12 || p + bl > len || !cw || !ch || (uint32_t)cw * ch * 2 + 12 > bl) return 0;
        clut = d + p + 12;
        p += bl;
    } else if (pmode < 2) return 0;
    if (p + 12 > len) return 0;
    uint32_t bl = le32(d + p);
    int wh = le16(d + p + 8), h = le16(d + p + 10);
    if (bl < 12 || p + bl > len || !wh || !h || h > 1024 || wh > 1024) return 0;
    int w = pmode == 0 ? wh * 4 : pmode == 1 ? wh * 2 : pmode == 2 ? wh : wh * 2 / 3;
    if ((uint32_t)wh * 2 * h + 12 > bl || !w) return 0;
    const uint8_t *pix = d + p + 12;
    if (clut_row < 0 || clut_row >= ch) clut_row = 0;
    const uint8_t *pal = clut ? clut + (size_t)clut_row * cw * 2 : NULL;
    int npal = cw;
    uint8_t *o = malloc((size_t)w * h * 4);
    if (!o) return 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = pix + (size_t)y * wh * 2;
        for (int x = 0; x < w; x++) {
            uint8_t *q = o + ((size_t)y * w + x) * 4;
            int idx;
            switch (pmode) {
            case 0: idx = (row[x >> 1] >> ((x & 1) * 4)) & 15; if (idx < npal) c15(le16(pal + idx * 2), q, 1); else memset(q, 0, 4); break;
            case 1: idx = row[x]; if (idx < npal) c15(le16(pal + idx * 2), q, 1); else memset(q, 0, 4); break;
            case 2: c15(le16(row + x * 2), q, 1); break;
            default: q[0] = row[x * 3]; q[1] = row[x * 3 + 1]; q[2] = row[x * 3 + 2]; q[3] = 255; break;
            }
        }
    }
    out->w = w; out->h = h; out->rgba = o;
    out->bpp = pmode == 0 ? 4 : pmode == 1 ? 8 : pmode == 2 ? 16 : 24;
    return p + bl;
}

void tim_free(tim_image_t *t) { free(t->rgba); t->rgba = NULL; }

int tim_extract_all(const char *file, const char *out_dir, volatile int *cancel, void (*cb)(uint64_t, const char *))
{
    vfs_file_t *f = vfs_open_read(file);
    if (!f) return -1;
    uint64_t size = vfs_size(f);
    const uint32_t WIN = 4 * 1024 * 1024, KEEP = 2 * 1024 * 1024;   /* a TIM must fit inside the 2 MB overlap */
    uint8_t *b = malloc(WIN);
    if (!b) { vfs_close(f); return -1; }
    vfs_mkdir(out_dir);
    const char *bn = strrchr(file, '/'); bn = bn ? bn + 1 : file;
    uint64_t base = 0; uint32_t have = 0; int count = 0;
    for (;;) {
        if (cancel && *cancel) break;
        int64_t r = vfs_read(f, b + have, WIN - have);
        if (r < 0) break;
        uint32_t total = have + (uint32_t)r;
        int last = r == 0 || base + total >= size;
        uint32_t limit = last ? total : (total > KEEP ? total - KEEP : 0);
        uint32_t i = 0;
        while (i + 20 <= total && i < limit) {
            if (b[i] == 0x10 && !b[i + 1] && !b[i + 2] && !b[i + 3] && b[i + 5] == 0 && b[i + 6] == 0 && b[i + 7] == 0 && b[i + 4] <= 0x0B) {
                tim_image_t t;
                uint32_t used = tim_decode(b + i, total - i, 0, &t);
                if (used) {
                    char path[1400];
                    snprintf(path, sizeof(path), "%s/%s_%08llX_%dx%d_%dbpp.png", out_dir, bn, (unsigned long long)(base + i), t.w, t.h, t.bpp);
                    if (png_write(path, t.rgba, t.w, t.h, t.w * 4, 0)) { count++; if (cb) cb(0, strrchr(path, '/') + 1); }
                    tim_free(&t);
                    i += used;
                    continue;
                }
            }
            i += 4;     /* TIMs are word aligned inside PS1 files */
        }
        if (cb) cb(i, NULL);
        if (last) break;
        memmove(b, b + i, total - i);
        base += i; have = total - i;
    }
    free(b);
    vfs_close(f);
    return count;
}

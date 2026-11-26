#include "pngw.h"
#include "vfs.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int chunk(vfs_file_t *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4);
    uint32_t c = crc32(0, (const Bytef *)type, 4);
    if (n) c = crc32(c, d, n);
    uint8_t t[4]; be32(t, c);
    return vfs_write(f, h, 8) == 8 && (!n || vfs_write(f, d, n) == (int64_t)n) && vfs_write(f, t, 4) == 4;
}

int png_write(const char *path, const uint8_t *px, int w, int h, int stride, int fmt)
{
    int alpha = fmt == 0, bpp = alpha ? 4 : 3;
    size_t rowb = (size_t)w * bpp + 1, raw_n = rowb * h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *o = raw + rowb * y; const uint8_t *s = px + (size_t)stride * y;
        *o++ = 0;
        if (alpha) memcpy(o, s, (size_t)w * 4);
        else for (int x = 0; x < w; x++, s += 4) { *o++ = s[1]; *o++ = s[2]; *o++ = s[3]; }
    }
    uLongf zn = compressBound(raw_n);
    uint8_t *z = malloc(zn);
    int ok = 0;
    if (z && compress2(z, &zn, raw, raw_n, 3) == Z_OK) {
        vfs_file_t *f = vfs_open_write(path);
        if (f) {
            static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
            uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = alpha ? 6 : 2; ih[10] = ih[11] = ih[12] = 0;
            ok = vfs_write(f, sig, 8) == 8 && chunk(f, "IHDR", ih, 13) && chunk(f, "IDAT", z, (uint32_t)zn) && chunk(f, "IEND", NULL, 0);
            vfs_close(f);
            if (!ok) vfs_unlink(path);
        }
    }
    free(z); free(raw);
    return ok;
}

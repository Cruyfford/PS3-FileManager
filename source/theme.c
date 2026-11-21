/* PS3 theme (.p3t, CXML container) preview: pick the largest JPEG/PNG stored in the file table,
 * inflating zlib-compressed entries when needed. Header: magic "P3TF", version, then
 * (offset,size) pairs for tree, id table, string table, int array, float array, file table (big-endian). */
#include "theme.h"
#include "vfs.h"
#include <zlib.h>
#include <string.h>
#include <stdlib.h>

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int is_img(const uint8_t *p, uint32_t n)
{
    if (n < 8) return 0;
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return 1;
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return 2;
    return 0;
}

int p3t_preview(const char *path, uint8_t **out, uint32_t *outlen)
{
    *out = NULL; *outlen = 0;
    vfs_file_t *f = vfs_open_read(path);
    if (!f) return 0;
    uint8_t h[0x40];
    if (vfs_read(f, h, sizeof(h)) != (int64_t)sizeof(h) || memcmp(h, "P3TF", 4)) { vfs_close(f); return 0; }
    uint32_t ft_off = be32(h + 0x30), ft_size = be32(h + 0x34);
    uint64_t fsize = vfs_size(f);
    if (!ft_off || ft_off >= fsize) { ft_off = 0x40; ft_size = (uint32_t)(fsize - 0x40); }
    if ((uint64_t)ft_off + ft_size > fsize) ft_size = (uint32_t)(fsize - ft_off);
    if (ft_size > 40 * 1024 * 1024) ft_size = 40 * 1024 * 1024;
    uint8_t *d = (uint8_t *)malloc(ft_size);
    if (!d) { vfs_close(f); return 0; }
    uint32_t got = 0;
    vfs_seek(f, ft_off);
    while (got < ft_size) { int64_t r = vfs_read(f, d + got, ft_size - got); if (r <= 0) break; got += (uint32_t)r; }
    vfs_close(f);
    ft_size = got;

    /* 1) uncompressed images: largest JPEG/PNG span between signatures */
    uint32_t best_off = 0, best_len = 0;
    uint32_t prev = 0xFFFFFFFF;
    for (uint32_t i = 0; i + 8 < ft_size; i++) {
        if (!is_img(d + i, ft_size - i)) continue;
        if (prev != 0xFFFFFFFF && i - prev > best_len) { best_off = prev; best_len = i - prev; }
        prev = i;
        i += 64;   /* skip header bytes so embedded EXIF thumbnails don't split the image */
    }
    if (prev != 0xFFFFFFFF && ft_size - prev > best_len) { best_off = prev; best_len = ft_size - prev; }

    /* 2) zlib-compressed entries: inflate candidates and keep the largest image */
    uint8_t *zbest = NULL; uint32_t zbest_len = 0;
    int tries = 0;
    for (uint32_t i = 0; i + 2 < ft_size && tries < 400; i++) {
        if (d[i] != 0x78 || (d[i + 1] != 0xDA && d[i + 1] != 0x9C && d[i + 1] != 0x01 && d[i + 1] != 0x5E)) continue;
        if (((d[i] << 8) | d[i + 1]) % 31) continue;
        tries++;
        uint8_t head[16];
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit(&zs) != Z_OK) continue;
        zs.next_in = d + i; zs.avail_in = ft_size - i;
        zs.next_out = head; zs.avail_out = sizeof(head);
        int r = inflate(&zs, Z_NO_FLUSH);
        if ((r != Z_OK && r != Z_STREAM_END) || zs.total_out < 8 || !is_img(head, (uint32_t)zs.total_out)) { inflateEnd(&zs); continue; }
        /* full inflate */
        uint32_t cap = 4 * 1024 * 1024;
        uint8_t *buf = (uint8_t *)malloc(cap);
        if (!buf) { inflateEnd(&zs); break; }
        memcpy(buf, head, zs.total_out);
        uint32_t len = (uint32_t)zs.total_out;
        while (r == Z_OK) {
            if (len == cap) {
                if (cap >= 16 * 1024 * 1024) break;
                uint8_t *nb = (uint8_t *)realloc(buf, cap * 2);
                if (!nb) break;
                buf = nb; cap *= 2;
            }
            zs.next_out = buf + len; zs.avail_out = cap - len;
            r = inflate(&zs, Z_NO_FLUSH);
            len = (uint32_t)zs.total_out;
        }
        uint32_t consumed = (uint32_t)zs.total_in;
        inflateEnd(&zs);
        if (len > zbest_len) { free(zbest); zbest = buf; zbest_len = len; } else free(buf);
        if (consumed > 2) i += consumed - 1;
    }

    if (zbest && zbest_len > best_len) {
        *out = zbest; *outlen = zbest_len;
        free(d);
        return 1;
    }
    free(zbest);
    if (best_len >= 64) {
        uint8_t *o = (uint8_t *)malloc(best_len);
        if (o) { memcpy(o, d + best_off, best_len); *out = o; *outlen = best_len; }
    }
    free(d);
    return *out != NULL;
}

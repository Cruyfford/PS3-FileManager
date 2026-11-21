/* PS3 PKG reader: header, metadata, and (decrypted) PARAM.SFO / ICON0.PNG.
 * Format reference: https://www.psdevwiki.com/ps3/PKG_files */
#include "pkginfo.h"
#include "vfs.h"
#include <polarssl/aes.h>
#include <polarssl/sha1.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const uint8_t PS3_PKG_KEY[16] = {
    0x2E, 0x7B, 0x71, 0xD7, 0xC9, 0xC9, 0xA1, 0x4E, 0xA3, 0x22, 0x1F, 0x18, 0x88, 0x28, 0xB8, 0xF8
};

static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static inline uint32_t le32(const uint8_t *p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[1] << 8 | p[0]); }

typedef struct {
    vfs_file_t *fd;
    int retail;
    uint64_t data_off;
    uint8_t riv[16];      /* AES-CTR IV (retail) */
    uint8_t digest[16];   /* SHA1 keystream seed (debug) */
    aes_context aes;
} pkg_ctx;

static int read_at(vfs_file_t *fd, uint64_t off, void *buf, uint64_t len)
{
    if (vfs_seek(fd, off) != 0) return 0;
    uint64_t got = 0;
    while (got < len) {
        int64_t r = vfs_read(fd, (uint8_t *)buf + got, (uint32_t)(len - got));
        if (r <= 0) break;
        got += (uint64_t)r;
    }
    return got == len;
}

/* decrypt `len` bytes located at data-relative offset `rel` */
static int read_dec(pkg_ctx *c, uint64_t rel, uint8_t *out, uint32_t len)
{
    uint64_t blk = rel / 16;
    uint32_t skip = (uint32_t)(rel % 16);
    uint32_t total = skip + len;
    uint32_t aligned = (total + 15) & ~15u;
    uint8_t *tmp = (uint8_t *)malloc(aligned);
    if (!tmp) return 0;
    memset(tmp, 0, aligned);
    uint64_t start = c->data_off + blk * 16;
    uint64_t rd = 0;
    if (vfs_seek(c->fd, start) == 0)
        while (rd < aligned) { int64_t r = vfs_read(c->fd, tmp + rd, (uint32_t)(aligned - rd)); if (r <= 0) break; rd += (uint64_t)r; }

    uint8_t ks[20], ctr[64];
    for (uint32_t i = 0; i < aligned; i += 16) {
        uint64_t n = blk + i / 16;
        if (c->retail) {
            uint8_t iv[16];
            memcpy(iv, c->riv, 16);
            /* 128-bit big-endian add of block number */
            uint64_t lo = be64(iv + 8), hi = be64(iv);
            uint64_t nlo = lo + n;
            if (nlo < lo) hi++;
            for (int b = 0; b < 8; b++) { iv[7 - b] = (uint8_t)(hi >> (8 * b)); iv[15 - b] = (uint8_t)(nlo >> (8 * b)); }
            aes_crypt_ecb(&c->aes, AES_ENCRYPT, iv, ks);
        } else {
            memset(ctr, 0, sizeof(ctr));
            memcpy(ctr + 0x00, c->digest, 8);
            memcpy(ctr + 0x08, c->digest, 8);
            memcpy(ctr + 0x10, c->digest + 8, 8);
            memcpy(ctr + 0x18, c->digest + 8, 8);
            for (int b = 0; b < 8; b++) ctr[0x3F - b] = (uint8_t)(n >> (8 * b));
            sha1(ctr, 0x40, ks);
        }
        for (int k = 0; k < 16; k++) tmp[i + k] ^= ks[k];
    }
    memcpy(out, tmp + skip, len);
    free(tmp);
    return rd >= (uint64_t)total;
}

static void sfo_parse(const uint8_t *s, uint32_t size, pkg_info_t *info)
{
    if (size < 0x14 || be32(s) != 0x00505346) return;   /* "\0PSF" */
    uint32_t key_off = le32(s + 0x08), data_off = le32(s + 0x0C), count = le32(s + 0x10);
    for (uint32_t i = 0; i < count && 0x14 + i * 16 + 16 <= size; i++) {
        const uint8_t *e = s + 0x14 + i * 16;
        uint32_t ko = key_off + le16(e), fmt = le16(e + 2), len = le32(e + 4), doff = data_off + le32(e + 12);
        if (ko >= size || doff + len > size) continue;
        const char *key = (const char *)s + ko;
        if (fmt != 0x0204 && fmt != 0x0004) continue;   /* utf8 strings */
        char *dst = NULL; size_t cap = 0;
        if (!strcmp(key, "TITLE"))        { dst = info->title; cap = sizeof(info->title); }
        else if (!strcmp(key, "TITLE_ID")) { dst = info->title_id; cap = sizeof(info->title_id); }
        else if (!strcmp(key, "APP_VER") || (!strcmp(key, "VERSION") && !info->version[0])) { dst = info->version; cap = sizeof(info->version); }
        else if (!strcmp(key, "CATEGORY")) { dst = info->category; cap = sizeof(info->category); }
        if (!dst) continue;
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(dst, s + doff, n);
        dst[n] = 0;
        for (char *p = dst; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    }
}

int pkg_read_info(const char *path, pkg_info_t *info, int deep)
{
    memset(info, 0, sizeof(*info));
    pkg_ctx c;
    memset(&c, 0, sizeof(c));
    c.fd = vfs_open_read(path);
    if (!c.fd) return 0;

    uint8_t h[0x80];
    if (!read_at(c.fd, 0, h, sizeof(h)) || be32(h) != 0x7F504B47) { vfs_close(c.fd); return 0; }

    uint16_t revision = (uint16_t)(be32(h + 4) >> 16);
    uint16_t type = (uint16_t)(be32(h + 4) & 0xFFFF);
    uint32_t meta_off = be32(h + 0x08), meta_cnt = be32(h + 0x0C);
    uint32_t item_cnt = be32(h + 0x14);
    info->total_size = be64(h + 0x18);
    c.data_off = be64(h + 0x20);
    memcpy(info->content_id, h + 0x30, 36);
    info->content_id[36] = 0;
    memcpy(c.digest, h + 0x60, 16);
    memcpy(c.riv, h + 0x70, 16);
    c.retail = (revision & 0x8000) ? 1 : 0;
    info->retail = c.retail;
    info->valid = 1;

    /* unencrypted metadata */
    if (meta_off && meta_cnt < 64) {
        uint8_t m[0x400];
        if (read_at(c.fd, meta_off, m, sizeof(m))) {
            uint32_t p = 0;
            for (uint32_t i = 0; i < meta_cnt && p + 8 <= sizeof(m); i++) {
                uint32_t id = be32(m + p), sz = be32(m + p + 4);
                if (p + 8 + sz > sizeof(m)) break;
                if (id == 1 && sz >= 4) info->drm_type = be32(m + p + 8);
                if (id == 2 && sz >= 4) info->content_type = be32(m + p + 8);
                p += 8 + sz;
            }
        }
    }

    if (deep && type == 1 && item_cnt > 0 && item_cnt < 200000) {
        aes_setkey_enc(&c.aes, PS3_PKG_KEY, 128);
        uint32_t scan = item_cnt < 4096 ? item_cnt : 4096;
        uint8_t *tab = (uint8_t *)malloc(scan * 32);
        if (tab && read_dec(&c, 0, tab, scan * 32)) {
            for (uint32_t i = 0; i < scan; i++) {
                const uint8_t *e = tab + i * 32;
                uint32_t noff = be32(e), nlen = be32(e + 4);
                uint64_t doff = be64(e + 8), dsize = be64(e + 16);
                if (nlen == 0 || nlen > 255) continue;
                char name[256];
                if (!read_dec(&c, noff, (uint8_t *)name, nlen)) continue;
                name[nlen] = 0;
                if (!strcmp(name, "PARAM.SFO") && dsize > 0 && dsize < 64 * 1024) {
                    uint8_t *sfo = (uint8_t *)malloc((size_t)dsize);
                    if (sfo && read_dec(&c, doff, sfo, (uint32_t)dsize)) sfo_parse(sfo, (uint32_t)dsize, info);
                    free(sfo);
                } else if (!strcmp(name, "ICON0.PNG") && dsize > 8 && dsize < 1024 * 1024 && !info->icon) {
                    uint8_t *ic = (uint8_t *)malloc((size_t)dsize);
                    if (ic && read_dec(&c, doff, ic, (uint32_t)dsize) && ic[1] == 'P' && ic[2] == 'N' && ic[3] == 'G') {
                        info->icon = ic; info->icon_size = (uint32_t)dsize;
                    } else free(ic);
                }
                if (info->title[0] && info->icon) break;
            }
        }
        free(tab);
    }

    if (!info->title_id[0] && info->content_id[0]) {
        memcpy(info->title_id, info->content_id + 7, 9);
        info->title_id[9] = 0;
    }
    vfs_close(c.fd);
    return 1;
}

void pkg_free_info(pkg_info_t *info)
{
    if (info->icon) free(info->icon);
    info->icon = NULL;
}

const char *pkg_type_label(uint32_t t)
{
    switch (t) {
        case 0x04: case 0x05: return "PS3";
        case 0x06: return "PS1";
        case 0x07: case 0x0E: case 0x0F: return "PSP";
        case 0x09: return "THEME";
        case 0x0A: return "WIDGET";
        case 0x0B: return "LICENSE";
        case 0x0C: return "VSH";
        case 0x0D: return "AVATAR";
        case 0x10: return "MINIS";
        case 0x11: return "NEOGEO";
        case 0x12: return "VMC";
        case 0x13: return "PS2";
        case 0x14: case 0x15: return "PSP";
        case 0x19: return "WEBTV";
    }
    return "PKG";
}

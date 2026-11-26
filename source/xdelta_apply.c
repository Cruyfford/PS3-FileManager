/* xdelta3 / VCDIFF patch decoding (xdelta3 by Joshua MacDonald, Apache-2.0), decoder only, DJW/FGK secondary. */
#define XD3_ENCODER 0
#define XD3_MAIN 0
#define SECONDARY_DJW 1
#define SECONDARY_FGK 1
#define SECONDARY_LZMA 0
#define EXTERNAL_COMPRESSION 0
#define SIZEOF_SIZE_T __SIZEOF_SIZE_T__
#define SIZEOF_UNSIGNED_LONG_LONG 8
#define XD3_USE_LARGEFILE64 1
#define XD3_DEBUG 0
#include "../third_party/xdelta3/xdelta3.h"
#include "../third_party/xdelta3/xdelta3.c"

#include "patcher.h"
#include "fileops.h"
#include "vfs.h"
#include <stdio.h>

#define BLK (1u << 20)
#define NCACHE 24

typedef struct { xoff_t no; usize_t len; uint8_t *d; unsigned long long used; } cblk_t;

int xdelta_apply(const char *patch, const char *src, const char *dst, volatile int *cancel, patch_cb cb, volatile uint64_t *total, char *err, int en)
{
    vfs_file_t *pf = vfs_open_read(patch), *sf = vfs_open_read(src), *of = NULL;
    if (!pf || !sf) { if (pf) vfs_close(pf); if (sf) vfs_close(sf); snprintf(err, en, "%s", !pf ? patch : src); return OPE_OPEN_SRC; }
    of = vfs_open_write(dst);
    if (!of) { vfs_close(pf); vfs_close(sf); snprintf(err, en, "%s", dst); return OPE_OPEN_DST; }
    *total = vfs_size(sf);
    xd3_stream stream; xd3_config config; xd3_source source;
    memset(&stream, 0, sizeof(stream)); memset(&source, 0, sizeof(source));
    xd3_init_config(&config, 0);
    config.winsize = BLK;
    int ret = 0, rc = 0;
    cblk_t cache[NCACHE]; memset(cache, 0, sizeof(cache));
    unsigned long long tick = 0;
    uint8_t *in = malloc(BLK);
    if (!in || xd3_config_stream(&stream, &config) != 0) { rc = OPE_MEM; goto out; }
    source.blksize = BLK;
    source.curblkno = (xoff_t)-1;
    source.curblk = NULL; source.onblk = 0;
    xd3_set_source(&stream, &source);
    for (;;) {
        int64_t n = vfs_read(pf, in, BLK);
        if (n < 0) { rc = OPE_READ; snprintf(err, en, "%s", patch); goto out; }
        if (n < (int64_t)BLK) xd3_set_flags(&stream, XD3_FLUSH | stream.flags);
        xd3_avail_input(&stream, in, (usize_t)n);
    process:
        if (cancel && *cancel) { rc = 0; goto out; }
        ret = xd3_decode_input(&stream);
        switch (ret) {
        case XD3_INPUT: if (n < (int64_t)BLK) goto done; continue;
        case XD3_OUTPUT:
            if (vfs_write(of, stream.next_out, stream.avail_out) != (int64_t)stream.avail_out) { rc = OPE_WRITE; snprintf(err, en, "%s", dst); goto out; }
            if (cb) cb(stream.avail_out, NULL);
            xd3_consume_output(&stream);
            goto process;
        case XD3_GETSRCBLK: {
            cblk_t *c = NULL, *old = &cache[0];
            for (int i = 0; i < NCACHE; i++) {
                if (cache[i].d && cache[i].no == source.getblkno) { c = &cache[i]; break; }
                if (!cache[i].d || cache[i].used < old->used) old = &cache[i];
            }
            if (!c) {
                c = old;
                if (!c->d) c->d = malloc(BLK);
                if (!c->d) { rc = OPE_MEM; goto out; }
                int64_t r = vfs_seek(sf, (uint64_t)source.getblkno * BLK) == 0 ? vfs_read(sf, c->d, BLK) : -1;
                if (r < 0) { rc = OPE_READ; snprintf(err, en, "%s", src); goto out; }
                c->no = source.getblkno; c->len = (usize_t)r;
            }
            c->used = ++tick;
            source.curblkno = c->no; source.curblk = c->d; source.onblk = c->len;
            goto process;
        }
        case XD3_GOTHEADER: case XD3_WINSTART: case XD3_WINFINISH: goto process;
        default:
            rc = OPE_READ;
            snprintf(err, en, "xdelta: %s", stream.msg ? stream.msg : "invalid patch (LZMA-compressed xdelta patches are not supported)");
            goto out;
        }
    }
done:
    rc = 0;
out:
    xd3_close_stream(&stream);
    xd3_free_stream(&stream);
    for (int i = 0; i < NCACHE; i++) free(cache[i].d);
    free(in);
    vfs_close(pf); vfs_close(sf); vfs_close(of);
    if (rc || (cancel && *cancel)) vfs_unlink(dst);
    return rc;
}

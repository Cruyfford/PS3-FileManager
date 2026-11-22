#include "save_pfd.h"
#include "vfs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t read_be64(const unsigned char *p)
{
    uint64_t n=0;
    for (int i=0;i<8;i++) n=(n<<8)|p[i];
    return n;
}

/* PFD header is 0x10, header key 0x10, signature 0x40,
 * then hash-table header (capacity, reserved, used) at 0x60.
 * Only inspect these fields. This is NOT a cryptographic verification. */
int pfd_probe_save(const char *savedir, pfd_probe_info_t *out)
{
    if (out) memset(out,0,sizeof(*out));
    if (!savedir || !*savedir || strlen(savedir)>900) return -1;
    char path[1024];
    snprintf(path,sizeof(path),"%s/PARAM.PFD",savedir);
    if (!vfs_exists(path)) return 0;
    vfs_file_t *f=vfs_open_read(path);
    if (!f) return -1;
    const uint64_t sz=vfs_size(f);
    unsigned char buf[120];
    int64_t got=(sz>=sizeof(buf)) ? vfs_read(f,buf,sizeof(buf)) : -1;
    vfs_close(f);
    if (got!=(int64_t)sizeof(buf) ||
        read_be64(buf)!=0x50464442ULL) return -1;
    uint64_t ver=read_be64(buf+8);
    if (ver!=3 && ver!=4) return -1;
    uint64_t capacity=read_be64(buf+0x60);
    uint64_t reserved=read_be64(buf+0x68);
    uint64_t used=read_be64(buf+0x70);
    if (!capacity || capacity>2048 || reserved>2048 || used>reserved ||
        0x60ULL+24ULL+capacity*8ULL+reserved*272ULL+capacity*20ULL>sz)
        return -1;
    if (out) {
        out->version=(unsigned)ver;
        out->entries=(unsigned)used;
        out->reserved=(unsigned)reserved;
    }
    return 1;
}

/* Only the PARAM.SFO-only case can be resigned without per-file Secure File IDs.
 * Never take this check as authorization to re-encrypt a protected game file. */
int pfd_probe_sfo_only(const char *savedir)
{
    pfd_probe_info_t info;
    if (pfd_probe_save(savedir,&info)!=1 || info.entries!=1) return 0;
    char path[1024];
    snprintf(path,sizeof(path),"%s/PARAM.PFD",savedir);
    vfs_file_t *f=vfs_open_read(path);
    if (!f) return 0;
    unsigned char data[32768];
    int64_t n=vfs_read(f,data,sizeof(data));
    vfs_close(f);
    if (n<120) return 0;
    uint64_t cap=read_be64(data+0x60);
    if (cap>2048) return 0;
    uint64_t off=0x60+24+cap*8+8;
    if (off+65>(uint64_t)n) return 0;
    /* The single entry must be the exact PARAM.SFO record and must not
     * have a bogus chaining index which could loop in the signing backend. */
    const uint64_t entry=off-8;
    uint64_t chained=read_be64(data+entry);
    return chained>=info.reserved &&
           !strncmp((const char *)(data+off),"PARAM.SFO",9) &&
           data[off+9]==0;
}

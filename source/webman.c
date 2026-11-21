/* webMAN MOD: mount ISO / JB folder games via its local web server. */
#include "webman.h"
#include "vfs.h"
#include "ftp.h"
#include <net/net.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define WMTMP "/dev_hdd0/tmp/wmtmp"

static const char *ext(const char *n) { const char *d = strrchr(n, '.'); return d ? d + 1 : ""; }

int webman_is_game(const char *name, int is_dir, const char *full)
{
    if (is_dir) {
        char p[1400];
        snprintf(p, sizeof(p), "%s/PS3_GAME/PARAM.SFO", full);
        return vfs_exists(p);
    }
    const char *e = ext(name);
    if (!strcasecmp(e, "iso")) return 1;
    if (strstr(full, "PSXISO") && (!strcasecmp(e, "bin") || !strcasecmp(e, "img"))) return 1;
    if (strstr(full, "PS2ISO") && (!strcasecmp(e, "bin") || !strcasecmp(e, "img"))) return 1;
    return 0;
}

int webman_resolve(const char *path, char *out, int n)
{
    if (vfs_backend(path) == VFS_NATIVE) { snprintf(out, n, "%s", path); return WM_OK; }
    /* exFAT / NTFS: prepISO (bundled with webMAN) creates <name>.ntfs[TYPE] entries in wmtmp */
    const char *bn = strrchr(path, '/');
    bn = bn ? bn + 1 : path;
    char base[256];
    snprintf(base, sizeof(base), "%s", bn);
    char *dot = strrchr(base, '.'); if (dot && strcasecmp(dot, ".iso") == 0) *dot = 0;
    vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * 2048);
    if (!e) return WM_NO_CACHE;
    int cnt = vfs_list(WMTMP, e, 2048), found = 0;
    size_t bl = strlen(base);
    for (int i = 0; i < cnt && !found; i++) {
        if (e[i].is_dir || strncasecmp(e[i].name, base, bl)) continue;
        if (!strstr(e[i].name, ".ntfs[")) continue;
        snprintf(out, n, "%s/%s", WMTMP, e[i].name);
        found = 1;
    }
    free(e);
    return found ? WM_OK : WM_NO_CACHE;
}

static void url_encode(const char *in, char *out, int n)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < n - 4; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/-_.~[]()", *p)) out[o++] = *p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

static int http_get(const char *path_and_query)
{
    if (net_up() < 0) return WM_NOT_RUNNING;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return WM_NOT_RUNNING;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(80); a.sin_addr.s_addr = htonl(0x7F000001);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { close(s); return WM_NOT_RUNNING; }
    char req[2400];
    int l = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path_and_query);
    if (send(s, req, l, 0) != l) { close(s); return WM_HTTP_ERR; }
    char resp[512];
    int r = recv(s, resp, sizeof(resp) - 1, 0);
    close(s);
    if (r <= 0) return WM_HTTP_ERR;
    resp[r] = 0;
    return (strstr(resp, " 200") || strstr(resp, " 30")) ? WM_OK : WM_HTTP_ERR;
}

int webman_mount(const char *path)
{
    char enc[2048], q[2200];
    url_encode(path, enc, sizeof(enc));
    snprintf(q, sizeof(q), "/mount_ps3%s", enc);
    return http_get(q);
}

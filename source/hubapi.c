/* "Phone link": tiny HTTP/JSON API on port 8080 used by the PS3 Hub iPhone app.
 * Works on every file system the File Manager can open (HDD, FAT32, exFAT, NTFS, ext).
 *   GET  /api/info                     GET  /api/volumes
 *   GET  /api/list?path=P              GET  /api/file?path=P        (download)
 *   PUT  /api/file?path=P   (upload)   POST /api/mkdir?path=P
 *   POST /api/delete?path=P            POST /api/rename?from=A&to=B
 *   POST /api/install?path=P (PKG)     GET  /api/status */
#include "hubapi.h"
#include "ftp.h"
#include "vfs.h"
#include <net/net.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>

#define HUB_PORT 8080
#define IOBUF (512 * 1024)

static volatile int running, stop_req;
static int lfd = -1;
static volatile int active = -1;
static sys_ppu_thread_t thr;
static volume_t hv[24];
static volatile int hv_n;
static char install_req[1024];
static volatile int install_pending;
static char status_json[1024] = "{\"busy\":false}";
static sys_mutex_t mtx; static int mtx_ok;

void hub_set_volumes(const volume_t *v, int n) { if (n > 24) n = 24; memcpy(hv, v, sizeof(volume_t) * n); hv_n = n; }
int hub_running(void) { return running; }

int hub_take_install(char *out, int n)
{
    if (!install_pending) return 0;
    snprintf(out, n, "%s", install_req);
    install_pending = 0;
    return 1;
}

void hub_set_status(const char *json)
{
    if (mtx_ok) sysMutexLock(mtx, 0);
    snprintf(status_json, sizeof(status_json), "%s", json);
    if (mtx_ok) sysMutexUnlock(mtx);
}

/* ------------------------------------------------------------ helpers */
static int send_all(int s, const void *d, size_t n)
{
    const char *p = d;
    while (n) {
        int r = send(s, p, n > 65536 ? 65536 : n, 0);
        if (r <= 0) return 0;
        p += r; n -= r;
    }
    return 1;
}

static void respond(int s, int code, const char *type, const char *body, size_t len)
{
    char h[400];
    const char *msg = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 400 ? "Bad Request" : code == 409 ? "Conflict" : "Error";
    int hl = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                      code, msg, type, (unsigned)len);
    send_all(s, h, hl);
    if (len) send_all(s, body, len);
}

static void json(int s, int code, const char *body) { respond(s, code, "application/json; charset=utf-8", body, strlen(body)); }
static void ok(int s) { json(s, 200, "{\"ok\":true}"); }
static void fail(int s, int code, const char *why) { char b[300]; snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\"}", why); json(s, code, b); }

static void urldec(const char *in, char *out, int n)
{
    int o = 0;
    for (const char *p = in; *p && *p != '&' && o < n - 1; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { char h[3] = { p[1], p[2], 0 }; out[o++] = (char)strtol(h, NULL, 16); p += 2; }
        else if (*p == '+') out[o++] = ' ';
        else out[o++] = *p;
    }
    out[o] = 0;
}

static int qparam(const char *q, const char *name, char *out, int n)
{
    size_t l = strlen(name);
    for (const char *p = q; p && *p; ) {
        if (!strncmp(p, name, l) && p[l] == '=') { urldec(p + l + 1, out, n); return 1; }
        p = strchr(p, '&'); if (p) p++;
    }
    out[0] = 0;
    return 0;
}

/* JSON string escaping into a growing buffer */
typedef struct { char *d; size_t n, cap; } sb_t;
static void sb_add(sb_t *b, const char *s, size_t l)
{
    if (b->n + l + 1 > b->cap) { size_t c = (b->n + l + 1) * 2 + 1024; char *d = realloc(b->d, c); if (!d) return; b->d = d; b->cap = c; }
    memcpy(b->d + b->n, s, l); b->n += l; b->d[b->n] = 0;
}
static void sb_str(sb_t *b, const char *s) { sb_add(b, s, strlen(s)); }
static void sb_jstr(sb_t *b, const char *s)
{
    sb_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char e[8];
        if (*p == '"' || *p == '\\') { e[0] = '\\'; e[1] = *p; sb_add(b, e, 2); }
        else if (*p < 32) { snprintf(e, sizeof(e), "\\u%04x", *p); sb_add(b, e, 6); }
        else sb_add(b, (const char *)p, 1);
    }
    sb_add(b, "\"", 1);
}

static int path_ok(const char *p) { return p[0] && !strstr(p, "/../") && !strstr(p, "/.."); }

static int delete_tree(const char *path, int depth)
{
    int d = 0; uint64_t sz;
    if (vfs_stat(path, &d, &sz)) return -1;
    if (!d) return vfs_unlink(path);
    if (depth > 32) return -1;
    vfs_entry_t *e = malloc(sizeof(vfs_entry_t) * 1024);
    if (!e) return -1;
    int n, r = 0;
    char c[1100];
    while ((n = vfs_list(path, e, 1024)) > 0 && !r) {
        for (int i = 0; i < n && !r; i++) {
            size_t l = strlen(path);
            snprintf(c, sizeof(c), l && path[l - 1] == '/' ? "%s%s" : "%s/%s", path, e[i].name);
            r = delete_tree(c, depth + 1);
        }
    }
    free(e);
    return r ? r : vfs_rmdir(path);
}

/* ------------------------------------------------------------ handlers */
static void h_volumes(int s)
{
    sb_t b = { 0 };
    sb_str(&b, "{\"volumes\":[");
    for (int i = 0; i < hv_n; i++) {
        char t[200];
        if (i) sb_str(&b, ",");
        sb_str(&b, "{\"label\":"); sb_jstr(&b, hv[i].label);
        sb_str(&b, ",\"root\":"); sb_jstr(&b, hv[i].root);
        sb_str(&b, ",\"fs\":"); sb_jstr(&b, hv[i].fs_name);
        snprintf(t, sizeof(t), ",\"kind\":%d,\"writable\":%s,\"free\":%llu}", hv[i].kind, hv[i].writable ? "true" : "false",
                 (unsigned long long)vfs_free_space(hv[i].root));
        sb_str(&b, t);
    }
    sb_str(&b, "]}");
    json(s, 200, b.d ? b.d : "{}");
    free(b.d);
}

static void h_list(int s, const char *path)
{
    vfs_entry_t *e = malloc(sizeof(vfs_entry_t) * 4096);
    if (!e) { fail(s, 500, "memory"); return; }
    int n = vfs_list(path, e, 4096);
    if (n < 0) { free(e); fail(s, 404, "cannot open folder"); return; }
    sb_t b = { 0 };
    sb_str(&b, "{\"path\":"); sb_jstr(&b, path); sb_str(&b, ",\"entries\":[");
    for (int i = 0; i < n; i++) {
        char t[96];
        if (i) sb_str(&b, ",");
        sb_str(&b, "{\"name\":"); sb_jstr(&b, e[i].name);
        snprintf(t, sizeof(t), ",\"dir\":%s,\"size\":%llu}", e[i].is_dir ? "true" : "false", (unsigned long long)e[i].size);
        sb_str(&b, t);
    }
    sb_str(&b, "]}");
    free(e);
    json(s, 200, b.d ? b.d : "{}");
    free(b.d);
}

static void h_get(int s, const char *path, uint8_t *buf)
{
    vfs_file_t *f = vfs_open_read(path);
    if (!f) { fail(s, 404, "cannot open file"); return; }
    uint64_t sz = vfs_size(f);
    const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
    char h[1400];
    int hl = snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %llu\r\n"
                      "Content-Disposition: attachment; filename=\"%s\"\r\nConnection: close\r\n\r\n", (unsigned long long)sz, bn);
    if (send_all(s, h, hl)) {
        for (;;) {
            int64_t r = vfs_read(f, buf, IOBUF);
            if (r <= 0 || !send_all(s, buf, (size_t)r)) break;
        }
    }
    vfs_close(f);
}

static void h_put(int s, const char *path, uint64_t len, const char *pre, int pre_n, uint8_t *buf)
{
    char tmp[1100]; snprintf(tmp, sizeof(tmp), "%s.part", path);
    vfs_file_t *f = vfs_open_write(tmp);
    if (!f) { fail(s, 409, "cannot create file"); return; }
    uint64_t got = 0; int good = 1;
    if (pre_n > 0) { if (vfs_write(f, pre, pre_n) != pre_n) good = 0; got += pre_n; }
    while (good && got < len) {
        uint64_t want = len - got; if (want > IOBUF) want = IOBUF;
        int r = recv(s, buf, (int)want, 0);
        if (r <= 0) { good = 0; break; }
        if (vfs_write(f, buf, r) != r) { good = 0; break; }
        got += r;
    }
    vfs_close(f);
    if (!good || got != len) { vfs_unlink(tmp); fail(s, 500, "upload failed"); return; }
    vfs_unlink(path);
    if (vfs_rename(tmp, path)) { vfs_unlink(tmp); fail(s, 500, "rename failed"); return; }
    ok(s);
}

static void session(int s, uint8_t *buf)
{
    char req[8192];
    int n = 0, hend = -1;
    while (n < (int)sizeof(req) - 1) {
        int r = recv(s, req + n, sizeof(req) - 1 - n, 0);
        if (r <= 0) return;
        n += r; req[n] = 0;
        char *e = strstr(req, "\r\n\r\n");
        if (e) { hend = (int)(e - req) + 4; break; }
    }
    if (hend < 0) return;
    char method[8] = "", target[4096] = "";
    sscanf(req, "%7s %4095s", method, target);
    uint64_t clen = 0;
    char *cl = strcasestr(req, "\r\nContent-Length:");
    if (cl && cl < req + hend) clen = strtoull(cl + 17, NULL, 10);
    char *q = strchr(target, '?');
    if (q) *q++ = 0; else q = "";
    char path[1100];
    qparam(q, "path", path, sizeof(path));

    if (!strcmp(method, "OPTIONS")) { respond(s, 200, "text/plain", "", 0); return; }
    if (!strcmp(target, "/api/info")) {
        char b[300]; snprintf(b, sizeof(b), "{\"app\":\"File Manager\",\"id\":\"CRPK00001\",\"api\":1,\"ip\":\"%s\"}", ftp_ip());
        json(s, 200, b); return;
    }
    if (!strcmp(target, "/api/volumes")) { h_volumes(s); return; }
    if (!strcmp(target, "/api/status")) {
        char b[1100];
        if (mtx_ok) sysMutexLock(mtx, 0);
        snprintf(b, sizeof(b), "%s", status_json);
        if (mtx_ok) sysMutexUnlock(mtx);
        json(s, 200, b); return;
    }
    if (!strcmp(target, "/api/list")) { if (!path_ok(path)) { fail(s, 400, "path"); return; } h_list(s, path); return; }
    if (!strcmp(target, "/api/file")) {
        if (!path_ok(path)) { fail(s, 400, "path"); return; }
        if (!strcmp(method, "GET")) h_get(s, path, buf);
        else if (!strcmp(method, "PUT") || !strcmp(method, "POST")) h_put(s, path, clen, req + hend, n - hend, buf);
        else fail(s, 400, "method");
        return;
    }
    if (!strcmp(target, "/api/mkdir")) { if (!path_ok(path)) { fail(s, 400, "path"); return; } if (vfs_mkdir(path) == 0 || vfs_exists(path)) ok(s); else fail(s, 409, "mkdir failed"); return; }
    if (!strcmp(target, "/api/delete")) { if (!path_ok(path) || strlen(path) < 8) { fail(s, 400, "path"); return; } if (delete_tree(path, 0) == 0) ok(s); else fail(s, 409, "delete failed"); return; }
    if (!strcmp(target, "/api/rename")) {
        char a[1100], b[1100]; qparam(q, "from", a, sizeof(a)); qparam(q, "to", b, sizeof(b));
        if (!path_ok(a) || !path_ok(b)) { fail(s, 400, "path"); return; }
        if (vfs_rename(a, b) == 0) ok(s); else fail(s, 409, "rename failed");
        return;
    }
    if (!strcmp(target, "/api/install")) {
        size_t l = strlen(path);
        if (l < 5 || strcasecmp(path + l - 4, ".pkg") || !vfs_exists(path)) { fail(s, 400, "not a pkg"); return; }
        if (install_pending) { fail(s, 409, "busy"); return; }
        snprintf(install_req, sizeof(install_req), "%s", path);
        install_pending = 1;
        ok(s); return;
    }
    fail(s, 404, "unknown");
}

static void server(void *a)
{
    (void)a;
    uint8_t *buf = malloc(IOBUF);
    while (!stop_req && buf) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int c = accept(lfd, (struct sockaddr *)&ca, &cl);
        if (c < 0) { if (stop_req) break; usleep(100000); continue; }
        active = c;
        session(c, buf);
        active = -1;
        shutdown(c, SHUT_RDWR);
        close(c);
    }
    free(buf);
    running = 0;
    sysThreadExit(0);
}

int hub_start(void)
{
    if (running) return 1;
    if (!mtx_ok) { sys_mutex_attr_t at; memset(&at, 0, sizeof(at)); at.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO; at.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE; at.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED; at.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE; if (sysMutexCreate(&mtx, &at) == 0) mtx_ok = 1; }
    if (net_up() < 0) return 0;
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return 0;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(HUB_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) || listen(lfd, 4)) { close(lfd); lfd = -1; return 0; }
    stop_req = 0; running = 1;
    if (sysThreadCreate(&thr, server, NULL, 1300, 0x20000, THREAD_JOINABLE, "hubapi") != 0) { running = 0; close(lfd); lfd = -1; return 0; }
    return 1;
}

void hub_stop_async(void)
{
    if (!running) return;
    stop_req = 1;
    if (active >= 0) shutdown(active, SHUT_RDWR);
    if (lfd >= 0) { shutdown(lfd, SHUT_RDWR); close(lfd); lfd = -1; }
}

void hub_stop(void)
{
    if (!running) return;
    hub_stop_async();
    u64 rv; sysThreadJoin(thr, &rv);
    running = 0;
}

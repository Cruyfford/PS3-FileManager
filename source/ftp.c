/* Minimal FTP server (passive mode, one client at a time) over the VFS.
 * Virtual root: /dev_hdd0, /dev_usb000 ... (native), /ntfs0 /ext0 (NTFS/ext), /exfat0 ... (exFAT/FAT via FatFs). */
#include "ftp.h"
#include <net/net.h>
#include <net/netctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define FTP_PORT 21
#define DATA_PORT_BASE 50000

static volatile int running, stop_req, client_on;
static int listen_fd = -1;
static sys_ppu_thread_t thr;
static char ip_str[32];
static volume_t vols[24];
static volatile int vol_n;
static int net_ready;
static int ftp_port_num = 21;
static char last_err[64];

int ftp_port(void) { return ftp_port_num; }
const char *ftp_error(void) { return last_err; }

void ftp_set_volumes(const volume_t *v, int n)
{
    if (n > 24) n = 24;
    memcpy(vols, v, sizeof(volume_t) * n);
    vol_n = n;
}

const char *ftp_ip(void) { return ip_str; }
int ftp_running(void) { return running; }
int ftp_clients(void) { return client_on; }

static void send_str(int s, const char *t) { send(s, t, strlen(t), 0); }
static void reply(int s, const char *fmt, const char *arg)
{
    char b[1200];
    snprintf(b, sizeof(b), fmt, arg ? arg : "");
    send_str(s, b);
}

/* ---------- path mapping ---------- */

/* virtual name for a volume root */
static void vol_vname(const volume_t *v, char *out, int n)
{
    if (v->fs == VFS_NATIVE) { snprintf(out, n, "%s", v->root + 1); return; }     /* dev_hdd0 */
    if (v->fs == VFS_NTFS) {                                                      /* ntfs0:/ -> ntfs0 */
        snprintf(out, n, "%s", v->root);
        char *c = strchr(out, ':'); if (c) *c = 0;
        return;
    }
    snprintf(out, n, "exfat%c", v->root[0]);                                     /* 0:/ -> exfat0 */
}

/* virtual "/a/b" -> real path; returns 0 for the virtual root */
static int to_real(const char *vpath, char *real, int n)
{
    if (!strcmp(vpath, "/") || !vpath[0]) return 0;
    const char *p = vpath + 1;
    const char *sl = strchr(p, '/');
    int fl = sl ? (int)(sl - p) : (int)strlen(p);
    const char *rest = sl ? sl + 1 : "";
    char first[64];
    if (fl > 62) fl = 62;
    memcpy(first, p, fl); first[fl] = 0;

    if (!strncmp(first, "exfat", 5) && first[5] >= '0' && first[5] <= '9') { snprintf(real, n, "%c:/%s", first[5], rest); return 1; }
    if (!strncmp(first, "ntfs", 4) || !strncmp(first, "ext", 3)) { snprintf(real, n, "%s:/%s", first, rest); return 1; }
    snprintf(real, n, "/%s%s%s", first, rest[0] ? "/" : "", rest);
    return 1;
}

static void normalize(const char *cwd, const char *arg, char *out, int n)
{
    char tmp[1100];
    if (arg[0] == '/') snprintf(tmp, sizeof(tmp), "%s", arg);
    else snprintf(tmp, sizeof(tmp), "%s/%s", strcmp(cwd, "/") ? cwd : "", arg);
    /* resolve . and .. */
    char *parts[64]; int np = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp, "/", &save); t && np < 64; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        parts[np++] = t;
    }
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < np && o < n - 2; i++) o += snprintf(out + o, n - o, "/%s", parts[i]);
    if (!np) snprintf(out, n, "/");
}

/* ---------- data connection ---------- */

static int pasv_listen(int *port_out)
{
    static int next = 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    for (int tries = 0; tries < 50; tries++) {
        int port = DATA_PORT_BASE + (next++ % 1000);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(s, 1) == 0) { *port_out = port; return s; }
    }
    close(s);
    return -1;
}

static int data_accept(int pasv)
{
    if (pasv < 0) return -1;
    struct sockaddr_in ca; socklen_t cl = sizeof(ca);
    int d = accept(pasv, (struct sockaddr *)&ca, &cl);
    close(pasv);
    return d;
}

static void list_dir(int ctrl, int data, const char *vpath, int names_only)
{
    char real[1100], line[600];
    if (!to_real(vpath, real, sizeof(real))) {
        for (int i = 0; i < vol_n; i++) {
            char vn[64];
            vol_vname(&vols[i], vn, sizeof(vn));
            if (vols[i].kind == 4) continue;   /* packages shortcut duplicates dev_hdd0 */
            if (names_only) snprintf(line, sizeof(line), "%s\r\n", vn);
            else snprintf(line, sizeof(line), "drwxrwxrwx 1 ps3 ps3 0 Jan 01 2026 %s\r\n", vn);
            send_str(data, line);
        }
        return;
    }
    vfs_entry_t *e = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * 4096);
    if (!e) return;
    int n = vfs_list(real, e, 4096);
    for (int i = 0; i < n; i++) {
        if (names_only) snprintf(line, sizeof(line), "%s\r\n", e[i].name);
        else snprintf(line, sizeof(line), "%s 1 ps3 ps3 %llu Jan 01 2026 %s\r\n",
                      e[i].is_dir ? "drwxrwxrwx" : "-rw-rw-rw-", (unsigned long long)e[i].size, e[i].name);
        send_str(data, line);
    }
    free(e);
    (void)ctrl;
}

/* ---------- session ---------- */

static int recv_line(int s, char *buf, int n)
{
    int o = 0;
    while (o < n - 1) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[o++] = c;
    }
    buf[o] = 0;
    return o;
}

static void session(int c)
{
    char cwd[1100] = "/", line[1200], path[1100], real[1100], rnfr[1100] = "";
    int pasv = -1;
    uint8_t *buf = (uint8_t *)malloc(1024 * 1024);
    uint64_t rest = 0;
    reply(c, "220 PS3 File Manager FTP ready\r\n", NULL);

    while (!stop_req && buf) {
        if (recv_line(c, line, sizeof(line)) < 0) break;
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0; else arg = "";
        for (char *p = line; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;

        if (!strcmp(line, "USER")) reply(c, "331 Any password\r\n", NULL);
        else if (!strcmp(line, "PASS")) reply(c, "230 Logged in\r\n", NULL);
        else if (!strcmp(line, "SYST")) reply(c, "215 UNIX Type: L8\r\n", NULL);
        else if (!strcmp(line, "FEAT")) reply(c, "211-Features:\r\n SIZE\r\n REST STREAM\r\n UTF8\r\n211 End\r\n", NULL);
        else if (!strcmp(line, "OPTS")) reply(c, "200 OK\r\n", NULL);
        else if (!strcmp(line, "NOOP")) reply(c, "200 OK\r\n", NULL);
        else if (!strcmp(line, "TYPE")) reply(c, "200 Type set\r\n", NULL);
        else if (!strcmp(line, "PWD") || !strcmp(line, "XPWD")) { snprintf(path, sizeof(path), "257 \"%s\"\r\n", cwd); send_str(c, path); }
        else if (!strcmp(line, "CWD") || !strcmp(line, "CDUP")) {
            normalize(cwd, !strcmp(line, "CDUP") ? ".." : arg, path, sizeof(path));
            int d = 1;
            if (to_real(path, real, sizeof(real)) && (vfs_stat(real, &d, NULL) != 0 || !d)) reply(c, "550 No such directory\r\n", NULL);
            else { snprintf(cwd, sizeof(cwd), "%s", path); reply(c, "250 OK\r\n", NULL); }
        }
        else if (!strcmp(line, "PASV")) {
            if (pasv >= 0) close(pasv);
            int port = 0;
            pasv = pasv_listen(&port);
            if (pasv < 0) { reply(c, "425 Can't open data port\r\n", NULL); continue; }
            int a, b2, cc, dd;
            if (sscanf(ip_str, "%d.%d.%d.%d", &a, &b2, &cc, &dd) != 4) { a = 127; b2 = 0; cc = 0; dd = 1; }
            snprintf(path, sizeof(path), "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\r\n", a, b2, cc, dd, port >> 8, port & 255);
            send_str(c, path);
        }
        else if (!strcmp(line, "LIST") || !strcmp(line, "NLST")) {
            const char *a2 = (arg[0] == '-') ? "" : arg;     /* ignore "-la" */
            normalize(cwd, a2[0] ? a2 : ".", path, sizeof(path));
            reply(c, "150 Listing\r\n", NULL);
            int d = data_accept(pasv); pasv = -1;
            if (d < 0) { reply(c, "425 No data connection\r\n", NULL); continue; }
            list_dir(c, d, path, line[0] == 'N');
            close(d);
            reply(c, "226 Done\r\n", NULL);
        }
        else if (!strcmp(line, "SIZE")) {
            normalize(cwd, arg, path, sizeof(path));
            uint64_t sz = 0; int d = 0;
            if (to_real(path, real, sizeof(real)) && vfs_stat(real, &d, &sz) == 0 && !d) {
                snprintf(path, sizeof(path), "213 %llu\r\n", (unsigned long long)sz); send_str(c, path);
            } else reply(c, "550 No such file\r\n", NULL);
        }
        else if (!strcmp(line, "REST")) { rest = strtoull(arg, NULL, 10); reply(c, "350 Restarting\r\n", NULL); }
        else if (!strcmp(line, "RETR")) {
            normalize(cwd, arg, path, sizeof(path));
            vfs_file_t *f = to_real(path, real, sizeof(real)) ? vfs_open_read(real) : NULL;
            if (!f) { reply(c, "550 Can't open\r\n", NULL); continue; }
            if (rest) { vfs_seek(f, rest); rest = 0; }
            reply(c, "150 Sending\r\n", NULL);
            int d = data_accept(pasv); pasv = -1;
            if (d < 0) { vfs_close(f); reply(c, "425 No data connection\r\n", NULL); continue; }
            int ok = 1;
            for (;;) {
                int64_t r = vfs_read(f, buf, 1024 * 1024);
                if (r <= 0) { ok = r == 0; break; }
                int64_t o = 0;
                while (o < r) { int w = send(d, buf + o, (size_t)(r - o), 0); if (w <= 0) { ok = 0; break; } o += w; }
                if (!ok || stop_req) break;
            }
            vfs_close(f); close(d);
            reply(c, ok ? "226 Done\r\n" : "426 Transfer aborted\r\n", NULL);
        }
        else if (!strcmp(line, "STOR")) {
            normalize(cwd, arg, path, sizeof(path));
            vfs_file_t *f = to_real(path, real, sizeof(real)) ? vfs_open_write(real) : NULL;
            if (!f) { reply(c, "550 Can't create\r\n", NULL); continue; }
            reply(c, "150 Receiving\r\n", NULL);
            int d = data_accept(pasv); pasv = -1;
            if (d < 0) { vfs_close(f); reply(c, "425 No data connection\r\n", NULL); continue; }
            int ok = 1;
            for (;;) {
                int r = recv(d, buf, 1024 * 1024, 0);
                if (r < 0) { ok = 0; break; }
                if (r == 0) break;
                if (vfs_write(f, buf, (uint32_t)r) != r) { ok = 0; break; }
            }
            vfs_close(f); close(d);
            reply(c, ok ? "226 Done\r\n" : "451 Write error\r\n", NULL);
        }
        else if (!strcmp(line, "DELE") || !strcmp(line, "RMD") || !strcmp(line, "XRMD") || !strcmp(line, "MKD") || !strcmp(line, "XMKD")) {
            normalize(cwd, arg, path, sizeof(path));
            int ok = to_real(path, real, sizeof(real));
            int is_mkd = !strcmp(line, "MKD") || !strcmp(line, "XMKD");
            if (ok) ok = !strcmp(line, "DELE") ? vfs_unlink(real) == 0 : is_mkd ? vfs_mkdir(real) == 0 : vfs_rmdir(real) == 0;
            reply(c, ok ? "250 OK\r\n" : "550 Failed\r\n", NULL);
        }
        else if (!strcmp(line, "RNFR")) { normalize(cwd, arg, rnfr, sizeof(rnfr)); reply(c, "350 Ready\r\n", NULL); }
        else if (!strcmp(line, "RNTO")) {
            char a1[1100], b1[1100];
            normalize(cwd, arg, path, sizeof(path));
            int ok = to_real(rnfr, a1, sizeof(a1)) && to_real(path, b1, sizeof(b1)) && vfs_rename(a1, b1) == 0;
            reply(c, ok ? "250 Renamed\r\n" : "550 Rename failed\r\n", NULL);
        }
        else if (!strcmp(line, "QUIT")) { reply(c, "221 Bye\r\n", NULL); break; }
        else reply(c, "502 Not implemented\r\n", NULL);
    }
    if (pasv >= 0) close(pasv);
    free(buf);
}

static void server_thread(void *arg)
{
    (void)arg;
    while (!stop_req) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int c = accept(listen_fd, (struct sockaddr *)&ca, &cl);
        if (c < 0) { if (stop_req) break; usleep(100000); continue; }
        client_on = 1;
        session(c);
        close(c);
        client_on = 0;
    }
    running = 0;
    sysThreadExit(0);
}

static void refresh_ip(void)
{
    union net_ctl_info info;
    ip_str[0] = 0;
    if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) == 0) snprintf(ip_str, sizeof(ip_str), "%s", info.ip_address);
}

int ftp_start(void)
{
    if (running) return 1;
    last_err[0] = 0;
    if (!net_ready) {
        sysModuleLoad(SYSMODULE_NET);
        sysModuleLoad(SYSMODULE_NETCTL);
        int r = netInitialize();
        if (r < 0) { snprintf(last_err, sizeof(last_err), "netInitialize %d", r); return 0; }
        netCtlInit();
        net_ready = 1;
    }
    refresh_ip();
    if (!ip_str[0]) { snprintf(last_err, sizeof(last_err), "no IP"); }

    const int ports[] = { 21, 2121, 2221 };
    for (unsigned i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) { snprintf(last_err, sizeof(last_err), "socket %d/%d", listen_fd, net_errno); return 0; }
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons(ports[i]); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(listen_fd, 2) == 0) { ftp_port_num = ports[i]; break; }
        snprintf(last_err, sizeof(last_err), "bind %d: %d", ports[i], net_errno);
        close(listen_fd); listen_fd = -1;
    }
    if (listen_fd < 0) return 0;
    stop_req = 0; running = 1;
    if (sysThreadCreate(&thr, server_thread, NULL, 1300, 0x20000, THREAD_JOINABLE, "ftpd") != 0) {
        snprintf(last_err, sizeof(last_err), "thread");
        running = 0; close(listen_fd); listen_fd = -1; return 0;
    }
    last_err[0] = 0;
    return 1;
}

void ftp_stop(void)
{
    if (!running) return;
    stop_req = 1;
    shutdown(listen_fd, SHUT_RDWR);
    close(listen_fd);
    listen_fd = -1;
    u64 rv;
    sysThreadJoin(thr, &rv);
    running = 0;
}

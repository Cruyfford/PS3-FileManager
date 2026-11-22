/* Non-blocking CRC32 file integrity checker for PS3 native/NTFS/exFAT drives. */
#include "integrity.h"
#include "crc32_core.h"
#include "vfs.h"
#include <sys/thread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char integrity_path[1024];
static volatile int integrity_state; /* 0 idle, 1 running, 2 success, 3 error */
static sys_ppu_thread_t integrity_thread;
static int integrity_thread_created;
static uint32_t integrity_value;

static void integrity_worker(void *unused)
{
    (void)unused;
    uint8_t *data = (uint8_t *)malloc(65536);
    vfs_file_t *f = data ? vfs_open_read(integrity_path) : NULL;
    if (!f) {
        free(data);
        integrity_state = 3;
        sysThreadExit(0);
        return;
    }
    uint32_t crc = 0xFFFFFFFFu;
    int ok = 1;
    for (;;) {
        int64_t n = vfs_read(f, data, 65536);
        if (n < 0) { ok = 0; break; }
        if (!n) break;
        crc = crc32_update(crc, data, (unsigned)n);
    }
    vfs_close(f);
    free(data);
    integrity_value = ~crc;
    __sync_synchronize();
    integrity_state = ok ? 2 : 3;
    sysThreadExit(0);
}

int integrity_start(const char *path)
{
    if (integrity_state == 1 || !path || !*path) return 0;
    if (integrity_thread_created) {
        u64 status;
        sysThreadJoin(integrity_thread, &status);
        integrity_thread_created = 0;
    }
    snprintf(integrity_path, sizeof(integrity_path), "%s", path);
    integrity_state = 1;
    if (sysThreadCreate(&integrity_thread, integrity_worker, NULL, 1700, 0x10000, THREAD_JOINABLE, "crc32") != 0) {
        integrity_state = 0;
        return 0;
    }
    integrity_thread_created = 1;
    return 1;
}

int integrity_running(void) { return integrity_state == 1; }

int integrity_poll(uint32_t *crc)
{
    if (integrity_state != 2 && integrity_state != 3) return 0;
    int result = integrity_state == 2 ? 1 : -1;
    if (crc) *crc = integrity_value;
    integrity_state = 0;
    if (integrity_thread_created) {
        u64 status;
        sysThreadJoin(integrity_thread, &status);
        integrity_thread_created = 0;
    }
    return result;
}

void integrity_shutdown(void)
{
    if (integrity_thread_created) {
        u64 status;
        sysThreadJoin(integrity_thread, &status);
        integrity_thread_created = 0;
    }
    integrity_state = 0;
}

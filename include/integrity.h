#pragma once
#include <stdint.h>
/* Run a background integrity check on a file via VFS. */
int integrity_start(const char *path);
int integrity_running(void);
/* Returns 1 for a finished CRC, -1 for read error, 0 while pending. */
int integrity_poll(uint32_t *crc);
void integrity_shutdown(void);

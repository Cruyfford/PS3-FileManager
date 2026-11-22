#pragma once
#include <stdint.h>
/* Streaming CRC32 for file integrity checks. */
uint32_t crc32_update(uint32_t crc, const void *data, unsigned len);

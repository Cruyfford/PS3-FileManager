/* IEEE CRC-32 (polynomial 0xEDB88320).
 * Input CRC seed 0xFFFFFFFF; invert final CRC with ~crc. */
#include "crc32_core.h"
#include <stdint.h>

uint32_t crc32_update(uint32_t crc, const void *data, unsigned len)
{
    const unsigned char *p = (const unsigned char *)data;
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (unsigned i = 0; i < 256; i++) {
            uint32_t v = i;
            for (unsigned j = 0; j < 8; j++)
                v = (v >> 1) ^ (0xEDB88320u & (0u - (v & 1u)));
            table[i] = v;
        }
        ready = 1;
    }
    for (unsigned i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 255u] ^ (crc >> 8);
    return crc;
}

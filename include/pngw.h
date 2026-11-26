#pragma once
#include <stdint.h>
/* Minimal PNG writer (zlib). fmt: 0 = RGBA bytes, 1 = ARGB bytes (PS3 framebuffer, alpha ignored). Writes through the VFS. */
int png_write(const char *path, const uint8_t *px, int w, int h, int stride, int fmt);

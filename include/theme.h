#pragma once
#include <stdint.h>
#define THEME_DIR "/dev_hdd0/theme"
/* Extracts the theme's largest embedded image (usually its background) as JPEG/PNG bytes (malloc'd). */
int p3t_preview(const char *path, uint8_t **out, uint32_t *len);

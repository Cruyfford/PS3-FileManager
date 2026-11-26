#pragma once
#include <stdint.h>
/* PlayStation TIM images (4/8/16/24 bpp, with CLUT). */
typedef struct { int w, h, bpp; uint8_t *rgba; } tim_image_t;
/* decode one TIM at data[0]; returns bytes consumed (0 = not a TIM). clut selects the palette row. */
uint32_t tim_decode(const uint8_t *data, uint32_t len, int clut, tim_image_t *out);
void tim_free(tim_image_t *t);
/* scan any file for embedded TIMs and save each as PNG into out_dir. Returns count, -1 on error. */
int tim_extract_all(const char *file, const char *out_dir, volatile int *cancel, void (*cb)(uint64_t add, const char *cur));

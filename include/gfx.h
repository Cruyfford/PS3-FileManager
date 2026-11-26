#pragma once
#include <stdint.h>
#include <ya2d/ya2d.h>

#define SCR_W 848
#define SCR_H 512

#define RGBA8(r,g,b,a) ((((uint32_t)(r))<<24)|(((uint32_t)(g))<<16)|(((uint32_t)(b))<<8)|((uint32_t)(a)))
#define WITH_ALPHA(c,a) (((c) & 0xFFFFFF00u) | ((uint32_t)(a) & 0xFF))

enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

typedef ya2d_Texture* texture_t;

int  gfx_init(const char *font_regular, const char *font_bold);
void gfx_begin_frame(void);
void gfx_end_frame(void);
void gfx_shutdown(void);

void gfx_rect(float x, float y, float w, float h, uint32_t c);
void gfx_grad_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void gfx_grad_h(float x, float y, float w, float h, uint32_t left, uint32_t right);
void gfx_rrect(float x, float y, float w, float h, float r, uint32_t c);
void gfx_rrect_grad(float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom);
void gfx_rrect_border(float x, float y, float w, float h, float r, float t, uint32_t c);
void gfx_circle(float cx, float cy, float r, uint32_t c);
void gfx_glow(float cx, float cy, float r, uint32_t center);
void gfx_line(float x1, float y1, float x2, float y2, float t, uint32_t c);
void gfx_polyline(const float *xy, int n, float t, uint32_t c);   /* one strip, xy = x0,y0,x1,y1... */
void gfx_triangle(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c);

texture_t gfx_load_png_buffer(const void *buf, uint32_t size);
texture_t gfx_load_png_file(const char *path);
texture_t gfx_load_jpg_buffer(const void *buf, uint32_t size);
int gfx_texture_size(texture_t t, int *w, int *h);
/* downscaled copy (keeps aspect) for list thumbnails; frees src */
texture_t gfx_make_thumb(texture_t src, int maxw, int maxh);
/* 8-bit luminance texture (for video planes); pitch = width */
texture_t gfx_create_l8(int w, int h);
texture_t gfx_create_argb(int w, int h);
uint8_t *gfx_texture_data(texture_t t);
texture_t gfx_create_rgba(int w, int h, const uint8_t *rgba);
/* draw Y/U/V L8 planes with tiny3d's YUV shader; uw/vh = used fraction of each texture */
void gfx_draw_yuv(texture_t ty, texture_t tu, texture_t tv, float x, float y, float w, float h, float uw, float vh);
void gfx_texture(texture_t t, float x, float y, float w, float h, uint32_t tint);
void gfx_free_texture(texture_t t);

/* text: y is the top of the line box. Arabic is shaped and reordered automatically. */
int  gfx_text_width(const char *utf8, int size, int bold);
int  gfx_text(float x, float y, const char *utf8, int size, int bold, uint32_t color, int align);
/* same as gfx_text but truncates with an ellipsis to fit max_w */
int  gfx_text_fit(float x, float y, float max_w, const char *utf8, int size, int bold, uint32_t color, int align);

/* PlayStation face-button glyphs drawn as vector shapes */
enum { BTN_CROSS, BTN_CIRCLE, BTN_SQUARE, BTN_TRIANGLE, BTN_START, BTN_SELECT, BTN_L1, BTN_R1 };
float gfx_button_icon(float x, float y, int btn, float size);

uint64_t gfx_frame(void);
/* PNG screenshot of the screen (written on a background thread) */
int gfx_screenshot(const char *path);
int gfx_screenshot_result(void);   /* 1 saved, -1 failed, 0 nothing new */

/* Rendering layer: tiny3d (via ya2d init) + FreeType glyph cache with real metrics. */
#include "gfx.h"
#include "arabic.h"
#include <tiny3d.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mutex.h>

#define GLYPH_SLOT   64
#define GLYPH_SLOTS  384
#define HASH_SIZE    1024
#define RENDER_SCALE 1.5f

typedef struct {
    uint32_t key;
    uint32_t *pix;
    uint32_t offset;
    int16_t w, h, left, top, adv;   /* in render pixels */
    uint64_t last;
    uint8_t used;
} glyph_t;

static FT_Library ft;
static FT_Face face[2];
static int face_ok[2];
static glyph_t glyphs[GLYPH_SLOTS];
static int16_t hash_tab[HASH_SIZE];
static uint64_t frame_no = 1;
static float zc;
static sys_mutex_t tex_lock;
static int tex_lock_ok;
static void TL(void) { if (tex_lock_ok) sysMutexLock(tex_lock, 0); }
static void TU(void) { if (tex_lock_ok) sysMutexUnlock(tex_lock); }

uint64_t gfx_frame(void) { return frame_no; }

static inline float Z(void) { zc -= 0.5f; if (zc < 1.0f) zc = 1.0f; return zc; }

int gfx_init(const char *font_regular, const char *font_bold)
{
    ya2d_init();
    {
        sys_mutex_attr_t a;
        memset(&a, 0, sizeof(a));
        a.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
        a.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
        a.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
        a.attr_adaptive = SYS_MUTEX_ATTR_ADAPTIVE;
        strcpy(a.name, "texlock");
        tex_lock_ok = sysMutexCreate(&tex_lock, &a) == 0;
    }
    ya2d_screenSetClearColor(RGBA8(10, 10, 20, 255));

    /* carve glyph slots out of ya2d's static texture area, 128-byte aligned */
    uintptr_t p = (uintptr_t)ya2d_texturePointer;
    p = (p + 127) & ~(uintptr_t)127;
    for (int i = 0; i < GLYPH_SLOTS; i++) {
        glyphs[i].pix = (uint32_t *)p;
        glyphs[i].offset = tiny3d_TextureOffset((void *)p);
        glyphs[i].used = 0;
        p += GLYPH_SLOT * GLYPH_SLOT * 4;
    }
    ya2d_texturePointer = (u32 *)p;
    for (int i = 0; i < HASH_SIZE; i++) hash_tab[i] = -1;

    FT_Init_FreeType(&ft);
    face_ok[0] = (FT_New_Face(ft, font_regular, 0, &face[0]) == 0);
    face_ok[1] = (FT_New_Face(ft, font_bold, 0, &face[1]) == 0);
    if (!face_ok[1] && face_ok[0]) { face[1] = face[0]; face_ok[1] = 1; }
    if (!face_ok[0]) {
        /* fall back to the system font if our bundled font is missing */
        face_ok[0] = (FT_New_Face(ft, "/dev_flash/data/font/SCE-PS3-SR-R-LATIN2.TTF", 0, &face[0]) == 0);
        face[1] = face[0]; face_ok[1] = face_ok[0];
    }
    return face_ok[0];
}

void gfx_begin_frame(void)
{
    ya2d_screenClear();
    ya2d_screenBeginDrawing();
    zc = 60000.0f;
}

void gfx_end_frame(void)
{
    ya2d_screenFlip();
    frame_no++;
}

void gfx_shutdown(void)
{
    if (face_ok[0]) FT_Done_Face(face[0]);
    if (face_ok[1] && face[1] != face[0]) FT_Done_Face(face[1]);
    FT_Done_FreeType(ft);
    ya2d_deinit();
}

/* ---------------- shapes ---------------- */

void gfx_rect(float x, float y, float w, float h, uint32_t c)
{
    float z = Z();
    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(x, y, z);         tiny3d_VertexColor(c);
    tiny3d_VertexPos(x + w, y, z);     tiny3d_VertexColor(c);
    tiny3d_VertexPos(x + w, y + h, z); tiny3d_VertexColor(c);
    tiny3d_VertexPos(x, y + h, z);     tiny3d_VertexColor(c);
    tiny3d_End();
}

void gfx_grad_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
    float z = Z();
    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(x, y, z);         tiny3d_VertexColor(top);
    tiny3d_VertexPos(x + w, y, z);     tiny3d_VertexColor(top);
    tiny3d_VertexPos(x + w, y + h, z); tiny3d_VertexColor(bottom);
    tiny3d_VertexPos(x, y + h, z);     tiny3d_VertexColor(bottom);
    tiny3d_End();
}

void gfx_grad_h(float x, float y, float w, float h, uint32_t left, uint32_t right)
{
    float z = Z();
    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(x, y, z);         tiny3d_VertexColor(left);
    tiny3d_VertexPos(x + w, y, z);     tiny3d_VertexColor(right);
    tiny3d_VertexPos(x + w, y + h, z); tiny3d_VertexColor(right);
    tiny3d_VertexPos(x, y + h, z);     tiny3d_VertexColor(left);
    tiny3d_End();
}

static uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        float ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        r |= ((uint32_t)(ca + (cb - ca) * t) & 0xFF) << s;
    }
    return r;
}

#define ARC_SEG 6

/* rounded-rect outline, clockwise from the top-left corner */
static int rrect_outline(float x, float y, float w, float h, float r, float *px, float *py)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    const float cx[4] = { x + r,     x + w - r, x + w - r, x + r };
    const float cy[4] = { y + r,     y + r,     y + h - r, y + h - r };
    const float start[4] = { (float)M_PI, (float)(1.5 * M_PI), 0.0f, (float)(0.5 * M_PI) };
    int n = 0;
    for (int c = 0; c < 4; c++)
        for (int s = 0; s <= ARC_SEG; s++) {
            float a = start[c] + (float)s / ARC_SEG * (float)(M_PI / 2);
            px[n] = cx[c] + cosf(a) * r;
            py[n] = cy[c] + sinf(a) * r;
            n++;
        }
    return n;
}

void gfx_rrect_grad(float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom)
{
    float px[4 * (ARC_SEG + 1)], py[4 * (ARC_SEG + 1)];
    int n = rrect_outline(x, y, w, h, r, px, py);
    float z = Z();
    tiny3d_SetPolygon(TINY3D_TRIANGLE_FAN);
    tiny3d_VertexPos(x + w / 2, y + h / 2, z);
    tiny3d_VertexColor(lerp_color(top, bottom, 0.5f));
    for (int i = 0; i <= n; i++) {
        int k = i % n;
        tiny3d_VertexPos(px[k], py[k], z);
        tiny3d_VertexColor(lerp_color(top, bottom, (py[k] - y) / h));
    }
    tiny3d_End();
}

void gfx_rrect(float x, float y, float w, float h, float r, uint32_t c)
{
    gfx_rrect_grad(x, y, w, h, r, c, c);
}

void gfx_rrect_border(float x, float y, float w, float h, float r, float t, uint32_t c)
{
    float ox[4 * (ARC_SEG + 1)], oy[4 * (ARC_SEG + 1)];
    float ix[4 * (ARC_SEG + 1)], iy[4 * (ARC_SEG + 1)];
    int n = rrect_outline(x, y, w, h, r, ox, oy);
    rrect_outline(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, ix, iy);
    float z = Z();
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    for (int i = 0; i <= n; i++) {
        int k = i % n;
        tiny3d_VertexPos(ox[k], oy[k], z); tiny3d_VertexColor(c);
        tiny3d_VertexPos(ix[k], iy[k], z); tiny3d_VertexColor(c);
    }
    tiny3d_End();
}

void gfx_circle(float cx, float cy, float r, uint32_t c)
{
    float z = Z();
    int seg = 28;
    tiny3d_SetPolygon(TINY3D_TRIANGLE_FAN);
    tiny3d_VertexPos(cx, cy, z); tiny3d_VertexColor(c);
    for (int i = 0; i <= seg; i++) {
        float a = (float)i / seg * 2.0f * (float)M_PI;
        tiny3d_VertexPos(cx + cosf(a) * r, cy + sinf(a) * r, z);
        tiny3d_VertexColor(c);
    }
    tiny3d_End();
}

void gfx_glow(float cx, float cy, float r, uint32_t center)
{
    float z = Z();
    int seg = 36;
    uint32_t edge = WITH_ALPHA(center, 0);
    tiny3d_SetPolygon(TINY3D_TRIANGLE_FAN);
    tiny3d_VertexPos(cx, cy, z); tiny3d_VertexColor(center);
    for (int i = 0; i <= seg; i++) {
        float a = (float)i / seg * 2.0f * (float)M_PI;
        tiny3d_VertexPos(cx + cosf(a) * r, cy + sinf(a) * r, z);
        tiny3d_VertexColor(edge);
    }
    tiny3d_End();
}

void gfx_line(float x1, float y1, float x2, float y2, float t, uint32_t c)
{
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    float z = Z();
    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(x1 + nx, y1 + ny, z); tiny3d_VertexColor(c);
    tiny3d_VertexPos(x2 + nx, y2 + ny, z); tiny3d_VertexColor(c);
    tiny3d_VertexPos(x2 - nx, y2 - ny, z); tiny3d_VertexColor(c);
    tiny3d_VertexPos(x1 - nx, y1 - ny, z); tiny3d_VertexColor(c);
    tiny3d_End();
}

void gfx_polyline(const float *xy, int n, float t, uint32_t c)
{
    if (n < 2) return;
    float z = Z(), h = t * 0.5f;
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    for (int i = 0; i < n; i++) {
        int a = i > 0 ? i - 1 : i, b = i < n - 1 ? i + 1 : i;
        float dx = xy[b * 2] - xy[a * 2], dy = xy[b * 2 + 1] - xy[a * 2 + 1];
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) len = 1;
        float nx = -dy / len * h, ny = dx / len * h;
        tiny3d_VertexPos(xy[i * 2] + nx, xy[i * 2 + 1] + ny, z); tiny3d_VertexColor(c);
        tiny3d_VertexPos(xy[i * 2] - nx, xy[i * 2 + 1] - ny, z); tiny3d_VertexColor(c);
    }
    tiny3d_End();
}

void gfx_triangle(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c)
{
    float z = Z();
    tiny3d_SetPolygon(TINY3D_TRIANGLES);
    tiny3d_VertexPos(x1, y1, z); tiny3d_VertexColor(c);
    tiny3d_VertexPos(x2, y2, z); tiny3d_VertexColor(c);
    tiny3d_VertexPos(x3, y3, z); tiny3d_VertexColor(c);
    tiny3d_End();
}

/* ---------------- textures ---------------- */

texture_t gfx_load_png_buffer(const void *buf, uint32_t size)
{
    if (!buf || size < 8) return NULL;
    TL(); texture_t t = ya2d_loadPNGfromBuffer(buf, size); TU();
    return t;
}

texture_t gfx_load_jpg_buffer(const void *buf, uint32_t size)
{
    if (!buf || size < 4) return NULL;
    TL(); texture_t t = ya2d_loadJPGfromBuffer(buf, size); TU();
    return t;
}

int gfx_texture_size(texture_t t, int *w, int *h)
{
    if (!t) return 0;
    *w = t->imageWidth; *h = t->imageHeight;
    return 1;
}

texture_t gfx_make_thumb(texture_t src, int maxw, int maxh)
{
    if (!src) return NULL;
    int sw = src->imageWidth, sh = src->imageHeight;
    if (src->format != TINY3D_TEX_FORMAT_A8R8G8B8 || (sw <= maxw && sh <= maxh)) return src;
    float sc = (float)maxw / sw;
    if ((float)maxh / sh < sc) sc = (float)maxh / sh;
    int dw = (int)(sw * sc), dh = (int)(sh * sc);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    TL(); texture_t t = ya2d_createTexture(dw, dh, TINY3D_TEX_FORMAT_A8R8G8B8); TU();
    if (!t) return src;
    const uint32_t *in = (const uint32_t *)src->data;
    uint32_t *out = (uint32_t *)t->data;
    int spitch = src->rowBytes / 4, dpitch = t->rowBytes / 4;
    /* 2x2 box filter around each sample point */
    for (int y = 0; y < dh; y++) {
        int sy = (int)((y + 0.5f) / sc), sy2 = sy + 1 < sh ? sy + 1 : sy;
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((x + 0.5f) / sc), sx2 = sx + 1 < sw ? sx + 1 : sx;
            if (sx >= sw) sx = sw - 1;
            uint32_t p[4] = { in[sy * spitch + sx], in[sy * spitch + sx2], in[sy2 * spitch + sx], in[sy2 * spitch + sx2] };
            uint32_t r = 0;
            for (int sft = 0; sft < 32; sft += 8) {
                uint32_t sum = 0;
                for (int k = 0; k < 4; k++) sum += (p[k] >> sft) & 0xFF;
                r |= ((sum / 4) & 0xFF) << sft;
            }
            out[y * dpitch + x] = r;
        }
    }
    TL(); ya2d_freeTexture(src); TU();
    return t;
}

texture_t gfx_create_l8(int w, int h)
{
    TL(); texture_t t = ya2d_createTexture(w, h, TINY3D_TEX_FORMAT_L8); TU();
    return t;
}

texture_t gfx_create_argb(int w, int h)
{
    TL(); texture_t t = ya2d_createTexture(w, h, TINY3D_TEX_FORMAT_A8R8G8B8); TU();
    return t;
}

uint8_t *gfx_texture_data(texture_t t) { return t ? (uint8_t *)t->data : NULL; }

void gfx_draw_yuv(texture_t ty, texture_t tu, texture_t tv, float x, float y, float w, float h, float uw, float vh)
{
    if (!ty || !tu || !tv) return;
    float z = Z();
    tiny3d_SetTextureWrap(0, ty->textureOffset, ty->imageWidth, ty->imageHeight, ty->rowBytes, TINY3D_TEX_FORMAT_L8, TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, TEXTURE_LINEAR);
    tiny3d_SetTextureWrap(1, tu->textureOffset, tu->imageWidth, tu->imageHeight, tu->rowBytes, TINY3D_TEX_FORMAT_L8, TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, TEXTURE_LINEAR);
    tiny3d_SetTextureWrap(2, tv->textureOffset, tv->imageWidth, tv->imageHeight, tv->rowBytes, TINY3D_TEX_FORMAT_L8, TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, TEXTURE_LINEAR);
    tiny3d_Enable_YUV(USE_YUV_8BIT_TEXTURES);
    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(x, y, z);         tiny3d_VertexColor(0xFFFFFFFF); tiny3d_VertexTexture(0.0f, 0.0f);
    tiny3d_VertexPos(x + w, y, z);     tiny3d_VertexColor(0xFFFFFFFF); tiny3d_VertexTexture(uw, 0.0f);
    tiny3d_VertexPos(x + w, y + h, z); tiny3d_VertexColor(0xFFFFFFFF); tiny3d_VertexTexture(uw, vh);
    tiny3d_VertexPos(x, y + h, z);     tiny3d_VertexColor(0xFFFFFFFF); tiny3d_VertexTexture(0.0f, vh);
    tiny3d_End();
    tiny3d_Disable_YUV();
    tiny3d_Project2D();
}

texture_t gfx_load_png_file(const char *path)
{
    return ya2d_loadPNGfromFile(path);
}

void gfx_texture(texture_t t, float x, float y, float w, float h, uint32_t tint)
{
    if (!t || !t->data) return;
    float z = Z();
    ya2d_setTexture(t);
    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(x, y, z);         tiny3d_VertexColor(tint); tiny3d_VertexTexture(0.0f, 0.0f);
    tiny3d_VertexPos(x + w, y, z);     tiny3d_VertexColor(tint); tiny3d_VertexTexture(1.0f, 0.0f);
    tiny3d_VertexPos(x + w, y + h, z); tiny3d_VertexColor(tint); tiny3d_VertexTexture(1.0f, 1.0f);
    tiny3d_VertexPos(x, y + h, z);     tiny3d_VertexColor(tint); tiny3d_VertexTexture(0.0f, 1.0f);
    tiny3d_End();
}

void gfx_free_texture(texture_t t)
{
    if (t) { TL(); ya2d_freeTexture(t); TU(); }
}

/* ---------------- text ---------------- */

static glyph_t *get_glyph(uint32_t cp, int size, int bold)
{
    if (size < 6) size = 6;
    if (size > 40) size = 40;
    bold = bold ? 1 : 0;
    uint32_t key = (cp & 0x1FFFFF) | ((uint32_t)size << 21) | ((uint32_t)bold << 28);
    uint32_t h = (key * 2654435761u) % HASH_SIZE;

    for (int probe = 0; probe < 16; probe++) {
        int idx = hash_tab[(h + probe) % HASH_SIZE];
        if (idx < 0) break;
        if (glyphs[idx].used && glyphs[idx].key == key) {
            glyphs[idx].last = frame_no;
            return &glyphs[idx];
        }
    }

    /* pick a free slot or the least-recently-used one not drawn in the last 2 frames */
    int victim = -1;
    uint64_t oldest = ~0ULL;
    for (int i = 0; i < GLYPH_SLOTS; i++) {
        if (!glyphs[i].used) { victim = i; break; }
        if (glyphs[i].last + 2 < frame_no && glyphs[i].last < oldest) { oldest = glyphs[i].last; victim = i; }
    }
    if (victim < 0) return NULL;

    glyph_t *g = &glyphs[victim];
    if (g->used) {
        /* remove old key from hash */
        uint32_t oh = (g->key * 2654435761u) % HASH_SIZE;
        for (int probe = 0; probe < 16; probe++) {
            int s = (oh + probe) % HASH_SIZE;
            if (hash_tab[s] == victim) { hash_tab[s] = -1; break; }
        }
    }

    FT_Face f = face[bold];
    int px = (int)(size * RENDER_SCALE + 0.5f);
    FT_Set_Pixel_Sizes(f, 0, px);
    FT_UInt gi = FT_Get_Char_Index(f, cp);
    if (!gi && bold) { f = face[0]; FT_Set_Pixel_Sizes(f, 0, px); gi = FT_Get_Char_Index(f, cp); }
    if (FT_Load_Glyph(f, gi, FT_LOAD_RENDER) != 0) return NULL;

    FT_GlyphSlot s = f->glyph;
    memset(g->pix, 0, GLYPH_SLOT * GLYPH_SLOT * 4);
    int bw = s->bitmap.width, bh = s->bitmap.rows;
    if (bw > GLYPH_SLOT) bw = GLYPH_SLOT;
    if (bh > GLYPH_SLOT) bh = GLYPH_SLOT;
    for (int yy = 0; yy < bh; yy++)
        for (int xx = 0; xx < bw; xx++) {
            uint8_t a = s->bitmap.buffer[yy * s->bitmap.pitch + xx];
            g->pix[yy * GLYPH_SLOT + xx] = ((uint32_t)a << 24) | 0x00FFFFFF;
        }
    g->w = bw; g->h = bh;
    g->left = s->bitmap_left; g->top = s->bitmap_top;
    g->adv = (int16_t)(s->advance.x >> 6);
    g->key = key; g->used = 1; g->last = frame_no;

    for (int probe = 0; probe < 16; probe++) {
        int slot = (h + probe) % HASH_SIZE;
        if (hash_tab[slot] < 0) { hash_tab[slot] = victim; break; }
    }
    return g;
}

#define MAX_CP 512

static int prepare(const char *utf8, uint32_t *vis)
{
    static uint32_t logical[MAX_CP];
    int n = utf8_decode(utf8, logical, MAX_CP);
    return bidi_visual(logical, n, vis);
}

/* advance-only cache: measuring text never rasterizes glyphs (keeps the glyph atlas for drawing) */
#define ADV_HASH 8192
static uint32_t adv_key[ADV_HASH];
static int16_t adv_val[ADV_HASH];

static int glyph_adv(uint32_t cp, int size, int bold)
{
    if (size < 6) size = 6;
    if (size > 40) size = 40;
    bold = bold ? 1 : 0;
    uint32_t key = (cp & 0x1FFFFF) | ((uint32_t)size << 21) | ((uint32_t)bold << 28) | 0x80000000u;
    uint32_t h = (key * 2654435761u) % ADV_HASH;
    for (int p = 0; p < 8; p++) {
        uint32_t s = (h + p) % ADV_HASH;
        if (adv_key[s] == key) return adv_val[s];
        if (!adv_key[s]) break;
    }
    FT_Face f = face[bold];
    int px = (int)(size * RENDER_SCALE + 0.5f);
    FT_Set_Pixel_Sizes(f, 0, px);
    FT_UInt gi = FT_Get_Char_Index(f, cp);
    if (!gi && bold) { f = face[0]; FT_Set_Pixel_Sizes(f, 0, px); gi = FT_Get_Char_Index(f, cp); }
    int adv = 0;
    if (FT_Load_Glyph(f, gi, FT_LOAD_DEFAULT) == 0) adv = (int)(f->glyph->advance.x >> 6);
    uint32_t slot = h;
    for (int p = 0; p < 8; p++) { uint32_t s = (h + p) % ADV_HASH; if (!adv_key[s]) { slot = s; break; } }
    adv_key[slot] = key; adv_val[slot] = (int16_t)adv;
    return adv;
}

static float run_width(const uint32_t *cp, int n, int size, int bold)
{
    float w = 0, inv = 1.0f / RENDER_SCALE;
    for (int i = 0; i < n; i++) w += glyph_adv(cp[i], size, bold) * inv;
    return w;
}

int gfx_text_width(const char *utf8, int size, int bold)
{
    static uint32_t vis[MAX_CP];
    int n = prepare(utf8, vis);
    return (int)(run_width(vis, n, size, bold) + 0.5f);
}

static void draw_run(float x, float y, const uint32_t *cp, int n, int size, int bold, uint32_t color)
{
    float inv = 1.0f / RENDER_SCALE;
    float baseline = y + size * 0.98f;
    for (int i = 0; i < n; i++) {
        glyph_t *g = get_glyph(cp[i], size, bold);
        if (!g) continue;
        if (g->w > 0 && g->h > 0) {
            float gx = x + g->left * inv;
            float gy = baseline - g->top * inv;
            float gw = g->w * inv, gh = g->h * inv;
            float u = (float)g->w / GLYPH_SLOT, v = (float)g->h / GLYPH_SLOT;
            float z = Z();
            tiny3d_SetTextureWrap(0, g->offset, GLYPH_SLOT, GLYPH_SLOT, GLYPH_SLOT * 4,
                                  TINY3D_TEX_FORMAT_A8R8G8B8, TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, TEXTURE_LINEAR);
            tiny3d_SetPolygon(TINY3D_QUADS);
            tiny3d_VertexPos(gx, gy, z);           tiny3d_VertexColor(color); tiny3d_VertexTexture(0.0f, 0.0f);
            tiny3d_VertexPos(gx + gw, gy, z);      tiny3d_VertexColor(color); tiny3d_VertexTexture(u, 0.0f);
            tiny3d_VertexPos(gx + gw, gy + gh, z); tiny3d_VertexColor(color); tiny3d_VertexTexture(u, v);
            tiny3d_VertexPos(gx, gy + gh, z);      tiny3d_VertexColor(color); tiny3d_VertexTexture(0.0f, v);
            tiny3d_End();
        }
        x += g->adv * inv;
    }
}

int gfx_text(float x, float y, const char *utf8, int size, int bold, uint32_t color, int align)
{
    static uint32_t vis[MAX_CP];
    int n = prepare(utf8, vis);
    float w = run_width(vis, n, size, bold);
    if (align == ALIGN_CENTER) x -= w / 2;
    else if (align == ALIGN_RIGHT) x -= w;
    /* soft shadow for legibility */
    draw_run(x + 1, y + 1, vis, n, size, bold, RGBA8(0, 0, 0, (color & 0xFF) / 2));
    draw_run(x, y, vis, n, size, bold, color);
    return (int)(w + 0.5f);
}

int gfx_text_fit(float x, float y, float max_w, const char *utf8, int size, int bold, uint32_t color, int align)
{
    if (gfx_text_width(utf8, size, bold) <= max_w)
        return gfx_text(x, y, utf8, size, bold, color, align);

    static uint32_t logical[MAX_CP];
    static char buf[MAX_CP * 4 + 8];
    int n = utf8_decode(utf8, logical, MAX_CP - 2);
    while (n > 1) {
        n--;
        logical[n] = 0x2026; /* … */
        utf8_encode(logical, n + 1, buf, sizeof(buf));
        if (gfx_text_width(buf, size, bold) <= max_w) break;
    }
    return gfx_text(x, y, buf, size, bold, color, align);
}

/* ---------------- controller glyphs ---------------- */

float gfx_button_icon(float x, float y, int btn, float s)
{
    float r = s / 2, cx = x + r, cy = y + r;
    uint32_t bg = RGBA8(255, 255, 255, 34);
    switch (btn) {
    case BTN_CROSS: {
        uint32_t c = RGBA8(124, 178, 232, 255);
        gfx_circle(cx, cy, r, bg);
        float k = r * 0.45f;
        gfx_line(cx - k, cy - k, cx + k, cy + k, s * 0.12f, c);
        gfx_line(cx - k, cy + k, cx + k, cy - k, s * 0.12f, c);
        return s;
    }
    case BTN_CIRCLE: {
        uint32_t c = RGBA8(255, 102, 102, 255);
        gfx_circle(cx, cy, r, bg);
        gfx_circle(cx, cy, r * 0.55f, c);
        gfx_circle(cx, cy, r * 0.40f, RGBA8(40, 40, 56, 255));
        return s;
    }
    case BTN_SQUARE: {
        uint32_t c = RGBA8(231, 164, 214, 255);
        gfx_circle(cx, cy, r, bg);
        float k = r * 0.42f, t = s * 0.11f;
        gfx_line(cx - k, cy - k, cx + k, cy - k, t, c);
        gfx_line(cx + k, cy - k, cx + k, cy + k, t, c);
        gfx_line(cx + k, cy + k, cx - k, cy + k, t, c);
        gfx_line(cx - k, cy + k, cx - k, cy - k, t, c);
        return s;
    }
    case BTN_TRIANGLE: {
        uint32_t c = RGBA8(64, 226, 160, 255);
        gfx_circle(cx, cy, r, bg);
        float k = r * 0.5f, t = s * 0.11f;
        float ax = cx, ay = cy - k, bx = cx + k, by = cy + k * 0.7f, qx = cx - k, qy = cy + k * 0.7f;
        gfx_line(ax, ay, bx, by, t, c);
        gfx_line(bx, by, qx, qy, t, c);
        gfx_line(qx, qy, ax, ay, t, c);
        return s;
    }
    default: {
        const char *lbl = btn == BTN_START ? "START" : btn == BTN_SELECT ? "SELECT" : btn == BTN_L1 ? "L1" : "R1";
        int ts = (int)(s * 0.5f);
        float w = gfx_text_width(lbl, ts, 1) + s * 0.6f;
        gfx_rrect(x, y + s * 0.1f, w, s * 0.8f, s * 0.4f, bg);
        gfx_text(x + w / 2, y + s * 0.5f - ts * 0.62f, lbl, ts, 1, RGBA8(230, 230, 240, 255), ALIGN_CENTER);
        return w;
    }
    }
}

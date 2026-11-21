#pragma once
#include <stdint.h>

/* Decode UTF-8 into code points. Returns count. */
int utf8_decode(const char *s, uint32_t *out, int max);
/* Encode code points to UTF-8. Returns bytes written (without NUL). */
int utf8_encode(const uint32_t *cp, int n, char *out, int max);

/* Shape Arabic letters (contextual forms + lam-alef) and reorder to visual order.
   in/n: logical code points. out must hold n entries. Returns visual count. */
int bidi_visual(const uint32_t *in, int n, uint32_t *out);

int is_rtl_cp(uint32_t c);
int has_rtl(const char *utf8);

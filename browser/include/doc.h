#pragma once
#include <stdint.h>
#include "gfx.h"

enum { RUN_TEXT = 0, RUN_IMG, RUN_FIELD, RUN_BUTTON, RUN_RULE, RUN_BULLET };
enum { FLD_TEXT = 0, FLD_PASSWORD, FLD_HIDDEN, FLD_SUBMIT, FLD_CHECK, FLD_RADIO, FLD_SELECT, FLD_TEXTAREA };

typedef struct {
    float x, y, w, h;
    int kind;
    const char *text;
    int size, bold;
    uint32_t color;
    int link;            /* -1 none */
    int img;             /* image index or -1 */
    int field;           /* field index or -1 */
} run_t;

typedef struct { const char *href; } link_t;

typedef struct {
    const char *src, *alt;
    int aw, ah;          /* size from attributes (0 = unknown) */
    texture_t tex;
    int state;           /* 0 pending, 1 loaded, 2 failed */
    int link;
} img_t;

typedef struct {
    int form, type;
    const char *name;
    char value[256];
    int checked;
    const char *opt_val[32], *opt_txt[32];
    int nopt, sel;
} field_t;

typedef struct { const char *action; int post; } form_t;

typedef struct {
    uint32_t id;
    char title[256];
    char url[2048];
    char base[2048];
    run_t *runs; int nruns, caprun;
    link_t *links; int nlinks, caplink;
    img_t *imgs; int nimgs, capimg;
    field_t *fields; int nfields, capfield;
    form_t *forms; int nforms, capform;
    float height, width;
    void *arena;
    void *dom;           /* parsed tree, kept for re-layout */
    int plain;           /* text/plain document */
    char *raw;           /* plain text body */
} doc_t;

/* parse (can run on any thread) */
doc_t *doc_parse(const char *data, size_t len, const char *url, int plain, const char *ctype);
/* layout to the given width (main thread: measures text) */
void doc_layout(doc_t *d, float width);
void doc_free(doc_t *d);
/* builds the url (and POST body, or NULL) for submitting form f; extra = clicked submit field or -1 */
void doc_form_submit(doc_t *d, int f, int extra, char *url, int nurl, char **post);

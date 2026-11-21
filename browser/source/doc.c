/* Nova browser: HTML -> token stream (libxml2, any thread) -> laid-out runs (main thread).
 * Pages are shown in a consistent dark reading theme; no CSS and no JavaScript. */
#include "doc.h"
#include "net.h"
#include "arabic.h"
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define C_TXT   0xE9E8F6FF
#define C_HEAD  0xFFFFFFFF
#define C_LINK  0x8FB8FFFF
#define C_DIM   0x9A98C0FF

/* ---------------- arena ---------------- */
typedef struct blk { struct blk *next; size_t used, cap; char data[]; } blk_t;

static void *amalloc(doc_t *d, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    blk_t *b = (blk_t *)d->arena;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > 256 * 1024 ? n : 256 * 1024;
        blk_t *nb = (blk_t *)malloc(sizeof(blk_t) + cap);
        if (!nb) return NULL;
        nb->next = b; nb->used = 0; nb->cap = cap;
        d->arena = nb; b = nb;
    }
    void *p = b->data + b->used;
    b->used += n;
    return p;
}

static const char *astr(doc_t *d, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)amalloc(d, n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

#define GROW(arr, n, cap, T) do { if ((n) >= (cap)) { int nc = (cap) ? (cap) * 2 : 64; T *na = (T *)realloc((arr), sizeof(T) * nc); if (!na) break; (arr) = na; (cap) = nc; } } while (0)

/* ---------------- tokens ---------------- */
enum { T_TEXT, T_BREAK, T_IMG, T_FIELD, T_RULE, T_BULLET, T_NL };
typedef struct { uint8_t kind, size, bold, pre; int16_t indent; int16_t gap; int link, ref; uint32_t color; const char *text; } tok_t;

typedef struct {
    doc_t *d;
    tok_t *t; int n, cap;
    int skip_depth;
} pctx_t;

typedef struct { int size, bold, link, indent, pre, form; uint32_t color; int ol, olnum; } st_t;

static void emit(pctx_t *c, tok_t k) { GROW(c->t, c->n, c->cap, tok_t); if (c->n < c->cap) c->t[c->n++] = k; }
static void emit_break(pctx_t *c, int gap, const st_t *s) { tok_t k = { T_BREAK, 0, 0, 0, (int16_t)s->indent, (int16_t)gap, -1, -1, 0, NULL }; emit(c, k); }

static const char *attr(xmlNode *n, const char *name)
{
    for (xmlAttr *a = n->properties; a; a = a->next)
        if (!strcasecmp((const char *)a->name, name) && a->children && a->children->content) return (const char *)a->children->content;
    return NULL;
}

static int is_hidden(xmlNode *n)
{
    if (attr(n, "hidden")) return 1;
    const char *ah = attr(n, "aria-hidden");
    if (ah && !strcmp(ah, "true")) return 1;
    const char *st = attr(n, "style");
    if (st) {
        char s[256]; int k = 0;
        for (const char *p = st; *p && k < 255; p++) if (*p != ' ') s[k++] = tolower((unsigned char)*p);
        s[k] = 0;
        if (strstr(s, "display:none") || strstr(s, "visibility:hidden")) return 1;
    }
    return 0;
}

static void collect_text(xmlNode *n, char *out, int cap, int *len)
{
    for (xmlNode *c = n; c; c = c->next) {
        if (c->type == XML_TEXT_NODE && c->content) {
            for (const char *p = (const char *)c->content; *p && *len < cap - 1; p++) {
                char ch = isspace((unsigned char)*p) ? ' ' : *p;
                if (ch == ' ' && (*len == 0 || out[*len - 1] == ' ')) continue;
                out[(*len)++] = ch;
            }
        } else if (c->type == XML_ELEMENT_NODE && strcasecmp((const char *)c->name, "script") && strcasecmp((const char *)c->name, "style")) collect_text(c->children, out, cap, len);
    }
    out[*len] = 0;
}

static int new_field(doc_t *d, int form, int type, const char *name)
{
    GROW(d->fields, d->nfields, d->capfield, field_t);
    if (d->nfields >= d->capfield) return -1;
    field_t *f = &d->fields[d->nfields];
    memset(f, 0, sizeof(*f));
    f->form = form; f->type = type; f->name = astr(d, name ? name : "");
    return d->nfields++;
}

static int new_link(doc_t *d, const char *href)
{
    char abs[URL_MAX];
    url_join(d->base, href, abs, sizeof(abs));
    GROW(d->links, d->nlinks, d->caplink, link_t);
    if (d->nlinks >= d->caplink) return -1;
    d->links[d->nlinks].href = astr(d, abs);
    return d->nlinks++;
}

static int heading_size(const char *n) { switch (n[1]) { case '1': return 26; case '2': return 22; case '3': return 19; case '4': return 17; default: return 16; } }

static int is_block(const char *n)
{
    static const char *b[] = { "p", "div", "section", "article", "header", "footer", "nav", "main", "aside", "form", "ul", "ol", "dl", "dt", "dd",
        "pre", "blockquote", "table", "tr", "figure", "figcaption", "center", "details", "summary", "address", "fieldset", "legend", "caption",
        "tbody", "thead", "tfoot", "menu", "hgroup", "noscript", NULL };
    for (int i = 0; b[i]; i++) if (!strcmp(n, b[i])) return 1;
    return 0;
}

static void walk(pctx_t *c, xmlNode *node, st_t st);

static void walk_children(pctx_t *c, xmlNode *n, st_t st) { for (xmlNode *k = n->children; k; k = k->next) walk(c, k, st); }

static void walk(pctx_t *c, xmlNode *n, st_t st)
{
    doc_t *d = c->d;
    if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
        if (!n->content || !n->content[0]) return;
        tok_t k = { T_TEXT, (uint8_t)st.size, (uint8_t)st.bold, (uint8_t)st.pre, (int16_t)st.indent, 0, st.link, -1, st.color, astr(d, (const char *)n->content) };
        emit(c, k);
        return;
    }
    if (n->type != XML_ELEMENT_NODE) return;
    const char *nm = (const char *)n->name;
    if (!strcmp(nm, "script") || !strcmp(nm, "style") || !strcmp(nm, "template") || !strcmp(nm, "svg") || !strcmp(nm, "math") ||
        !strcmp(nm, "iframe") || !strcmp(nm, "object") || !strcmp(nm, "embed") || !strcmp(nm, "canvas") || !strcmp(nm, "option") ||
        !strcmp(nm, "datalist") || !strcmp(nm, "link") || !strcmp(nm, "meta")) return;
    if (!strcmp(nm, "head")) {
        for (xmlNode *k = n->children; k; k = k->next) {
            if (k->type != XML_ELEMENT_NODE) continue;
            if (!strcmp((const char *)k->name, "title") && !d->title[0]) { int l = 0; collect_text(k->children, d->title, sizeof(d->title), &l); }
            if (!strcmp((const char *)k->name, "base")) { const char *h = attr(k, "href"); if (h) { char b[URL_MAX]; url_join(d->url, h, b, sizeof(b)); snprintf(d->base, sizeof(d->base), "%s", b); } }
        }
        return;
    }
    if (!strcmp(nm, "title")) { if (!d->title[0]) { int l = 0; collect_text(n->children, d->title, sizeof(d->title), &l); } return; }
    if (is_hidden(n)) return;

    if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6' && !nm[2]) {
        emit_break(c, 14, &st);
        st.size = heading_size(nm); st.bold = 1; st.color = st.link >= 0 ? C_LINK : C_HEAD;
        walk_children(c, n, st);
        emit_break(c, 8, &st);
        return;
    }
    if (!strcmp(nm, "br")) { tok_t k = { T_NL, (uint8_t)st.size, 0, 0, (int16_t)st.indent, 0, -1, -1, 0, NULL }; emit(c, k); return; }
    if (!strcmp(nm, "hr")) { emit_break(c, 6, &st); tok_t k = { T_RULE, 0, 0, 0, (int16_t)st.indent, 0, -1, -1, 0, NULL }; emit(c, k); emit_break(c, 6, &st); return; }
    if (!strcmp(nm, "b") || !strcmp(nm, "strong") || !strcmp(nm, "th")) st.bold = 1;
    if (!strcmp(nm, "small") || !strcmp(nm, "sub") || !strcmp(nm, "sup") || !strcmp(nm, "figcaption") || !strcmp(nm, "cite")) { st.size = st.size > 13 ? 13 : st.size; st.color = st.link >= 0 ? C_LINK : C_DIM; }
    if (!strcmp(nm, "pre") || !strcmp(nm, "textarea") || !strcmp(nm, "listing")) st.pre = 1;

    if (!strcmp(nm, "a")) {
        const char *h = attr(n, "href");
        if (h && strncasecmp(h, "javascript:", 11) && h[0] != '#') { st.link = new_link(d, h); st.color = C_LINK; }
        walk_children(c, n, st);
        return;
    }
    if (!strcmp(nm, "img")) {
        const char *src = attr(n, "data-src");
        if (!src) src = attr(n, "data-lazy-src");
        if (!src) src = attr(n, "src");
        const char *alt = attr(n, "alt");
        if (!src) return;
        GROW(d->imgs, d->nimgs, d->capimg, img_t);
        if (d->nimgs >= d->capimg) return;
        img_t *im = &d->imgs[d->nimgs];
        memset(im, 0, sizeof(*im));
        char abs[URL_MAX];
        url_join(d->base, src, abs, sizeof(abs));
        im->src = astr(d, abs); im->alt = astr(d, alt ? alt : "");
        const char *w = attr(n, "width"), *h = attr(n, "height");
        im->aw = w ? atoi(w) : 0; im->ah = h ? atoi(h) : 0;
        im->link = st.link;
        tok_t k = { T_IMG, (uint8_t)st.size, 0, 0, (int16_t)st.indent, 0, st.link, d->nimgs, 0, NULL };
        d->nimgs++;
        emit(c, k);
        return;
    }
    if (!strcmp(nm, "form")) {
        GROW(d->forms, d->nforms, d->capform, form_t);
        if (d->nforms < d->capform) {
            const char *act = attr(n, "action"), *m = attr(n, "method");
            char abs[URL_MAX];
            url_join(d->base, act ? act : "", abs, sizeof(abs));
            if (!act || !act[0]) snprintf(abs, sizeof(abs), "%s", d->url);
            d->forms[d->nforms].action = astr(d, abs);
            d->forms[d->nforms].post = m && !strcasecmp(m, "post");
            st.form = d->nforms++;
        }
        emit_break(c, 4, &st);
        walk_children(c, n, st);
        emit_break(c, 4, &st);
        return;
    }
    if (!strcmp(nm, "input") || !strcmp(nm, "button") || !strcmp(nm, "select") || !strcmp(nm, "textarea")) {
        const char *type = attr(n, "type");
        const char *name = attr(n, "name");
        const char *val = attr(n, "value");
        int ft = FLD_TEXT;
        if (!strcmp(nm, "button")) ft = (type && !strcasecmp(type, "button")) ? -1 : FLD_SUBMIT;
        else if (!strcmp(nm, "select")) ft = FLD_SELECT;
        else if (!strcmp(nm, "textarea")) ft = FLD_TEXTAREA;
        else if (type) {
            if (!strcasecmp(type, "hidden")) ft = FLD_HIDDEN;
            else if (!strcasecmp(type, "password")) ft = FLD_PASSWORD;
            else if (!strcasecmp(type, "submit") || !strcasecmp(type, "image")) ft = FLD_SUBMIT;
            else if (!strcasecmp(type, "checkbox")) ft = FLD_CHECK;
            else if (!strcasecmp(type, "radio")) ft = FLD_RADIO;
            else if (!strcasecmp(type, "button") || !strcasecmp(type, "reset") || !strcasecmp(type, "file")) ft = -1;
        }
        if (ft < 0) return;
        int fi = new_field(d, st.form, ft, name);
        if (fi < 0) return;
        field_t *f = &d->fields[fi];
        if (ft == FLD_SUBMIT && !strcmp(nm, "button")) { int l = 0; collect_text(n->children, f->value, sizeof(f->value), &l); if (!l && val) snprintf(f->value, sizeof(f->value), "%s", val); }
        else if (ft == FLD_TEXTAREA) { int l = 0; collect_text(n->children, f->value, sizeof(f->value), &l); }
        else if (val) snprintf(f->value, sizeof(f->value), "%s", val);
        if (ft == FLD_SUBMIT && !f->value[0]) snprintf(f->value, sizeof(f->value), "Submit");
        if ((ft == FLD_CHECK || ft == FLD_RADIO)) { f->checked = attr(n, "checked") != NULL; if (!val) snprintf(f->value, sizeof(f->value), "on"); }
        if (ft == FLD_TEXT || ft == FLD_PASSWORD || ft == FLD_TEXTAREA) {
            const char *ph = attr(n, "placeholder");
            if (!ph) ph = attr(n, "aria-label");
            f->opt_txt[0] = astr(d, ph ? ph : "");
        }
        if (ft == FLD_SELECT) {
            for (xmlNode *o = n->children; o && f->nopt < 32; o = o->next) {
                if (o->type != XML_ELEMENT_NODE) continue;
                xmlNode *list = !strcmp((const char *)o->name, "optgroup") ? o->children : o;
                for (xmlNode *op = list; op && f->nopt < 32; op = (list == o ? NULL : op->next)) {
                    if (op->type != XML_ELEMENT_NODE || strcmp((const char *)op->name, "option")) continue;
                    char txt[200]; int l = 0;
                    collect_text(op->children, txt, sizeof(txt), &l);
                    const char *ov = attr(op, "value");
                    f->opt_txt[f->nopt] = astr(d, txt);
                    f->opt_val[f->nopt] = astr(d, ov ? ov : txt);
                    if (attr(op, "selected")) f->sel = f->nopt;
                    f->nopt++;
                }
            }
            if (f->nopt) snprintf(f->value, sizeof(f->value), "%s", f->opt_val[f->sel]);
        }
        if (ft != FLD_HIDDEN) { tok_t k = { T_FIELD, (uint8_t)st.size, 0, 0, (int16_t)st.indent, 0, -1, fi, 0, NULL }; emit(c, k); }
        return;
    }

    int block = is_block(nm);
    if (!strcmp(nm, "li")) {
        emit_break(c, 3, &st);
        char b[16];
        if (st.ol) snprintf(b, sizeof(b), "%d.", st.olnum); else snprintf(b, sizeof(b), "\xE2\x80\xA2");
        tok_t k = { T_BULLET, (uint8_t)st.size, 0, 0, (int16_t)(st.indent + 20), 0, -1, -1, C_DIM, astr(d, b) };
        emit(c, k);
        st.indent += 20; st.ol = 0;
        walk_children(c, n, st);
        emit_break(c, 3, &st);
        return;
    }
    if (!strcmp(nm, "ul") || !strcmp(nm, "ol") || !strcmp(nm, "menu")) {
        emit_break(c, 6, &st);
        int num = 1;
        st_t cs = st;
        cs.indent += 8;
        for (xmlNode *k = n->children; k; k = k->next) {
            cs.ol = !strcmp(nm, "ol"); cs.olnum = num;
            walk(c, k, cs);
            if (k->type == XML_ELEMENT_NODE && !strcmp((const char *)k->name, "li")) num++;
        }
        emit_break(c, 6, &st);
        return;
    }
    if (!strcmp(nm, "blockquote") || !strcmp(nm, "dd")) st.indent += 24;
    if (!strcmp(nm, "td") || !strcmp(nm, "th")) {
        tok_t k = { T_TEXT, (uint8_t)st.size, 0, 0, (int16_t)st.indent, 0, -1, -1, st.color, "   " };
        emit(c, k);
    }
    int gap = !strcmp(nm, "p") || !strcmp(nm, "blockquote") || !strcmp(nm, "pre") || !strcmp(nm, "table") || !strcmp(nm, "figure") ? 10 : 2;
    if (block) emit_break(c, gap, &st);
    walk_children(c, n, st);
    if (block) emit_break(c, gap, &st);
}

/* kept token stream for re-layout */
typedef struct { tok_t *t; int n; } toks_t;

doc_t *doc_parse(const char *data, size_t len, const char *url, int plain, const char *ctype)
{
    static uint32_t next_id = 1;
    doc_t *d = (doc_t *)calloc(1, sizeof(doc_t));
    if (!d) return NULL;
    d->id = next_id++;
    snprintf(d->url, sizeof(d->url), "%s", url);
    snprintf(d->base, sizeof(d->base), "%s", url);
    pctx_t c = { d, NULL, 0, 0, 0 };
    st_t st = { 15, 0, -1, 0, 0, -1, C_TXT, 0, 0 };
    if (plain) {
        st.pre = 1;
        char *t = (char *)amalloc(d, len + 1);
        if (t) { memcpy(t, data, len); t[len] = 0; tok_t k = { T_TEXT, 14, 0, 1, 0, 0, -1, -1, C_TXT, t }; emit(&c, k); }
    } else {
        char enc[48] = "";
        const char *cs = ctype ? strcasestr(ctype, "charset=") : NULL;
        if (cs) { cs += 8; if (*cs == '"') cs++; int k = 0; while (cs[k] && cs[k] != ';' && cs[k] != '"' && cs[k] != ' ' && k < 47) { enc[k] = cs[k]; k++; } enc[k] = 0; }
        htmlDocPtr h = htmlReadMemory(data, (int)len, url, enc[0] ? enc : NULL,
                                      HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET | HTML_PARSE_COMPACT);
        if (h) {
            xmlNode *root = xmlDocGetRootElement(h);
            if (root) walk(&c, root, st);
            xmlFreeDoc(h);
        }
    }
    toks_t *tk = (toks_t *)amalloc(d, sizeof(toks_t));
    if (tk) { tk->t = c.t; tk->n = c.n; }
    d->dom = tk;
    if (!d->title[0]) url_host(url, d->title, sizeof(d->title));
    return d;
}

/* ---------------- layout ---------------- */

typedef struct {
    doc_t *d;
    float left, right, x, y;
    int line_start;
    float line_h;
    int last_gap;      /* pending vertical gap */
    int at_line_start;
    int space_pending;
} lay_t;

static run_t *add_run(lay_t *L)
{
    doc_t *d = L->d;
    GROW(d->runs, d->nruns, d->caprun, run_t);
    if (d->nruns >= d->caprun) return NULL;
    run_t *r = &d->runs[d->nruns++];
    memset(r, 0, sizeof(*r));
    r->link = r->img = r->field = -1;
    return r;
}

static float line_h_for(int size) { return size * 1.45f; }

static void flush_line(lay_t *L, int force_empty, int size)
{
    doc_t *d = L->d;
    int n = d->nruns - L->line_start;
    if (!n && !force_empty) return;
    float lh = L->line_h;
    if (!n) lh = line_h_for(size ? size : 15) * 0.8f;
    /* direction: first text run decides */
    int rtl = 0;
    for (int i = L->line_start; i < d->nruns; i++) if (d->runs[i].kind == RUN_TEXT) { rtl = has_rtl(d->runs[i].text); break; }
    float minx = 1e9, maxx = 0;
    for (int i = L->line_start; i < d->nruns; i++) { run_t *r = &d->runs[i]; if (r->x < minx) minx = r->x; if (r->x + r->w > maxx) maxx = r->x + r->w; }
    for (int i = L->line_start; i < d->nruns; i++) {
        run_t *r = &d->runs[i];
        r->y = L->y + (lh - r->h) * (r->kind == RUN_TEXT ? 0.5f : 0.85f);
        if (r->kind == RUN_IMG || r->kind == RUN_FIELD || r->kind == RUN_BUTTON) r->y = L->y + (lh - r->h) / 2;
        if (rtl) r->x = L->right - (r->x - L->left) - r->w;
    }
    L->y += lh;
    if (L->y > d->height) d->height = L->y;
    L->line_start = d->nruns;
    L->line_h = 0;
    L->x = L->left;
    L->at_line_start = 1;
    L->space_pending = 0;
}

static void apply_gap(lay_t *L)
{
    if (L->last_gap > 0 && L->y > 0) L->y += L->last_gap;
    L->last_gap = 0;
}

static void set_indent(lay_t *L, int indent, float width)
{
    if (!L->at_line_start) return;
    float nl = 14 + indent;
    if (nl > width / 2) nl = width / 2;
    L->left = nl; L->x = nl;
    L->right = width - 14;
}

static void place(lay_t *L, run_t tmpl, float w, float h)
{
    if (!L->at_line_start && L->x + (L->space_pending ? 4 : 0) + w > L->right) flush_line(L, 0, 0);
    if (L->at_line_start) apply_gap(L);
    float sp = (!L->at_line_start && L->space_pending) ? gfx_text_width(" ", tmpl.size ? tmpl.size : 15, 0) : 0;
    run_t *r = add_run(L);
    if (!r) return;
    *r = tmpl;
    r->x = L->x + sp; r->w = w; r->h = h;
    L->x = r->x + w;
    if (h > L->line_h) L->line_h = h;
    L->at_line_start = 0;
    L->space_pending = 0;
}

static void place_word(lay_t *L, const char *w, int len, const tok_t *t)
{
    doc_t *d = L->d;
    char buf[400];
    if (len > (int)sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(buf, w, len); buf[len] = 0;
    float ww = gfx_text_width(buf, t->size, t->bold);
    float lh = line_h_for(t->size);
    float avail = L->right - L->left;
    /* very long word: hard-break by characters */
    while (ww > avail && len > 1) {
        int cut = len;
        while (cut > 1) {
            cut--;
            while (cut > 0 && (buf[cut] & 0xC0) == 0x80) cut--;
            char s = buf[cut]; buf[cut] = 0;
            float cw = gfx_text_width(buf, t->size, t->bold);
            buf[cut] = s;
            if (cw <= avail || cut <= 1) break;
        }
        if (cut < 1) cut = 1;
        char part[400]; memcpy(part, buf, cut); part[cut] = 0;
        run_t tm = { 0 }; tm.kind = RUN_TEXT; tm.size = t->size; tm.bold = t->bold; tm.color = t->color; tm.link = t->link; tm.img = tm.field = -1;
        tm.text = astr(d, part);
        place(L, tm, gfx_text_width(part, t->size, t->bold), t->size * 1.3f);
        if (lh > L->line_h) L->line_h = lh;
        flush_line(L, 0, 0);
        memmove(buf, buf + cut, len - cut + 1); len -= cut;
        ww = gfx_text_width(buf, t->size, t->bold);
    }
    /* merge with the previous run on this line when the style matches */
    if (!L->at_line_start && d->nruns > L->line_start) {
        run_t *p = &d->runs[d->nruns - 1];
        float sp = L->space_pending ? gfx_text_width(" ", t->size, 0) : 0;
        if (p->kind == RUN_TEXT && p->size == t->size && p->bold == t->bold && p->color == t->color && p->link == t->link &&
            p->x + p->w == L->x && L->x + sp + ww <= L->right && strlen(p->text) + len + 2 < 380 && (has_rtl(p->text) == has_rtl(buf))) {
            size_t pl = strlen(p->text);
            char *nt = (char *)amalloc(d, pl + len + 2);
            if (nt) {
                memcpy(nt, p->text, pl);
                int o = pl;
                if (L->space_pending) nt[o++] = ' ';
                memcpy(nt + o, buf, len + 1);
                p->text = nt;
                p->w += sp + ww;
                L->x = p->x + p->w;
                L->space_pending = 0;
                if (lh > L->line_h) L->line_h = lh;
                return;
            }
        }
    }
    run_t tm = { 0 }; tm.kind = RUN_TEXT; tm.size = t->size; tm.bold = t->bold; tm.color = t->color; tm.link = t->link; tm.img = tm.field = -1;
    tm.text = astr(d, buf);
    place(L, tm, ww, t->size * 1.3f);
    if (lh > L->line_h) L->line_h = lh;
}

static void layout_text(lay_t *L, const tok_t *t, float width)
{
    const char *s = t->text;
    if (t->pre) {
        const char *p = s;
        while (*p) {
            const char *e = p;
            while (*e && *e != '\n') e++;
            if (e > p) {
                /* keep runs of spaces as single gaps; place word by word */
                const char *q = p;
                while (q < e) {
                    while (q < e && (*q == ' ' || *q == '\t' || *q == '\r')) { L->space_pending = 1; q++; }
                    const char *w = q;
                    while (q < e && *q != ' ' && *q != '\t' && *q != '\r') q++;
                    if (q > w) { set_indent(L, t->indent, width); place_word(L, w, q - w, t); }
                }
            }
            if (*e == '\n') { set_indent(L, t->indent, width); flush_line(L, 1, t->size); e++; }
            p = e;
        }
        return;
    }
    const char *p = s;
    while (*p) {
        if (isspace((unsigned char)*p)) { L->space_pending = 1; p++; continue; }
        const char *w = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        set_indent(L, t->indent, width);
        place_word(L, w, p - w, t);
    }
}

void doc_layout(doc_t *d, float width)
{
    toks_t *tk = (toks_t *)d->dom;
    d->nruns = 0;
    d->height = 0;
    d->width = width;
    lay_t L = { d, 14, width - 14, 14, 0, 0, 0, 0, 1, 0 };
    if (!tk) return;
    for (int i = 0; i < tk->n; i++) {
        const tok_t *t = &tk->t[i];
        switch (t->kind) {
        case T_TEXT: layout_text(&L, t, width); break;
        case T_BREAK:
            flush_line(&L, 0, 0);
            if (t->gap > L.last_gap) L.last_gap = t->gap;
            break;
        case T_NL:
            if (L.at_line_start) { set_indent(&L, t->indent, width); apply_gap(&L); }
            flush_line(&L, L.at_line_start, t->size);
            break;
        case T_RULE: {
            flush_line(&L, 0, 0);
            set_indent(&L, t->indent, width);
            apply_gap(&L);
            run_t *r = add_run(&L);
            if (r) { r->kind = RUN_RULE; r->x = L.left; r->y = L.y + 2; r->w = L.right - L.left; r->h = 1; }
            L.y += 6; if (L.y > d->height) d->height = L.y;
            break;
        }
        case T_BULLET: {
            flush_line(&L, 0, 0);
            set_indent(&L, t->indent, width);
            apply_gap(&L);
            float bw = gfx_text_width(t->text, t->size, 0);
            run_t tm = { 0 }; tm.kind = RUN_BULLET; tm.text = t->text; tm.size = t->size; tm.color = C_DIM; tm.link = tm.img = tm.field = -1;
            run_t *r = add_run(&L);
            if (r) { *r = tm; r->x = L.left - bw - 6; r->w = bw; r->h = t->size * 1.3f; }
            if (line_h_for(t->size) > L.line_h) L.line_h = line_h_for(t->size);
            break;
        }
        case T_IMG: {
            img_t *im = &d->imgs[t->ref];
            float w = 0, h = 0;
            if (im->state == 1 && im->tex) { int tw, th; gfx_texture_size(im->tex, &tw, &th); w = tw; h = th; if (im->aw > 0 && im->aw < w) { h = h * im->aw / w; w = im->aw; } }
            else if (im->state != 2 && im->aw > 0 && im->ah > 0) { w = im->aw; h = im->ah; }
            else if (im->state != 2) { w = 0; h = 0; }
            float maxw = L.right - L.left;
            if (w > maxw) { h = h * maxw / w; w = maxw; }
            if (w < 1 || h < 1) {
                if (im->state == 2 && im->alt && im->alt[0]) {
                    tok_t at = *t; at.kind = T_TEXT; at.text = im->alt; at.color = t->link >= 0 ? C_LINK : C_DIM; at.size = 13; at.bold = 0; at.pre = 0;
                    set_indent(&L, t->indent, width);
                    L.space_pending = 1;
                    place_word(&L, "[", 1, &at); L.space_pending = 0;
                    layout_text(&L, &at, width);
                    place_word(&L, "]", 1, &at);
                }
                break;
            }
            set_indent(&L, t->indent, width);
            int big = w > 160 || h > 60;
            if (big) flush_line(&L, 0, 0);
            run_t tm = { 0 }; tm.kind = RUN_IMG; tm.img = t->ref; tm.link = t->link; tm.field = -1; tm.size = 15;
            L.space_pending = !big;
            place(&L, tm, w, h);
            if (h + 4 > L.line_h) L.line_h = h + 4;
            if (big) flush_line(&L, 0, 0);
            break;
        }
        case T_FIELD: {
            field_t *f = &d->fields[t->ref];
            float w, h = 30;
            int kind = RUN_FIELD;
            if (f->type == FLD_SUBMIT) { w = gfx_text_width(f->value, 14, 1) + 30; kind = RUN_BUTTON; }
            else if (f->type == FLD_CHECK || f->type == FLD_RADIO) { w = 22; h = 22; }
            else if (f->type == FLD_SELECT) { w = 60; for (int k = 0; k < f->nopt; k++) { float ow = gfx_text_width(f->opt_txt[k], 14, 0) + 44; if (ow > w) w = ow; } if (w > 380) w = 380; }
            else w = f->type == FLD_TEXTAREA ? 420 : 300;
            if (w > L.right - L.left) w = L.right - L.left;
            set_indent(&L, t->indent, width);
            run_t tm = { 0 }; tm.kind = kind; tm.field = t->ref; tm.link = tm.img = -1; tm.size = 14;
            L.space_pending = 1;
            place(&L, tm, w, h);
            if (h + 8 > L.line_h) L.line_h = h + 8;
            break;
        }
        }
    }
    flush_line(&L, 0, 0);
    d->height += 30;
}

void doc_free(doc_t *d)
{
    if (!d) return;
    for (int i = 0; i < d->nimgs; i++) if (d->imgs[i].tex) gfx_free_texture(d->imgs[i].tex);
    toks_t *tk = (toks_t *)d->dom;
    if (tk) free(tk->t);
    free(d->runs); free(d->links); free(d->imgs); free(d->fields); free(d->forms);
    blk_t *b = (blk_t *)d->arena;
    while (b) { blk_t *n = b->next; free(b); b = n; }
    free(d);
}

void doc_form_submit(doc_t *d, int fi, int extra, char *url, int nurl, char **post)
{
    char q[8192]; int o = 0;
    q[0] = 0;
    for (int i = 0; i < d->nfields; i++) {
        field_t *f = &d->fields[i];
        if (f->form != fi || !f->name[0]) continue;
        if (f->type == FLD_SUBMIT && i != extra) continue;
        if ((f->type == FLD_CHECK || f->type == FLD_RADIO) && !f->checked) continue;
        char en[600], ev[800];
        url_encode(f->name, en, sizeof(en));
        url_encode(f->value, ev, sizeof(ev));
        int need = strlen(en) + strlen(ev) + 2;
        if (o + need >= (int)sizeof(q) - 1) break;
        o += snprintf(q + o, sizeof(q) - o, "%s%s=%s", o ? "&" : "", en, ev);
    }
    form_t *f = (fi >= 0 && fi < d->nforms) ? &d->forms[fi] : NULL;
    const char *act = f ? f->action : d->url;
    if (f && f->post) { snprintf(url, nurl, "%s", act); *post = strdup(q); return; }
    *post = NULL;
    char base[URL_MAX];
    snprintf(base, sizeof(base), "%s", act);
    char *qq = strchr(base, '?'); if (qq) *qq = 0;
    char *hh = strchr(base, '#'); if (hh) *hh = 0;
    snprintf(url, nurl, "%s?%s", base, q);
}

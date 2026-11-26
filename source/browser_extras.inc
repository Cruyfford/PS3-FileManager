/* ===========================================================
 * Browser extras (included by main.c):
 *   marked-file helpers, favorites, dual pane, patch / hash / TIM / compare menu actions
 * =========================================================== */

/* marked regular files ordered by the moment they were marked; returns the count */
static int marked_files(int *idx, int max)
{
    int n = 0, tmp[64];
    for (int i = 0; i < ent_count && n < 64; i++) if (ents[i].marked && !ents[i].is_dir) tmp[n++] = i;
    for (int a = 1; a < n; a++) for (int b = a; b > 0 && ents[tmp[b]].mark_seq < ents[tmp[b - 1]].mark_seq; b--) { int t = tmp[b]; tmp[b] = tmp[b - 1]; tmp[b - 1] = t; }
    if (idx) for (int i = 0; i < n && i < max; i++) idx[i] = tmp[i];
    return n;
}

/* a patch file + the file to patch: two marked files, or the cursor plus one marked file */
static int patch_pair(int *patch, int *target)
{
    int m[3], n = marked_files(m, 3), cand[2], c = 0;
    if (n == 2) { cand[0] = m[0]; cand[1] = m[1]; c = 2; }
    else if (n == 1 && ent_count && sel != m[0] && !ents[sel].is_dir) { cand[0] = sel; cand[1] = m[0]; c = 2; }
    if (c != 2) return 0;
    int k0 = patch_kind(ents[cand[0]].name) != PATCH_NONE, k1 = patch_kind(ents[cand[1]].name) != PATCH_NONE;
    if (k0 == k1) return 0;
    if (patch) *patch = k0 ? cand[0] : cand[1];
    if (target) *target = k0 ? cand[1] : cand[0];
    return 1;
}

/* ---------------------------------------------------------------- favorites */
#define FAV_MAX 16
#define FAV_FILE APP_DIR "/favorites.txt"
static char favs[FAV_MAX][1024];
static int fav_sel;

static void fav_load(void)
{
    fav_n = 0;
    FILE *f = fopen(FAV_FILE, "rb");
    if (!f) return;
    while (fav_n < FAV_MAX && fgets(favs[fav_n], sizeof(favs[0]), f)) {
        favs[fav_n][strcspn(favs[fav_n], "\r\n")] = 0;
        if (favs[fav_n][0]) fav_n++;
    }
    fclose(f);
}

static void fav_save(void)
{
    mkdir(APP_DIR, 0777);
    FILE *f = fopen(FAV_FILE, "wb");
    if (!f) return;
    for (int i = 0; i < fav_n; i++) fprintf(f, "%s\n", favs[i]);
    fclose(f);
}

static void fav_add(const char *path)
{
    for (int i = 0; i < fav_n; i++) if (!strcmp(favs[i], path)) { show_toast(T(S_FAV_EXISTS)); return; }
    if (fav_n >= FAV_MAX) { memmove(favs[0], favs[1], sizeof(favs[0]) * (FAV_MAX - 1)); fav_n--; }
    snprintf(favs[fav_n++], sizeof(favs[0]), "%s", path);
    fav_save();
    show_toast(T(S_FAV_ADDED));
}

/* open any path: pick the volume whose root is the longest prefix */
static void open_path_anywhere(const char *path, const char *select)
{
    int best = -1; size_t bl = 0;
    for (int i = 0; i < vol_count; i++) {
        size_t l = strlen(vols[i].root);
        char r[80]; snprintf(r, sizeof(r), "%s", vols[i].root);
        if (l > 1 && r[l - 1] == '/' && r[l - 2] != ':' && strncmp(r, "smb://", 6)) r[--l] = 0;
        if (!strncmp(path, r, l) && (path[l] == 0 || path[l] == '/' || r[l - 1] == '/') && l > bl) { best = i; bl = l; }
    }
    if (best >= 0) { vol_sel = best; vol_free = vfs_free_space(vols[vol_sel].root); }
    open_dir(path, select);
    focus_list = 1;
}

static void draw_favs(void)
{
    float x, y, w = 600, h = 360;
    draw_modal_frame(w, h, &x, &y);
    gfx_text(SCR_W / 2, y + 18, T(S_FAVS), 20, 1, C_TEXT, ALIGN_CENTER);
    if (fav_sel >= fav_n) fav_sel = fav_n - 1;
    if (fav_sel < 0) fav_sel = 0;
    for (int i = 0; i < fav_n && i < 8; i++) {
        int k = fav_sel >= 8 ? fav_sel - 7 + i : i;
        if (k >= fav_n) break;
        float iy = y + 56 + i * 34;
        int on = k == fav_sel;
        if (on) gfx_rrect_grad(x + 18, iy, w - 36, 30, 10, C_ACC1, C_ACC2);
        else gfx_rrect(x + 18, iy, w - 36, 30, 10, RGBA8(255, 255, 255, 12));
        L_text_fit(x + 34 - (RTL() ? 0 : 0), iy + 8, w - 70, favs[k], 12, on, C_TEXT, ALIGN_LEFT);
    }
    hint_t hh[3] = { { ok_icon, S_OPEN }, { BTN_SQUARE, S_FAV_REMOVE }, { cancel_icon, S_CLOSE } };
    draw_hints(hh, 3);
}

static void handle_favs(void)
{
    if (in_pressed & (B_CANCEL | B_TRI)) { ui = UI_BROWSE; return; }
    if (!fav_n) { ui = UI_BROWSE; return; }
    if (in_active & B_UP) fav_sel = (fav_sel + fav_n - 1) % fav_n;
    if (in_active & B_DOWN) fav_sel = (fav_sel + 1) % fav_n;
    if (in_pressed & B_SQU) {
        memmove(favs[fav_sel], favs[fav_sel + 1], sizeof(favs[0]) * (fav_n - fav_sel - 1));
        fav_n--; fav_save();
        if (!fav_n) ui = UI_BROWSE;
        return;
    }
    if (in_pressed & B_OK) {
        char p[1024]; snprintf(p, sizeof(p), "%s", favs[fav_sel]);
        ui = UI_BROWSE;
        if (!vfs_exists(p)) { show_toast(T(S_FAV_MISSING)); return; }
        int d = 0; uint64_t sz; vfs_stat(p, &d, &sz);
        if (d) open_path_anywhere(p, NULL);
        else {
            char dir[1024], child[256]; snprintf(dir, sizeof(dir), "%s", p);
            if (path_parent(dir, child, sizeof(child))) open_path_anywhere(dir, child);
        }
    }
}

/* ---------------------------------------------------------------- dual pane */
typedef struct { char cwd[1024]; int sel, top, vol; vfs_entry_t *list; int n; } pane_t;
static pane_t panes[2];

static int pane_cmp(const void *a, const void *b)
{
    const vfs_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void pane_list(pane_t *p)
{
    if (!p->list) p->list = calloc(MAX_ENTRIES, sizeof(vfs_entry_t));
    if (!p->list) { p->n = 0; return; }
    p->n = vfs_list(p->cwd, p->list, MAX_ENTRIES);
    if (p->n < 0) p->n = 0;
    qsort(p->list, p->n, sizeof(vfs_entry_t), pane_cmp);
}

static void pane_refresh_other(void) { if (dual_on) pane_list(&panes[!pane_act]); }

static void dual_layout(void)
{
    if (!dual_on) { list_x = 240; list_w = 584; return; }
    list_w = 288;
    list_x = pane_act == 0 ? 240 : 536;
}

static void dual_toggle(void)
{
    dual_on = !dual_on;
    if (dual_on) {
        pane_act = 0;
        pane_t *o = &panes[1];
        if (!o->cwd[0]) snprintf(o->cwd, sizeof(o->cwd), "%s", cwd);
        o->vol = vol_sel; o->sel = 0; o->top = 0;
        pane_list(o);
    }
    dual_layout();
}

static void dual_switch(void)
{
    pane_t *a = &panes[pane_act], *b = &panes[!pane_act];
    snprintf(a->cwd, sizeof(a->cwd), "%s", cwd);
    a->sel = sel; a->top = scroll_top; a->vol = vol_sel;
    pane_list(a);
    pane_act = !pane_act;
    if (b->vol >= 0 && b->vol < vol_count) { vol_sel = b->vol; vol_free = vfs_free_space(vols[vol_sel].root); }
    open_dir(b->cwd[0] ? b->cwd : cwd, NULL);
    if (b->sel < ent_count) sel = b->sel;
    scroll_top = b->top; sel_anim = sel; scroll_anim = scroll_top;
    focus_list = 1;
    dual_layout();
}

/* the inactive pane: a light read-only listing */
static void draw_other_pane(void)
{
    const pane_t *p = &panes[!pane_act];
    float x = pane_act == 0 ? 536 : 240, y = 78, w = 288, h = 366;
    L_rrect(x, y, w, h, 16, RGBA8(255, 255, 255, 8));
    L_border(x, y, w, h, 16, 1, C_EDGE);
    L_rrect(x + 12, y + 9, w - 24, 22, 11, RGBA8(0, 0, 0, 50));
    L_text_fit(x + 24, y + 13, w - 48, p->cwd, 11, 0, C_DIM, ALIGN_LEFT);
    int rows = 7, top = p->sel - 3; if (top < 0) top = 0;
    for (int i = top; i < p->n && i < top + rows; i++) {
        float ry = y + 40 + (i - top) * 47;
        L_rrect(x + 10, ry, w - 22, 42, 12, RGBA8(255, 255, 255, i == p->sel ? 22 : 7));
        draw_file_icon(x + 20, ry + 6, 30, file_type(p->list[i].name, p->list[i].is_dir), p->list[i].name);
        L_text_fit(x + 64, ry + 13, w - 90, p->list[i].name, 12, 0, C_DIM, ALIGN_LEFT);
    }
    if (!p->n) gfx_text(MXP(x + w / 2), y + 170, T(S_EMPTY), 14, 0, C_DIM, ALIGN_CENTER);
}

/* ---------------------------------------------------------------- menu actions */
static void out_name(char *out, int n, const char *name, const char *suffix, const char *newext)
{
    char base[256]; snprintf(base, sizeof(base), "%s", name);
    char *dot = strrchr(base, '.');
    char ext[32] = "";
    if (dot) { snprintf(ext, sizeof(ext), "%s", dot); *dot = 0; }
    char file[400]; snprintf(file, sizeof(file), "%s%s%s", base, suffix, newext ? newext : ext);
    path_join(out, n, output_dir(), file);
}

static int extras_menu_action(int m)
{
    char a[1400], b[1400], o[1400];
    switch (m) {
    case M_HASH:
        path_join(a, sizeof(a), cwd, ents[sel].name);
        ui = UI_BROWSE;
        if (ops_start_hash(a)) open_modal(UI_PROGRESS_OPS);
        return 1;
    case M_COMPARE: {
        int mk[2]; marked_files(mk, 2);
        path_join(a, sizeof(a), cwd, ents[mk[0]].name); path_join(b, sizeof(b), cwd, ents[mk[1]].name);
        hex_open(a, ents[mk[0]].size);
        hex_cmp = 1;
        snprintf(hexb_path, sizeof(hexb_path), "%s", b);
        hexb_size = ents[mk[1]].size; hexb_cache_off = ~0ULL;
        ui = UI_HEX;
        return 1;
    }
    case M_PATCH: {
        int pi, ti;
        if (!patch_pair(&pi, &ti)) return 1;
        path_join(a, sizeof(a), cwd, ents[pi].name); path_join(b, sizeof(b), cwd, ents[ti].name);
        out_name(o, sizeof(o), ents[ti].name, "_patched", NULL);
        ui = UI_BROWSE;
        if (ops_start_patch(0, a, b, o)) open_modal(UI_PROGRESS_OPS);
        return 1;
    }
    case M_MKPATCH: {
        int mk[2]; marked_files(mk, 2);
        path_join(a, sizeof(a), cwd, ents[mk[0]].name); path_join(b, sizeof(b), cwd, ents[mk[1]].name);
        int small = ents[mk[0]].size <= 0xFFFFFF && ents[mk[1]].size <= 0xFFFFFF;
        out_name(o, sizeof(o), ents[mk[1]].name, "", small ? ".ips" : ".ppf");
        ui = UI_BROWSE;
        if (ops_start_patch(1, a, b, o)) open_modal(UI_PROGRESS_OPS);
        return 1;
    }
    case M_TIM_PNG: {
        path_join(a, sizeof(a), cwd, ents[sel].name);
        out_name(o, sizeof(o), ents[sel].name, "", ".png");
        ui = UI_BROWSE;
        int ok = 0;
        vfs_file_t *f = ents[sel].size < 8 * 1024 * 1024 ? vfs_open_read(a) : NULL;
        if (f) {
            uint32_t sz = (uint32_t)ents[sel].size; uint8_t *d = malloc(sz);
            if (d && vfs_read(f, d, sz) == (int64_t)sz) {
                tim_image_t t;
                if (tim_decode(d, sz, img_clut, &t)) { ok = png_write(o, t.rgba, t.w, t.h, t.w * 4, 0); tim_free(&t); }
            }
            free(d); vfs_close(f);
        }
        show_toast(ok ? T(S_SAVED) : T(S_SAVE_FAIL));
        if (ok) refresh_keep();
        return 1;
    }
    case M_TIMX: {
        path_join(a, sizeof(a), cwd, ents[sel].name);
        out_name(o, sizeof(o), ents[sel].name, "_TIM", "");
        ui = UI_BROWSE;
        if (ops_start_timx(a, o)) open_modal(UI_PROGRESS_OPS);
        return 1;
    }
    case M_COPY_OTHER: case M_MOVE_OTHER: {
        static char srcs[OPS_MAX_SRC][1024];
        int n = collect_targets(srcs, OPS_MAX_SRC, 0);
        ui = UI_BROWSE;
        if (n) ops_go(m == M_COPY_OTHER ? OP_COPY : OP_MOVE, srcs, n, panes[!pane_act].cwd);
        return 1;
    }
    case M_FAV_ADD:
        ui = UI_BROWSE;
        if (ent_count && ents[sel].is_dir && !marked_count()) { path_join(a, sizeof(a), cwd, ents[sel].name); fav_add(a); }
        else fav_add(cwd);
        return 1;
    case M_FAVS: fav_sel = 0; ui = UI_FAVS; return 1;
    case M_DUAL: ui = UI_BROWSE; dual_toggle(); return 1;
    case M_NOWPLAY: aud_bg = 0; ui = UI_AUDIO; modal_anim = 0.6f; return 1;
    }
    return 0;
}

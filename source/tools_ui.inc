/* =========================================================== Tools: disc copy, saves, space, search
 * (included by main.c — uses its drawing helpers and state) */

enum { TS_HUB, TS_DISC, TS_SAVES, TS_SPACE, TS_SEARCH };
static int tl_scr, tl_sel, ops_ret_tools;
static float tl_anim;

/* writable destination drives (HDD + USB) */
static int tl_dests[MAX_VOL], tl_ndest;
static void tl_scan_dests(void)
{
    tl_ndest = 0;
    for (int i = 0; i < vol_count; i++) if (vols[i].writable && (vols[i].kind == 0 || vols[i].kind == 1)) tl_dests[tl_ndest++] = i;
}

static texture_t load_png_vfs(const char *path)
{
    vfs_file_t *f = vfs_open_read(path);
    if (!f) return NULL;
    uint64_t sz = vfs_size(f);
    texture_t t = NULL;
    if (sz > 8 && sz < 4 * 1024 * 1024) {
        uint8_t *b = malloc((size_t)sz);
        if (b && vfs_read(f, b, (uint32_t)sz) == (int64_t)sz) t = gfx_load_png_buffer(b, (uint32_t)sz);
        free(b);
    }
    vfs_close(f);
    return t;
}

static void tl_title(const char *t, const char *sub)
{
    L_text(28, 22, t, 24, 1, C_TEXT, ALIGN_LEFT);
    if (sub) L_text_fit(SCR_W - 28, 30, 520, sub, 12, 0, C_DIM, ALIGN_RIGHT);
}

static void tl_spinner(float cx, float cy)
{
    float tt = gfx_frame() / 60.0f;
    for (int i = 0; i < 8; i++) { float an = tt * 5 + i * 0.785f; gfx_circle(cx + cosf(an) * 12, cy + sinf(an) * 12, 2.4f, WITH_ALPHA(C_TEXT, 40 + i * 26)); }
}

/* ---------------------------------------------------------------- hub */
static const int tl_items[4][2] = { { S_T_DISC, S_T_DISC_D }, { S_T_SAVES, S_T_SAVES_D }, { S_T_SPACE, S_T_SPACE_D }, { S_T_SEARCH, S_T_SEARCH_D } };
static const uint32_t tl_cols[4][2] = {
    { RGBA8(64, 120, 255, 255), RGBA8(150, 70, 255, 255) }, { RGBA8(40, 200, 150, 255), RGBA8(40, 120, 200, 255) },
    { RGBA8(255, 160, 60, 255), RGBA8(224, 70, 110, 255) }, { RGBA8(180, 90, 255, 255), RGBA8(70, 120, 255, 255) } };

static void draw_tools_hub(void)
{
    tl_title(T(S_TOOLS), NULL);
    float cw = 400, ch = 150, gx = (SCR_W - 2 * cw - 24) / 2, gy = 96;
    for (int i = 0; i < 4; i++) {
        float x = gx + (i % 2) * (cw + 24), y = gy + (i / 2) * (ch + 24) + (1 - tl_anim) * 24;
        int on = i == tl_sel;
        uint32_t ca = tl_cols[i][0], cb = tl_cols[i][1];
        if (on) gfx_glow(MXP(x + cw / 2), y + ch / 2, cw * 0.6f, WITH_ALPHA(ca, 90));
        if (on) L_rrect_grad(x, y, cw, ch, 20, WITH_ALPHA(ca, 235), WITH_ALPHA(cb, 235));
        else L_rrect(x, y, cw, ch, 20, C_PANEL);
        L_border(x, y, cw, ch, 20, on ? 2 : 1, on ? RGBA8(255, 255, 255, 170 + (int)(sinf(gfx_frame() * 0.1f) * 60)) : C_EDGE);
        /* icon disc */
        float ix = MXP(x + 52), iy = y + 56;
        gfx_circle(ix, iy, 26, on ? RGBA8(255, 255, 255, 60) : WITH_ALPHA(ca, 70));
        gfx_circle(ix, iy, 16, on ? RGBA8(255, 255, 255, 90) : WITH_ALPHA(ca, 120));
        gfx_circle(ix, iy, 5, on ? C_TEXT : ca);
        L_text(x + 96, y + 34, T(tl_items[i][0]), 19, 1, C_TEXT, ALIGN_LEFT);
        L_text_fit(x + 96, y + 66, cw - 112, T(tl_items[i][1]), 11, 0, on ? C_TEXT : C_DIM, ALIGN_LEFT);
    }
}

/* ---------------------------------------------------------------- disc copy */
static disc_info_t dc_info;
static volatile int dc_state;      /* 0 probing, 1 found, 2 none */
static sys_ppu_thread_t dc_thr; static int dc_thr_on;
static int dc_fmt, dc_dest, dc_row;

static void dc_thread(void *a) { (void)a; dc_state = disc_probe(&dc_info) == 0 ? 1 : 2; sysThreadExit(0); }

static void dc_start_probe(void)
{
    if (dc_thr_on) { u64 rv; sysThreadJoin(dc_thr, &rv); dc_thr_on = 0; }
    dc_state = 0; dc_row = 0; dc_fmt = 0;
    if (sysThreadCreate(&dc_thr, dc_thread, NULL, 1100, 0x20000, THREAD_JOINABLE, "discprobe") == 0) dc_thr_on = 1;
    else dc_state = 2;
}

static const char *dc_subdir(void)
{
    switch (dc_info.type) {
        case DISC_PS3: return dc_fmt ? "GAMES" : "PS3ISO";
        case DISC_PS2: return "PS2ISO";
        case DISC_PS1: return "PSXISO";
    }
    return "ISO";
}

static void draw_tools_disc(void)
{
    tl_title(T(S_T_DISC), T(S_T_DISC_D));
    float x = 120, y = 92, w = SCR_W - 240, h = 330;
    L_rrect(x, y, w, h, 20, C_PANEL);
    L_border(x, y, w, h, 20, 1, C_EDGE);
    if (dc_state == 0) { tl_spinner(SCR_W / 2, y + 120); gfx_text(SCR_W / 2, y + 160, T(S_DISC_READING), 14, 0, C_DIM, ALIGN_CENTER); return; }
    if (dc_state == 2) {
        draw_result_icon(SCR_W / 2, y + 110, 0, RGBA8(255, 180, 60, 255));
        gfx_text(SCR_W / 2, y + 160, T(S_DISC_NONE), 15, 1, C_TEXT, ALIGN_CENTER);
        return;
    }
    /* disc card */
    uint32_t pc = dc_info.type == DISC_PS3 ? RGBA8(64, 120, 255, 255) : dc_info.type == DISC_PS2 ? RGBA8(60, 90, 220, 255) :
                  dc_info.type == DISC_PS1 ? RGBA8(150, 150, 170, 255) : RGBA8(120, 120, 140, 255);
    float dcx = MXP(x + 80), dcy = y + 80;
    gfx_glow(dcx, dcy, 70, WITH_ALPHA(pc, 90));
    gfx_circle(dcx, dcy, 48, RGBA8(210, 214, 235, 255));
    gfx_circle(dcx, dcy, 44, WITH_ALPHA(pc, 230));
    gfx_circle(dcx, dcy, 14, RGBA8(230, 232, 245, 255));
    gfx_circle(dcx, dcy, 6, RGBA8(20, 20, 40, 255));
    L_text_fit(x + 150, y + 36, w - 180, dc_info.title, 20, 1, C_TEXT, ALIGN_LEFT);
    char nfo[200], sz[32];
    fmt_size(dc_info.bytes, sz, sizeof(sz));
    snprintf(nfo, sizeof(nfo), "%s %s   \xC2\xB7   %s %s   \xC2\xB7   %s", T(S_DISC_TYPE), disc_type_name(dc_info.type),
             T(S_DISC_ID), dc_info.id[0] ? dc_info.id : "-", sz);
    L_text_fit(x + 150, y + 70, w - 180, nfo, 12, 0, C_DIM, ALIGN_LEFT);
    if (dc_info.type == DISC_PS2) L_text_fit(x + 150, y + 92, w - 180, T(S_DISC_PS2NOTE), 11, 0, RGBA8(255, 190, 120, 255), ALIGN_LEFT);

    /* options */
    const char *fmt = dc_info.is_cd ? T(S_FMT_BIN) : (dc_info.type == DISC_PS3 && dc_fmt) ? T(S_FMT_FOLDER) : T(S_FMT_ISO);
    char dest[300] = "-";
    if (tl_ndest) {
        volume_t *v = &vols[tl_dests[dc_dest % tl_ndest]];
        snprintf(dest, sizeof(dest), "%s  \xC2\xB7  %s%s%s", v->label, v->root, v->root[strlen(v->root) - 1] == '/' ? "" : "/", dc_subdir());
    } else snprintf(dest, sizeof(dest), "%s", T(S_NO_USB));
    const char *lab[3] = { T(S_DISC_FMT), T(S_DISC_DEST), NULL };
    const char *val[3] = { fmt, dest, NULL };
    for (int r = 0; r < 3; r++) {
        float ry = y + 150 + r * 52;
        int on = r == dc_row;
        if (r == 2) {
            float bw = 240, bx = SCR_W / 2 - bw / 2;
            if (on) gfx_rrect_grad(bx, ry + 4, bw, 40, 20, C_ACC1, C_ACC2); else gfx_rrect(bx, ry + 4, bw, 40, 20, RGBA8(255, 255, 255, 20));
            gfx_text(SCR_W / 2, ry + 15, T(S_DISC_START), 15, 1, C_TEXT, ALIGN_CENTER);
            continue;
        }
        if (on) L_rrect(x + 20, ry, w - 40, 44, 14, WITH_ALPHA(C_ACC1, 120));
        L_text(x + 40, ry + 14, lab[r], 13, 1, on ? C_TEXT : C_DIM, ALIGN_LEFT);
        int can = r == 0 ? (dc_info.type == DISC_PS3) : tl_ndest > 1;
        char v[340]; snprintf(v, sizeof(v), can ? "\xE2\x97\x80  %s  \xE2\x96\xB6" : "%s", val[r]);
        L_text_fit(x + w - 40, ry + 14, w - 220, v, 13, 0, C_TEXT, ALIGN_RIGHT);
    }
}

static void tl_start_disc(void)
{
    if (!tl_ndest) { show_toast(T(S_NO_USB)); return; }
    volume_t *v = &vols[tl_dests[dc_dest % tl_ndest]];
    if (dc_info.type == DISC_PS3 && !dc_fmt && vfs_backend(v->root) != VFS_NATIVE) { show_toast(T(S_DISC_NATIVE)); return; }
    char d[1024];
    path_join(d, sizeof(d), v->root, dc_subdir());
    vfs_mkdir(d);
    ops_ret_tools = 1;
    if (ops_start_disc(dc_info.type == DISC_PS3 && dc_fmt, d)) open_modal(UI_PROGRESS_OPS);
    else ops_ret_tools = 0;
}

static void handle_tools_disc(void)
{
    uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
    if (in_pressed & B_CANCEL) { tl_scr = TS_HUB; return; }
    if (in_pressed & B_SELECT) { dc_start_probe(); return; }
    if (dc_state != 1) { if (in_pressed & B_OK) dc_start_probe(); return; }
    if (in_active & B_UP) dc_row = (dc_row + 2) % 3;
    if (in_active & B_DOWN) dc_row = (dc_row + 1) % 3;
    int d = (in_pressed & R) ? 1 : (in_pressed & L) ? -1 : 0;
    if (d && dc_row == 0 && dc_info.type == DISC_PS3) dc_fmt = !dc_fmt;
    if (d && dc_row == 1 && tl_ndest) dc_dest = (dc_dest + d + tl_ndest) % tl_ndest;
    if (in_pressed & B_OK) {
        if (dc_row == 0 && dc_info.type == DISC_PS3) dc_fmt = !dc_fmt;
        else if (dc_row == 1 && tl_ndest) dc_dest = (dc_dest + 1) % tl_ndest;
        else if (dc_row == 2) tl_start_disc();
    }
}

/* ---------------------------------------------------------------- save data */
#define SV_MAX 512
typedef struct { char path[512]; char dir[64]; char title[128]; char sub[128]; char user[16]; int marked; } save_t;
static save_t *sv; static int sv_n, sv_sel, sv_top, sv_tab, sv_dest;
static char sv_user[16] = "00000001";
static texture_t sv_icon; static int sv_icon_for = -1;

static void sv_add(const char *dir, const char *name, const char *user)
{
    if (sv_n >= SV_MAX) return;
    save_t *s = &sv[sv_n];
    memset(s, 0, sizeof(*s));
    path_join(s->path, sizeof(s->path), dir, name);
    snprintf(s->dir, sizeof(s->dir), "%s", name);
    snprintf(s->user, sizeof(s->user), "%s", user ? user : "");
    char sfo[600]; path_join(sfo, sizeof(sfo), s->path, "PARAM.SFO");
    if (!vfs_exists(sfo)) return;
    snprintf(s->title, sizeof(s->title), "%s", name);
    int nf = sfo_load(sfo, sfo_fields, SFO_MAX_FIELDS);
    if (nf > 0) {
        for (int i = 0; i < nf; i++) {
            if (!strcmp(sfo_fields[i].key, "TITLE")) snprintf(s->title, sizeof(s->title), "%s", sfo_fields[i].value);
            if (!strcmp(sfo_fields[i].key, "SUB_TITLE")) snprintf(s->sub, sizeof(s->sub), "%s", sfo_fields[i].value);
        }
        for (char *p = s->title; *p; p++) if (*p == '\n') *p = ' ';
    }
    sv_n++;
}

static int sv_cmp(const void *a, const void *b) { return strcasecmp(((const save_t *)a)->title, ((const save_t *)b)->title); }

static void sv_scan(void)
{
    if (!sv) sv = calloc(SV_MAX, sizeof(save_t));
    if (!sv) return;
    sv_n = 0; sv_sel = 0; sv_top = 0;
    if (sv_icon) { gfx_free_texture(sv_icon); sv_icon = NULL; } sv_icon_for = -1;
    vfs_entry_t *e = malloc(sizeof(vfs_entry_t) * 1024);
    if (!e) return;
    if (sv_tab == 0) {
        vfs_entry_t *u = malloc(sizeof(vfs_entry_t) * 32);
        int nu = u ? vfs_list("/dev_hdd0/home", u, 32) : 0, best = -1;
        for (int k = 0; k < nu; k++) {
            if (!u[k].is_dir) continue;
            char d[256]; snprintf(d, sizeof(d), "/dev_hdd0/home/%s/savedata", u[k].name);
            int n = vfs_list(d, e, 1024), before = sv_n;
            for (int i = 0; i < n; i++) if (e[i].is_dir) sv_add(d, e[i].name, u[k].name);
            if (sv_n - before > best) { best = sv_n - before; snprintf(sv_user, sizeof(sv_user), "%s", u[k].name); }
        }
        free(u);
    } else {
        for (int k = 0; k < tl_ndest; k++) {
            volume_t *v = &vols[tl_dests[k]];
            if (v->kind != 1) continue;
            char d[256]; path_join(d, sizeof(d), v->root, "PS3/SAVEDATA");
            int n = vfs_list(d, e, 1024);
            for (int i = 0; i < n; i++) if (e[i].is_dir) sv_add(d, e[i].name, v->label);
        }
    }
    free(e);
    qsort(sv, sv_n, sizeof(save_t), sv_cmp);
}

/* Save editing workspace: copies selected save away from the live user profile.
 * This does NOT decrypt encrypted PFD files or resign hashes. */
static int sv_work_pending;
static char sv_work_path[1024];
static void sv_copy_workspace(void)
{
    if (sv_n <= 0 || sv_sel < 0 || sv_sel >= sv_n) return;
    static char src[1][1024];
    char root[1024], dst[1024];
    snprintf(root, sizeof(root), "/dev_hdd0/CRUYFFORD_SAVE_WORKSPACE");
    if (vfs_mkdir(root) != 0 && !vfs_exists(root)) { show_toast(T(S_SAVE_WORK_FAIL)); return; }
    snprintf(dst, sizeof(dst), "%s/edit_%llu", root, (unsigned long long)ops_now_ms());
    if (vfs_mkdir(dst) != 0) { show_toast(T(S_SAVE_WORK_FAIL)); return; }
    snprintf(src[0], sizeof(src[0]), "%s", sv[sv_sel].path);
    snprintf(sv_work_path, sizeof(sv_work_path), "%s/%s", dst, sv[sv_sel].dir);
    sv_work_pending = 1;
    ops_ret_tools = 1;
    if (ops_start(OP_COPY, src, 1, dst)) open_modal(UI_PROGRESS_OPS);
    else { sv_work_pending = 0; ops_ret_tools = 0; show_toast(T(S_SAVE_WORK_FAIL)); }
}

static int sv_usb[MAX_VOL], sv_nusb;
static void sv_scan_usb(void) { sv_nusb = 0; for (int k = 0; k < tl_ndest; k++) if (vols[tl_dests[k]].kind == 1) sv_usb[sv_nusb++] = tl_dests[k]; }

static void draw_tools_saves(void)
{
    tl_title(T(S_T_SAVES), T(S_SAVES_NOTE));
    /* tabs */
    float tx = 28;
    for (int t = 0; t < 2; t++) {
        const char *nm = T(t ? S_SAVES_USB : S_SAVES_HDD);
        float w = gfx_text_width(nm, 13, 1) + 32;
        if (t == sv_tab) L_grad_pill(tx, 60, w, 28, 14, C_ACC1, C_ACC2); else L_rrect(tx, 60, w, 28, 14, RGBA8(255, 255, 255, 16));
        L_text(tx + w / 2, 66, nm, 13, 1, t == sv_tab ? C_TEXT : C_DIM, ALIGN_CENTER);
        tx += w + 8;
    }
    /* destination (backup tab) */
    if (sv_tab == 0) {
        char d[200];
        if (sv_nusb) { volume_t *v = &vols[sv_usb[sv_dest % sv_nusb]]; snprintf(d, sizeof(d), "%s: %s%s", T(S_BACKUP_TO), v->label, sv_nusb > 1 ? "   \xE2\x97\x80 \xE2\x96\xB6" : ""); }
        else snprintf(d, sizeof(d), "%s", T(S_NO_USB));
        L_text_fit(SCR_W - 28, 66, 420, d, 12, 1, sv_nusb ? C_TEXT : RGBA8(255, 180, 60, 255), ALIGN_RIGHT);
    }
    float x = 28, y = 100, w = SCR_W - 56, ih = 44;
    int rows = 7;
    if (!sv_n) { L_text_fit(SCR_W / 2, 240, SCR_W - 80, T(S_SAVES_NONE), 14, 0, C_DIM, ALIGN_CENTER); return; }
    if (sv_sel < sv_top) sv_top = sv_sel;
    if (sv_sel >= sv_top + rows) sv_top = sv_sel - rows + 1;
    float lw = w - 240;
    for (int i = sv_top; i < sv_n && i < sv_top + rows; i++) {
        float iy = y + (i - sv_top) * ih;
        int on = i == sv_sel;
        if (on) L_grad_pill(x, iy, lw, ih - 4, 12, WITH_ALPHA(C_ACC1, 200), WITH_ALPHA(C_ACC2, 200));
        else L_rrect(x, iy, lw, ih - 4, 12, RGBA8(255, 255, 255, 10));
        if (sv[i].marked) { L_rrect(x + 10, iy + 12, 16, 16, 5, C_OKC); L_text(x + 18, iy + 12, "\xE2\x9C\x93", 11, 1, C_TEXT, ALIGN_CENTER); }
        L_text_fit(x + 36, iy + 5, lw - 50, sv[i].title, 13, 1, C_TEXT, ALIGN_LEFT);
        char sub[260]; snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", sv[i].dir, sv[i].sub[0] ? sv[i].sub : sv[i].user);
        L_text_fit(x + 36, iy + 22, lw - 50, sub, 10, 0, on ? C_TEXT : C_DIM, ALIGN_LEFT);
    }
    /* icon of the selected save */
    if (sv_icon_for != sv_sel) {
        if (sv_icon) { gfx_free_texture(sv_icon); sv_icon = NULL; }
        char ip[600]; path_join(ip, sizeof(ip), sv[sv_sel].path, "ICON0.PNG");
        sv_icon = load_png_vfs(ip);
        sv_icon_for = sv_sel;
    }
    float px = x + lw + 16, pw = w - lw - 16;
    L_rrect(px, y, pw, 290, 16, C_PANEL);
    if (sv_icon) gfx_texture(sv_icon, MX(px + 12, pw - 24), y + 14, pw - 24, (pw - 24) * 176.0f / 320.0f, 0xFFFFFFFF);
    L_text_fit(px + pw / 2, y + 150, pw - 20, sv[sv_sel].title, 12, 1, C_TEXT, ALIGN_CENTER);
    char cnt[64]; snprintf(cnt, sizeof(cnt), "%d / %d", sv_sel + 1, sv_n);
    L_text(px + pw / 2, y + 260, cnt, 11, 0, C_DIM, ALIGN_CENTER);
    L_text_fit(28, 425, SCR_W - 56, T(S_SAVE_WORK_COPY), 12, 1, C_TEXT, ALIGN_LEFT);
    char pfd[1100];
    snprintf(pfd, sizeof(pfd), "%s/PARAM.PFD", sv[sv_sel].path);
    if (vfs_exists(pfd)) L_text_fit(28, 450, SCR_W - 56, T(S_SAVE_WORK_PFD), 11, 0, RGBA8(255, 190, 120, 255), ALIGN_LEFT);
}

static void sv_run(void)
{
    static char srcs[OPS_MAX_SRC][1024];
    int n = 0, any = 0;
    for (int i = 0; i < sv_n; i++) if (sv[i].marked) any = 1;
    for (int i = 0; i < sv_n && n < OPS_MAX_SRC; i++) if (any ? sv[i].marked : i == sv_sel) snprintf(srcs[n++], 1024, "%s", sv[i].path);
    if (!n) return;
    char dst[600];
    if (sv_tab == 0) {
        if (!sv_nusb) { show_toast(T(S_NO_USB)); return; }
        volume_t *v = &vols[sv_usb[sv_dest % sv_nusb]];
        path_join(dst, sizeof(dst), v->root, "PS3"); vfs_mkdir(dst);
        path_join(dst, sizeof(dst), v->root, "PS3/SAVEDATA"); vfs_mkdir(dst);
    } else snprintf(dst, sizeof(dst), "/dev_hdd0/home/%s/savedata", sv_user);
    ops_ret_tools = 1;
    if (ops_start(OP_COPY, srcs, n, dst)) open_modal(UI_PROGRESS_OPS); else ops_ret_tools = 0;
}

static void handle_tools_saves(void)
{
    uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
    if (in_pressed & B_CANCEL) { tl_scr = TS_HUB; return; }
    if (in_pressed & (B_L1 | B_R1)) { sv_tab = !sv_tab; sv_scan(); return; }
    if (sv_tab == 0 && sv_nusb && (in_pressed & (L | R))) sv_dest = (sv_dest + ((in_pressed & R) ? 1 : sv_nusb - 1)) % sv_nusb;
    if (!sv_n) return;
    if (in_active & B_UP) sv_sel = sv_sel > 0 ? sv_sel - 1 : sv_n - 1;
    if (in_active & B_DOWN) sv_sel = sv_sel < sv_n - 1 ? sv_sel + 1 : 0;
    if (in_active & B_L2) { sv_sel -= 7; if (sv_sel < 0) sv_sel = 0; }
    if (in_active & B_R2) { sv_sel += 7; if (sv_sel >= sv_n) sv_sel = sv_n - 1; }
    if (in_pressed & B_TRI) { sv_copy_workspace(); return; }
    if (in_pressed & B_SQU) { sv[sv_sel].marked = !sv[sv_sel].marked; if (sv_sel < sv_n - 1) sv_sel++; }
    if (in_pressed & B_SELECT) { int any = 0; for (int i = 0; i < sv_n; i++) any |= sv[i].marked; for (int i = 0; i < sv_n; i++) sv[i].marked = !any; }
    if (in_pressed & B_OK) sv_run();
}

/* ---------------------------------------------------------------- storage analyzer */
static char sp_root[1024], sp_dir[1024];
static int sp_sel, sp_top;

static void draw_tools_space(void)
{
    char tot[32], head[1100];
    fmt_size(space_total(), tot, sizeof(tot));
    snprintf(head, sizeof(head), "%s   \xC2\xB7   %s %s", sp_dir, T(S_SPACE_TOTAL), tot);
    tl_title(T(S_T_SPACE), head);
    int n = space_count();
    int running = space_running();
    if (running) { tl_spinner(SCR_W - 40, 74); L_text(SCR_W - 60, 68, T(S_SPACE_SCAN), 11, 0, C_DIM, ALIGN_RIGHT); }
    if (!n) { if (!running) L_text(SCR_W / 2, 240, T(S_EMPTY), 14, 0, C_DIM, ALIGN_CENTER); return; }
    if (sp_sel >= n) sp_sel = n - 1;
    int rows = 9;
    if (sp_sel < sp_top) sp_top = sp_sel;
    if (sp_sel >= sp_top + rows) sp_top = sp_sel - rows + 1;
    uint64_t total = space_total(), mx = 1;
    for (int i = 0; i < n; i++) { space_item_t *it = space_item(i); if (it && it->size > mx) mx = it->size; }
    float x = 28, y = 96, w = SCR_W - 56, ih = 36;
    for (int i = sp_top; i < n && i < sp_top + rows; i++) {
        space_item_t *it = space_item(i);
        if (!it) continue;
        float iy = y + (i - sp_top) * ih;
        int on = i == sp_sel;
        if (on) L_rrect(x, iy, w, ih - 4, 10, WITH_ALPHA(C_ACC1, 120));
        float bw = (w - 420) * (float)((double)it->size / (double)mx);
        L_rrect(x + 300, iy + 9, w - 420, 14, 7, RGBA8(255, 255, 255, 14));
        if (bw > 2) L_rrect_grad(x + 300, iy + 9, bw, 14, 7, it->is_dir ? RGBA8(90, 150, 255, 255) : RGBA8(255, 160, 80, 255), it->is_dir ? RGBA8(150, 80, 255, 255) : RGBA8(230, 80, 110, 255));
        draw_file_icon(MX(x + 10, 20), iy + 6, 20, it->is_dir ? FT_DIR : file_type(it->name, 0), it->name);
        L_text_fit(x + 38, iy + 9, 250, it->name, 12, on, on ? C_TEXT : C_DIM, ALIGN_LEFT);
        char sz[32], pc[64]; fmt_size(it->size, sz, sizeof(sz));
        snprintf(pc, sizeof(pc), "%s  %d%%%s", sz, total ? (int)(it->size * 100 / total) : 0, it->done ? "" : " \xE2\x80\xA6");
        L_text(x + w - 12, iy + 9, pc, 12, 1, C_TEXT, ALIGN_RIGHT);
    }
}

static void sp_open(const char *dir) { snprintf(sp_dir, sizeof(sp_dir), "%s", dir); sp_sel = 0; sp_top = 0; space_start(sp_dir); }

static void handle_tools_space(void)
{
    if (in_pressed & B_CANCEL) {
        if (strcmp(sp_dir, sp_root)) { char p[1024]; snprintf(p, sizeof(p), "%s", sp_dir); char *s = strrchr(p, '/'); if (s && s > p) { *s = 0; if (strlen(p) < strlen(sp_root)) snprintf(p, sizeof(p), "%s", sp_root); sp_open(p); return; } }
        space_cancel(); tl_scr = TS_HUB; return;
    }
    int n = space_count();
    if (!n) return;
    if (in_active & B_UP) sp_sel = sp_sel > 0 ? sp_sel - 1 : n - 1;
    if (in_active & B_DOWN) sp_sel = sp_sel < n - 1 ? sp_sel + 1 : 0;
    space_item_t *it = space_item(sp_sel);
    if (!it) return;
    if ((in_pressed & B_OK) && it->is_dir && !space_running()) { char p[1024]; path_join(p, sizeof(p), sp_dir, it->name); sp_open(p); }
    if (in_pressed & B_TRI) { space_cancel(); ui = UI_BROWSE; focus_list = 1; open_dir(sp_dir, it->name); }
}

/* ---------------------------------------------------------------- search */
static char sr_q[256];
static int sr_sel, sr_top;

static void sr_begin(void)
{
    osk_purpose = 5;
    if (osk_open(T(S_SEARCH_Q), sr_q)) ui = UI_OSK;
}

static void sr_run(void)
{
    static char roots[16][64];
    int n = 0;
    for (int i = 0; i < vol_count && n < 16; i++) if (vols[i].kind == 0 || vols[i].kind == 1 || vols[i].kind == 3) snprintf(roots[n++], 64, "%s", vols[i].root);
    sr_sel = 0; sr_top = 0;
    search_start(roots, n, sr_q);
}

static void draw_tools_search(void)
{
    char sub[400];
    int n = search_count();
    snprintf(sub, sizeof(sub), "\"%s\"   \xC2\xB7   ", sr_q);
    size_t l = strlen(sub); snprintf(sub + l, sizeof(sub) - l, T(S_SEARCH_RES), n);
    tl_title(T(S_T_SEARCH), sub);
    if (search_running()) {
        tl_spinner(40, 76);
        char w[1100]; snprintf(w, sizeof(w), "%s: %s", T(S_SEARCHING), search_where());
        L_text_fit(64, 68, SCR_W - 100, w, 11, 0, C_DIM, ALIGN_LEFT);
    }
    if (!n) { if (!search_running()) L_text(SCR_W / 2, 240, T(S_NO_FILES), 14, 0, C_DIM, ALIGN_CENTER); return; }
    if (sr_sel >= n) sr_sel = n - 1;
    int rows = 9;
    if (sr_sel < sr_top) sr_top = sr_sel;
    if (sr_sel >= sr_top + rows) sr_top = sr_sel - rows + 1;
    float x = 28, y = 96, w = SCR_W - 56, ih = 36;
    for (int i = sr_top; i < n && i < sr_top + rows; i++) {
        float iy = y + (i - sr_top) * ih;
        int on = i == sr_sel;
        if (on) L_grad_pill(x, iy, w, ih - 4, 10, WITH_ALPHA(C_ACC1, 200), WITH_ALPHA(C_ACC2, 200));
        const char *p = search_path(i), *nm = strrchr(p, '/'); nm = nm ? nm + 1 : p;
        draw_file_icon(MX(x + 10, 20), iy + 6, 20, search_is_dir(i) ? FT_DIR : file_type(nm, 0), nm);
        L_text_fit(x + 38, iy + 3, w - 200, nm, 12, 1, C_TEXT, ALIGN_LEFT);
        char dir[1024]; snprintf(dir, sizeof(dir), "%s", p); char *s = strrchr(dir, '/'); if (s) *s = 0;
        L_text_fit(x + 38, iy + 18, w - 200, dir, 9, 0, on ? C_TEXT : C_DIM, ALIGN_LEFT);
        if (!search_is_dir(i)) { char sz[32]; fmt_size(search_size(i), sz, sizeof(sz)); L_text(x + w - 12, iy + 9, sz, 11, 0, C_TEXT, ALIGN_RIGHT); }
    }
}

static void handle_tools_search(void)
{
    if (in_pressed & B_CANCEL) { search_cancel(); tl_scr = TS_HUB; return; }
    if (in_pressed & B_SQU) { search_cancel(); sr_begin(); return; }
    int n = search_count();
    if (!n) return;
    if (in_active & B_UP) sr_sel = sr_sel > 0 ? sr_sel - 1 : n - 1;
    if (in_active & B_DOWN) sr_sel = sr_sel < n - 1 ? sr_sel + 1 : 0;
    if (in_active & B_L2) { sr_sel -= 9; if (sr_sel < 0) sr_sel = 0; }
    if (in_active & B_R2) { sr_sel += 9; if (sr_sel >= n) sr_sel = n - 1; }
    if (in_pressed & B_OK) {
        char p[1024]; snprintf(p, sizeof(p), "%s", search_path(sr_sel));
        char *s = strrchr(p, '/');
        if (!s) return;
        char name[256]; snprintf(name, sizeof(name), "%s", s + 1);
        if (s == p || s[-1] == ':') s[1] = 0; else *s = 0;
        search_cancel();
        for (int i = 0; i < vol_count; i++) if (!strncmp(p, vols[i].root, strlen(vols[i].root))) vol_sel = i;
        ui = UI_BROWSE; focus_list = 1;
        open_dir(p, name);
    }
}

/* ---------------------------------------------------------------- dispatch */
static void tools_open(void) { tl_scr = TS_HUB; tl_anim = 0; tl_scan_dests(); ui = UI_TOOLS; }

static void draw_tools(void)
{
    tl_anim += (1.0f - tl_anim) * 0.2f;
    switch (tl_scr) {
        case TS_HUB: draw_tools_hub(); break;
        case TS_DISC: draw_tools_disc(); break;
        case TS_SAVES: draw_tools_saves(); break;
        case TS_SPACE: draw_tools_space(); break;
        case TS_SEARCH: draw_tools_search(); break;
    }
}

static void draw_tools_hints(void)
{
    hint_t h[6]; int n = 0;
    switch (tl_scr) {
        case TS_HUB: h[n++] = (hint_t){ ok_icon, S_ENTER }; break;
        case TS_DISC: if (dc_state == 1) h[n++] = (hint_t){ ok_icon, S_CHANGE }; h[n++] = (hint_t){ BTN_SELECT, S_REFRESH }; break;
        case TS_SAVES: h[n++] = (hint_t){ ok_icon, sv_tab ? S_RESTORE : S_BACKUP }; h[n++] = (hint_t){ BTN_SQUARE, S_MARK }; h[n++] = (hint_t){ BTN_L1, S_SWITCH }; break;
        case TS_SPACE: h[n++] = (hint_t){ ok_icon, S_OPEN }; h[n++] = (hint_t){ BTN_TRIANGLE, S_GO_TO }; break;
        case TS_SEARCH: h[n++] = (hint_t){ ok_icon, S_GO_TO }; h[n++] = (hint_t){ BTN_SQUARE, S_NEW_SEARCH }; break;
    }
    h[n++] = (hint_t){ cancel_icon, S_BACK };
    draw_hints(h, n);
}

static void handle_tools(void)
{
    switch (tl_scr) {
    case TS_HUB: {
        uint32_t L = RTL() ? B_RIGHT : B_LEFT, R = RTL() ? B_LEFT : B_RIGHT;
        if (in_pressed & B_CANCEL) { ui = UI_BROWSE; return; }
        if (in_active & R) tl_sel = (tl_sel + 1) % 4;
        if (in_active & L) tl_sel = (tl_sel + 3) % 4;
        if (in_active & (B_UP | B_DOWN)) tl_sel = (tl_sel + 2) % 4;
        if (in_pressed & B_OK) {
            tl_scan_dests();
            if (tl_sel == 0) { tl_scr = TS_DISC; dc_dest = 0; for (int k = 0; k < tl_ndest; k++) if (vols[tl_dests[k]].kind == 1) { dc_dest = k; break; } dc_start_probe(); }
            else if (tl_sel == 1) { tl_scr = TS_SAVES; sv_scan_usb(); sv_scan(); }
            else if (tl_sel == 2) { tl_scr = TS_SPACE; snprintf(sp_root, sizeof(sp_root), "%s", cwd[0] ? cwd : "/dev_hdd0"); sp_open(sp_root); }
            else { tl_scr = TS_SEARCH; search_cancel(); sr_begin(); }
        }
        break;
    }
    case TS_DISC: handle_tools_disc(); break;
    case TS_SAVES: handle_tools_saves(); break;
    case TS_SPACE: handle_tools_space(); break;
    case TS_SEARCH: handle_tools_search(); break;
    }
}

/* called when a tool's copy finishes */
static void tools_after_ops(void)
{
    if (sv_work_pending) {
        sv_work_pending = 0;
        if (vfs_exists(sv_work_path)) { open_dir(sv_work_path, NULL); tl_scr = TS_HUB; ui = UI_BROWSE; }
        else show_toast(T(S_SAVE_WORK_FAIL));
        return;
    }
    if (tl_scr == TS_SAVES) sv_scan();
}

/* =========================================================== hex: .tbl tables (Arabization) */
#define TBL_MAX 8192
typedef struct { uint8_t b[4]; uint8_t n; char s[24]; } tbl_e;
static tbl_e *tbl; static int tbl_n;
static int tbl_first[257];          /* entries sorted by first byte, longest first */
static char tbl_name[256];
static int tbl_pick = -1;

static int tbl_cmp(const void *a, const void *b)
{
    const tbl_e *x = a, *y = b;
    if (x->b[0] != y->b[0]) return x->b[0] - y->b[0];
    return y->n - x->n;
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static int tbl_load(const char *path)
{
    vfs_file_t *f = vfs_open_read(path);
    if (!f) return 0;
    uint64_t sz = vfs_size(f);
    if (sz > 2 * 1024 * 1024) { vfs_close(f); return 0; }
    char *t = malloc((size_t)sz + 1);
    if (!t) { vfs_close(f); return 0; }
    int64_t r = vfs_read(f, t, (uint32_t)sz); vfs_close(f);
    t[r > 0 ? r : 0] = 0;
    if (!tbl) tbl = malloc(sizeof(tbl_e) * TBL_MAX);
    if (!tbl) { free(t); return 0; }
    tbl_n = 0;
    char *p = t;
    if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    while (*p && tbl_n < TBL_MAX) {
        char *eol = p; while (*eol && *eol != '\n') eol++;
        char save = *eol; *eol = 0;
        char *eq = strchr(p, '=');
        if (eq && hexv(p[0]) >= 0) {
            tbl_e *e = &tbl[tbl_n]; int nb = 0;
            for (char *h = p; h + 1 < eq && nb < 4; h += 2) { int a = hexv(h[0]), b = hexv(h[1]); if (a < 0 || b < 0) break; e->b[nb++] = a << 4 | b; }
            char *v = eq + 1; size_t vl = strlen(v);
            while (vl && (v[vl - 1] == '\r')) v[--vl] = 0;
            if (nb && vl && vl < sizeof(e->s)) { e->n = nb; memcpy(e->s, v, vl + 1); tbl_n++; }
        }
        *eol = save;
        p = *eol ? eol + 1 : eol;
    }
    free(t);
    qsort(tbl, tbl_n, sizeof(tbl_e), tbl_cmp);
    for (int i = 0, j = 0; i <= 256; i++) { while (j < tbl_n && tbl[j].b[0] < i) j++; tbl_first[i] = j; }
    const char *bn = strrchr(path, '/');
    snprintf(tbl_name, sizeof(tbl_name), "%s", bn ? bn + 1 : path);
    return tbl_n;
}

/* longest table entry matching at off; returns its length in bytes, 0 = none */
static int tbl_match(uint64_t off, const char **s)
{
    int b0 = hex_byte(off, NULL);
    if (b0 < 0) return 0;
    for (int i = tbl_first[b0]; i < tbl_first[b0 + 1]; i++) {
        int ok = 1;
        for (int k = 1; k < tbl[i].n && ok; k++) { int v = hex_byte(off + k, NULL); if (v != tbl[i].b[k]) ok = 0; }
        if (ok) { *s = tbl[i].s; return tbl[i].n; }
    }
    return 0;
}

static void tbl_row_text(uint64_t ro, char *out, int on)
{
    int o = 0;
    for (uint64_t c = 0; c < 16 && ro + c < hex_size && o < on - 30;) {
        const char *s; int n = tbl_n ? tbl_match(ro + c, &s) : 0;
        if (n) { int l = strlen(s); memcpy(out + o, s, l); o += l; c += n; }
        else { int v = hex_byte(ro + c, NULL); out[o++] = (!tbl_n && v >= 32 && v < 127) ? (char)v : '.'; c++; }
    }
    out[o] = 0;
}

/* text -> bytes using the table (greedy longest match); without a table, raw UTF-8 */
static int tbl_encode(const char *txt, uint8_t *out, int max, char *bad, int bn)
{
    int o = 0;
    if (!tbl_n) { int l = strlen(txt); if (l > max) l = max; memcpy(out, txt, l); return l; }
    const char *p = txt;
    while (*p && o < max) {
        int best = -1; size_t bl = 0;
        for (int i = 0; i < tbl_n; i++) { size_t l = strlen(tbl[i].s); if (l > bl && !strncmp(p, tbl[i].s, l)) { best = i; bl = l; } }
        if (best < 0) {
            int cl = ((uint8_t)*p >= 0xF0) ? 4 : ((uint8_t)*p >= 0xE0) ? 3 : ((uint8_t)*p >= 0xC0) ? 2 : 1;
            snprintf(bad, bn, "%.*s", cl, p);
            return -1;
        }
        for (int k = 0; k < tbl[best].n && o < max; k++) out[o++] = tbl[best].b[k];
        p += bl;
    }
    return o;
}

/* cycle through the .tbl files next to the open file (then "off") */
static void tbl_cycle(void)
{
    char dir[1400]; snprintf(dir, sizeof(dir), "%s", hex_path);
    char *s = strrchr(dir, '/'); if (s) *s = 0;
    vfs_entry_t *e = malloc(sizeof(vfs_entry_t) * 1024);
    if (!e) return;
    int n = vfs_list(dir, e, 1024), k = 0, cnt = 0;
    char names[64][256];
    for (int i = 0; i < n && cnt < 64; i++) if (!e[i].is_dir && !strcasecmp(ext_of(e[i].name), "tbl")) snprintf(names[cnt++], 256, "%s", e[i].name);
    free(e);
    if (!cnt) { show_toast(T(S_TBL_NONE)); return; }
    tbl_pick++;
    if (tbl_pick >= cnt) { tbl_pick = -1; tbl_n = 0; tbl_name[0] = 0; show_toast(T(S_TBL_OFF)); return; }
    k = tbl_pick;
    char p[1700]; path_join(p, sizeof(p), dir, names[k]);
    int r = tbl_load(p);
    char m[400]; snprintf(m, sizeof(m), T(S_TBL_LOADED), names[k], r);
    show_toast(m);
}

static void hex_find_text(const char *txt)
{
    uint8_t pat[256]; char bad[8];
    int pn = tbl_encode(txt, pat, sizeof(pat), bad, sizeof(bad));
    if (pn < 0) { char m[96]; snprintf(m, sizeof(m), T(S_TBL_BADCHAR), bad); show_toast(m); return; }
    if (pn <= 0) return;
    vfs_file_t *f = vfs_open_read(hex_path);
    if (!f) return;
    const uint32_t CH = 1024 * 1024;
    uint8_t *b = malloc(CH + 256);
    uint64_t pos = hex_cur + 1, limit = pos + 512ULL * 1024 * 1024;
    int found = 0;
    while (b && pos < hex_size && pos < limit && !found) {
        if (vfs_seek(f, pos)) break;
        int64_t r = vfs_read(f, b, CH + pn - 1);
        if (r < pn) break;
        for (int64_t i = 0; i + pn <= r; i++) {
            if (b[i] == pat[0] && !memcmp(b + i, pat, pn)) { hex_cur = pos + i; found = 1; break; }
        }
        pos += CH;
    }
    free(b);
    vfs_close(f);
    if (!found) show_toast(T(S_TBL_NOTFOUND));
}

static void hex_write_text(const char *txt)
{
    uint8_t v[256]; char bad[8];
    int n = tbl_encode(txt, v, sizeof(v), bad, sizeof(bad));
    if (n < 0) { char m[96]; snprintf(m, sizeof(m), T(S_TBL_BADCHAR), bad); show_toast(m); return; }
    int w = 0;
    for (int i = 0; i < n && hex_cur < hex_size; i++) {
        int e = hex_find_edit(hex_cur);
        if (e < 0 && hex_edits < HEX_MAX_EDITS) { e = hex_edits++; hex_edit_off[e] = hex_cur; }
        if (e < 0) break;
        hex_edit_val[e] = v[i]; w++;
        if (hex_cur + 1 < hex_size) hex_cur++; else break;
    }
    char m[96]; snprintf(m, sizeof(m), T(S_TBL_WROTE), w); show_toast(m);
}

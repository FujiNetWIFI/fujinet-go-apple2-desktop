/*
 * apple2vidtab -- the Video tab's controller (see include/apple2vidtab.h).
 *
 * One refresh = one capture of the soft switches and the video pages, a diff
 * against the previous capture (for the CHANGED and HEAT overlays), and, when
 * anything the picture shows moved, a rebuild: decode the pages into a
 * 560x192 grid, optionally take the session's frame instead, scale into one
 * reused buffer and draw the overlays on it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "a2video.h"
#include "apple2_host.h"
#include "apple2debug.h"
#include "apple2session.h"
#include "apple2vidtab.h"

#define GW APPLE2VID_W
#define GH APPLE2VID_H
#define MAX_FW APPLE2_HOST_MAX_WIDTH
#define MAX_FH APPLE2_HOST_MAX_HEIGHT

_Static_assert(APPLE2HOST_VF_TEXT == 0x40 && APPLE2HOST_VF_80COL == 1, "VF bits");

/* ---- the tables -------------------------------------------------------------------------- */
static const char *const k_view_names[APPLE2VID_V_COUNT] = { "Screen", "Decoded" };
static const char *const k_view_keys[APPLE2VID_V_COUNT]  = { "screen", "decoded" };
static const char *const k_mode_names[APPLE2VID_M_COUNT] = {
    "Text 40", "Text 80", "Lo-res", "Double lo-res", "Hi-res", "Double hi-res" };
static const char *const k_mode_keys[APPLE2VID_M_COUNT] = {
    "text40", "text80", "lores", "dlores", "hires", "dhires" };
static const char *const k_sec_names[APPLE2VID_S_COUNT] = { "Pixel", "Switches", "Colours", "Timing" };
static const char *const k_sec_keys[APPLE2VID_S_COUNT]  = { "pixel", "switches", "colours", "timing" };

static const apple2vid_overlay_info k_overlays[] = {
    { APPLE2VID_OV_GRID,    "grid",    "Grid" },
    { APPLE2VID_OV_MIXED,   "mixed",   "Mixed split" },
    { APPLE2VID_OV_BEAM,    "beam",    "Beam" },
    { APPLE2VID_OV_HEAT,    "heat",    "Heat" },
    { APPLE2VID_OV_CHANGED, "changed", "Changed" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

static const int k_zooms[] = { 0, 1, 2, 3, 4 };

typedef struct {
    const char *name;
    unsigned vf;                /* video-mode bit, 0 for ALTCHARSET */
    unsigned set_addr, clr_addr;
    const char *on, *off;
} sw_def;
static const sw_def k_sw[APPLE2VID_SW_COUNT] = {
    { "TEXT",    APPLE2HOST_VF_TEXT,    0xC051, 0xC050, "text mode", "graphics mode" },
    { "MIXED",   APPLE2HOST_VF_MIXED,   0xC053, 0xC052, "4 text lines below line 160", "full-screen graphics" },
    { "PAGE2",   APPLE2HOST_VF_PAGE2,   0xC055, 0xC054, "page 2 ($0800 text, $4000 hi-res)", "page 1 ($0400 text, $2000 hi-res)" },
    { "HIRES",   APPLE2HOST_VF_HIRES,   0xC057, 0xC056, "hi-res graphics", "lo-res graphics" },
    { "80COL",   APPLE2HOST_VF_80COL,   0xC00D, 0xC00C, "80 columns (aux columns first)", "40 columns" },
    { "80STORE", APPLE2HOST_VF_80STORE, 0xC001, 0xC000, "PAGE2 picks aux for the CPU; the display stays on page 1", "PAGE2 picks the display page" },
    { "ALTCHARSET", 0,                  0xC00F, 0xC00E, "alternate set (lowercase, MouseText)", "primary set (uppercase, flash)" },
    { "DHIRES",  APPLE2HOST_VF_DHIRES,  0xC05E, 0xC05F, "AN3 clear: double modes (with 80COL)", "AN3 set: single (40-column) graphics" },
};

const char *apple2vid_view_name(int v) { return v >= 0 && v < APPLE2VID_V_COUNT ? k_view_names[v] : ""; }
const char *apple2vid_view_key(int v) { return v >= 0 && v < APPLE2VID_V_COUNT ? k_view_keys[v] : ""; }
const char *apple2vid_mode_name(int m) { return m >= 0 && m < APPLE2VID_M_COUNT ? k_mode_names[m] : ""; }
const char *apple2vid_mode_key(int m) { return m >= 0 && m < APPLE2VID_M_COUNT ? k_mode_keys[m] : ""; }
const char *apple2vid_section_name(int s) { return s >= 0 && s < APPLE2VID_S_COUNT ? k_sec_names[s] : ""; }
const char *apple2vid_section_key(int s) { return s >= 0 && s < APPLE2VID_S_COUNT ? k_sec_keys[s] : ""; }

const apple2vid_overlay_info *apple2vid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

const int *apple2vid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

unsigned apple2vid_parse_overlays(const char *list)
{
    char buf[200], *tok, *sp;
    unsigned ov = 0;
    if (!list) return 0;
    snprintf(buf, sizeof buf, "%s", list);
    for (tok = strtok_r(buf, ", ", &sp); tok; tok = strtok_r(NULL, ", ", &sp)) {
        if (!strcasecmp(tok, "all")) ov |= APPLE2VID_OV_ALL;
        else if (!strcasecmp(tok, "none")) ov = 0;
        else
            for (int i = 0; i < N_OVERLAYS; i++)
                if (!strcasecmp(tok, k_overlays[i].name)) ov |= k_overlays[i].bit;
    }
    return ov;
}

/* ---- the tab ------------------------------------------------------------------------------ */
#define PAL_MAX 16
#define HEAT_FULL 255
#define HEAT_STEP 16

struct apple2vid_tab {
    apple2session *s;
    apple2debug *d;
    int env_section;

    int view, follow;
    int c_mode, c_page, c_mixed;        /* the chosen ones (Follow off) */
    unsigned ov;
    int zoom, vw, vh;

    /* captures */
    apple2vid_capture *cap, *prev;
    int have, have_prev, fed, running;
    uint32_t *frame;                    /* the session's frame */
    int fw, fh, have_frame;
    uint64_t fserial;
    uint8_t *heat[2], *chg[2];          /* [0] main, [1] aux; indexed by address */
    int any_heat;
    uint64_t refreshes;

    /* effective state */
    int e_mode, e_page, e_mixed, e_alt;
    unsigned e_vmode;
    int e_text80;                       /* the text lines of MIXED are 80-column */

    /* pictures */
    uint32_t *dec;                      /* 560x192 */
    uint32_t *scr;                      /* the frame as shown */
    uint32_t *pic; size_t pic_cap;
    int bw, bh, pw, ph, ez;
    int dirty;
    uint64_t sig;
    char why[160], status[256];

    /* inspector */
    int hdx, hdy, pdx, pdy;             /* decoded-grid coordinates, -1 none */
    apple2vid_pixel hover, pin;

    /* tables */
    apple2vid_switch sw[APPLE2VID_SW_COUNT];
    int sw_prev[APPLE2VID_SW_COUNT], sw_have;
    apple2vid_pal_entry pal[PAL_MAX];
    int npal;
    char timing_buf[10][96];
    const char *timing_ptr[10];
    int ntiming;
    int beam_line, beam_horz, beam_permille;
};

static void why_set(apple2vid_tab *t, const char *s) { snprintf(t->why, sizeof t->why, "%s", s); }

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    uint32_t *p;
    if (*cap >= need) return 1;
    p = realloc(*buf, need * sizeof *p);
    if (!p) return 0;
    *buf = p; *cap = need;
    return 1;
}

static apple2vid_tab *tab_alloc(void)
{
    apple2vid_tab *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->cap = calloc(1, sizeof *t->cap);
    t->prev = calloc(1, sizeof *t->prev);
    t->frame = calloc((size_t)MAX_FW * MAX_FH, sizeof *t->frame);
    t->dec = calloc((size_t)GW * GH, sizeof *t->dec);
    t->scr = calloc((size_t)MAX_FW * MAX_FH, sizeof *t->scr);
    for (int i = 0; i < 2; i++) {
        t->heat[i] = calloc(0x10000, 1);
        t->chg[i] = calloc(0x10000, 1);
    }
    if (!t->cap || !t->prev || !t->frame || !t->dec || !t->scr || !t->heat[0] ||
        !t->heat[1] || !t->chg[0] || !t->chg[1]) {
        apple2vid_tab_free(t);
        return NULL;
    }
    t->view = APPLE2VID_V_SCREEN;
    t->follow = 1;
    t->c_mode = APPLE2VID_M_TEXT40;
    t->c_page = 1;
    t->ov = APPLE2VID_OV_MIXED | APPLE2VID_OV_BEAM;
    t->zoom = 0;
    t->vw = GW * 2;
    t->vh = GH * 4;
    t->hdx = t->hdy = t->pdx = t->pdy = -1;
    t->dirty = 1;
    t->ez = 1;
    return t;
}

apple2vid_tab *apple2vid_tab_new_for_debugger(apple2debug *d)
{
    apple2vid_tab *t = tab_alloc();
    if (t) t->d = d;
    return t;
}

apple2vid_tab *apple2vid_tab_new(apple2session *s)
{
    apple2vid_tab *t = tab_alloc();
    const char *e;
    if (!t) return NULL;
    t->s = s;
    t->d = s ? apple2session_debugger(s) : NULL;

    if ((e = getenv("APPLE2_DEBUGGER_VIDEO_VIEW")) && *e) {
        for (int i = 0; i < APPLE2VID_V_COUNT; i++)
            if (!strncasecmp(e, k_view_keys[i], strlen(e))) { t->view = i; break; }
    }
    if ((e = getenv("APPLE2_DEBUGGER_VIDEO_MODE")) && *e) {
        for (int i = 0; i < APPLE2VID_M_COUNT; i++)
            if (!strcasecmp(e, k_mode_keys[i])) { t->c_mode = i; t->follow = 0; break; }
    }
    if ((e = getenv("APPLE2_DEBUGGER_VIDEO_PAGE")) && *e) {
        if (atoi(e) == 1 || atoi(e) == 2) { t->c_page = atoi(e); t->follow = 0; }
    }
    if ((e = getenv("APPLE2_DEBUGGER_VIDEO_OVERLAYS")))
        t->ov = apple2vid_parse_overlays(e);
    if ((e = getenv("APPLE2_DEBUGGER_VIDEO_ZOOM")) && *e) {
        if (!strcasecmp(e, "fit")) t->zoom = 0;
        else {
            int z = atoi(e);
            for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
                if (k_zooms[i] == z) t->zoom = z;
        }
    }
    if ((e = getenv("APPLE2_DEBUGGER_VIDEO_SECTION")) && *e) {
        for (int i = 0; i < APPLE2VID_S_COUNT; i++)
            if (!strncasecmp(e, k_sec_keys[i], strlen(e))) { t->env_section = i; break; }
    }
    return t;
}

void apple2vid_tab_free(apple2vid_tab *t)
{
    if (!t) return;
    free(t->cap); free(t->prev); free(t->frame); free(t->dec); free(t->scr); free(t->pic);
    for (int i = 0; i < 2; i++) { free(t->heat[i]); free(t->chg[i]); }
    free(t);
}

void apple2vid_tab_set_view(apple2vid_tab *t, int v)
{
    if (t && v >= 0 && v < APPLE2VID_V_COUNT) { t->view = v; t->dirty = 1; }
}
int apple2vid_tab_view(const apple2vid_tab *t) { return t ? t->view : 0; }
void apple2vid_tab_set_overlays(apple2vid_tab *t, unsigned ov)
{
    if (t) { t->ov = ov & APPLE2VID_OV_ALL; t->dirty = 1; }
}
unsigned apple2vid_tab_overlays(const apple2vid_tab *t) { return t ? t->ov : 0; }
void apple2vid_tab_set_zoom(apple2vid_tab *t, int z)
{
    if (!t) return;
    t->zoom = (z >= 1 && z <= 4) ? z : 0;
    t->dirty = 1;
}
int apple2vid_tab_zoom(const apple2vid_tab *t) { return t ? t->zoom : 0; }
void apple2vid_tab_set_viewport(apple2vid_tab *t, int w, int h)
{
    if (!t) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w != t->vw || h != t->vh) { t->vw = w; t->vh = h; if (!t->zoom) t->dirty = 1; }
}
int apple2vid_tab_env_section(const apple2vid_tab *t) { return t ? t->env_section : 0; }

void apple2vid_tab_set_follow(apple2vid_tab *t, int follow)
{
    if (!t) return;
    follow = follow ? 1 : 0;
    if (!follow && t->follow && t->have) {      /* start from what is shown now */
        t->c_mode = t->e_mode; t->c_page = t->e_page; t->c_mixed = t->e_mixed;
    }
    t->follow = follow;
    t->dirty = 1;
}
int apple2vid_tab_follow(const apple2vid_tab *t) { return t ? t->follow : 1; }
void apple2vid_tab_set_mode(apple2vid_tab *t, int mode)
{
    if (!t || mode < 0 || mode >= APPLE2VID_M_COUNT) return;
    apple2vid_tab_set_follow(t, 0);
    t->c_mode = mode;
    t->dirty = 1;
}
void apple2vid_tab_set_page(apple2vid_tab *t, int page)
{
    if (!t || (page != 1 && page != 2)) return;
    apple2vid_tab_set_follow(t, 0);
    t->c_page = page;
    t->dirty = 1;
}
void apple2vid_tab_set_mixed(apple2vid_tab *t, int mixed)
{
    if (!t) return;
    apple2vid_tab_set_follow(t, 0);
    t->c_mixed = mixed ? 1 : 0;
    t->dirty = 1;
}
int apple2vid_tab_mode(const apple2vid_tab *t) { return t ? (t->have && t->follow ? t->e_mode : t->c_mode) : 0; }
int apple2vid_tab_page(const apple2vid_tab *t) { return t ? (t->have && t->follow ? t->e_page : t->c_page) : 1; }
int apple2vid_tab_mixed(const apple2vid_tab *t) { return t ? (t->have && t->follow ? t->e_mixed : t->c_mixed) : 0; }

/* ---- capture ------------------------------------------------------------------------------ */
static const struct { unsigned lo, len; } k_ranges[] = { { 0x0400, 0x0800 }, { 0x2000, 0x4000 } };

static void capture_machine(apple2vid_tab *t, apple2vid_capture *c)
{
    unsigned line = 0, horz = 0;
    uint64_t cy;

    c->vmode = apple2host_video_mode();
    c->altcharset = apple2host_altcharset();
    c->has_aux = apple2host_has_aux();
    c->cycles = cy = apple2host_cycles();
    c->cycles_per_frame = apple2host_cycles_per_frame();
    c->lines_per_frame = apple2host_lines_per_frame();
    if (!c->cycles_per_frame) c->cycles_per_frame = 17030;
    if (!c->lines_per_frame) c->lines_per_frame = 262;
    if (t->d && apple2debug_is_stopped(t->d)) {
        apple2host_get_beam(&line, &horz);
    } else {
        /* running: the beam counter is the emulator thread's; estimate it */
        const unsigned in_frame = (unsigned)(cy % c->cycles_per_frame);
        line = in_frame / 65;
        horz = in_frame % 65;
    }
    c->beam_line = line;
    c->beam_horz = horz;
    c->frame_count = t->s ? apple2session_frame_count(t->s) : 0;
    for (int b = 0; b < 2; b++)
        for (size_t i = 0; i < sizeof k_ranges / sizeof *k_ranges; i++)
            apple2host_read_video_ram(b, (uint16_t)k_ranges[i].lo,
                                      (b ? c->aux : c->main) + k_ranges[i].lo, k_ranges[i].len);
    c->running = 1;
}

/* keep the last capture's video pages to diff against */
static void save_prev(apple2vid_tab *t)
{
    if (!t->have) return;
    for (size_t i = 0; i < sizeof k_ranges / sizeof *k_ranges; i++) {
        memcpy(t->prev->main + k_ranges[i].lo, t->cap->main + k_ranges[i].lo, k_ranges[i].len);
        memcpy(t->prev->aux + k_ranges[i].lo, t->cap->aux + k_ranges[i].lo, k_ranges[i].len);
    }
    t->have_prev = 1;
}

void apple2vid_tab_feed(apple2vid_tab *t, const apple2vid_capture *cap)
{
    if (!t || !cap) return;
    save_prev(t);
    t->fed = 1;
    memcpy(t->cap, cap, sizeof *t->cap);
    t->cap->frame = NULL;
    if (cap->frame && cap->fw > 0 && cap->fh > 0 && cap->fw <= MAX_FW && cap->fh <= MAX_FH) {
        memcpy(t->frame, cap->frame, (size_t)cap->fw * cap->fh * sizeof *t->frame);
        t->fw = cap->fw; t->fh = cap->fh; t->have_frame = 1;
        t->fserial++;
    }
}

/* returns 1 when there is a capture (t->cap is current) */
static int acquire(apple2vid_tab *t)
{
    apple2vid_capture *c = t->cap;

    if (t->fed) {
        t->have = c->running;
        t->running = c->running;
        return t->have;
    }
    save_prev(t);
    t->running = 0;
    t->have = 0;
    if (t->d) {
        uint8_t probe;
        if (apple2debug_read(t->d, 0, &probe, 1) > 0) {
            capture_machine(t, c);
            t->running = 1;
            t->have = 1;
        }
    }
    if (t->s && t->running) {
        int w = 0, h = 0;
        if (apple2session_copy_frame(t->s, t->frame, &t->fserial, &w, &h) && w > 0 && h > 0 &&
            w <= MAX_FW && h <= MAX_FH) {
            t->fw = w; t->fh = h; t->have_frame = 1;
        }
    }
    return t->have;
}

static void effective(apple2vid_tab *t)
{
    const apple2vid_capture *c = t->cap;
    const unsigned vm = c->vmode;
    const int col80 = (vm & APPLE2HOST_VF_80COL) != 0;
    const int dbl = col80 && (vm & APPLE2HOST_VF_DHIRES);
    const int s80 = (vm & APPLE2HOST_VF_80STORE) != 0;
    int mode, page, mixed;

    t->e_vmode = vm;
    t->e_alt = c->altcharset;
    if (vm & APPLE2HOST_VF_TEXT)
        mode = col80 ? APPLE2VID_M_TEXT80 : APPLE2VID_M_TEXT40;
    else if (vm & APPLE2HOST_VF_HIRES)
        mode = dbl ? APPLE2VID_M_DHIRES : APPLE2VID_M_HIRES;
    else
        mode = dbl ? APPLE2VID_M_DLORES : APPLE2VID_M_LORES;
    page = ((vm & APPLE2HOST_VF_PAGE2) && !s80) ? 2 : 1;
    mixed = (vm & APPLE2HOST_VF_MIXED) && !(vm & APPLE2HOST_VF_TEXT);
    if (t->follow) {
        t->e_text80 = col80;
    } else {
        mode = t->c_mode; page = t->c_page; mixed = t->c_mixed;
        t->e_text80 = (mode == APPLE2VID_M_DLORES || mode == APPLE2VID_M_DHIRES);
    }
    t->e_mode = mode; t->e_page = page; t->e_mixed = mixed;
}

static int is_text(int m) { return m == APPLE2VID_M_TEXT40 || m == APPLE2VID_M_TEXT80; }

/* the mode of the cell on decoded line y */
static int cell_mode(const apple2vid_tab *t, int y)
{
    if (is_text(t->e_mode)) return t->e_mode;
    if (t->e_mixed && y >= APPLE2VID_MIXED_LINE)
        return t->e_text80 ? APPLE2VID_M_TEXT80 : APPLE2VID_M_TEXT40;
    return t->e_mode;
}

static unsigned text_base(const apple2vid_tab *t) { return t->e_page == 2 ? 0x0800 : 0x0400; }
static unsigned hires_base(const apple2vid_tab *t) { return t->e_page == 2 ? 0x4000 : 0x2000; }

/* ---- decoding ----------------------------------------------------------------------------- */
static uint32_t opaque(uint32_t rgb) { return 0xFF000000u | (rgb & 0xFFFFFFu); }

static void render_row(const apple2vid_tab *t, int y, uint32_t *out)
{
    const apple2vid_capture *c = t->cap;
    const int m = cell_mode(t, y);
    const int row = y / 8, gy = y % 8;
    const unsigned ca = a2video_text_row_addr((uint16_t)text_base(t), row);

    switch (m) {
    case APPLE2VID_M_TEXT40:
    case APPLE2VID_M_TEXT80: {
        const int n = m == APPLE2VID_M_TEXT80 ? 80 : 40;
        const int cw = GW / n;
        for (int col = 0; col < n; col++) {
            const uint8_t b = m == APPLE2VID_M_TEXT80
                ? ((col & 1) ? c->main[ca + col / 2] : c->aux[ca + col / 2])
                : c->main[ca + col];
            uint8_t rows[7];
            int inv = 0;
            a2video_glyph(b, t->e_alt, rows, &inv, NULL, NULL);
            const uint8_t bits = gy < 7 ? rows[gy] : 0;
            for (int gx = 0; gx < 7; gx++) {
                const int on = (gx >= 1 && gx <= 5) && (bits & (1 << (gx - 1)));
                const uint32_t px = opaque((on != inv) ? 0xFFFFFF : 0x000000);
                if (cw == 14) { out[col * 14 + gx * 2] = px; out[col * 14 + gx * 2 + 1] = px; }
                else out[col * 7 + gx] = px;
            }
        }
        break;
    }
    case APPLE2VID_M_LORES:
        for (int col = 0; col < 40; col++) {
            const uint8_t b = c->main[ca + col];
            const uint32_t px = opaque(a2video_lores_rgb(gy < 4 ? b & 15 : b >> 4));
            for (int i = 0; i < 14; i++) out[col * 14 + i] = px;
        }
        break;
    case APPLE2VID_M_DLORES:
        for (int col = 0; col < 80; col++) {
            const int aux = !(col & 1);
            const uint8_t b = aux ? c->aux[ca + col / 2] : c->main[ca + col / 2];
            int nib = gy < 4 ? b & 15 : b >> 4;
            if (aux) nib = a2video_dlores_rotate(nib);
            const uint32_t px = opaque(a2video_lores_rgb(nib));
            for (int i = 0; i < 7; i++) out[col * 7 + i] = px;
        }
        break;
    case APPLE2VID_M_HIRES: {
        const unsigned ha = a2video_hires_row_addr((uint16_t)hires_base(t), y);
        for (int x = 0; x < 280; x++) {
            const uint32_t px = opaque(a2video_hires_pixel(c->main + ha, x));
            out[2 * x] = px; out[2 * x + 1] = px;
        }
        break;
    }
    default: {                          /* DHIRES */
        const unsigned ha = a2video_hires_row_addr((uint16_t)hires_base(t), y);
        uint8_t dots[GW];
        for (int j = 0; j < 40; j++) {
            const uint8_t a = c->aux[ha + j], mn = c->main[ha + j];
            for (int i = 0; i < 7; i++) {
                dots[14 * j + i] = (a >> i) & 1;
                dots[14 * j + 7 + i] = (mn >> i) & 1;
            }
        }
        for (int g = 0; g < GW / 4; g++) {
            const int n = dots[4 * g] | dots[4 * g + 1] << 1 | dots[4 * g + 2] << 2 | dots[4 * g + 3] << 3;
            const uint32_t px = opaque(a2video_lores_rgb(a2video_dhires_colour(n)));
            for (int i = 0; i < 4; i++) out[4 * g + i] = px;
        }
        break;
    }
    }
}

static void decode(apple2vid_tab *t)
{
    for (int y = 0; y < GH; y++) render_row(t, y, &t->dec[(size_t)y * GW]);
}

/* ---- the palette table ---------------------------------------------------------------------- */
static void build_palette(apple2vid_tab *t)
{
    int n = 0;
    memset(t->pal, 0, sizeof t->pal);
    if (is_text(t->e_mode)) {
        t->pal[n].name = "black"; t->pal[n++].rgb = opaque(0x000000);
        t->pal[n].name = "white"; t->pal[n++].rgb = opaque(0xFFFFFF);
    } else if (t->e_mode == APPLE2VID_M_HIRES) {
        static const uint32_t k[6] = { 0x000000, 0xFFFFFF, 0x14F53C, 0xFF44FD, 0xFF6A3C, 0x14CFFD };
        for (int i = 0; i < 6; i++) {
            t->pal[n].name = a2video_hires_name(k[i]);
            t->pal[n++].rgb = opaque(k[i]);
        }
    } else {
        for (int i = 0; i < 16; i++) {
            t->pal[n].name = a2video_lores_name(i);
            t->pal[n++].rgb = opaque(a2video_lores_rgb(i));
        }
    }
    t->npal = n;
    for (size_t i = 0; i < (size_t)GW * GH; i++)
        for (int k = 0; k < n; k++)
            if (t->dec[i] == t->pal[k].rgb) { t->pal[k].uses++; break; }
}

static int pal_index(const apple2vid_tab *t, uint32_t rgb)
{
    for (int k = 0; k < t->npal; k++)
        if (t->pal[k].rgb == rgb) return k;
    return -1;
}

/* ---- cells ---------------------------------------------------------------------------------- */
typedef struct {
    int mode;
    int rx, ry, rw, rh;         /* the cell */
    int col, row;
    int main_addr, aux_addr;
    int own_aux;                /* the cell's own byte is the aux one */
    int half;                   /* lo-res: 0 top, 1 bottom */
} cellinfo;

static void cell_at(const apple2vid_tab *t, int dx, int dy, cellinfo *o)
{
    const int m = cell_mode(t, dy);
    const int pair = dx / 14;           /* byte column within the line */
    const int ca = (int)a2video_text_row_addr((uint16_t)text_base(t), dy / 8);
    memset(o, 0, sizeof *o);
    o->mode = m;
    o->main_addr = o->aux_addr = -1;
    switch (m) {
    case APPLE2VID_M_TEXT40:
    case APPLE2VID_M_LORES:
        o->rx = pair * 14; o->rw = 14;
        o->col = pair; o->row = dy / 8;
        o->main_addr = ca + pair;
        if (m == APPLE2VID_M_TEXT40) { o->ry = o->row * 8; o->rh = 8; }
        else { o->half = (dy % 8) >= 4; o->ry = o->row * 8 + o->half * 4; o->rh = 4; }
        break;
    case APPLE2VID_M_TEXT80:
    case APPLE2VID_M_DLORES:
        o->col = dx / 7; o->rx = o->col * 7; o->rw = 7;
        o->row = dy / 8;
        o->main_addr = o->aux_addr = ca + o->col / 2;
        o->own_aux = !(o->col & 1);
        if (m == APPLE2VID_M_TEXT80) { o->ry = o->row * 8; o->rh = 8; }
        else { o->half = (dy % 8) >= 4; o->ry = o->row * 8 + o->half * 4; o->rh = 4; }
        break;
    case APPLE2VID_M_HIRES:
        o->col = pair; o->row = dy;
        o->rx = pair * 14; o->rw = 14; o->ry = dy; o->rh = 1;
        o->main_addr = (int)a2video_hires_row_addr((uint16_t)hires_base(t), dy) + pair;
        break;
    default:                            /* DHIRES */
        o->col = dx / 7; o->row = dy;
        o->rx = o->col * 7; o->rw = 7; o->ry = dy; o->rh = 1;
        o->main_addr = o->aux_addr =
            (int)a2video_hires_row_addr((uint16_t)hires_base(t), dy) + o->col / 2;
        o->own_aux = !(o->col & 1);
        break;
    }
}

/* ---- the inspector -------------------------------------------------------------------------- */
static void locate(apple2vid_tab *t, int dx, int dy, apple2vid_pixel *o)
{
    cellinfo ci;
    char *p;
    int left;
    const apple2vid_capture *c = t->cap;

    memset(o, 0, sizeof *o);
    o->main_addr = o->aux_addr = o->main_val = o->aux_val = o->pal = -1;
    if (!t->have || dx < 0 || dy < 0 || dx >= GW || dy >= GH) return;
    cell_at(t, dx, dy, &ci);
    o->valid = 1;
    o->dx = dx; o->dy = dy;
    if (t->view == APPLE2VID_V_SCREEN && t->have_frame) {
        o->x = dx * t->fw / GW; o->y = dy * t->fh / GH;
    } else {
        o->x = dx; o->y = dy;
    }
    o->mode = ci.mode;
    o->page = t->e_page;
    o->text_cell = is_text(ci.mode);
    o->col = ci.col; o->row = ci.row;
    o->main_addr = ci.main_addr;
    o->aux_addr = ci.aux_addr;
    o->main_val = c->main[ci.main_addr & 0xFFFF];
    o->aux_val = ci.aux_addr >= 0 ? c->aux[ci.aux_addr & 0xFFFF] : -1;
    o->rx = ci.rx; o->ry = ci.ry; o->rw = ci.rw; o->rh = ci.rh;
    o->rgb = t->dec[(size_t)dy * GW + dx];
    o->pal = pal_index(t, o->rgb);

    p = o->text;
    left = (int)sizeof o->text;
#define PUT(...) do { \
        int w_ = snprintf(p, (size_t)left, __VA_ARGS__); \
        if (w_ < 0) { w_ = 0; } \
        if (w_ >= left) { w_ = left - 1; } \
        p += w_; left -= w_; \
    } while (0)
    PUT("%s%s page %d  ", t->view == APPLE2VID_V_SCREEN ? "Screen: " : "", apple2vid_mode_name(ci.mode), t->e_page);
    switch (ci.mode) {
    case APPLE2VID_M_TEXT40:
    case APPLE2VID_M_TEXT80: {
        const int b = (ci.mode == APPLE2VID_M_TEXT80 && ci.own_aux) ? o->aux_val : o->main_val;
        uint8_t rows[7];
        int inv = 0, fl = 0, mt = 0;
        char ch = a2video_glyph((uint8_t)b, t->e_alt, rows, &inv, &fl, &mt);
        PUT("col %d row %d\n", ci.col, ci.row);
        if (ci.mode == APPLE2VID_M_TEXT80)
            PUT("%s $%04X = $%02X   (pair: aux $%02X, main $%02X)\n", ci.own_aux ? "aux" : "main",
                ci.main_addr, b, o->aux_val, o->main_val);
        else
            PUT("main $%04X = $%02X\n", ci.main_addr, b);
        PUT("character '%c'  %s", ch, mt ? "MouseText" : inv ? "inverse" : fl ? "flashing" : "normal");
        break;
    }
    case APPLE2VID_M_LORES: {
        const int nib = ci.half ? o->main_val >> 4 : o->main_val & 15;
        PUT("block %d,%d\nmain $%04X = $%02X  (low nibble %X on top, high nibble %X below)\n"
            "this block: %s (%d)",
            ci.col, ci.row * 2 + ci.half, ci.main_addr, o->main_val, o->main_val & 15, o->main_val >> 4,
            a2video_lores_name(nib), nib);
        break;
    }
    case APPLE2VID_M_DLORES: {
        const int raw = ci.half ? (ci.own_aux ? o->aux_val : o->main_val) >> 4
                                : (ci.own_aux ? o->aux_val : o->main_val) & 15;
        const int shown = ci.own_aux ? a2video_dlores_rotate(raw) : raw;
        PUT("block %d,%d\n%s $%04X: aux $%02X, main $%02X\nthis block: %s nibble %X%s = %s (%d)",
            ci.col, ci.row * 2 + ci.half, ci.own_aux ? "aux" : "main", ci.main_addr, o->aux_val, o->main_val,
            ci.own_aux ? "aux" : "main", raw, ci.own_aux ? " rotated left once" : "",
            a2video_lores_name(shown), shown);
        break;
    }
    case APPLE2VID_M_HIRES: {
        const int hx = dx / 2;
        const int bit = hx % 7;
        const int lit = (o->main_val >> bit) & 1;
        PUT("x %d y %d\nmain $%04X = $%02X  pixel bit %d = %d  palette bit 7 = %d (%s)\ncolour: %s",
            hx, dy, ci.main_addr, o->main_val, bit, lit, (o->main_val >> 7) & 1,
            (o->main_val & 0x80) ? "blue/orange" : "violet/green", a2video_hires_name(o->rgb));
        break;
    }
    default: {                          /* DHIRES */
        const int dot = dx % 14;
        const int own = dot < 7 ? o->aux_val : o->main_val;
        const int g = dx & ~3;
        int n = 0;
        for (int i = 0; i < 4; i++) {
            const int q = g + i, j = q / 14, k = q % 14;
            const uint8_t bb = k < 7 ? c->aux[(ci.main_addr - ci.col / 2 + j) & 0xFFFF]
                                     : c->main[(ci.main_addr - ci.col / 2 + j) & 0xFFFF];
            n |= ((bb >> (k % 7)) & 1) << i;
        }
        PUT("dot %d y %d\naux $%04X = $%02X  main $%04X = $%02X  (this dot: %s bit %d = %d)\n"
            "4-dot group %X = %s",
            dx, dy, ci.main_addr, o->aux_val, ci.main_addr, o->main_val, dot < 7 ? "aux" : "main",
            dot % 7, (own >> (dot % 7)) & 1, n, a2video_lores_name(a2video_dhires_colour(n)));
        break;
    }
    }
    if (is_text(ci.mode) || ci.mode == APPLE2VID_M_LORES || ci.mode == APPLE2VID_M_DLORES)
        PUT("\ncolour #%06X", o->rgb & 0xFFFFFF);
    else
        PUT("\n#%06X", o->rgb & 0xFFFFFF);
    if (t->view == APPLE2VID_V_SCREEN)
        PUT("\n(screen pixel %d,%d mapped onto the decoded cell)", o->x, o->y);
#undef PUT
}

static int to_decoded(const apple2vid_tab *t, int x, int y, int *dx, int *dy)
{
    int bx, by;
    if (!t->ez || x < 0 || y < 0 || !t->pw) return 0;
    bx = x / t->ez; by = y / t->ez;
    if (bx >= t->bw || by >= t->bh) return 0;
    if (t->view == APPLE2VID_V_SCREEN) {
        *dx = bx * GW / t->bw; *dy = by * GH / t->bh;
    } else {
        *dx = bx; *dy = by;
    }
    return 1;
}

int apple2vid_tab_hover(apple2vid_tab *t, int x, int y)
{
    char old[sizeof t->hover.text];
    int dx, dy;
    if (!t) return 0;
    snprintf(old, sizeof old, "%s", t->hover.valid ? t->hover.text : "");
    if (to_decoded(t, x, y, &dx, &dy)) {
        t->hdx = dx; t->hdy = dy;
        locate(t, dx, dy, &t->hover);
    } else {
        t->hdx = t->hdy = -1;
        memset(&t->hover, 0, sizeof t->hover);
    }
    return strcmp(old, t->hover.valid ? t->hover.text : "") != 0;
}

void apple2vid_tab_unpin(apple2vid_tab *t)
{
    if (!t) return;
    t->pdx = t->pdy = -1;
    memset(&t->pin, 0, sizeof t->pin);
}

void apple2vid_tab_pin(apple2vid_tab *t, int x, int y)
{
    int dx, dy;
    apple2vid_pixel tmp;
    if (!t) return;
    if (!to_decoded(t, x, y, &dx, &dy)) return;
    locate(t, dx, dy, &tmp);
    if (!tmp.valid) return;
    if (t->pin.valid && t->pin.rx == tmp.rx && t->pin.ry == tmp.ry && t->pin.rw == tmp.rw &&
        t->pin.rh == tmp.rh) {
        apple2vid_tab_unpin(t);
        return;
    }
    t->pdx = dx; t->pdy = dy;
    t->pin = tmp;
}

const apple2vid_pixel *apple2vid_tab_pixel(const apple2vid_tab *t)
{
    if (!t) return NULL;
    if (t->pin.valid) return &t->pin;
    if (t->hover.valid) return &t->hover;
    return NULL;
}

static void mark_rect(const apple2vid_tab *t, const apple2vid_pixel *p, int r[4])
{
    const int x0 = p->rx * t->pw / GW, x1 = (p->rx + p->rw) * t->pw / GW;
    const int y0 = p->ry * t->ph / GH, y1 = (p->ry + p->rh) * t->ph / GH;
    r[0] = x0; r[1] = y0;
    r[2] = x1 > x0 ? x1 - x0 : 1;
    r[3] = y1 > y0 ? y1 - y0 : 1;
}

int apple2vid_tab_marks(const apple2vid_tab *t, int hover[4], int pin[4])
{
    int r = 0;
    if (!t || !t->pw) return 0;
    if (t->hover.valid) { mark_rect(t, &t->hover, hover); r |= 1; }
    if (t->pin.valid) { mark_rect(t, &t->pin, pin); r |= 2; }
    return r;
}

static int add_bp(apple2vid_tab *t, int type, unsigned a)
{
    char err[96];
    return apple2debug_breakpoint_add(t->d, type, (uint16_t)a, (uint16_t)a, NULL, err, sizeof err);
}

int apple2vid_tab_break_on_pixel(apple2vid_tab *t)
{
    const apple2vid_pixel *p;
    int first, id;
    if (!t || !t->d || !(p = apple2vid_tab_pixel(t))) return -1;
    first = add_bp(t, APPLE2DEBUG_BP_WRITE, (unsigned)p->main_addr);
    if (first < 0) return -1;
    if (p->aux_addr >= 0) {
        id = add_bp(t, APPLE2DEBUG_BP_WRITE, (unsigned)p->aux_addr);
        if (id < 0) return -1;
    }
    return first;
}

int apple2vid_tab_break_on_switch(apple2vid_tab *t, int index)
{
    int first, id;
    const int rw = APPLE2DEBUG_BP_READ | APPLE2DEBUG_BP_WRITE;
    if (!t || !t->d || index < 0 || index >= APPLE2VID_SW_COUNT) return -1;
    first = add_bp(t, rw, k_sw[index].set_addr);
    if (first < 0) return -1;
    id = add_bp(t, rw, k_sw[index].clr_addr);
    return id < 0 ? -1 : first;
}

int apple2vid_tab_pixel_palette_index(const apple2vid_tab *t)
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(t);
    return p ? p->pal : -1;
}

/* ---- the picture ---------------------------------------------------------------------------- */
typedef struct { uint32_t *p; int w, h; } canvas;

static void px_blend(canvas *c, int x, int y, uint32_t rgb, int a)
{
    uint32_t *d;
    uint32_t o;
    int ch;
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    d = &c->p[(size_t)y * c->w + x];
    o = 0xFF000000u;
    for (ch = 0; ch < 24; ch += 8) {
        const int s = (int)((rgb >> ch) & 0xFF), q = (int)((*d >> ch) & 0xFF);
        o |= (uint32_t)((q * (256 - a) + s * a) >> 8) << ch;
    }
    *d = o;
}

static void fill_rect(canvas *c, int x, int y, int w, int h, uint32_t rgb, int a)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px_blend(c, x + i, y + j, rgb, a);
}

static void hline(canvas *c, int y, int x0, int x1, uint32_t rgb, int a)
{
    for (int x = x0; x < x1; x++) px_blend(c, x, y, rgb, a);
}

static void vline(canvas *c, int x, int y0, int y1, uint32_t rgb, int a)
{
    for (int y = y0; y < y1; y++) px_blend(c, x, y, rgb, a);
}

/* decoded grid -> picture, per axis */
static int gx(const apple2vid_tab *t, int v) { return v * t->pw / GW; }
static int gy(const apple2vid_tab *t, int v) { return v * t->ph / GH; }

static void cell_rect(const apple2vid_tab *t, canvas *cv, int rx, int ry, int rw, int rh,
                      uint32_t rgb, int a)
{
    const int x0 = gx(t, rx), x1 = gx(t, rx + rw), y0 = gy(t, ry), y1 = gy(t, ry + rh);
    fill_rect(cv, x0, y0, x1 > x0 ? x1 - x0 : 1, y1 > y0 ? y1 - y0 : 1, rgb, a);
}

/* every byte the page shows: cb(rx, ry, rw, rh, bank, addr) */
typedef void (*cell_cb)(const apple2vid_tab *t, canvas *cv, int rx, int ry, int rw, int rh,
                        int bank, int addr);
static void for_cells(const apple2vid_tab *t, canvas *cv, cell_cb cb)
{
    for (int y = 0; y < GH; ) {
        const int m = cell_mode(t, y);
        int cx;
        cellinfo ci;
        cell_at(t, 0, y, &ci);
        for (cx = 0; cx < GW; cx += ci.rw) {
            cell_at(t, cx, y, &ci);
            if (m == APPLE2VID_M_TEXT80 || m == APPLE2VID_M_DLORES || m == APPLE2VID_M_DHIRES)
                cb(t, cv, ci.rx, ci.ry, ci.rw, ci.rh, ci.own_aux ? 1 : 0, ci.main_addr);
            else
                cb(t, cv, ci.rx, ci.ry, ci.rw, ci.rh, 0, ci.main_addr);
        }
        /* lo-res blocks are 4 lines, everything else advances by its cell */
        y += ci.rh;
    }
}

static void cb_heat(const apple2vid_tab *t, canvas *cv, int rx, int ry, int rw, int rh, int bank, int addr)
{
    const int h = t->heat[bank][addr & 0xFFFF];
    if (h) cell_rect(t, cv, rx, ry, rw, rh, 0xFF3030, 40 + h * 150 / 255);
}

static void cb_chg(const apple2vid_tab *t, canvas *cv, int rx, int ry, int rw, int rh, int bank, int addr)
{
    if (t->chg[bank][addr & 0xFFFF]) cell_rect(t, cv, rx, ry, rw, rh, 0x00E0FF, 150);
}

static void draw_overlays(apple2vid_tab *t, canvas *cv)
{
    const unsigned ov = t->ov;
    if (!t->have) return;
    if (ov & APPLE2VID_OV_HEAT) for_cells(t, cv, cb_heat);
    if (ov & APPLE2VID_OV_CHANGED) for_cells(t, cv, cb_chg);
    if (ov & APPLE2VID_OV_GRID) {
        for (int y = 0; y < GH; ) {
            cellinfo ci;
            const int step = 8;
            cell_at(t, 0, y, &ci);
            const int m = ci.mode;
            const int ycap = (t->e_mixed && !is_text(t->e_mode) && y < APPLE2VID_MIXED_LINE)
                                 ? APPLE2VID_MIXED_LINE : GH;
            const int cwid = (m == APPLE2VID_M_TEXT80 || m == APPLE2VID_M_DLORES ||
                              m == APPLE2VID_M_DHIRES) ? 7 : 14;
            const int hstep = (m == APPLE2VID_M_LORES || m == APPLE2VID_M_DLORES) ? 4 : step;
            for (int x = 0; x < GW; x += cwid)
                vline(cv, gx(t, x), gy(t, y), gy(t, ycap), 0xFFFFFF, 50);
            for (int yy = y; yy < ycap; yy += hstep)
                hline(cv, gy(t, yy), 0, cv->w, 0xFFFFFF, 50);
            y = ycap;
        }
    }
    if ((ov & APPLE2VID_OV_MIXED) && t->e_mixed)
        hline(cv, gy(t, APPLE2VID_MIXED_LINE), 0, cv->w, 0xFF4040, 230);
    if ((ov & APPLE2VID_OV_BEAM) && t->beam_line >= 0 && t->beam_line < GH) {
        const int y = gy(t, t->beam_line);
        hline(cv, y, 0, cv->w, 0xFFE000, 220);
        if (t->beam_horz >= 25 && t->beam_horz < 65) {
            const int x = gx(t, (t->beam_horz - 25) * 14);
            fill_rect(cv, x - 1, y - 2, 3, 5, 0xFFE000, 255);
        }
    }
    if (t->hover.valid || t->pin.valid) { /* marks are the frontend's, nothing here */ }
}

static int pick_zoom(const apple2vid_tab *t)
{
    int z = t->zoom;
    if (z == 0) {
        z = 1;
        for (int i = 2; i <= 4; i++)
            if (t->bw * i <= t->vw && t->bh * i <= t->vh) z = i;
    }
    return z;
}

static void build_picture(apple2vid_tab *t)
{
    const uint32_t *src;
    int z;
    canvas cv;

    t->why[0] = '\0';
    t->pw = t->ph = 0;
    if (t->view == APPLE2VID_V_SCREEN) {
        if (!t->have_frame) {
            why_set(t, t->s || t->fed ? "No frame has been drawn yet." :
                                         "The Screen view needs a running session.");
            return;
        }
        t->bw = t->fw; t->bh = t->fh;
        src = t->frame;
    } else {
        t->bw = GW; t->bh = GH;
        src = t->dec;
    }
    z = pick_zoom(t);
    t->ez = z;
    t->pw = t->bw * z; t->ph = t->bh * z;
    if (!ensure(&t->pic, &t->pic_cap, (size_t)t->pw * t->ph)) {
        t->pw = t->ph = 0;
        why_set(t, "Out of memory.");
        return;
    }
    for (int y = 0; y < t->ph; y++) {
        const uint32_t *s = &src[(size_t)(y / z) * t->bw];
        uint32_t *d = &t->pic[(size_t)y * t->pw];
        for (int x = 0; x < t->pw; x++) d[x] = s[x / z] | 0xFF000000u;
    }
    cv.p = t->pic; cv.w = t->pw; cv.h = t->ph;
    draw_overlays(t, &cv);
}

/* ---- tables ----------------------------------------------------------------------------------- */
static void build_tables(apple2vid_tab *t)
{
    const apple2vid_capture *c = t->cap;
    int n = 0;
    char fsz[24];

    for (int i = 0; i < APPLE2VID_SW_COUNT; i++) {
        const int on = k_sw[i].vf ? (c->vmode & k_sw[i].vf) != 0 : c->altcharset != 0;
        apple2vid_switch *s = &t->sw[i];
        if (!t->sw_have || on != t->sw_prev[i]) s->age = t->sw_have ? 0 : 1000;
        t->sw_prev[i] = on;
        s->name = k_sw[i].name;
        s->on = on;
        s->set_addr = k_sw[i].set_addr;
        s->clr_addr = k_sw[i].clr_addr;
        snprintf(s->meaning, sizeof s->meaning, "%s", on ? k_sw[i].on : k_sw[i].off);
    }
    t->sw_have = 1;

    t->beam_line = (int)c->beam_line;
    t->beam_horz = (int)c->beam_horz;
    {
        const int lines = c->lines_per_frame ? (int)c->lines_per_frame : 262;
        t->beam_permille = (int)(((long)c->beam_line * 65 + c->beam_horz) * 1000 / ((long)lines * 65));
        if (t->beam_permille > 1000) t->beam_permille = 1000;
    }
#define T(...) do { if (n < 10) { snprintf(t->timing_buf[n], sizeof t->timing_buf[n], __VA_ARGS__); \
                                  t->timing_ptr[n] = t->timing_buf[n]; n++; } } while (0)
    T("Mode\t%s, page %d%s", apple2vid_mode_name(t->e_mode), t->e_page, t->e_mixed ? ", mixed" : "");
    T("Cycles per frame\t%u (%u lines)", c->cycles_per_frame, c->lines_per_frame);
    T("Beam\tline %d, horizontal %d", t->beam_line, t->beam_horz);
    T("Vertical blank\t%s", t->beam_line >= GH ? "yes" : "no");
    T("Frame count\t%llu", (unsigned long long)c->frame_count);
    T("Cycles\t%llu", (unsigned long long)c->cycles);
    if (t->have_frame) T("Frame size\t%dx%d", t->fw, t->fh);
    t->ntiming = n;
#undef T
    if (t->have_frame) snprintf(fsz, sizeof fsz, "%dx%d", t->fw, t->fh);
    else snprintf(fsz, sizeof fsz, "none yet");
    snprintf(t->status, sizeof t->status, "%s: %s, page %d%s, %s\nscreen frame %s  beam line %d%s",
             apple2vid_view_name(t->view), apple2vid_mode_name(t->e_mode), t->e_page,
             t->e_mixed ? ", mixed" : "", t->follow ? "following the screen" : "chosen mode",
             fsz,
             t->beam_line, t->beam_line >= GH ? " (blanking)" : "");
}

/* ---- refresh ---------------------------------------------------------------------------------- */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

static void note_changes(apple2vid_tab *t)
{
    int any = 0;
    for (int b = 0; b < 2; b++) {
        const uint8_t *now = b ? t->cap->aux : t->cap->main;
        const uint8_t *was = b ? t->prev->aux : t->prev->main;
        for (size_t i = 0; i < sizeof k_ranges / sizeof *k_ranges; i++)
            for (unsigned a = k_ranges[i].lo; a < k_ranges[i].lo + k_ranges[i].len; a++) {
                int ch = t->have_prev && now[a] != was[a];
                t->chg[b][a] = (uint8_t)ch;
                if (ch) t->heat[b][a] = HEAT_FULL;
                else if (t->heat[b][a]) t->heat[b][a] = t->heat[b][a] > HEAT_STEP ? t->heat[b][a] - HEAT_STEP : 0;
                if (t->heat[b][a]) any = 1;
            }
    }
    t->any_heat = any;
}

unsigned apple2vid_tab_refresh(apple2vid_tab *t, int force)
{
    uint64_t sig = 1469598103934665603ull;
    unsigned r = 0;
    int fresh;

    if (!t) return 0;
    t->refreshes++;
    if (!acquire(t)) {
        why_set(t, "The machine is not running.");
        t->pw = t->ph = 0;
        t->status[0] = '\0';
        return APPLE2VID_TAB_OFF | APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES;
    }
    for (int i = 0; i < APPLE2VID_SW_COUNT; i++) if (t->sw_have && t->sw[i].age < 1000) t->sw[i].age++;
    effective(t);
    note_changes(t);

    {
        const struct { int v[8]; } st = { { t->view, t->e_mode, t->e_page, t->e_mixed, t->e_alt,
                                            (int)t->ov, t->zoom, (t->zoom ? 0 : t->vw * 4096 + t->vh) } };
        sig = fnv(sig, &st, sizeof st);
        sig = fnv(sig, &t->fserial, sizeof t->fserial);
        sig = fnv(sig, &t->fw, sizeof t->fw);
        for (size_t i = 0; i < sizeof k_ranges / sizeof *k_ranges; i++) {
            sig = fnv(sig, t->cap->main + k_ranges[i].lo, k_ranges[i].len);
            sig = fnv(sig, t->cap->aux + k_ranges[i].lo, k_ranges[i].len);
        }
        if (t->ov & APPLE2VID_OV_BEAM) sig = fnv(sig, &t->cap->beam_line, sizeof t->cap->beam_line);
    }
    fresh = force || t->dirty || sig != t->sig;
    if (fresh || (t->any_heat && (t->ov & (APPLE2VID_OV_HEAT | APPLE2VID_OV_CHANGED)))) {
        t->sig = sig;
        decode(t);
        build_palette(t);
        build_tables(t);
        build_picture(t);
        t->dirty = 0;
        if (t->hdx >= 0) locate(t, t->hdx, t->hdy, &t->hover);
        if (t->pdx >= 0) locate(t, t->pdx, t->pdy, &t->pin);
        for (int k = 0; k < t->npal; k++) t->pal[k].inspected = 0;
        {
            const apple2vid_pixel *p = apple2vid_tab_pixel(t);
            if (p && p->pal >= 0) t->pal[p->pal].inspected = 1;
        }
        r |= APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES;
    } else {
        /* nothing moved: still keep the clocks and ages fresh for the tables */
        build_tables(t);
        r |= APPLE2VID_TAB_TABLES;
    }
    return r;
}

const uint32_t *apple2vid_tab_picture(apple2vid_tab *t, int *w, int *h)
{
    if (!t || !t->pw) return NULL;
    if (w) *w = t->pw;
    if (h) *h = t->ph;
    return t->pic;
}
const char *apple2vid_tab_why(const apple2vid_tab *t) { return t ? t->why : ""; }
int apple2vid_tab_effective_zoom(const apple2vid_tab *t) { return t ? t->ez : 1; }
const char *apple2vid_tab_status(const apple2vid_tab *t) { return t ? t->status : ""; }
int apple2vid_tab_switches(const apple2vid_tab *t, const apple2vid_switch **out)
{
    if (!t || !t->sw_have) { if (out) *out = NULL; return 0; }
    if (out) *out = t->sw;
    return APPLE2VID_SW_COUNT;
}
int apple2vid_tab_palette(const apple2vid_tab *t, const apple2vid_pal_entry **out)
{
    if (!t || !t->have) { if (out) *out = NULL; return 0; }
    if (out) *out = t->pal;
    return t->npal;
}
int apple2vid_tab_timing(const apple2vid_tab *t, const char *const **lines)
{
    if (!t || !t->have) { if (lines) *lines = NULL; return 0; }
    if (lines) *lines = t->timing_ptr;
    return t->ntiming;
}
int apple2vid_tab_beam_permille(const apple2vid_tab *t) { return t && t->have ? t->beam_permille : 0; }

int apple2vid_tab_text(apple2vid_tab *t, int section, char *dst, int dstsz)
{
    int n = 0;
#define P(...) do { if (n < dstsz) n += snprintf(dst + n, (size_t)(dstsz - n), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!t || !t->have) { P("The machine is not running.\n"); return n < dstsz ? n : dstsz - 1; }
    switch (section) {
    case APPLE2VID_S_SWITCHES:
        for (int i = 0; i < APPLE2VID_SW_COUNT; i++) {
            const apple2vid_switch *s = &t->sw[i];
            P("%-10s %-3s  age %-4d  on $%04X  off $%04X  %s\n", s->name, s->on ? "on" : "off",
              s->age >= 1000 ? 999 : s->age, s->set_addr, s->clr_addr, s->meaning);
        }
        break;
    case APPLE2VID_S_COLOURS:
        for (int i = 0; i < t->npal; i++)
            P("%-12s #%06X  %ld pixels%s\n", t->pal[i].name, t->pal[i].rgb & 0xFFFFFF, t->pal[i].uses,
              t->pal[i].inspected ? "  <- inspected" : "");
        break;
    case APPLE2VID_S_TIMING:
        for (int i = 0; i < t->ntiming; i++) {
            char line[96], *tab;
            snprintf(line, sizeof line, "%s", t->timing_buf[i]);
            tab = strchr(line, '\t');
            if (tab) *tab = '\0';
            P("%-17s %s\n", line, tab ? tab + 1 : "");
        }
        break;
    default:
        P("%s\n", t->status);
        if (apple2vid_tab_pixel(t)) P("%s\n", apple2vid_tab_pixel(t)->text);
        break;
    }
#undef P
    return n < dstsz ? n : dstsz - 1;
}

/*
 * The Video tab controller (apple2vidtab.h). The pure parts feed synthetic soft
 * switches and video memory through apple2vid_tab_feed and check pictures
 * against hand-computed pixels; the last part runs against a real machine
 * (Break on writes, the console `video` command) and exits 77 when the core
 * cannot start, like video_test.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apple2_host.h"
#include "apple2debug.h"
#include "apple2session.h"
#include "apple2vidtab.h"

static int g_fail;
#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            g_fail++;                                            \
        }                                                        \
    } while (0)
static int g_checks;
#define OK(cond, ...) do { g_checks++; CHECK(cond, __VA_ARGS__); } while (0)

static apple2vid_capture g_cap;
static uint32_t g_frame[640 * 400];

#define VF_80COL APPLE2HOST_VF_80COL
#define VF_DHIRES APPLE2HOST_VF_DHIRES
#define VF_HIRES APPLE2HOST_VF_HIRES
#define VF_80STORE APPLE2HOST_VF_80STORE
#define VF_MIXED APPLE2HOST_VF_MIXED
#define VF_PAGE2 APPLE2HOST_VF_PAGE2
#define VF_TEXT APPLE2HOST_VF_TEXT

static void cap_reset(uint32_t vmode)
{
    memset(&g_cap, 0, sizeof g_cap);
    g_cap.vmode = vmode;
    g_cap.has_aux = 1;
    g_cap.running = 1;
    g_cap.cycles_per_frame = 17030;
    g_cap.lines_per_frame = 262;
    memset(g_cap.main + 0x0400, 0xA0, 0x800);   /* normal spaces */
    memset(g_cap.aux + 0x0400, 0xA0, 0x800);
}

static apple2vid_tab *mk(int view, int zoom, unsigned ov)
{
    apple2vid_tab *t = apple2vid_tab_new(NULL);
    apple2vid_tab_set_view(t, view);
    apple2vid_tab_set_zoom(t, zoom);
    apple2vid_tab_set_overlays(t, ov);
    return t;
}

static const uint32_t *shoot(apple2vid_tab *t, int *w, int *h)
{
    apple2vid_tab_feed(t, &g_cap);
    apple2vid_tab_refresh(t, 1);
    return apple2vid_tab_picture(t, w, h);
}

static uint32_t at(const uint32_t *p, int w, int x, int y) { return p[(size_t)y * w + x] & 0xFFFFFFu; }

static void test_tables(void)
{
    int n, i;
    const apple2vid_overlay_info *o = apple2vid_overlays(&n);
    OK(n == 5 && o[0].bit == APPLE2VID_OV_GRID, "overlay table");
    OK(apple2vid_parse_overlays("all") == APPLE2VID_OV_ALL, "parse all");
    OK(apple2vid_parse_overlays("none") == 0, "parse none");
    OK(apple2vid_parse_overlays("grid, beam") == (APPLE2VID_OV_GRID | APPLE2VID_OV_BEAM), "parse list");
    OK(apple2vid_parse_overlays("bogus,heat") == APPLE2VID_OV_HEAT, "parse unknown");
    for (i = 0; i < APPLE2VID_V_COUNT; i++) OK(*apple2vid_view_name(i) && *apple2vid_view_key(i), "view %d", i);
    for (i = 0; i < APPLE2VID_M_COUNT; i++) OK(*apple2vid_mode_name(i) && *apple2vid_mode_key(i), "mode %d", i);
    for (i = 0; i < APPLE2VID_S_COUNT; i++) OK(*apple2vid_section_name(i) && *apple2vid_section_key(i), "sec %d", i);
    OK(!*apple2vid_view_name(9) && !*apple2vid_mode_key(-1), "out of range names");
    OK(apple2vid_zooms(&n)[0] == 0 && n == 5, "zooms");
}

static void test_env(void)
{
    apple2vid_tab *t;
    setenv("APPLE2_DEBUGGER_VIDEO_VIEW", "dec", 1);
    setenv("APPLE2_DEBUGGER_VIDEO_MODE", "dhires", 1);
    setenv("APPLE2_DEBUGGER_VIDEO_PAGE", "2", 1);
    setenv("APPLE2_DEBUGGER_VIDEO_OVERLAYS", "grid,heat", 1);
    setenv("APPLE2_DEBUGGER_VIDEO_ZOOM", "3", 1);
    setenv("APPLE2_DEBUGGER_VIDEO_SECTION", "colours", 1);
    t = apple2vid_tab_new(NULL);
    OK(apple2vid_tab_view(t) == APPLE2VID_V_DECODED, "env view");
    OK(!apple2vid_tab_follow(t), "env mode turns follow off");
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_DHIRES && apple2vid_tab_page(t) == 2, "env mode/page");
    OK(apple2vid_tab_overlays(t) == (APPLE2VID_OV_GRID | APPLE2VID_OV_HEAT), "env overlays");
    OK(apple2vid_tab_zoom(t) == 3, "env zoom");
    OK(apple2vid_tab_env_section(t) == APPLE2VID_S_COLOURS, "env section");
    apple2vid_tab_free(t);
    setenv("APPLE2_DEBUGGER_VIDEO_ZOOM", "fit", 1);
    setenv("APPLE2_DEBUGGER_VIDEO_OVERLAYS", "all", 1);
    setenv("APPLE2_DEBUGGER_VIDEO_VIEW", "screen", 1);
    t = apple2vid_tab_new(NULL);
    OK(apple2vid_tab_zoom(t) == 0 && apple2vid_tab_overlays(t) == APPLE2VID_OV_ALL &&
       apple2vid_tab_view(t) == APPLE2VID_V_SCREEN, "env fit/all/screen");
    apple2vid_tab_free(t);
    setenv("APPLE2_DEBUGGER_VIDEO_OVERLAYS", "none", 1);
    setenv("APPLE2_DEBUGGER_VIDEO_ZOOM", "9", 1);
    setenv("APPLE2_DEBUGGER_VIDEO_MODE", "junk", 1);
    unsetenv("APPLE2_DEBUGGER_VIDEO_PAGE");
    t = apple2vid_tab_new(NULL);
    OK(apple2vid_tab_overlays(t) == 0 && apple2vid_tab_zoom(t) == 0 && apple2vid_tab_follow(t), "env bad values");
    apple2vid_tab_free(t);
    unsetenv("APPLE2_DEBUGGER_VIDEO_VIEW");
    unsetenv("APPLE2_DEBUGGER_VIDEO_MODE");
    unsetenv("APPLE2_DEBUGGER_VIDEO_PAGE");
    unsetenv("APPLE2_DEBUGGER_VIDEO_OVERLAYS");
    unsetenv("APPLE2_DEBUGGER_VIDEO_ZOOM");
    unsetenv("APPLE2_DEBUGGER_VIDEO_SECTION");
}

static void test_text40(void)
{
    int w, h;
    const uint32_t *p;
    apple2vid_tab *t = mk(APPLE2VID_V_DECODED, 1, 0);

    cap_reset(VF_TEXT);
    g_cap.main[0x0400] = 0x20;      /* row 0: inverse space = solid cell */
    g_cap.main[0x0480] = 0x20;      /* row 1 */
    g_cap.main[0x0428] = 0x20;      /* row 8: the band boundary */
    g_cap.main[0x0450] = 0x20;      /* row 16 */
    g_cap.main[0x0401] = 0xC1;      /* 'A' normal */
    p = shoot(t, &w, &h);
    OK(p && w == 560 && h == 192, "decoded size %dx%d", w, h);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_TEXT40 && apple2vid_tab_page(t) == 1, "text40 mode");
    OK(at(p, w, 0, 0) == 0xFFFFFF && at(p, w, 13, 7) == 0xFFFFFF, "cell 0,0 solid");
    OK(at(p, w, 5, 8 + 3) == 0xFFFFFF, "row 1 ($0480)");
    OK(at(p, w, 5, 64 + 3) == 0xFFFFFF, "row 8 ($0428)");
    OK(at(p, w, 5, 128 + 3) == 0xFFFFFF, "row 16 ($0450)");
    OK(at(p, w, 5, 16 + 3) == 0x000000, "row 2 blank");
    OK(at(p, w, 14 + 13, 7) == 0x000000, "cell below 'A' glyph blank line");
    /* 'A': row 0 of the 5x7 glyph is 0x0E (bits 1-3), drawn in cell cols 2-4, doubled */
    OK(at(p, w, 14 + 2 * 2, 0) == 0x000000 && at(p, w, 14 + 2 * 2 + 2, 0) == 0xFFFFFF &&
       at(p, w, 14 + 2 * 2 + 3, 0) == 0xFFFFFF, "glyph A top row, doubled horizontally");
    apple2vid_tab_free(t);

    /* page 2 */
    t = mk(APPLE2VID_V_DECODED, 1, 0);
    cap_reset(VF_TEXT | VF_PAGE2);
    g_cap.main[0x0800] = 0x20;
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_page(t) == 2 && at(p, w, 3, 3) == 0xFFFFFF, "text page 2");
    /* 80STORE pins the display to page 1 */
    cap_reset(VF_TEXT | VF_PAGE2 | VF_80STORE);
    g_cap.main[0x0400] = 0x20;
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_page(t) == 1 && at(p, w, 3, 3) == 0xFFFFFF, "80STORE keeps page 1");
    apple2vid_tab_free(t);

    /* flash, inverse and lowercase (ALTCHARSET) */
    t = mk(APPLE2VID_V_DECODED, 1, 0);
    cap_reset(VF_TEXT);
    g_cap.main[0x0400] = 0x41;      /* flashing A */
    g_cap.main[0x0401] = 0xE1;      /* lowercase code: folds to A without ALTCHARSET */
    p = shoot(t, &w, &h);
    OK(at(p, w, 2 * 2 + 2, 0) == 0xFFFFFF, "flash A drawn steady, normal polarity");
    OK(at(p, w, 14 + 2 * 2 + 2, 0) == 0xFFFFFF, "primary set folds lowercase");
    g_cap.altcharset = 1;
    g_cap.main[0x0400] = 0xE1;      /* alt set 'a': row 0 blank, row 2 has ".###." */
    p = shoot(t, &w, &h);
    OK(at(p, w, 2 * 2 + 2, 0) == 0x000000, "alt 'a' has no top row");
    OK(at(p, w, 1 * 2 + 2 * 2 + 1, 2) == 0xFFFFFF, "alt 'a' row 2");
    apple2vid_tab_free(t);
}

static void test_text80(void)
{
    int w, h;
    const uint32_t *p;
    apple2vid_tab *t = mk(APPLE2VID_V_DECODED, 1, 0);
    cap_reset(VF_TEXT | VF_80COL);
    g_cap.aux[0x0400] = 0x20;       /* column 0 */
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_TEXT80, "text80 mode");
    OK(at(p, w, 0, 0) == 0xFFFFFF && at(p, w, 6, 7) == 0xFFFFFF, "aux byte is column 0");
    OK(at(p, w, 7, 0) == 0x000000 && at(p, w, 13, 7) == 0x000000, "main byte is column 1");
    g_cap.main[0x0400] = 0x20;
    g_cap.main[0x0401] = 0x20;      /* column 3 */
    p = shoot(t, &w, &h);
    OK(at(p, w, 7, 0) == 0xFFFFFF && at(p, w, 3 * 7 + 1, 4) == 0xFFFFFF, "main byte is column 1, 0x0401 col 3");
    OK(at(p, w, 2 * 7 + 1, 4) == 0x000000, "aux $0401 is column 2, still blank");

    /* hover: column 1 is the main byte of the pair at $0400 */
    apple2vid_tab_hover(t, 8, 3);
    {
        const apple2vid_pixel *px = apple2vid_tab_pixel(t);
        OK(px && px->valid && px->col == 1 && px->main_addr == 0x400 && px->aux_addr == 0x400, "text80 hover addrs");
        OK(px && px->rx == 7 && px->rw == 7 && px->rh == 8, "text80 cell rect");
        OK(px && px->mode == APPLE2VID_M_TEXT80 && px->aux_val == 0x20 && px->main_val == 0x20, "text80 hover values");
    }
    apple2vid_tab_free(t);
}

static void test_lores(void)
{
    int w, h, n;
    const uint32_t *p;
    const apple2vid_pal_entry *pal;
    apple2vid_tab *t = mk(APPLE2VID_V_DECODED, 1, 0);

    cap_reset(0);                   /* graphics, lo-res */
    memset(g_cap.main + 0x0400, 0, 0x400);
    g_cap.main[0x0400] = 0x2F;      /* top white, bottom dark blue */
    g_cap.main[0x0480] = 0x01;      /* row 1 = y 8..15, deep red on top */
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_LORES, "lores mode");
    OK(at(p, w, 0, 0) == 0xFFFFFF && at(p, w, 13, 3) == 0xFFFFFF, "top block white");
    OK(at(p, w, 0, 4) == 0x000099 && at(p, w, 13, 7) == 0x000099, "bottom block dark blue");
    OK(at(p, w, 14, 0) == 0x000000, "next column black");
    OK(at(p, w, 0, 8) == 0xDD0033 && at(p, w, 0, 12) == 0x000000, "row 1 deep red");
    n = apple2vid_tab_palette(t, &pal);
    OK(n == 16, "lores palette size %d", n);
    OK(pal[15].uses == 56 && pal[2].uses == 56 && pal[1].uses == 56, "colour counts %ld %ld %ld",
       pal[15].uses, pal[2].uses, pal[1].uses);
    {
        long total = 0;
        for (int i = 0; i < n; i++) total += pal[i].uses;
        OK(total == 560 * 192, "counts cover the picture (%ld)", total);
    }
    apple2vid_tab_hover(t, 3, 1);
    apple2vid_tab_refresh(t, 1);
    OK(apple2vid_tab_pixel_palette_index(t) == 15, "inspected colour is white");
    apple2vid_tab_palette(t, &pal);
    OK(pal[15].inspected && !pal[2].inspected, "inspected mark");
    OK(strstr(apple2vid_tab_pixel(t)->text, "white"), "inspector text names the colour: %s", apple2vid_tab_pixel(t)->text);

    /* double lo-res: aux nibble is rotated left once */
    cap_reset(VF_80COL | VF_DHIRES);
    memset(g_cap.main + 0x0400, 0, 0x400);
    memset(g_cap.aux + 0x0400, 0, 0x400);
    g_cap.aux[0x0400] = 0x01;       /* rotated -> 2 = dark blue */
    g_cap.main[0x0400] = 0x01;      /* deep red */
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_DLORES, "dlores mode");
    OK(at(p, w, 0, 0) == 0x000099 && at(p, w, 6, 3) == 0x000099, "aux column, rotated");
    OK(at(p, w, 7, 0) == 0xDD0033 && at(p, w, 13, 3) == 0xDD0033, "main column");
    OK(at(p, w, 14, 0) == 0x000000, "next pair black");
    apple2vid_tab_free(t);
}

static void test_hires(void)
{
    int w, h, n;
    const uint32_t *p;
    const apple2vid_pal_entry *pal;
    apple2vid_tab *t = mk(APPLE2VID_V_DECODED, 1, 0);

    cap_reset(VF_HIRES);
    memset(g_cap.main + 0x2000, 0, 0x4000);
    g_cap.main[0x2000] = 0x01;      /* y 0: lone pixel, even column, no shift: violet */
    g_cap.main[0x2400] = 0x81;      /* y 1: shifted: blue */
    g_cap.main[0x2080] = 0x03;      /* y 8: two pixels: white */
    g_cap.main[0x2028] = 0x02;      /* y 64: odd column: green */
    g_cap.main[0x4000] = 0x01;      /* page 2 only */
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_HIRES, "hires mode");
    OK(at(p, w, 0, 0) == 0xFF44FD && at(p, w, 1, 0) == 0xFF44FD, "violet, doubled");
    OK(at(p, w, 2, 0) == 0x000000, "next pixel black");
    OK(at(p, w, 0, 1) == 0x14CFFD, "blue (y 1 = $2400)");
    OK(at(p, w, 0, 8) == 0xFFFFFF && at(p, w, 3, 8) == 0xFFFFFF && at(p, w, 4, 8) == 0x000000, "white pair (y 8 = $2080)");
    OK(at(p, w, 2, 64) == 0x14F53C, "green on an odd column (y 64 = $2028)");
    n = apple2vid_tab_palette(t, &pal);
    OK(n == 6, "hires palette size %d", n);
    OK(pal[2].uses == 2 && pal[3].uses == 2 && pal[4].uses == 0, "hires usage green %ld violet %ld orange %ld", pal[2].uses, pal[3].uses, pal[4].uses);

    apple2vid_tab_hover(t, 30, 8);
    {
        const apple2vid_pixel *px = apple2vid_tab_pixel(t);
        OK(px && px->main_addr == 0x2080 + 2 && px->aux_addr == -1 && px->col == 2 && px->row == 8, "hires hover addr");
        OK(px && px->rw == 14 && px->rh == 1 && px->rx == 28, "hires byte cell");
    }
    cap_reset(VF_HIRES | VF_PAGE2);
    memset(g_cap.main + 0x2000, 0, 0x4000);
    g_cap.main[0x4000] = 0x01;
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_page(t) == 2 && at(p, w, 0, 0) == 0xFF44FD, "hires page 2 at $4000");

    /* double hi-res: dots are aux bits 0-6 then main bits 0-6; 4 dots make a colour */
    cap_reset(VF_HIRES | VF_80COL | VF_DHIRES);
    memset(g_cap.main + 0x2000, 0, 0x4000);
    memset(g_cap.aux + 0x2000, 0, 0x4000);
    g_cap.aux[0x2000] = 0x01;       /* group value 1 -> dark blue */
    g_cap.aux[0x2400] = 0x0F;       /* y 1: group value 15 -> white, then dots 4-6 black */
    g_cap.main[0x2000] = 0x7F;      /* dots 7-13 lit */
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_DHIRES, "dhires mode");
    OK(at(p, w, 0, 0) == 0x000099 && at(p, w, 3, 0) == 0x000099 && at(p, w, 4, 0) == 0xDD0033, "dhires dark blue group, then dots 4-7 = 0001 = deep red");
    OK(at(p, w, 0, 1) == 0xFFFFFF && at(p, w, 3, 1) == 0xFFFFFF && at(p, w, 4, 1) == 0x000000, "dhires white group");
    /* dots 8..11 are all lit (group 2): white */
    OK(at(p, w, 8, 0) == 0xFFFFFF && at(p, w, 11, 0) == 0xFFFFFF && at(p, w, 7, 0) == 0xDD0033, "dhires main dots");
    apple2vid_tab_hover(t, 9, 0);
    {
        const apple2vid_pixel *px = apple2vid_tab_pixel(t);
        OK(px && px->main_addr == 0x2000 && px->aux_addr == 0x2000 && px->mode == APPLE2VID_M_DHIRES, "dhires hover addrs");
        OK(px && px->main_val == 0x7F && px->aux_val == 0x01 && px->rw == 7, "dhires hover values");
    }
    apple2vid_tab_free(t);
}

static void test_mixed_follow(void)
{
    int w, h;
    const uint32_t *p;
    apple2vid_tab *t = mk(APPLE2VID_V_DECODED, 1, APPLE2VID_OV_MIXED);

    cap_reset(VF_HIRES | VF_MIXED);
    memset(g_cap.main + 0x2000, 0x7F, 0x2000);      /* all lit: white */
    /* row 20 starts at $0650: a normal space over white hi-res */
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_mixed(t) && apple2vid_tab_mode(t) == APPLE2VID_M_HIRES, "mixed hires");
    OK(at(p, w, 100, 100) == 0xFFFFFF, "graphics above the split");
    OK(at(p, w, 100, 159) == 0xFFFFFF, "line 159 is graphics (overlay line is 160)");
    OK(at(p, w, 5, 161) == 0x000000 && at(p, w, 5, 191) == 0x000000, "text lines replace the graphics");
    OK(at(p, w, 100, 160) == 0xFF4040 || (p[160 * w + 100] & 0xFF0000) > 0x800000, "split line overlay is drawn");
    g_cap.main[0x0650] = 0x20;                      /* row 20, col 0, inverse */
    g_cap.main[0x0651] = 0xC1;
    p = shoot(t, &w, &h);
    OK(at(p, w, 3, 165) == 0xFFFFFF, "text over hi-res at row 20");
    apple2vid_tab_hover(t, 3, 165);
    {
        const apple2vid_pixel *px = apple2vid_tab_pixel(t);
        OK(px && px->text_cell && px->mode == APPLE2VID_M_TEXT40 && px->main_addr == 0x650 && px->row == 20, "hover in the text lines");
    }
    apple2vid_tab_hover(t, 3, 100);
    OK(!apple2vid_tab_pixel(t)->text_cell && apple2vid_tab_pixel(t)->mode == APPLE2VID_M_HIRES, "hover in the graphics");

    /* follow vs fixed */
    cap_reset(VF_TEXT);
    g_cap.main[0x0400] = 0x20;
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_follow(t) && apple2vid_tab_mode(t) == APPLE2VID_M_TEXT40 && !apple2vid_tab_mixed(t), "follows the screen");
    OK(at(p, w, 3, 3) == 0xFFFFFF, "text drawn");
    apple2vid_tab_set_mode(t, APPLE2VID_M_LORES);
    OK(!apple2vid_tab_follow(t), "set_mode turns Follow off");
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_LORES, "chosen mode");
    /* $0400 = $20: lo-res low nibble 0 top, high nibble 2 bottom */
    OK(at(p, w, 3, 1) == 0x000000 && at(p, w, 3, 5) == 0x000099, "same bytes read as lo-res");
    apple2vid_tab_set_page(t, 2);
    OK(apple2vid_tab_page(t) == 2, "chosen page");
    g_cap.vmode = VF_HIRES;                          /* the machine moves on; chosen mode stays */
    shoot(t, &w, &h);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_LORES && apple2vid_tab_page(t) == 2, "fixed mode ignores the switches");
    apple2vid_tab_set_follow(t, 1);
    shoot(t, &w, &h);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_HIRES && apple2vid_tab_page(t) == 1, "follow again");
    /* unfollow keeps what is shown */
    apple2vid_tab_set_follow(t, 0);
    OK(apple2vid_tab_mode(t) == APPLE2VID_M_HIRES, "unfollow keeps the current mode");
    apple2vid_tab_set_mixed(t, 1);
    p = shoot(t, &w, &h);
    OK(apple2vid_tab_mixed(t), "chosen mixed");
    apple2vid_tab_free(t);
}

static void test_hover_pin_marks(void)
{
    int w, h, hm[4], pm[4], r;
    const uint32_t *p;
    apple2vid_tab *t = mk(APPLE2VID_V_DECODED, 2, 0);

    cap_reset(VF_TEXT);
    g_cap.main[0x0400] = 0x20;
    p = shoot(t, &w, &h);
    OK(p && w == 1120 && h == 384 && apple2vid_tab_effective_zoom(t) == 2, "zoom 2 size %dx%d", w, h);
    OK(apple2vid_tab_hover(t, 6, 6) == 1, "hover reports a change");
    OK(apple2vid_tab_hover(t, 7, 7) == 0, "same cell, no change");
    r = apple2vid_tab_marks(t, hm, pm);
    OK(r == 1 && hm[0] == 0 && hm[1] == 0 && hm[2] == 28 && hm[3] == 16, "hover mark %d,%d %dx%d", hm[0], hm[1], hm[2], hm[3]);
    OK(strstr(apple2vid_tab_pixel(t)->text, "$0400"), "text has the address: %s", apple2vid_tab_pixel(t)->text);
    apple2vid_tab_pin(t, 30, 20);          /* col 1, row 1 */
    r = apple2vid_tab_marks(t, hm, pm);
    OK(r == 3 && pm[0] == 28 && pm[1] == 16, "pin mark");
    apple2vid_tab_hover(t, -1, -1);
    OK(apple2vid_tab_pixel(t) && apple2vid_tab_pixel(t)->main_addr == 0x0480 + 1, "pin outlives hover: $%04X", apple2vid_tab_pixel(t)->main_addr);
    apple2vid_tab_pin(t, 30, 20);
    OK(apple2vid_tab_pixel(t) == NULL, "the same spot unpins");
    apple2vid_tab_pin(t, 6, 6);
    apple2vid_tab_unpin(t);
    OK(apple2vid_tab_pixel(t) == NULL, "unpin");
    OK(apple2vid_tab_break_on_pixel(t) == -1, "no engine, no breakpoint");
    /* a pin follows the memory */
    apple2vid_tab_pin(t, 6, 6);
    g_cap.main[0x0400] = 0xC1;
    shoot(t, &w, &h);
    OK(apple2vid_tab_pixel(t)->main_val == 0xC1, "pin re-reads");
    /* zoom change keeps the pin on the same cell */
    apple2vid_tab_set_zoom(t, 4);
    shoot(t, &w, &h);
    apple2vid_tab_marks(t, hm, pm);
    OK(w == 2240 && (apple2vid_tab_marks(t, hm, pm) & 2) && pm[2] == 56, "zoom 4 mark");
    apple2vid_tab_free(t);

    /* Screen view maps onto the cells */
    t = mk(APPLE2VID_V_SCREEN, 1, 0);
    cap_reset(VF_TEXT);
    for (int i = 0; i < 560 * 384; i++) g_frame[i] = 0x00123456;
    g_cap.frame = g_frame; g_cap.fw = 560; g_cap.fh = 384;
    p = shoot(t, &w, &h);
    OK(p && w == 560 && h == 384 && at(p, w, 10, 10) == 0x123456 && (p[0] >> 24) == 0xFF, "screen frame as shown, opaque");
    apple2vid_tab_hover(t, 29, 17);        /* dy = 17*192/384 = 8: row 1 */
    OK(apple2vid_tab_pixel(t) && apple2vid_tab_pixel(t)->row == 1 && apple2vid_tab_pixel(t)->col == 2, "screen hover maps (row %d col %d)",
       apple2vid_tab_pixel(t)->row, apple2vid_tab_pixel(t)->col);
    apple2vid_tab_marks(t, hm, pm);
    OK(hm[0] == 28 && hm[1] == 16 && hm[2] == 14 && hm[3] == 16, "mark scaled to the frame: %d,%d %dx%d", hm[0], hm[1], hm[2], hm[3]);
    for (int i = 0; i < 640 * 400; i++) g_frame[i] = 0x00654321;
    g_cap.fw = 640; g_cap.fh = 400;
    p = shoot(t, &w, &h);
    OK(w == 640 && h == 400 && at(p, w, 639, 399) == 0x654321, "VidHD-sized frame");
    apple2vid_tab_free(t);

    /* screen view with no frame yet */
    t = mk(APPLE2VID_V_SCREEN, 1, 0);
    cap_reset(VF_TEXT);
    g_cap.frame = NULL;
    p = shoot(t, &w, &h);
    OK(!p && *apple2vid_tab_why(t), "no frame: %s", apple2vid_tab_why(t));
    apple2vid_tab_free(t);
}

static void test_overlays(void)
{
    int w, h;
    const uint32_t *p;
    apple2vid_tab *t = mk(APPLE2VID_V_DECODED, 1, APPLE2VID_OV_BEAM);

    cap_reset(VF_TEXT);
    g_cap.beam_line = 50; g_cap.beam_horz = 30;
    p = shoot(t, &w, &h);
    OK((at(p, w, 300, 50) >> 16) > 0xC0 && ((at(p, w, 300, 50) >> 8) & 0xFF) > 0xA0 && (at(p, w, 300, 50) & 0xFF) < 0x20, "beam line %06X", at(p, w, 300, 50));
    OK(at(p, w, 300, 51) == 0x000000, "only that line");
    g_cap.beam_line = 220; g_cap.beam_horz = 0;
    p = shoot(t, &w, &h);
    OK(at(p, w, 300, 50) == 0x000000, "beam moved; none in blanking");
    OK(apple2vid_tab_beam_permille(t) == 220 * 1000 / 262, "permille %d", apple2vid_tab_beam_permille(t));

    apple2vid_tab_set_overlays(t, APPLE2VID_OV_GRID);
    g_cap.beam_line = 0;
    p = shoot(t, &w, &h);
    OK(at(p, w, 14, 3) != 0x000000 && at(p, w, 7, 8) != 0x000000 && at(p, w, 7, 3) == 0x000000, "grid lines at cell edges");

    /* heat and changed */
    apple2vid_tab_set_overlays(t, APPLE2VID_OV_HEAT);
    cap_reset(VF_TEXT);
    shoot(t, &w, &h);
    g_cap.main[0x0400] = 0xC1;
    g_cap.main[0x0401] = 0x00;
    p = shoot(t, &w, &h);
    {
        const uint32_t c = at(p, w, 0, 0);
        OK(((c >> 16) & 0xFF) > ((c >> 8) & 0xFF) + 20, "heat tints the written cell red: %06X", c);
    }
    {
        int n = 0;
        unsigned r;
        while (n++ < 40) {
            apple2vid_tab_feed(t, &g_cap);
            r = apple2vid_tab_refresh(t, 0);
            (void)r;
        }
        p = apple2vid_tab_picture(t, &w, &h);
        OK(at(p, w, 0, 0) != 0x000000, "the written glyph cell is still drawn after the heat fades");
        OK(at(p, w, 28, 7) == 0x000000, "unwritten cell never tinted");
    }
    apple2vid_tab_set_overlays(t, APPLE2VID_OV_CHANGED);
    cap_reset(VF_TEXT);
    shoot(t, &w, &h);
    g_cap.main[0x0428] = 0x20;      /* row 8 */
    p = shoot(t, &w, &h);
    OK((at(p, w, 3, 64 + 3) & 0xFF) > 0xA0 && at(p, w, 3, 3) == 0x000000, "changed cell highlighted: %06X", at(p, w, 3, 64 + 3));
    apple2vid_tab_free(t);
}

static void test_tables_and_status(void)
{
    int w, h, n;
    const apple2vid_switch *sw;
    const char *const *lines;
    char buf[2048];
    unsigned r;
    apple2vid_tab *t = mk(APPLE2VID_V_DECODED, 1, 0);

    cap_reset(VF_TEXT);
    g_cap.cycles = 123456; g_cap.frame_count = 77; g_cap.beam_line = 131; g_cap.beam_horz = 0;
    shoot(t, &w, &h);
    n = apple2vid_tab_switches(t, &sw);
    OK(n == APPLE2VID_SW_COUNT && !strcmp(sw[0].name, "TEXT") && sw[0].on && !sw[1].on, "switch table");
    OK(sw[0].set_addr == 0xC051 && sw[0].clr_addr == 0xC050 && sw[2].set_addr == 0xC055 && sw[4].clr_addr == 0xC00C &&
       sw[7].set_addr == 0xC05E, "switch addresses");
    OK(sw[1].age >= APPLE2VID_SW_FADE, "unchanged switch is old (%d)", sw[1].age);
    g_cap.vmode = VF_TEXT | VF_MIXED;
    apple2vid_tab_feed(t, &g_cap);
    r = apple2vid_tab_refresh(t, 0);
    apple2vid_tab_switches(t, &sw);
    OK((r & APPLE2VID_TAB_TABLES) && sw[1].on && sw[1].age == 0, "MIXED just changed (age %d)", sw[1].age);
    apple2vid_tab_feed(t, &g_cap);
    apple2vid_tab_refresh(t, 0);
    apple2vid_tab_feed(t, &g_cap);
    apple2vid_tab_refresh(t, 0);
    apple2vid_tab_switches(t, &sw);
    OK(sw[1].age == 2 && sw[0].age >= APPLE2VID_SW_FADE, "ages count refreshes (%d)", sw[1].age);
    g_cap.altcharset = 1;
    apple2vid_tab_feed(t, &g_cap);
    apple2vid_tab_refresh(t, 0);
    apple2vid_tab_switches(t, &sw);
    OK(sw[6].on && sw[6].age == 0 && !strcmp(sw[6].name, "ALTCHARSET"), "ALTCHARSET tracked");

    n = apple2vid_tab_timing(t, &lines);
    OK(n >= 5 && strstr(lines[0], "Mode\t"), "timing lines");
    OK(apple2vid_tab_beam_permille(t) == 500, "beam permille %d", apple2vid_tab_beam_permille(t));
    OK(strchr(apple2vid_tab_status(t), '\n') != NULL && strstr(apple2vid_tab_status(t), "Decoded"), "status: %s", apple2vid_tab_status(t));
    for (int s = 0; s < APPLE2VID_S_COUNT; s++) {
        OK(apple2vid_tab_text(t, s, buf, sizeof buf) > 0, "text section %d", s);
    }
    apple2vid_tab_text(t, APPLE2VID_S_SWITCHES, buf, sizeof buf);
    OK(strstr(buf, "DHIRES") && strstr(buf, "$C05E") && strstr(buf, "$C00D"), "switches text:\n%s", buf);
    apple2vid_tab_text(t, APPLE2VID_S_TIMING, buf, sizeof buf);
    OK(strstr(buf, "123456") && strstr(buf, "17030"), "timing text:\n%s", buf);
    OK(apple2vid_tab_text(t, APPLE2VID_S_COLOURS, buf, 16) < 16, "small buffer is respected");

    /* not running */
    g_cap.running = 0;
    apple2vid_tab_feed(t, &g_cap);
    r = apple2vid_tab_refresh(t, 0);
    OK((r & APPLE2VID_TAB_OFF) && !apple2vid_tab_picture(t, &w, &h) && *apple2vid_tab_why(t), "off: %s", apple2vid_tab_why(t));
    apple2vid_tab_text(t, APPLE2VID_S_SWITCHES, buf, sizeof buf);
    OK(strstr(buf, "not running"), "off text");
    apple2vid_tab_free(t);

    /* a tab with nothing at all */
    t = apple2vid_tab_new(NULL);
    r = apple2vid_tab_refresh(t, 1);
    OK((r & APPLE2VID_TAB_OFF) && !apple2vid_tab_picture(t, &w, &h), "empty tab is off");
    apple2vid_tab_free(t);
    apple2vid_tab_free(NULL);
}

static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

static void test_fuzz(void)
{
    apple2vid_tab *t = mk(APPLE2VID_V_DECODED, 0, APPLE2VID_OV_ALL);
    int w, h, hm[4], pm[4];
    for (int i = 0; i < 3000; i++) {
        switch (rnd() % 12) {
        case 0: cap_reset(rnd() & 0x7F); break;
        case 1: apple2vid_tab_set_zoom(t, (int)(rnd() % 7) - 1); break;
        case 2: apple2vid_tab_set_viewport(t, (int)(rnd() % 3000) - 10, (int)(rnd() % 2000) - 10); break;
        case 3: apple2vid_tab_set_view(t, (int)(rnd() % 4) - 1); break;
        case 4: apple2vid_tab_set_mode(t, (int)(rnd() % 8) - 1); break;
        case 5: apple2vid_tab_set_page(t, (int)(rnd() % 4)); break;
        case 6: apple2vid_tab_set_overlays(t, rnd()); break;
        case 7: apple2vid_tab_pin(t, (int)(rnd() % 3000) - 100, (int)(rnd() % 1700) - 100); break;
        case 8: apple2vid_tab_set_follow(t, (int)(rnd() & 1)); break;
        default: apple2vid_tab_hover(t, (int)(rnd() % 3000) - 100, (int)(rnd() % 1700) - 100); break;
        }
        if (rnd() % 4 == 0) {
            for (int k = 0; k < 64; k++) {
                g_cap.main[0x0400 + rnd() % 0x800] = (uint8_t)rnd();
                g_cap.aux[0x2000 + rnd() % 0x4000] = (uint8_t)rnd();
                g_cap.main[0x2000 + rnd() % 0x4000] = (uint8_t)rnd();
            }
            g_cap.beam_line = rnd() % 400; g_cap.beam_horz = rnd() % 100;
            g_cap.running = (rnd() % 16) != 0;
            g_cap.frame = (rnd() & 1) ? g_frame : NULL;
            g_cap.fw = (rnd() & 1) ? 560 : 640; g_cap.fh = (rnd() & 1) ? 384 : 400;
            apple2vid_tab_feed(t, &g_cap);
            apple2vid_tab_refresh(t, rnd() & 1);
            (void)apple2vid_tab_picture(t, &w, &h);
            apple2vid_tab_marks(t, hm, pm);
            apple2vid_tab_break_on_pixel(t);
        }
    }
    OK(1, "fuzz survived");
    apple2vid_tab_free(t);
}

/* ---- against the machine ------------------------------------------------------------------ */
static int wait_paused(apple2debug *d, int ms)
{
    while (ms > 0) {
        if (apple2debug_is_stopped(d)) return 1;
        usleep(5000);
        ms -= 5;
    }
    return 0;
}

static int count_bps(apple2debug *d)
{
    apple2debug_breakpoint b[64];
    return apple2debug_breakpoint_list(d, b, 64);
}

static int machine_part(void)
{
    apple2session *s = apple2session_new(NULL);
    apple2session_start_opts o;
    apple2debug *d;
    apple2vid_tab *t;
    int w, h, before, id;
    char buf[4096];
    const apple2vid_switch *sw;

    if (!s) { fprintf(stderr, "vidtab: could not create the session\n"); return 77; }
    apple2session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    o.slot7 = "Empty";
    if (apple2session_start(s, &o) != 0) {
        fprintf(stderr, "vidtab: %s\n", apple2session_last_error(s));
        apple2session_free(s);
        return 77;
    }
    d = apple2session_debugger(s);
    usleep(400000);
    apple2debug_attach(d);
    OK(wait_paused(d, 2000), "did not pause");

    t = apple2vid_tab_new(s);
    apple2vid_tab_set_zoom(t, 1);
    apple2vid_tab_set_overlays(t, 0);
    OK(!(apple2vid_tab_refresh(t, 1) & APPLE2VID_TAB_OFF), "tab is on");
    apple2vid_tab_set_view(t, APPLE2VID_V_SCREEN);
    apple2vid_tab_refresh(t, 1);
    OK(apple2vid_tab_picture(t, &w, &h) && w >= 280 && h >= 192, "screen frame %dx%d", w, h);
    apple2vid_tab_set_view(t, APPLE2VID_V_DECODED);
    for (int i = 0; i < 0x400; i++) apple2debug_write(d, (uint16_t)(0x0400 + i), 0xA0);
    apple2debug_write(d, 0x0400, 0x20);
    apple2vid_tab_set_mode(t, APPLE2VID_M_TEXT40);
    apple2vid_tab_set_page(t, 1);
    {
        const uint32_t *p;
        apple2vid_tab_refresh(t, 1);
        p = apple2vid_tab_picture(t, &w, &h);
        OK(p && w == 560 && h == 192 && at(p, w, 3, 3) == 0xFFFFFF && at(p, w, 20, 3) == 0x000000, "live text page");
    }
    apple2vid_tab_hover(t, 3, 3);
    before = count_bps(d);
    id = apple2vid_tab_break_on_pixel(t);
    OK(id >= 0 && count_bps(d) == before + 1, "text40: one write breakpoint (%d -> %d)", before, count_bps(d));
    {
        apple2debug_breakpoint b[64];
        int n = apple2debug_breakpoint_list(d, b, 64), found = 0;
        for (int i = 0; i < n; i++)
            if (b[i].id == id && (b[i].type & APPLE2DEBUG_BP_WRITE) && b[i].start == 0x0400) found = 1;
        OK(found, "the breakpoint is a write watch on $0400");
    }
    apple2vid_tab_set_mode(t, APPLE2VID_M_TEXT80);
    apple2vid_tab_refresh(t, 1);
    apple2vid_tab_hover(t, 3, 3);
    before = count_bps(d);
    id = apple2vid_tab_break_on_pixel(t);
    OK(id >= 0 && count_bps(d) == before + 2, "text80: main and aux write breakpoints (%d -> %d)", before, count_bps(d));

    apple2vid_tab_refresh(t, 1);
    OK(apple2vid_tab_switches(t, &sw) == APPLE2VID_SW_COUNT, "live switches");
    before = count_bps(d);
    id = apple2vid_tab_break_on_switch(t, 2);
    OK(id >= 0 && count_bps(d) == before + 2, "switch: both addresses (%d -> %d)", before, count_bps(d));
    OK(apple2vid_tab_break_on_switch(t, 99) == -1, "bad switch index");

    apple2debug_command(d, "video", buf, sizeof buf);
    OK(strstr(buf, "TEXT") && strstr(buf, "PAGE2") && strstr(buf, "Cycles"), "console video:\n%s", buf);
    apple2debug_command(d, "video timing", buf, sizeof buf);
    OK(strstr(buf, "Beam") && !strstr(buf, "PAGE2"), "console video timing:\n%s", buf);

    apple2debug_breakpoint_clear(d);
    apple2vid_tab_free(t);
    apple2debug_detach(d);
    apple2session_free(s);
    return 0;
}

int main(void)
{
    int rc;
    test_tables();
    test_env();
    test_text40();
    test_text80();
    test_lores();
    test_hires();
    test_mixed_follow();
    test_hover_pin_marks();
    test_overlays();
    test_tables_and_status();
    test_fuzz();
    if (g_fail) {
        fprintf(stderr, "vidtab: %d of %d check(s) failed (synthetic part)\n", g_fail, g_checks);
        return 1;
    }
    printf("vidtab: synthetic part OK (%d checks)\n", g_checks);
    rc = machine_part();
    if (rc == 77) {
        printf("vidtab: machine part skipped (core cannot start)\n");
        return 77;
    }
    if (g_fail) {
        fprintf(stderr, "vidtab: %d of %d check(s) failed\n", g_fail, g_checks);
        return 1;
    }
    printf("vidtab: OK (%d checks)\n", g_checks);
    return 0;
}

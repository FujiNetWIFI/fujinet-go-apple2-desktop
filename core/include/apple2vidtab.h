/*
 * apple2vidtab -- the debugger Video tab's controller. All four frontends draw
 * the same tab from it, so what they own is widgets: a view and zoom chooser,
 * a Follow screen box with mode and page choosers, overlay toggles, a picture,
 * the inspector and three tables. Modelled on astrovidtab.h:
 *
 *   - the machine is only COPIED: the soft switches and the video pages
 *     (main and aux, physical, whatever RAMRD/RAMWRT say) are read once per
 *     refresh, and the session's last finished frame is copied with
 *     apple2session_copy_frame. Decoding, scaling and overlays run on the
 *     caller's (UI) thread. A stopped machine is read live;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from apple2vid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * Two views. SCREEN is the last finished frame as the machine displayed it
 * (its size varies: 560x384, or 640x400 with a VidHD card). DECODED is video
 * memory decoded through the soft switches right now (or through the chosen
 * mode when Follow screen is off), always 560x192 before zoom: 40-column
 * modes are doubled horizontally so a byte cell is 14 pixels wide and the
 * 80-column and double modes (7 pixels per byte, aux first) fit the same
 * grid. The inspector maps the SCREEN view onto the same cells.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2VIDTAB_H
#define APPLE2VIDTAB_H

#include <stdint.h>

#include "apple2debug.h"
#include "apple2session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct apple2vid_tab apple2vid_tab;

/* The decoded picture, before zoom. */
#define APPLE2VID_W 560
#define APPLE2VID_H 192
/* the split line of MIXED mode: lines 160-191 are text */
#define APPLE2VID_MIXED_LINE 160

enum apple2vid_view {
    APPLE2VID_V_SCREEN = 0,     /* the last finished frame, as displayed */
    APPLE2VID_V_DECODED,        /* video memory through the soft switches */
    APPLE2VID_V_COUNT
};
const char *apple2vid_view_name(int view);          /* "Screen", "Decoded" */
const char *apple2vid_view_key(int view);           /* "screen", "decoded" (env) */

enum apple2vid_mode {
    APPLE2VID_M_TEXT40 = 0,
    APPLE2VID_M_TEXT80,         /* aux columns interleaved, aux first */
    APPLE2VID_M_LORES,
    APPLE2VID_M_DLORES,         /* double lo-res: aux nibble rotated one bit */
    APPLE2VID_M_HIRES,
    APPLE2VID_M_DHIRES,
    APPLE2VID_M_COUNT
};
const char *apple2vid_mode_name(int mode);          /* "Text 40", "Double hi-res" ... */
const char *apple2vid_mode_key(int mode);           /* "text40" "text80" "lores" "dlores" "hires" "dhires" */

/* Overlays: one table, bit per entry. */
enum {
    APPLE2VID_OV_GRID    = 1u << 0,  /* cells: 7x8 text, 7x4 lores blocks, byte cells hires */
    APPLE2VID_OV_MIXED   = 1u << 1,  /* the mixed-mode split line (line 160) */
    APPLE2VID_OV_BEAM    = 1u << 2,  /* the current scanline (and horizontal position) */
    APPLE2VID_OV_HEAT    = 1u << 3,  /* bytes of the displayed page written lately (fades) */
    APPLE2VID_OV_CHANGED = 1u << 4,  /* bytes changed since the last refresh */
    APPLE2VID_OV_ALL     = 0x1Fu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: grid, mixed, ... */
    const char *label;          /* the checkbox text */
} apple2vid_overlay_info;
const apple2vid_overlay_info *apple2vid_overlays(int *count);
/* "grid,beam" | "all" | "none" -> bits (unknown names are ignored) */
unsigned apple2vid_parse_overlays(const char *list);

/* Right-hand sections. */
enum { APPLE2VID_S_PIXEL = 0, APPLE2VID_S_SWITCHES, APPLE2VID_S_COLOURS,
       APPLE2VID_S_TIMING, APPLE2VID_S_COUNT };
const char *apple2vid_section_name(int section);    /* "Pixel", "Switches", ... */
const char *apple2vid_section_key(int section);     /* "pixel", "switches", ... */

/*
 * s: the session whose frame and debugger the tab reads (NULL: no machine --
 * only apple2vid_tab_feed gives it something to show).
 * The scripted-capture variables are read once here:
 *   APPLE2_DEBUGGER_VIDEO_VIEW      screen decoded
 *   APPLE2_DEBUGGER_VIDEO_MODE      text40 text80 lores dlores hires dhires  (turns Follow off)
 *   APPLE2_DEBUGGER_VIDEO_PAGE      1 2                                      (turns Follow off)
 *   APPLE2_DEBUGGER_VIDEO_OVERLAYS  grid,mixed,beam,heat,changed | all | none
 *   APPLE2_DEBUGGER_VIDEO_ZOOM      1..4 or fit
 *   APPLE2_DEBUGGER_VIDEO_SECTION   pixel switches colours timing   (names, never indexes)
 */
apple2vid_tab *apple2vid_tab_new(apple2session *s);
/* The same, from a debugger engine alone (the console's `video` command): no
 * Screen view, no env variables. d may be NULL. */
apple2vid_tab *apple2vid_tab_new_for_debugger(apple2debug *d);
void           apple2vid_tab_free(apple2vid_tab *t);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     apple2vid_tab_set_view(apple2vid_tab *t, int view);
int      apple2vid_tab_view(const apple2vid_tab *t);
void     apple2vid_tab_set_overlays(apple2vid_tab *t, unsigned ov);
unsigned apple2vid_tab_overlays(const apple2vid_tab *t);
/* 1, 2, 3, 4, or 0 for "fit the viewport" */
void     apple2vid_tab_set_zoom(apple2vid_tab *t, int zoom);
int      apple2vid_tab_zoom(const apple2vid_tab *t);
/* the room the picture has, for Fit; call when the widget resizes */
void     apple2vid_tab_set_viewport(apple2vid_tab *t, int w, int h);
/* the zoom steps the chooser offers: 0 (Fit), 1, 2, 3, 4 */
const int *apple2vid_zooms(int *count);
/* the starting section from the env, APPLE2VID_S_PIXEL when unset */
int      apple2vid_tab_env_section(const apple2vid_tab *t);

/* Follow screen (default on): Decoded tracks the displayed mode, page and
 * MIXED. Turning it off keeps what is shown at that moment as the starting
 * point; set_mode/set_page/set_mixed then override (and imply Follow off). */
void     apple2vid_tab_set_follow(apple2vid_tab *t, int follow);
int      apple2vid_tab_follow(const apple2vid_tab *t);
void     apple2vid_tab_set_mode(apple2vid_tab *t, int mode);      /* enum apple2vid_mode */
void     apple2vid_tab_set_page(apple2vid_tab *t, int page);      /* 1 or 2 */
void     apple2vid_tab_set_mixed(apple2vid_tab *t, int mixed);
/* what Decoded is drawing now (the machine's, or the chosen one) */
int      apple2vid_tab_mode(const apple2vid_tab *t);
int      apple2vid_tab_page(const apple2vid_tab *t);              /* 1 or 2 */
int      apple2vid_tab_mixed(const apple2vid_tab *t);

/* ---- refresh ------------------------------------------------------------------------ */
#define APPLE2VID_TAB_PICTURE 1u  /* repaint the picture (apple2vid_tab_picture) */
#define APPLE2VID_TAB_TABLES  2u  /* refill the status line and the sections */
#define APPLE2VID_TAB_OFF     4u  /* no machine/picture: show apple2vid_tab_why() instead */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. Call every ~50-100 ms while the tab is visible
 * (it is cheap when nothing changed). Ages and the heat fade count refreshes. */
unsigned apple2vid_tab_refresh(apple2vid_tab *t, int force);

/* For tests: use this state instead of the machine. Only the video pages are
 * read from main/aux ($0400-$0BFF and $2000-$5FFF). frame may be NULL. */
typedef struct {
    uint32_t vmode;             /* APPLE2HOST_VF_* (apple2_host.h): 80COL DHIRES HIRES 80STORE MIXED PAGE2 TEXT */
    int altcharset;
    int has_aux;
    uint64_t cycles, frame_count;
    unsigned cycles_per_frame, lines_per_frame, beam_line, beam_horz;
    int running;                /* 0: show "not running" */
    const uint32_t *frame;      /* the displayed frame, XRGB */
    int fw, fh;
    uint8_t main[0x10000];
    uint8_t aux[0x10000];
} apple2vid_capture;
void apple2vid_tab_feed(apple2vid_tab *t, const apple2vid_capture *cap);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture (zoomed, overlays drawn), 0xFFRRGGBB, valid until the
 * next refresh or set_*. NULL when this view cannot be drawn: apple2vid_tab_why()
 * says why, and the tab should show that text.
 */
const uint32_t *apple2vid_tab_picture(apple2vid_tab *t, int *w, int *h);
const char     *apple2vid_tab_why(const apple2vid_tab *t);
int             apple2vid_tab_effective_zoom(const apple2vid_tab *t);

/* ---- the tables -------------------------------------------------------------------- */
const char *apple2vid_tab_status(const apple2vid_tab *t);        /* two lines, '\n' between */

/* SWITCHES: TEXT MIXED PAGE2 HIRES 80COL 80STORE ALTCHARSET DHIRES(AN3), in that order */
#define APPLE2VID_SW_COUNT 8
#define APPLE2VID_SW_FADE  12
typedef struct {
    const char *name;           /* "TEXT" ... "DHIRES" */
    int on;
    int age;                    /* refreshes since it last changed (12+: not recent) */
    unsigned set_addr;          /* the $C0xx that turns it on */
    unsigned clr_addr;          /* the $C0xx that turns it off */
    char meaning[96];           /* "graphics" / "text" ... */
} apple2vid_switch;
int apple2vid_tab_switches(const apple2vid_tab *t, const apple2vid_switch **out);
/* read+write breakpoints on both of switch index's addresses; the first id, or -1 */
int apple2vid_tab_break_on_switch(apple2vid_tab *t, int index);

/* COLOURS: the palette of the Decoded picture (16 lo-res/double colours, the
 * six hi-res colours, or black and white for text) with usage counts. */
typedef struct {
    const char *name;
    uint32_t rgb;               /* 0xFFRRGGBB */
    long uses;                  /* pixels of the Decoded picture (before zoom) */
    int inspected;              /* the inspected pixel uses it */
} apple2vid_pal_entry;
int apple2vid_tab_palette(const apple2vid_tab *t, const apple2vid_pal_entry **out);

/* TIMING, as ready-made "label\tvalue" lines */
int  apple2vid_tab_timing(const apple2vid_tab *t, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  apple2vid_tab_beam_permille(const apple2vid_tab *t);

/* A section as plain text (the console's `video` command shares it). */
int  apple2vid_tab_text(apple2vid_tab *t, int section, char *dst, int dstsz);

/* ---- hover, pin ---------------------------------------------------------------------- */
typedef struct {
    int valid;
    int x, y;                   /* picture pixel BEFORE zoom, in the view shown (Screen: frame pixels) */
    int dx, dy;                 /* the same spot in the 560x192 decoded grid */
    int mode;                   /* enum apple2vid_mode of the cell */
    int page;                   /* 1 or 2 */
    int text_cell;              /* the cell is text (text modes, or the text lines of MIXED) */
    int col, row;               /* cell column (0..39 or 0..79) and text row / lo-res row 0..23; hi-res: byte column, scanline */
    int main_addr;              /* -1 when none */
    int aux_addr;               /* -1 when the mode uses no aux memory */
    int main_val, aux_val;      /* -1 when absent */
    int rx, ry, rw, rh;         /* the cell's rectangle in the 560x192 grid (the mark) */
    int pal;                    /* index into apple2vid_tab_palette(), or -1 */
    uint32_t rgb;               /* the colour under the pixel, 0xFFRRGGBB */
    char text[640];             /* ready-made inspector text */
} apple2vid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  apple2vid_tab_hover(apple2vid_tab *t, int x, int y);
void apple2vid_tab_pin(apple2vid_tab *t, int x, int y);      /* a click; the same spot unpins */
void apple2vid_tab_unpin(apple2vid_tab *t);
/* the pinned pixel if any, else the hovered one, else NULL */
const apple2vid_pixel *apple2vid_tab_pixel(const apple2vid_tab *t);
/* rectangles x,y,w,h in ZOOMED picture pixels for the marks drawn over the
 * picture; bit0 of the result: hover valid, bit1: pin valid */
int  apple2vid_tab_marks(const apple2vid_tab *t, int hover[4], int pin[4]);
/* Write breakpoints on the inspected pixel's byte: its main address, and its
 * aux address when the mode uses aux. The first id, or -1 (nothing inspected,
 * no engine, or a breakpoint failed). */
int  apple2vid_tab_break_on_pixel(apple2vid_tab *t);
/* the palette index the inspected pixel uses, or -1 */
int  apple2vid_tab_pixel_palette_index(const apple2vid_tab *t);

#ifdef __cplusplus
}
#endif

#endif

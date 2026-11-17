/*
 * The Win32 Controllers window (F9): the Apple II's game controllers drawn as a
 * player sees them, from the session's layout (apple2session_controls_layout),
 * so every frontend shows the same parts.
 *
 *   JOYSTICK  the Apple Joystick IIe and IIc: a cream (Beige, the IIe's) or
 *             platinum (the IIc's) square base with a raised block, a pyramid
 *             well, a black or grey stick that tilts in it, two orange square
 *             buttons and the small Apple on the case. The stick follows what
 *             drives the machine (this window's mouse, the keyboard, a gamepad),
 *             and is proportional, not eight-way.
 *   PADDLES   two Apple Hand Controllers: a ribbed wedge each with a knob you
 *             turn and a button at the front.
 *
 * Under them the console's buttons (Reset, Power Cycle, Keyboard), the Joystick
 * / Paddles and Beige / Platinum choices, the gamepads with the port each
 * drives, and Map mode. KEYBOARD shows the Apple IIe's own keyboard
 * (apple2session_keyboard_layout) below the controllers: a cap sends its key on
 * the press and releases on the release; SHIFT and CONTROL stay latched until
 * the next key, CAPS LOCK toggles, and the Open Apple and Option caps are held
 * buttons 0 and 1.
 *
 * MOUSE: a press on a button holds it; a press on the stick follows the pointer
 * (apple2session_joystick_from_point, proportional) and lets go on release; a
 * press on a paddle's dial turns the knob to the pointer's angle and the wheel
 * nudges it. Presses are the raw button-down and button-up, never a click: the
 * machine reads its buttons once a frame. The mouse is captured on press;
 * letting go anywhere, losing the capture, hiding or deactivating the window
 * releases whatever the pointer holds. Reset and Power Cycle fire on release
 * over the button, like a real one.
 *
 * What the machine sees held -- keyboard, gamepads and this window together
 * (apple2session_target_held, apple2session_axes_get) -- is polled every 40 ms
 * and painted in the accent, so the window doubles as a gamepad tester.
 *
 * FIXED SIZE: laid out for the controllers, two gamepad rows and the Map row
 * (and the keyboard when it is showing), in a caption-only tool window with no
 * thick frame and no maximize box, so neither Map mode nor a long gamepad name
 * changes its size; only the Keyboard button and the Joystick / Paddles choice
 * do.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control to pick the target
 * (the stick: the side of the well, the dial: its left half turns it
 * counter-clockwise, its right half clockwise, its lower part is the knob's
 * gamepad axis; Reset and Power Cycle too), then press a host key OR a gamepad
 * button (an axis for the knob); Esc cancels. While armed the controls show what
 * drives them, typing does not reach the machine, and rebinding steals the key
 * from whatever held it. Clear takes the picked target's key and gamepad input
 * away. "Show bindings" shows the same legends outside Map mode. Alt, the Windows
 * keys (the Open and Closed Apple keys), F9, F11 and F12 cannot be bound.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at the
 * window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include "controllers_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bindings.h"
#include "../help/help_window.h"
#include "../keys.h"
#include "../win_common.h"

#define PANEL_CLASS   "FujiNetGoApple2Controllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; SCL() scales them to the window's. */
#define UNIT96      34      /* pixels per controller layout unit */
#define KEY_UNIT    30      /* pixels per keyboard layout unit (a keycap's width) */
#define MARGIN      12
#define CASE_PAD    16
#define BRAND_H     18      /* the strip along the top of the mat */
#define KB_PAD      10      /* the keyboard plate's margin round its keys */
#define MAX_CAPS    96
#define HEAD_H      22
#define NAME_H      22      /* a gamepad row: its name line ... */
#define SEG_H       24      /* ... and its Automatic / Joystick / Paddle line */
#define ROW_GAP      6
#define PAD_ROWS     2      /* gamepad rows the window has room for: the game port has one stick, or two paddles */
#define BTN_H       28
#define MAP_W       56
#define CLEAR_W     56
#define DEFAULTS_W  74
#define SHOW_W     118
#define HELP_W      60
#define PI_DEG 3.14159265358979323846

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define HX(v) RGB(((v) >> 16) & 0xFF, ((v) >> 8) & 0xFF, (v) & 0xFF)

/* The buttons: the machine's two, the mode and colour choices, then the Map row. */
enum {
    SB_RESET, SB_POWER, SB_KEYBOARD,
    SB_MODE_JOY, SB_MODE_PAD, SB_MODEL_IIE, SB_MODEL_IIC,
    SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_HELP, SB_COUNT
};

typedef struct {
    unsigned generation;
    int mode, count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

typedef struct {
    float axes[2];          /* PDL0, PDL1: the stick, or the two knobs */
    unsigned held;          /* bit 0/1 joystick buttons, bit 2/3 paddle buttons */
} held_state;

static HWND g_panel, g_parent;
static apple2session *g_session;
static int g_dpi = 96;
static int g_mode;                          /* APPLE2_MODE_* as laid out */

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad input to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vkey;               /* the key Map took: its repeats and release too */
static char g_hint[300];
static int g_hover_part = -1;               /* the part under the idle pointer: its bindings are the hint */
static int g_tracking;

/* The controllers' layout (units), and where it sits in the client. */
static const apple2_control *g_layout;
static int g_numlayout;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;

/* What the pointer holds. */
static int g_mouse_part = -1;               /* a pressed button (layout index) */
static int g_mouse_stick;                   /* a stick drag */
static int g_mouse_knob = -1;               /* a dial drag: the paddle */
static int g_mouse_action = -1;             /* a pressed console button */
static int g_mouse_cap = -1;                /* the keycap the pointer holds */

/* What was last painted as held. */
static held_state g_shown;
static pads_state g_pads_shown;
static unsigned char g_shown_caps[MAX_CAPS];

static int g_total_w, g_total_h;
static int g_kb_on;                         /* the keyboard is showing */
static const apple2_keycap *g_cap;
static int g_ncap;
static float g_cap_wide, g_cap_high;
static double g_key_unit, g_key_x, g_key_y;
static RECT g_kb_rc;
static RECT g_case_rc, g_brand_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_hint_rc;

static HFONT g_font, g_bold, g_legend, g_tiny, g_brand, g_caption;
static HFONT g_keybig, g_keymid, g_keysmall, g_keytiny, g_keyshift;

static void refresh_held(void);
static void refresh_pads(void);

static int SCL(int val) { return apple2_scale(val, g_dpi); }

static int cur_view(void)
{
    return g_mode == APPLE2_MODE_PADDLES ? APPLE2_VIEW_PADDLES : APPLE2_VIEW_JOYSTICK;
}

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_legend, &g_tiny, &g_brand, &g_caption,
                             &g_keybig, &g_keymid, &g_keysmall, &g_keytiny, &g_keyshift };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static void make_fonts(void)
{
    free_fonts();
    g_font = apple2_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = apple2_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_legend = apple2_make_font(g_dpi, 11, FW_BOLD, NULL);
    g_tiny = apple2_make_font(g_dpi, 10, FW_NORMAL, NULL);
    g_brand = apple2_make_font(g_dpi, 12, FW_BOLD, NULL);
    g_caption = apple2_make_font(g_dpi, 12, FW_BOLD, NULL);
    g_keybig = apple2_make_font(g_dpi, 13, FW_BOLD, NULL);
    g_keymid = apple2_make_font(g_dpi, 11, FW_BOLD, NULL);
    g_keysmall = apple2_make_font(g_dpi, 9, FW_BOLD, NULL);
    g_keytiny = apple2_make_font(g_dpi, 7, FW_BOLD, NULL);
    g_keyshift = apple2_make_font(g_dpi, 8, FW_NORMAL, NULL);
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (legends, key names, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

static int text_width(HDC hdc, const char *utf8)
{
    wchar_t wide[128];
    SIZE ext;
    if (!utf8 || MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 128) <= 0) return 0;
    return GetTextExtentPoint32W(hdc, wide, (int)wcslen(wide), &ext) ? ext.cx : 0;
}

/* The colours live here and only here; the geometry is the core's. From the
 * photos of the real joysticks, paddles and keyboard (DESIGN-apple2.md). */
#define COL_ACCENT     RGB((APPLE2SESSION_ACCENT_RGB >> 16) & 0xFF, (APPLE2SESSION_ACCENT_RGB >> 8) & 0xFF, \
                           APPLE2SESSION_ACCENT_RGB & 0xFF)
#define COL_ON_ACCENT  RGB(0xFF, 0xFF, 0xFF)
#define COL_MAT_TOP    HX(0x3A3A40)             /* the desk the controllers lie on */
#define COL_MAT_BOT    HX(0x26262B)
#define COL_MAT_EDGE   HX(0x141416)
#define COL_BRAND_INK  HX(0xD8D6CC)
#define COL_SHADOW     HX(0x1B1B1F)
#define COL_CAPTION    HX(0xD8D6CC)
#define COL_KB_PLATE   HX(0xCFCDC4)
#define COL_KB_PLATE_E HX(0x8F8C82)
#define COL_KB_CAP     HX(0xE4E2DA)
#define COL_KB_SPEC    HX(0xCCCABF)
#define COL_KB_RESET   HX(0xF4F3EE)
#define COL_KB_INK     HX(0x2B2B2D)
#define COL_DIM_TO     HX(0x2C2C31)             /* unpicked parts fade toward the mat */
#define NOFILL         ((COLORREF)0xFFFFFFFFu)

typedef struct {
    COLORREF case_top, case_shade, well_tb, well_side, floor, shaft, cap_hi, btn, btn_down, btn_frame, logo;
    COLORREF pad_body, pad_shade, knob, knob_top, knob_rim;
    COLORREF label_ink;     /* binding legends printed on the well */
} scheme;

static const scheme k_beige = {
    HX(0xE6DBBF), HX(0xCFC3A4), HX(0x4A4A4E), HX(0x5C5C60), HX(0x2A2A2E), HX(0x1D1D26), HX(0x4C4C58),
    HX(0xF26A1F), HX(0xC9500F), HX(0xB9AC8B), HX(0xD6CAAC),
    HX(0xC9C3A8), HX(0xB3AD92), HX(0x6F6C54), HX(0x7B7860), HX(0x5C5A44),
    HX(0xEFEAE0)
};
static const scheme k_platinum = {
    HX(0xEBE9E1), HX(0xCFCDC4), HX(0xBDBBB2), HX(0xCBC9C0), HX(0xA9A79E), HX(0xA6A499), HX(0xC9C7BD),
    HX(0xF0641C), HX(0xC54B0D), HX(0xB6B4AB), HX(0xD7D5CC),
    HX(0xD5D3CA), HX(0xBFBDB4), HX(0x8A8878), HX(0x97958A), HX(0x74725F),
    HX(0x2E2E30)
};

static const scheme *cur_scheme(void)
{
    return apple2session_controller_model(g_session) == APPLE2_MODEL_IIC ? &k_platinum : &k_beige;
}

/* ---- art: GDI at a supersampled scale ------------------------------------------- */

/* One art pass draws the client rectangle with its top-left at (g_art_x,
 * g_art_y) into a bitmap g_art_k times the size and g_art_rows rows tall. */
static int g_art_k = 1, g_art_x, g_art_y, g_art_rows;

static int art_xpos(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_ypos(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

static COLORREF mix_color(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

/* An unpicked part in Map mode: 40% of its own colours over the mat's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? mix_color(col, COL_DIM_TO, 0.6) : col;
}

static void poly(HDC hdc, const POINT *pnts, int num, COLORREF fill, COLORREF line, double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pnts, num);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_xpos(ctrx - radx), art_ypos(ctry - rady), art_xpos(ctrx + radx), art_ypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_xpos(xpos), art_ypos(ypos), art_xpos(xpos + width), art_ypos(ypos + height),
              art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void line_seg(HDC hdc, double x0, double y0, double x1, double y1, COLORREF col, double width)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(width) > 0 ? art_len(width) : 1, col);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    MoveToEx(hdc, art_xpos(x0), art_ypos(y0), NULL);
    LineTo(hdc, art_xpos(x1), art_ypos(y1));
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

/* A rounded rectangle filled with a vertical gradient, then outlined. */
static void grad_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                       COLORREF top, COLORREF bottom, COLORREF edge, double edgew)
{
    const int left = art_xpos(xpos), right = art_xpos(xpos + width);
    const int first = art_ypos(ypos), last = art_ypos(ypos + height);
    HRGN clip = CreateRoundRectRgn(left, first, right + 1, last + 1, art_len(radius * 2), art_len(radius * 2));
    int row;
    HGDIOBJ oldb;
    SelectClipRgn(hdc, clip);
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < g_art_rows; row++) {
        SetDCBrushColor(hdc, mix_color(top, bottom, last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left + 1, 1, PATCOPY);
    }
    SelectObject(hdc, oldb);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    if (edgew > 0) rrect(hdc, xpos, ypos, width, height, radius, NOFILL, edge, edgew);
}

/* The pixel rectangle of a layout part. */
static void part_box(const apple2_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->xpos * g_unit;
    *ypos = g_orgy + (double)ctl->ypos * g_unit;
    *width = (double)ctl->width * g_unit;
    *height = (double)ctl->height * g_unit;
}

static void part_rect(const apple2_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

static const apple2_control *find_part(int part)
{
    int idx;
    for (idx = 0; idx < g_numlayout; idx++)
        if (g_layout[idx].kind == APPLE2_CTL_PART && g_layout[idx].part == part) return &g_layout[idx];
    return NULL;
}

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* ---- which parts are lit ------------------------------------------------------- */

/* The bit of g_shown.held a button target is, or -1. */
static int target_bit(int target)
{
    if (target == APPLE2_TARGET_JOY(APPLE2_ACT_BTN0)) return 0;
    if (target == APPLE2_TARGET_JOY(APPLE2_ACT_BTN1)) return 1;
    if (target == APPLE2_TARGET_PADDLE(0, APPLE2_PACT_BUTTON)) return 2;
    if (target == APPLE2_TARGET_PADDLE(1, APPLE2_PACT_BUTTON)) return 3;
    return -1;
}

static int is_joy_dir_target(int target)
{
    return target >= APPLE2_TARGET_JOY(APPLE2_ACT_LEFT) && target <= APPLE2_TARGET_JOY(APPLE2_ACT_DOWN);
}

static int is_knob_target(int target, int port)
{
    return target >= APPLE2_TARGET_PADDLES &&
           (target - APPLE2_TARGET_PADDLES) / APPLE2_PADDLE_ACTS == port &&
           (target - APPLE2_TARGET_PADDLES) % APPLE2_PADDLE_ACTS != APPLE2_PACT_BUTTON;
}

/* Whether a part is shown held: what the machine sees, in the accent. In Map
 * mode the picked target is the one lit. */
static int part_lit(const apple2_control *ctl)
{
    if (g_map_state != -2) {
        if (g_map_state < 0) return 0;
        switch (ctl->kind) {
        case APPLE2_CTL_BUTTON:
        case APPLE2_CTL_SYSBUTTON: return ctl->target == g_map_state;
        case APPLE2_CTL_STICK: return is_joy_dir_target(g_map_state);
        case APPLE2_CTL_KNOB: return is_knob_target(g_map_state, ctl->port);
        default: return 0;
        }
    }
    switch (ctl->kind) {
    case APPLE2_CTL_BUTTON: {
        const int bit = target_bit(ctl->target);
        return bit >= 0 && (g_shown.held & (1u << bit)) != 0;
    }
    case APPLE2_CTL_KNOB: return g_mouse_knob == ctl->port;
    case APPLE2_CTL_STICK: return g_mouse_stick;
    default: return 0;
    }
}

/* Map mode with a target picked: every other part is drawn at 40%. */
static int part_dim(const apple2_control *ctl)
{
    if (g_map_state < 0) return 0;
    if (ctl->kind != APPLE2_CTL_BUTTON && ctl->kind != APPLE2_CTL_STICK && ctl->kind != APPLE2_CTL_KNOB) return 0;
    return !part_lit(ctl);
}

/* ---- the Apple glyph ------------------------------------------------------------ */

/* The apple's body: two lobes with the join between them filled, at `scale` of
 * full size about its centre. */
static void apple_body(HDC hdc, double ctrx, double ctry, double size, double scale, COLORREF col)
{
    const double lobe = size * 0.34 * scale, offs = size * 0.16 * scale, midy = ctry + size * 0.1;
    ellipse(hdc, ctrx - offs, midy, lobe, lobe * 1.08, col, col, 0);
    ellipse(hdc, ctrx + offs, midy, lobe, lobe * 1.08, col, col, 0);
    rrect(hdc, ctrx - offs, midy - lobe * 1.0, offs * 2, lobe * 2.0, 0, col, col, 0);
}

/* A small apple at (ctrx, ctry), `size` tall: two lobes, a bite, a leaf. Filled,
 * or an outline (the Open Apple key). bg is what is behind it (the bite and the
 * outline's hollow). */
static void apple_glyph(HDC hdc, double ctrx, double ctry, double size, int filled, COLORREF ink, COLORREF bg)
{
    POINT leaf[4];
    apple_body(hdc, ctrx, ctry, size, 1.0, ink);
    if (!filled)
        apple_body(hdc, ctrx, ctry + size * 0.01, size, 0.5, bg);
    ellipse(hdc, ctrx + size * 0.46, ctry - size * 0.02, size * 0.17, size * 0.17, bg, bg, 0);   /* the bite */
    leaf[0].x = art_xpos(ctrx + size * 0.02);
    leaf[0].y = art_ypos(ctry - size * 0.2);
    leaf[1].x = art_xpos(ctrx + size * 0.2);
    leaf[1].y = art_ypos(ctry - size * 0.5);
    leaf[2].x = art_xpos(ctrx + size * 0.34);
    leaf[2].y = art_ypos(ctry - size * 0.42);
    leaf[3].x = art_xpos(ctrx + size * 0.12);
    leaf[3].y = art_ypos(ctry - size * 0.16);
    poly(hdc, leaf, 4, ink, ink, 0);
}

/* ---- the buttons (joystick and paddle) ------------------------------------------ */

static void art_button(HDC hdc, const apple2_control *ctl, COLORREF face, COLORREF down, COLORREF frame, int dim)
{
    double xpos, ypos, width, height;
    const double unit = g_unit;
    const int lit = part_lit(ctl);
    const int pressed = lit && g_map_state == -2;
    part_box(ctl, &xpos, &ypos, &width, &height);
    rrect(hdc, xpos - unit * 0.1, ypos - unit * 0.08, width + unit * 0.2, height + unit * 0.2, unit * 0.16,
          tone(frame, dim), tone(frame, dim), 0);
    rrect(hdc, xpos, ypos + (pressed ? unit * 0.08 : 0), width, height - (pressed ? unit * 0.06 : unit * 0.04), unit * 0.12,
          tone(pressed ? down : face, dim), tone(down, dim), 1.0);
    if (!pressed)    /* the lit top edge */
        line_seg(hdc, xpos + unit * 0.12, ypos + unit * 0.1, xpos + width - unit * 0.12, ypos + unit * 0.1,
                 mix_color(face, RGB(255, 255, 255), 0.3), 1.0);
    if (lit)
        rrect(hdc, xpos - unit * 0.2, ypos - unit * 0.18, width + unit * 0.4, height + unit * 0.4, unit * 0.22, NOFILL,
              COL_ACCENT, unit * 0.1);
}

/* ---- the joystick ------------------------------------------------------------------- */

static void art_stick(HDC hdc, const apple2_control *ctl, const scheme *sc)
{
    const double unit = g_unit;
    const double sockx = g_orgx + (double)ctl->cx * unit, socky = g_orgy + (double)ctl->cy * unit;
    const double capx = g_orgx + ((double)ctl->capx + g_shown.axes[0] * ctl->reach) * unit;
    const double capy = g_orgy + ((double)ctl->capy + g_shown.axes[1] * ctl->reach) * unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl) || g_shown.axes[0] != 0 || g_shown.axes[1] != 0;
    double dirx = capx - sockx, diry = capy - socky;
    const double len = sqrt(dirx * dirx + diry * diry);
    const COLORREF shaft = tone(sc->shaft, dim);
    POINT body[4];
    double back;

    if (len > 0) { dirx /= len; diry /= len; } else { dirx = 0; diry = -1; }
    /* the socket, then the shaft, then the thicker cap on the shaft's end */
    ellipse(hdc, sockx, socky, unit * 0.32, unit * 0.26, tone(mix_color(sc->floor, RGB(0, 0, 0), 0.4), dim),
            tone(mix_color(sc->floor, RGB(0, 0, 0), 0.4), dim), 0);
    line_seg(hdc, sockx, socky, capx, capy, shaft, unit * 0.3);
    back = unit * 0.95;
    {
        const double nx = -diry, ny = dirx, half = unit * 0.27;
        const double tailx = capx - dirx * back, taily = capy - diry * back;
        body[0].x = art_xpos(tailx + nx * half * 0.8);
        body[0].y = art_ypos(taily + ny * half * 0.8);
        body[1].x = art_xpos(capx + nx * half);
        body[1].y = art_ypos(capy + ny * half);
        body[2].x = art_xpos(capx - nx * half);
        body[2].y = art_ypos(capy - ny * half);
        body[3].x = art_xpos(tailx - nx * half * 0.8);
        body[3].y = art_ypos(taily - ny * half * 0.8);
        poly(hdc, body, 4, shaft, shaft, 0);
        ellipse(hdc, capx, capy, half, half, shaft, shaft, 0);
        /* a highlight along the cap's lit side */
        line_seg(hdc, tailx + nx * half * 0.35, taily + ny * half * 0.35, capx + nx * half * 0.35, capy + ny * half * 0.35,
                 tone(sc->cap_hi, dim), unit * 0.08);
        if (lit && g_map_state == -2)
            ellipse(hdc, capx, capy, half + unit * 0.16, half + unit * 0.16, NOFILL, COL_ACCENT, unit * 0.1);
    }
}

static void art_joystick(HDC hdc)
{
    const scheme *sc = cur_scheme();
    const apple2_control *floor_ctl = find_part(APPLE2_PART_JOY_FLOOR);
    const double unit = g_unit;
    int idx;

    for (idx = 0; idx < g_numlayout; idx++) {
        const apple2_control *ctl = &g_layout[idx];
        double xpos, ypos, width, height;
        part_box(ctl, &xpos, &ypos, &width, &height);
        if (ctl->kind == APPLE2_CTL_PART) {
            switch (ctl->part) {
            case APPLE2_PART_JOY_BASE:
                rrect(hdc, xpos + unit * 0.12, ypos + unit * 0.18, width, height, unit * 0.38, COL_SHADOW, COL_SHADOW, 0);
                grad_rrect(hdc, xpos, ypos, width, height, unit * 0.35, sc->case_top,
                           mix_color(sc->case_top, sc->case_shade, 0.45), sc->case_shade, 1.2);
                /* the seam of the lower shell */
                line_seg(hdc, xpos + unit * 0.14, ypos + height - unit * 0.6, xpos + width - unit * 0.14,
                         ypos + height - unit * 0.6, sc->case_shade, 1.0);
                break;
            case APPLE2_PART_JOY_BLOCK:
                /* it stands proud: a shadow down its left, a lighter top edge */
                rrect(hdc, xpos - unit * 0.14, ypos + unit * 0.12, width, height, unit * 0.3,
                      mix_color(sc->case_shade, RGB(0, 0, 0), 0.18), mix_color(sc->case_shade, RGB(0, 0, 0), 0.18), 0);
                grad_rrect(hdc, xpos, ypos, width, height, unit * 0.3, mix_color(sc->case_top, RGB(255, 255, 255), 0.28),
                           sc->case_top, sc->case_shade, 1.2);
                break;
            case APPLE2_PART_JOY_WELL: {
                const double fx = floor_ctl ? g_orgx + (double)floor_ctl->xpos * unit : xpos + width * 0.4;
                const double fy = floor_ctl ? g_orgy + (double)floor_ctl->ypos * unit : ypos + height * 0.4;
                const double fw = floor_ctl ? (double)floor_ctl->width * unit : unit;
                const double fh = floor_ctl ? (double)floor_ctl->height * unit : unit;
                POINT wall[4];
                struct { double x0, y0, x1, y1, x2, y2, x3, y3; COLORREF col; } walls[4] = {
                    { xpos, ypos, xpos + width, ypos, fx + fw, fy, fx, fy, sc->well_tb },                       /* top */
                    { xpos, ypos + height, xpos + width, ypos + height, fx + fw, fy + fh, fx, fy + fh, sc->well_tb },
                    { xpos, ypos, fx, fy, fx, fy + fh, xpos, ypos + height, sc->well_side },                   /* left */
                    { xpos + width, ypos, fx + fw, fy, fx + fw, fy + fh, xpos + width, ypos + height, sc->well_side },
                };
                int wdx;
                for (wdx = 0; wdx < 4; wdx++) {
                    wall[0].x = art_xpos(walls[wdx].x0); wall[0].y = art_ypos(walls[wdx].y0);
                    wall[1].x = art_xpos(walls[wdx].x1); wall[1].y = art_ypos(walls[wdx].y1);
                    wall[2].x = art_xpos(walls[wdx].x2); wall[2].y = art_ypos(walls[wdx].y2);
                    wall[3].x = art_xpos(walls[wdx].x3); wall[3].y = art_ypos(walls[wdx].y3);
                    poly(hdc, wall, 4, walls[wdx].col, mix_color(walls[wdx].col, RGB(0, 0, 0), 0.35), 0.8);
                }
                if (g_map_state >= 0 && is_joy_dir_target(g_map_state)) {
                    /* the side of the well the picked direction is */
                    const int dir = g_map_state - APPLE2_TARGET_JOY(APPLE2_ACT_LEFT);
                    if (dir == APPLE2_ACT_LEFT) line_seg(hdc, xpos + unit * 0.12, ypos + unit * 0.2, xpos + unit * 0.12, ypos + height - unit * 0.2, COL_ACCENT, unit * 0.12);
                    if (dir == APPLE2_ACT_RIGHT) line_seg(hdc, xpos + width - unit * 0.12, ypos + unit * 0.2, xpos + width - unit * 0.12, ypos + height - unit * 0.2, COL_ACCENT, unit * 0.12);
                    if (dir == APPLE2_ACT_UP) line_seg(hdc, xpos + unit * 0.2, ypos + unit * 0.12, xpos + width - unit * 0.2, ypos + unit * 0.12, COL_ACCENT, unit * 0.12);
                    if (dir == APPLE2_ACT_DOWN) line_seg(hdc, xpos + unit * 0.2, ypos + height - unit * 0.12, xpos + width - unit * 0.2, ypos + height - unit * 0.12, COL_ACCENT, unit * 0.12);
                }
                break;
            }
            case APPLE2_PART_JOY_FLOOR:
                rrect(hdc, xpos, ypos, width, height, unit * 0.06, sc->floor, mix_color(sc->floor, RGB(0, 0, 0), 0.4), 0.8);
                break;
            case APPLE2_PART_JOY_LOGO: {
                const COLORREF ink = mix_color(sc->logo, RGB(0, 0, 0), 0.16);
                rrect(hdc, xpos, ypos, width, height, unit * 0.14, sc->logo, ink, 1.0);
                apple_glyph(hdc, xpos + width / 2, ypos + height / 2 + unit * 0.03, height * 0.62, 1, ink, sc->logo);
                break;
            }
            default: break;
            }
        } else if (ctl->kind == APPLE2_CTL_BUTTON) {
            art_button(hdc, ctl, sc->btn, sc->btn_down, sc->btn_frame, part_dim(ctl));
        } else if (ctl->kind == APPLE2_CTL_STICK) {
            art_stick(hdc, ctl, sc);
        }
    }
}

/* ---- the paddles --------------------------------------------------------------------- */

static void art_knob(HDC hdc, const apple2_control *ctl, const scheme *sc)
{
    const double unit = g_unit;
    const double ctrx = g_orgx + (double)ctl->cx * unit, ctry = g_orgy + (double)ctl->cy * unit;
    const double rad = (double)ctl->reach * unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    const double value = ctl->port >= 0 && ctl->port < 2 ? g_shown.axes[ctl->port] : 0;
    const double ang = apple2session_knob_angle((float)value) * PI_DEG / 180.0;
    int idx;

    ellipse(hdc, ctrx + unit * 0.06, ctry + unit * 0.1, rad * 1.04, rad * 1.04, COL_SHADOW, COL_SHADOW, 0);
    ellipse(hdc, ctrx, ctry, rad, rad, tone(sc->knob, dim), tone(sc->knob_rim, dim), 1.0);
    for (idx = 0; idx < 24; idx++) {    /* the knurled rim */
        const double a = idx * 15.0 * PI_DEG / 180.0;
        line_seg(hdc, ctrx + rad * 0.86 * cos(a), ctry - rad * 0.86 * sin(a), ctrx + rad * 0.98 * cos(a),
                 ctry - rad * 0.98 * sin(a), tone(sc->knob_rim, dim), unit * 0.06);
    }
    ellipse(hdc, ctrx - rad * 0.03, ctry - rad * 0.05, rad * 0.74, rad * 0.74, tone(sc->knob_top, dim),
            tone(sc->knob_rim, dim), 0.8);
    /* the index mark: straight up at the centre, 135 degrees either way at the stops */
    line_seg(hdc, ctrx + rad * 0.14 * sin(ang), ctry - rad * 0.14 * cos(ang), ctrx + rad * 0.66 * sin(ang),
             ctry - rad * 0.66 * cos(ang), tone(HX(0xE9E5D0), dim), unit * 0.12);
    if (lit)
        ellipse(hdc, ctrx, ctry, rad + unit * 0.14, rad + unit * 0.14, NOFILL, COL_ACCENT, unit * 0.1);
}

static void art_paddles(HDC hdc)
{
    const scheme *sc = cur_scheme();
    const double unit = g_unit;
    int idx;

    for (idx = 0; idx < g_numlayout; idx++) {
        const apple2_control *ctl = &g_layout[idx];
        double xpos, ypos, width, height;
        part_box(ctl, &xpos, &ypos, &width, &height);
        if (ctl->kind == APPLE2_CTL_PART && ctl->part == APPLE2_PART_PADDLE_BODY) {
            static const double shape[6][2] = { { 0, 0.6 }, { 5.2, 0 }, { 6, 0.9 }, { 6, 2.4 }, { 5, 3 }, { 0, 2.6 } };
            POINT pts[6], shadow[6];
            int pdx, rib;
            for (pdx = 0; pdx < 6; pdx++) {
                pts[pdx].x = art_xpos(xpos + shape[pdx][0] * unit);
                pts[pdx].y = art_ypos(ypos + shape[pdx][1] * unit);
                shadow[pdx].x = art_xpos(xpos + shape[pdx][0] * unit + unit * 0.1);
                shadow[pdx].y = art_ypos(ypos + shape[pdx][1] * unit + unit * 0.16);
            }
            poly(hdc, shadow, 6, COL_SHADOW, COL_SHADOW, 0);
            poly(hdc, pts, 6, sc->pad_body, sc->pad_shade, 1.2);
            /* the ribs along the underside */
            for (rib = 0; rib < 11; rib++) {
                const double rx = 0.55 + rib * 0.42;
                const double ybot = 2.6 + 0.08 * rx;
                line_seg(hdc, xpos + rx * unit, ypos + (ybot - 0.62) * unit, xpos + rx * unit, ypos + (ybot - 0.1) * unit,
                         sc->pad_shade, unit * 0.07);
            }
        } else if (ctl->kind == APPLE2_CTL_KNOB) {
            art_knob(hdc, ctl, sc);
        } else if (ctl->kind == APPLE2_CTL_BUTTON) {
            art_button(hdc, ctl, HX(0xEB6A25), HX(0xC24E10), sc->pad_shade, part_dim(ctl));
        }
    }
}

/* ---- the keyboard ---------------------------------------------------------------- */

static void cap_rect(const apple2_keycap *cap, RECT *rect)
{
    const double xpos = g_key_x + (double)cap->xpos * g_key_unit, ypos = g_key_y + (double)cap->ypos * g_key_unit;
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + (double)cap->width * g_key_unit),
            (int)ceil(ypos + (double)cap->height * g_key_unit));
}

static int cap_at(POINT pnt)
{
    int idx;
    if (!g_kb_on) return -1;
    for (idx = 0; idx < g_ncap; idx++) {
        RECT rect;
        cap_rect(&g_cap[idx], &rect);
        if (PtInRect(&rect, pnt)) return idx;
    }
    return -1;
}

static COLORREF cap_colour(const apple2_keycap *cap)
{
    switch (cap->kind) {
    case APPLE2_KEYCAP_CHAR:
    case APPLE2_KEYCAP_SPACE: return COL_KB_CAP;
    case APPLE2_KEYCAP_RESET: return COL_KB_RESET;
    default: return COL_KB_SPEC;
    }
}

/* Map mode fades the keyboard: it is not remappable. */
static void art_keyboard(HDC hdc)
{
    const int dim = g_map_state != -2;
    int idx;
    rrect(hdc, g_kb_rc.left, g_kb_rc.top, g_kb_rc.right - g_kb_rc.left, g_kb_rc.bottom - g_kb_rc.top, SCL(8),
          tone(COL_KB_PLATE, dim), tone(COL_KB_PLATE_E, dim), 1.0);
    for (idx = 0; idx < g_ncap; idx++) {
        const apple2_keycap *cap = &g_cap[idx];
        const int held = idx < MAX_CAPS ? g_shown_caps[idx] : 0;
        const int lit = (held & 1) != 0;
        const COLORREF col = tone(cap_colour(cap), dim);
        const COLORREF dark = mix_color(col, RGB(0, 0, 0), 0.42);
        RECT rect;
        double xpos, ypos, width, height;
        cap_rect(cap, &rect);
        xpos = rect.left;
        ypos = rect.top;
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        rrect(hdc, xpos, ypos + SCL(2), width, height - SCL(2), SCL(4), lit ? COL_ACCENT : dark, lit ? COL_ACCENT : dark, 0);
        rrect(hdc, xpos, ypos, width, height - SCL(2), SCL(4), lit ? COL_ACCENT : col,
              lit ? COL_ON_ACCENT : mix_color(col, RGB(0, 0, 0), 0.5), lit ? 1.5 : 1.0);
        if (cap->kind == APPLE2_KEYCAP_APPLE)
            apple_glyph(hdc, xpos + width / 2, ypos + height * 0.36, height * 0.5, cap->keysym == 1,
                        lit ? COL_ON_ACCENT : tone(COL_KB_INK, dim), lit ? COL_ACCENT : col);
        if (held & 2)   /* a latch that is on: a dot as well, so it reads when other keys are down */
            ellipse(hdc, xpos + SCL(6), ypos + SCL(6), SCL(3), SCL(3), COL_ACCENT, COL_ACCENT, 0);
    }
}

/* The mat, its brand strip, the controller and the keyboard. */
static void art_case(HDC hdc)
{
    static const COLORREF rainbow[6] = { HX(0x61BB46), HX(0xFDB827), HX(0xF5821F), HX(0xE03A3E), HX(0x963D97), HX(0x009DDC) };
    int bar;
    grad_rrect(hdc, g_case_rc.left, g_case_rc.top, g_case_rc.right - g_case_rc.left,
               g_case_rc.bottom - g_case_rc.top, SCL(14), COL_MAT_TOP, COL_MAT_BOT, COL_MAT_EDGE, 1.5);
    /* the six-colour stripe of the Apple logo, then the name (text pass) */
    for (bar = 0; bar < 6; bar++)
        rrect(hdc, g_brand_rc.left + SCL(2), g_brand_rc.top + SCL(3) + bar * SCL(2), SCL(18), SCL(2),
              0, rainbow[bar], rainbow[bar], 0);
    if (g_mode == APPLE2_MODE_PADDLES)
        art_paddles(hdc);
    else
        art_joystick(hdc);
    if (g_kb_on) art_keyboard(hdc);
}

/* In Map mode and with Show bindings a target shows what drives it. */
static void binding_label(int target, char *out, int outsz)
{
    const uint32_t keysym = bindings_target_keysym(target);
    out[0] = '\0';
    if (keysym) {
        apple2session_keysym_name(keysym, out, outsz);
        /* the labels are small: "Keypad 5" reads "KP 5" */
        if (strncmp(out, "Keypad ", 7) == 0) {
            memmove(out + 3, out + 7, strlen(out + 7) + 1);    /* in place: "Keypad " is longer than "KP " */
            memcpy(out, "KP ", 3);
        }
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "%s", "\xe2\x80\x94");
}

static void text_keyboard(HDC hdc)
{
    const int dim = g_map_state != -2;
    int idx;
    for (idx = 0; idx < g_ncap; idx++) {
        const apple2_keycap *cap = &g_cap[idx];
        const int lit = idx < MAX_CAPS && (g_shown_caps[idx] & 1);
        RECT rect, main;
        cap_rect(cap, &rect);
        rect.bottom -= SCL(2);
        main = rect;
        SetTextColor(hdc, lit ? COL_ON_ACCENT : tone(COL_KB_INK, dim));
        switch (cap->kind) {
        case APPLE2_KEYCAP_APPLE: {
            RECT word = rect;
            word.top = rect.top + (rect.bottom - rect.top) * 11 / 20;
            SelectObject(hdc, g_keytiny);
            text_line(hdc, cap->keysym == 1 ? "Option" : "Open", &word, DT_CENTER);
            continue;
        }
        case APPLE2_KEYCAP_RESET:
            SelectObject(hdc, g_keytiny);
            text_line(hdc, "Reset", &rect, DT_CENTER);
            continue;
        case APPLE2_KEYCAP_SPACE:
            continue;
        default:
            break;
        }
        if (cap->shifted && *cap->shifted) {
            RECT top = rect;
            top.bottom = rect.top + (rect.bottom - rect.top) * 11 / 20;
            SelectObject(hdc, g_keyshift);
            text_line(hdc, cap->shifted, &top, DT_CENTER);
            main.top = rect.top + (rect.bottom - rect.top) * 8 / 20;
            SelectObject(hdc, g_keymid);
        } else if (cap->kind == APPLE2_KEYCAP_CHAR ||
                   (cap->kind == APPLE2_KEYCAP_SPECIAL && (unsigned char)cap->legend[0] >= 0xE2)) {
            SelectObject(hdc, g_keybig);     /* a letter, or an arrow */
        } else {
            /* a named key steps down a size until it clears both edges */
            SelectObject(hdc, g_keysmall);
            if (text_width(hdc, cap->legend) > (main.right - main.left) - SCL(3))
                SelectObject(hdc, g_keytiny);
            if (text_width(hdc, cap->legend) > (main.right - main.left) - SCL(3)) {
                /* still too wide at the smallest size: two lines at the space */
                const char *cut = strchr(cap->legend, ' ');
                if (cut) {
                    char upper[32];
                    char lower[32];
                    RECT first = main, second = main;
                    snprintf(upper, sizeof upper, "%.*s", (int)(cut - cap->legend), cap->legend);
                    snprintf(lower, sizeof lower, "%s", cut + 1);
                    first.bottom = main.top + (main.bottom - main.top) / 2 + SCL(1);
                    second.top = first.bottom - SCL(2);
                    text_line(hdc, upper, &first, DT_CENTER);
                    text_line(hdc, lower, &second, DT_CENTER);
                    continue;
                }
            }
        }
        text_line(hdc, cap->legend, &main, DT_CENTER);
    }
}

/* The labels over the art, at the window's own size. */
static void text_parts(HDC hdc)
{
    int idx;
    RECT brand = g_brand_rc;
    const scheme *sc = cur_scheme();
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_brand);
    SetTextColor(hdc, COL_BRAND_INK);
    brand.left += SCL(26);
    text_line(hdc, "Apple II", &brand, DT_LEFT);
    if (g_kb_on) text_keyboard(hdc);
    SetBkMode(hdc, TRANSPARENT);
    for (idx = 0; idx < g_numlayout; idx++) {
        const apple2_control *ctl = &g_layout[idx];
        RECT rect;
        const int dim = part_dim(ctl);
        char legend[96];
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case APPLE2_CTL_CAPTION:
            SelectObject(hdc, g_caption);
            SetTextColor(hdc, COL_CAPTION);
            text_line(hdc, ctl->label, &rect, DT_CENTER);
            break;
        case APPLE2_CTL_BUTTON:
            if (binding_mode() && ctl->target >= 0) {
                /* the key, printed on the button */
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, g_map_state == ctl->target ? COL_ON_ACCENT : RGB(0xFF, 0xFF, 0xFF));
                text_line(hdc, legend, &rect, DT_CENTER | DT_END_ELLIPSIS);
            }
            break;
        case APPLE2_CTL_STICK:
            if (binding_mode()) {
                /* the four directions' keys, on the walls of the well */
                static const int dirs[4] = { APPLE2_ACT_LEFT, APPLE2_ACT_RIGHT, APPLE2_ACT_UP, APPLE2_ACT_DOWN };
                int dir;
                SelectObject(hdc, g_tiny);
                for (dir = 0; dir < 4; dir++) {
                    RECT box;
                    const int half = (int)(g_unit * 0.6);
                    const int midx = (rect.left + rect.right) / 2, midy = (rect.top + rect.bottom) / 2;
                    const int target = APPLE2_TARGET_JOY(dirs[dir]);
                    int posx = midx, posy = midy;
                    if (dirs[dir] == APPLE2_ACT_LEFT) posx = rect.left + (int)(g_unit * 0.5);
                    if (dirs[dir] == APPLE2_ACT_RIGHT) posx = rect.right - (int)(g_unit * 0.5);
                    if (dirs[dir] == APPLE2_ACT_UP) posy = rect.top + (int)(g_unit * 0.35);
                    if (dirs[dir] == APPLE2_ACT_DOWN) posy = rect.bottom - (int)(g_unit * 0.35);
                    SetRect(&box, posx - half, posy - SCL(7), posx + half, posy + SCL(7));
                    binding_label(target, legend, sizeof legend);
                    SetTextColor(hdc, g_map_state == target ? COL_ON_ACCENT : tone(sc->label_ink, dim));
                    text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
                }
            }
            break;
        case APPLE2_CTL_KNOB:
            if (binding_mode()) {
                /* beside the dial, on the body: the two turn keys and the gamepad axis */
                const int rad = (int)((double)ctl->reach * g_unit);
                const int midx = (int)(g_orgx + (double)ctl->cx * g_unit), midy = (int)(g_orgy + (double)ctl->cy * g_unit);
                const int left = midx + rad + SCL(10), right = midx + rad + (int)(g_unit * 2.3);
                RECT line1, line2, line3;
                const int rowh = SCL(12);
                SelectObject(hdc, g_tiny);
                SetRect(&line1, left, midy - rowh - rowh / 2, right, midy - rowh / 2);
                SetRect(&line2, left, midy - rowh / 2, right, midy + rowh / 2);
                SetRect(&line3, left, midy + rowh / 2, right, midy + rowh + rowh / 2);
                SetTextColor(hdc, HX(0x3B3A30));
                {
                    char key[64];
                    binding_label(APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_CCW), key, sizeof key);
                    snprintf(legend, sizeof legend, "\xe2\x86\xba %s", key);
                    text_line(hdc, legend, &line1, DT_LEFT | DT_END_ELLIPSIS);
                    binding_label(APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_CW), key, sizeof key);
                    snprintf(legend, sizeof legend, "\xe2\x86\xbb %s", key);
                    text_line(hdc, legend, &line2, DT_LEFT | DT_END_ELLIPSIS);
                }
                {
                    const int pad = bindings_target_pad(APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_KNOB));
                    char name[48];
                    if (pad >= 0) apple2session_pad_input_name(pad, name, sizeof name);
                    else snprintf(name, sizeof name, "%s", "\xe2\x80\x94");
                    snprintf(legend, sizeof legend, "axis %s", name);
                    text_line(hdc, legend, &line3, DT_LEFT | DT_END_ELLIPSIS);
                }
            }
            break;
        default:
            break;
        }
    }
}

/* ---- the strip: console buttons, mode, gamepads, Map -------------------------------- */

static HBRUSH accent_brush(void)
{
    static HBRUSH brush;
    if (!brush) brush = CreateSolidBrush(COL_ACCENT);
    return brush;
}

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int accent, int inactive)
{
    RECT rect = *bounds;
    if (accent) {
        FillRect(hdc, &rect, accent_brush());
        FrameRect(hdc, &rect, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(hdc, COL_ON_ACCENT);
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_RESET: return "Reset";
    case SB_POWER: return "Power Cycle";
    case SB_KEYBOARD: return "Keyboard";
    case SB_MODE_JOY: return "Joystick";
    case SB_MODE_PAD: return "Paddles";
    case SB_MODEL_IIE: return "IIe Beige";
    case SB_MODEL_IIC: return "IIc Platinum";
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    default: return "Help";
    }
}

static int strip_target(int btn)
{
    return btn == SB_RESET ? APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_RESET)
         : btn == SB_POWER ? APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_POWER) : -1;
}

/* How many choices a pad row has: Automatic, then the joystick, or paddle 0 and 1. */
static int row_segments(void)
{
    return 1 + apple2session_controller_ports(g_session);
}

static void segment_rect(int row, int seg, RECT *rect)
{
    const RECT *rowrc = &g_gp_row_rc[row];
    const int count = row_segments(), gap = SCL(4), width = (rowrc->right - rowrc->left - (count - 1) * gap) / count;
    const int left = rowrc->left + seg * (width + gap);
    SetRect(rect, left, rowrc->top + SCL(NAME_H), left + width, rowrc->top + SCL(NAME_H) + SCL(SEG_H) - 1);
}

/* One block per gamepad: its name and the port it drives now over the
 * Automatic / Joystick (or Paddle 0 / Paddle 1) choice, the current one in the
 * accent. */
static void paint_gamepads(HDC hdc)
{
    static const char *const joy_segs[2] = { "Automatic", "Joystick" };
    static const char *const pad_segs[3] = { "Automatic", "Paddle 0", "Paddle 1" };
    const int paddles = g_mode == APPLE2_MODE_PADDLES;
    const int num = apple2session_gamepad_count(g_session), count = row_segments();
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (num > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
        text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
        const int assign = apple2session_gamepad_assignment(g_session, idx);
        const int effective = apple2session_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign >= count - 1 ? 0 : assign + 1;
        char name[128], line[40];
        RECT name_rc, eff_rc;

        name_rc = g_gp_row_rc[idx];
        name_rc.bottom = name_rc.top + SCL(NAME_H);
        eff_rc = name_rc;
        name_rc.right -= SCL(130);
        eff_rc.left = name_rc.right;
        apple2session_gamepad_name(g_session, idx, name, sizeof name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        if (effective >= 0 && paddles) snprintf(line, sizeof line, "drives paddle %d", effective);
        else if (effective >= 0) snprintf(line, sizeof line, "drives the joystick");
        else snprintf(line, sizeof line, "drives nothing");
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, line, &eff_rc, DT_RIGHT);
        for (seg = 0; seg < count; seg++) {
            RECT rect;
            segment_rect(idx, seg, &rect);
            draw_face(hdc, &rect, 0, seg == chosen, 0);
            text_line(hdc, paddles ? pad_segs[seg] : joy_segs[seg], &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(int part, char *out, int outsz)
{
    const apple2_control *ctl;
    out[0] = '\0';
    if (part < 0 || part >= g_numlayout) return;
    ctl = &g_layout[part];
    if (ctl->kind == APPLE2_CTL_STICK) {
        snprintf(out, (size_t)outsz, "Stick: drag to tilt it. Map: click a side of the well to rebind that direction.");
        return;
    }
    if (ctl->kind == APPLE2_CTL_KNOB) {
        snprintf(out, (size_t)outsz, "Paddle %d knob: drag or scroll to turn it. Map: left half, right half, lower part.", ctl->port);
        return;
    }
    if (ctl->target >= 0) {
        const uint32_t keysym = bindings_target_keysym(ctl->target);
        const int pad = bindings_target_pad(ctl->target);
        char key[48], padname[48];
        key[0] = '\0';
        padname[0] = '\0';
        if (keysym) apple2session_keysym_name(keysym, key, sizeof key);
        if (pad >= 0) apple2session_pad_input_name(pad, padname, sizeof padname);
        snprintf(out, (size_t)outsz, "%s   Key: %s   Gamepad: %s", bindings_target_label(ctl->target),
                 key[0] ? key : "none", padname[0] ? padname : "none");
    }
}

static void paint_strip(HDC hdc)
{
    int btn;
    char hover[200];
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings) ||
                          (btn == SB_KEYBOARD && g_kb_on) ||
                          (btn == SB_MODE_JOY && g_mode == APPLE2_MODE_JOYSTICK) ||
                          (btn == SB_MODE_PAD && g_mode == APPLE2_MODE_PADDLES) ||
                          (btn == SB_MODEL_IIE && apple2session_controller_model(g_session) == APPLE2_MODEL_IIE) ||
                          (btn == SB_MODEL_IIC && apple2session_controller_model(g_session) == APPLE2_MODEL_IIC) ||
                          (g_map_state >= 0 && strip_target(btn) == g_map_state);
        const int inactive = (btn == SB_CLEAR && g_map_state < 0) ||
                             ((btn == SB_KEYBOARD || (btn >= SB_MODE_JOY && btn <= SB_MODEL_IIC)) && g_map_state != -2);
        draw_face(hdc, &g_btn_rc[btn], btn == g_mouse_action, armed, inactive);
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    hover_text(g_map_state >= 0 ? -1 : g_hover_part, hover, sizeof hover);
    text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the art supersampled, its text, the strips. Only what the
 * update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int scale = 2;
    HDC mem;
    HBITMAP bmp;
    void *bits = NULL;
    BITMAPINFO bmi;
    HGDIOBJ old;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    /* the art: the intersection with the mat, at 2x */
    IntersectRect(&art, inval, &g_case_rc);
    if (!IsRectEmpty(&art)) {
        const int width = art.right - art.left, height = art.bottom - art.top;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = width * scale;
        bmi.bmiHeader.biHeight = -height * scale;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        mem = CreateCompatibleDC(hdc);
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, width * scale, height * scale };
            old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = scale;
            g_art_x = art.left;
            g_art_y = art.top;
            g_art_rows = height * scale;
            art_case(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ----------------------------------------------------------------- */

static void invalidate_part(int part)
{
    RECT rect;
    if (!g_panel || part < 0 || part >= g_numlayout) return;
    part_rect(&g_layout[part], &rect);
    InflateRect(&rect, SCL(14), SCL(14));
    InvalidateRect(g_panel, &rect, FALSE);
}

/* The controller area: any of its parts changed. */
static void invalidate_view(void)
{
    RECT rect;
    if (!g_panel) return;
    SetRect(&rect, g_case_rc.left, g_case_rc.top, g_case_rc.right,
            g_kb_on ? g_kb_rc.top : g_case_rc.bottom);
    InvalidateRect(g_panel, &rect, FALSE);
}

static void invalidate_cap(int idx)
{
    RECT rect;
    if (!g_panel || idx < 0 || idx >= g_ncap) return;
    cap_rect(&g_cap[idx], &rect);
    InflateRect(&rect, SCL(3), SCL(3));
    InvalidateRect(g_panel, &rect, FALSE);
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys). A console button fires on release inside
 * it, like any button. */
static void mouse_release(int fire_action)
{
    const int part = g_mouse_part, action = g_mouse_action, cap = g_mouse_cap;
    const int had_stick = g_mouse_stick, had_knob = g_mouse_knob;
    if (part >= 0 && part < g_numlayout && g_layout[part].target >= 0)
        apple2session_press(g_session, g_layout[part].target, 0);
    g_mouse_part = -1;
    g_mouse_stick = 0;
    g_mouse_knob = -1;
    g_mouse_action = -1;
    g_mouse_cap = -1;
    apple2session_pointer_release(g_session);
    if (cap >= 0) {
        apple2session_keyboard_press(g_session, cap, 0);
        invalidate_cap(cap);
    }
    if (part >= 0) invalidate_part(part);
    if (action >= 0) {
        if (fire_action)
            apple2session_sysaction_post(g_session, action == SB_RESET ? APPLE2SESSION_SYSACT_RESET
                                                                        : APPLE2SESSION_SYSACT_POWER);
        if (g_panel) InvalidateRect(g_panel, &g_btn_rc[action], FALSE);
    }
    if (had_stick || had_knob >= 0) invalidate_view();
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_part = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

static const char *idle_hint(void)
{
    return g_mode == APPLE2_MODE_PADDLES
               ? "Drag a knob or scroll to turn it, press a button to hold it. Map rebinds."
               : "Drag the stick to tilt it, press a button to hold it. Map rebinds.";
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        apple2session_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(idle_hint());
    } else if (state == -1) {
        set_hint("Click a control to remap. Esc finishes.");
    } else {
        char msg[200];
        mouse_release(0);
        if (state >= APPLE2_TARGET_PADDLES && (state - APPLE2_TARGET_PADDLES) % APPLE2_PADDLE_ACTS == APPLE2_PACT_KNOB) {
            snprintf(msg, sizeof msg, "Move a gamepad stick or trigger for %s. Esc cancels.", bindings_target_label(state));
            apple2session_gamepad_capture_begin_axis(g_session);
        } else {
            snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", bindings_target_label(state));
            apple2session_gamepad_capture_begin(g_session);
        }
        set_hint(msg);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, int stolen)
{
    char msg[260];
    if (stolen >= 0) snprintf(msg, sizeof msg, "Bound %s (it was %s's)", what, bindings_target_label(stolen));
    else snprintf(msg, sizeof msg, "Bound %s", what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

/* The mat with the controllers in it, the machine's buttons, the choices, the
 * gamepad rows, the Map row: the window is this size for the current mode and
 * keyboard. */
static void layout_static(void)
{
    const int left = SCL(MARGIN);
    int ypos = SCL(MARGIN), idx, xpos, content_w, third, quarter, gap;
    static const int widths[SB_COUNT] = { 0, 0, 0, 0, 0, 0, 0, MAP_W, CLEAR_W, DEFAULTS_W, SHOW_W, HELP_W };
    float padw = 0, paddh = 0;

    g_mode = apple2session_controller_mode(g_session);
    g_layout = apple2session_controls_layout(cur_view(), &g_numlayout, &g_layw, &g_layh);
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    g_key_unit = (double)KEY_UNIT * g_dpi / 96.0;
    g_cap = apple2session_keyboard_layout(&g_ncap, &g_cap_wide, &g_cap_high);
    /* the width is the wider of the paddles and the keyboard, always, so only the height
     * changes with the mode and the Keyboard button */
    apple2session_controls_layout(APPLE2_VIEW_PADDLES, NULL, &padw, &paddh);
    content_w = (int)ceil((double)padw * g_unit) + 2 * SCL(CASE_PAD);
    {
        const int kb_w = (int)ceil((double)g_cap_wide * g_key_unit) + 2 * SCL(KB_PAD + 8);
        if (kb_w > content_w) content_w = kb_w;
    }
    g_total_w = 2 * left + content_w;

    SetRect(&g_case_rc, left, ypos, left + content_w, 0);
    SetRect(&g_brand_rc, g_case_rc.left + SCL(12), g_case_rc.top + SCL(8), g_case_rc.right - SCL(12),
            g_case_rc.top + SCL(8) + SCL(BRAND_H));
    g_orgx = g_case_rc.left + (content_w - (double)g_layw * g_unit) / 2;
    g_orgy = g_brand_rc.bottom + SCL(12);
    g_case_rc.bottom = (int)ceil(g_orgy + (double)g_layh * g_unit) + SCL(CASE_PAD);
    SetRectEmpty(&g_kb_rc);
    if (g_kb_on) {
        g_key_x = g_case_rc.left + (content_w - (double)g_cap_wide * g_key_unit) / 2;
        g_key_y = g_case_rc.bottom - SCL(CASE_PAD) + SCL(KB_PAD + 4);
        SetRect(&g_kb_rc, (int)g_key_x - SCL(KB_PAD), (int)g_key_y - SCL(KB_PAD),
                (int)ceil(g_key_x + g_cap_wide * g_key_unit) + SCL(KB_PAD),
                (int)ceil(g_key_y + g_cap_high * g_key_unit) + SCL(KB_PAD));
        g_case_rc.bottom = g_kb_rc.bottom + SCL(CASE_PAD - 4);
    }
    ypos = g_case_rc.bottom + SCL(10);

    /* Reset, Power Cycle, Keyboard share the width */
    gap = SCL(8);
    third = (content_w - 2 * gap) / 3;
    SetRect(&g_btn_rc[SB_RESET], left, ypos, left + third, ypos + SCL(BTN_H));
    SetRect(&g_btn_rc[SB_POWER], left + third + gap, ypos, left + 2 * third + gap, ypos + SCL(BTN_H));
    SetRect(&g_btn_rc[SB_KEYBOARD], left + content_w - third, ypos, left + content_w, ypos + SCL(BTN_H));
    ypos += SCL(BTN_H) + SCL(6);

    /* the controller and the colour: two pairs of segments */
    quarter = (content_w - 2 * gap - 2 * SCL(2)) / 4;
    SetRect(&g_btn_rc[SB_MODE_JOY], left, ypos, left + quarter, ypos + SCL(BTN_H));
    SetRect(&g_btn_rc[SB_MODE_PAD], left + quarter + SCL(2), ypos, left + 2 * quarter + SCL(2), ypos + SCL(BTN_H));
    SetRect(&g_btn_rc[SB_MODEL_IIE], left + content_w - 2 * quarter - SCL(2), ypos, left + content_w - quarter - SCL(2),
            ypos + SCL(BTN_H));
    SetRect(&g_btn_rc[SB_MODEL_IIC], left + content_w - quarter, ypos, left + content_w, ypos + SCL(BTN_H));
    ypos += SCL(BTN_H) + SCL(12);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + SCL(HEAD_H));
    ypos += SCL(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + SCL(NAME_H + SEG_H));
        ypos += SCL(NAME_H + SEG_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += SCL(4);

    xpos = left;
    for (idx = SB_MAP; idx < SB_COUNT; idx++) {
        if (idx == SB_HELP) xpos = left + content_w - SCL(HELP_W);
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + SCL(widths[idx]), ypos + SCL(BTN_H));
        xpos += SCL(widths[idx]) + SCL(6);
    }
    ypos += SCL(BTN_H) + SCL(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + SCL(20));
    g_total_h = ypos + SCL(20) + SCL(MARGIN);
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

static void rebuild(void)
{
    mouse_release(0);
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    make_fonts();
    layout_static();
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- polling -------------------------------------------------------------------------- */

/* What the machine sees held, lit. */
static void refresh_held(void)
{
    held_state now;
    int idx, bit;
    static const int targets[4] = { APPLE2_TARGET_JOY(APPLE2_ACT_BTN0), APPLE2_TARGET_JOY(APPLE2_ACT_BTN1),
                                    APPLE2_TARGET_PADDLE(0, APPLE2_PACT_BUTTON),
                                    APPLE2_TARGET_PADDLE(1, APPLE2_PACT_BUTTON) };
    if (!g_panel || g_map_state != -2) return;
    memset(&now, 0, sizeof now);
    apple2session_axes_get(g_session, &now.axes[0], &now.axes[1]);
    for (bit = 0; bit < 4; bit++)
        if (apple2session_target_held(g_session, targets[bit])) now.held |= 1u << bit;
    if (memcmp(&now, &g_shown, sizeof now) != 0) {
        g_shown = now;
        invalidate_view();
    }
    if (g_kb_on)
        for (idx = 0; idx < g_ncap && idx < MAX_CAPS; idx++) {
            const unsigned char cur = (unsigned char)apple2session_keyboard_held(g_session, idx);
            if (cur != g_shown_caps[idx]) {
                g_shown_caps[idx] = cur;
                invalidate_cap(idx);
            }
        }
}

/* Hot-plugged pads, auto-assignment changes, a mode changed elsewhere. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    memset(&now, 0, sizeof now);
    now.generation = apple2session_gamepad_generation(g_session);
    now.mode = apple2session_controller_mode(g_session);
    now.count = apple2session_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = apple2session_gamepad_assignment(g_session, idx);
        now.effective[idx] = apple2session_gamepad_effective_port(g_session, idx);
    }
    if (now.mode != g_mode) {   /* changed in the Input settings: the window is another size */
        rebuild();
        fit_client();
        set_hint(idle_hint());
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
}

/* ---- keys ------------------------------------------------------------------------------ */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release(0);
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vkey = 0;
    apple2session_keyboard_release(g_session);
    apple2session_pointer_release(g_session);
    memset(g_shown_caps, 0, sizeof g_shown_caps);
    ShowWindow(g_panel, SW_HIDE);
}

/* The app's own keys, never the machine's or Map's. */
static int app_key(WPARAM vkey)
{
    return vkey == VK_F9 || vkey == VK_F11 || vkey == VK_F12;
}

/* Alt and the Windows keys are the Open and Closed Apple keys: the machine's. */
static int apple_key(uint32_t keysym)
{
    return keysym == APPLE2SESSION_KEYSYM_LALT || keysym == APPLE2SESSION_KEYSYM_RALT ||
           keysym == APPLE2SESSION_KEYSYM_LMETA || keysym == APPLE2SESSION_KEYSYM_RMETA ||
           keysym == APPLE2SESSION_KEYSYM_LSUPER || keysym == APPLE2SESSION_KEYSYM_RSUPER;
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9 or F12,
 * which are the app's, nor the Apple keys). */
int apple2_controllers_map_key(UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (!g_panel) return 0;
    if (g_swallow_vkey && wparam == g_swallow_vkey) {
        if (!down) g_swallow_vkey = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    /* Map is the panel's: with the game window active its keys (releases
     * included) are the machine's, not Map's. */
    if (GetActiveWindow() != g_panel) return 0;
    if (app_key(wparam) || wparam == VK_F10) return 0;
    if (msg == WM_SYSKEYDOWN && wparam == VK_F4) return 0;
    if (!down || (lparam & (1 << 30))) return 1;
    if (wparam == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vkey = wparam;
        return 1;
    }
    if (g_map_state >= 0) {
        const int target = g_map_state;
        const uint32_t keysym = apple2_win_keysym(wparam, lparam);
        if (!keysym) return 1;          /* nothing to call it: keep waiting */
        if (apple_key(keysym)) {
            set_hint("Alt and the Windows keys are the Open and Closed Apple keys and cannot be bound.");
            g_swallow_vkey = wparam;
            return 1;
        }
        if (target >= APPLE2_TARGET_PADDLES && (target - APPLE2_TARGET_PADDLES) % APPLE2_PADDLE_ACTS == APPLE2_PACT_KNOB) {
            set_hint("The knob itself takes a gamepad axis: move a stick or trigger. Esc cancels.");
            g_swallow_vkey = wparam;
            return 1;
        }
        {
            int stolen = -1;
            char name[48];
            bindings_set_keysym(g_session, target, keysym, &stolen);
            apple2session_keysym_name(keysym, name, sizeof name);
            g_swallow_vkey = wparam;
            set_map_state(-1);          /* stay armed: remapping several in a row is normal */
            bound_hint(name, stolen);
        }
    }
    return 1;
}

/* A key with the window focused: 1 if taken. The same path as the main
 * window's (keys.c), except in Map mode. */
static int panel_key(UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (apple2_controllers_map_key(msg, wparam, lparam)) return 1;
    if (wparam == VK_F9) {
        if (down && !(lparam & (1 << 30))) hide_panel();
        return 1;
    }
    if (app_key(wparam)) {
        /* the main window's shortcuts, as if it had the focus */
        if (g_parent && down && !(lparam & (1 << 30))) SendMessageA(g_parent, WM_KEYDOWN, wparam, lparam);
        return 1;
    }
    if (wparam == VK_F10) return 0;
    return apple2_win_key_msg(g_session, msg, wparam, lparam);
}

/* ---- the mouse ------------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_layout) return -1;
    return apple2session_controls_hit(cur_view(), (float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
}

/* A stick drag: the cap follows the pointer from where it rests, proportional. */
static void stick_follow(POINT pnt)
{
    float xval = 0, yval = 0;
    apple2session_joystick_from_point((float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit), &xval, &yval);
    apple2session_pointer_set(g_session, xval, yval);
    refresh_held();
}

/* A dial drag: the knob turns to the angle of the pointer. */
static void knob_follow(const apple2_control *ctl, POINT pnt)
{
    float value = 0;
    const double ctrx = g_orgx + (double)ctl->cx * g_unit, ctry = g_orgy + (double)ctl->cy * g_unit;
    if (apple2session_knob_from_point(pnt.x - ctrx, pnt.y - ctry, (double)ctl->reach * g_unit, &value))
        apple2session_knob_set(g_session, ctl->port, value);
    refresh_held();
}

/* The binding target Map picks for a click on a control. */
static int map_target_for(const apple2_control *ctl, POINT pnt)
{
    double xpos, ypos, width, height;
    switch (ctl->kind) {
    case APPLE2_CTL_BUTTON:
    case APPLE2_CTL_SYSBUTTON:
        return ctl->target;
    case APPLE2_CTL_STICK: {
        /* which side of the well: the larger of the two offsets from its middle */
        double delx, dely;
        part_box(ctl, &xpos, &ypos, &width, &height);
        delx = (pnt.x - (xpos + width / 2)) / width;
        dely = (pnt.y - (ypos + height / 2)) / height;
        if (fabs(delx) > fabs(dely)) return APPLE2_TARGET_JOY(delx < 0 ? APPLE2_ACT_LEFT : APPLE2_ACT_RIGHT);
        return APPLE2_TARGET_JOY(dely < 0 ? APPLE2_ACT_UP : APPLE2_ACT_DOWN);
    }
    case APPLE2_CTL_KNOB: {
        const double rad = (double)ctl->reach * g_unit;
        const double delx = pnt.x - (g_orgx + (double)ctl->cx * g_unit), dely = pnt.y - (g_orgy + (double)ctl->cy * g_unit);
        if (dely > rad * 0.55) return APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_KNOB);
        return APPLE2_TARGET_PADDLE(ctl->port, delx < 0 ? APPLE2_PACT_CCW : APPLE2_PACT_CW);
    }
    default:
        return -1;
    }
}

static void set_mode(int mode)
{
    if (mode == g_mode) return;
    mouse_release(0);
    if (GetCapture() == g_panel) ReleaseCapture();
    apple2session_keyboard_release(g_session);
    apple2session_pointer_release(g_session);
    apple2session_set_controller_mode(g_session, mode);
    memset(g_shown_caps, 0, sizeof g_shown_caps);
    memset(&g_shown, 0, sizeof g_shown);
    rebuild();
    fit_client();
    set_hint(idle_hint());
    refresh_pads();
    InvalidateRect(g_panel, NULL, FALSE);
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn;
    SetFocus(hwnd);
    if (g_hover_part >= 0) {
        g_hover_part = -1;
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++) {
        if (!PtInRect(&g_btn_rc[btn], pnt)) continue;
        switch (btn) {
        case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); return;
        case SB_CLEAR:
            if (g_map_state >= 0) {
                bindings_clear(g_session, g_map_state);
                set_hint("Cleared. Press a key or a gamepad button to bind it again");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return;
        case SB_DEFAULTS:
            bindings_reset_defaults(g_session);
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_SHOW:
            g_show_bindings = !g_show_bindings;
            apple2session_set_int(g_session, "controllers_show_bindings", g_show_bindings);
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_HELP: apple2_help_show(hwnd, "controllers"); return;
        case SB_KEYBOARD:
            if (g_map_state != -2) return;
            mouse_release(0);
            apple2session_keyboard_release(g_session);
            g_kb_on = !g_kb_on;
            apple2session_set_int(g_session, "controllers_keyboard", g_kb_on);
            memset(g_shown_caps, 0, sizeof g_shown_caps);
            layout_static();
            fit_client();
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_MODE_JOY:
        case SB_MODE_PAD:
            if (g_map_state == -2) set_mode(btn == SB_MODE_PAD ? APPLE2_MODE_PADDLES : APPLE2_MODE_JOYSTICK);
            return;
        case SB_MODEL_IIE:
        case SB_MODEL_IIC:
            if (g_map_state != -2) return;
            apple2session_set_controller_model(g_session, btn == SB_MODEL_IIC ? APPLE2_MODEL_IIC : APPLE2_MODEL_IIE);
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        default:    /* Reset, Power Cycle */
            if (g_map_state != -2) {                 /* armed: picks the action */
                set_map_state(strip_target(btn));
            } else {
                mouse_release(0);
                g_mouse_action = btn;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g_btn_rc[btn], FALSE);
            }
            return;
        }
    }
    for (row = 0; row < PAD_ROWS && row < apple2session_gamepad_count(g_session); row++)
        for (seg = 0; seg < row_segments(); seg++) {
            RECT rect;
            segment_rect(row, seg, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), 1 = the joystick or paddle 0, 2 = paddle 1 */
                apple2session_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    if (g_map_state == -2) {
        const int cap = cap_at(pnt);
        if (cap >= 0) {
            mouse_release(0);
            g_mouse_cap = cap;
            apple2session_keyboard_press(g_session, cap, 1);
            invalidate_cap(cap);
            SetCapture(hwnd);
            refresh_held();
            return;
        }
    }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const apple2_control *ctl = &g_layout[part];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            const int target = map_target_for(ctl, pnt);
            if (target >= 0) set_map_state(target);
            return;
        }
        mouse_release(0);
        switch (ctl->kind) {
        case APPLE2_CTL_STICK:
            g_mouse_stick = 1;
            stick_follow(pnt);
            break;
        case APPLE2_CTL_KNOB:
            g_mouse_knob = ctl->port;
            knob_follow(ctl, pnt);
            break;
        case APPLE2_CTL_BUTTON:
            g_mouse_part = part;
            apple2session_press(g_session, ctl->target, 1);
            break;
        case APPLE2_CTL_SYSBUTTON:
            return;
        default:
            return;
        }
        invalidate_part(part);
        SetCapture(hwnd);
        refresh_held();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_mouse_stick) {
        stick_follow(pnt);
        return;
    }
    if (g_mouse_knob >= 0) {
        int idx;
        for (idx = 0; idx < g_numlayout; idx++)
            if (g_layout[idx].kind == APPLE2_CTL_KNOB && g_layout[idx].port == g_mouse_knob) knob_follow(&g_layout[idx], pnt);
        return;
    }
    if (g_mouse_cap >= 0) {
        if (cap_at(pnt) != g_mouse_cap) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    if (g_mouse_part >= 0) {
        /* dragged off what it holds: let go, as a button does */
        if (g_mouse_part >= g_numlayout || part_at(pnt) != g_mouse_part) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    if (g_mouse_action >= 0) {
        if (!PtInRect(&g_btn_rc[g_mouse_action], pnt)) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        const int part = part_at(pnt);
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        if (part != g_hover_part) {
            g_hover_part = part;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd)
{
    if (g_mouse_part >= 0 || g_mouse_stick || g_mouse_knob >= 0 || g_mouse_action >= 0 || g_mouse_cap >= 0) {
        mouse_release(1);
        refresh_held();
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

/* The wheel over a dial nudges the knob. */
static void mouse_wheel(HWND hwnd, int delta, POINT screen)
{
    POINT pnt = screen;
    int part;
    ScreenToClient(hwnd, &pnt);
    if (g_map_state != -2 || g_mode != APPLE2_MODE_PADDLES) return;
    part = part_at(pnt);
    if (part >= 0 && g_layout[part].kind == APPLE2_CTL_KNOB) {
        apple2session_knob_nudge(g_session, g_layout[part].port, (float)delta * 0.08f / WHEEL_DELTA);
        refresh_held();
    }
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width <= 0 || height <= 0) { EndPaint(hwnd, &paint); return 0; }
        mem = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, width, height);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
        paint_rect(mem, &inval);
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_wheel(hwnd, GET_WHEEL_DELTA_WPARAM(wparam), pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_part >= 0) {
            g_hover_part = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd) {
            mouse_release(0);
            refresh_held();
        }
        return 0;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            int input;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (apple2session_gamepad_capture_poll(g_session, &input)) {
                int stolen = -1;
                char name[64];
                const int target = g_map_state;
                bindings_set_pad(g_session, target, input, &stolen);
                apple2session_pad_input_name(input, name, sizeof name);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(name, stolen);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: Alt is the machine's Open Apple */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_CHAR:
        return 0;           /* typing arrives as keys (keys.c), not characters */
    case WM_SYSCHAR:
        return 0;           /* Alt+letter is Open Apple + the letter, not a menu mnemonic */

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vkey = 0;     /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId()) {
                apple2session_release_all(g_session);
                apple2session_key(g_session, 0, 0xFFE9, 0, 0, 0);
                apple2session_key(g_session, 0, 0xFFEA, 0, 0, 0);
            }
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release(0);
        set_map_state(-2);
        g_panel = NULL;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    g_show_bindings = apple2session_get_int(g_session, "controllers_show_bindings", 0) != 0;
    g_kb_on = apple2session_get_int(g_session, "controllers_keyboard", 0) != 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = apple2_window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    apple2_help_set_context(g_panel, "controllers", 0);    /* F1 is bindable here */
    rescale(apple2_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    memset(&g_shown, 0, sizeof g_shown);
    memset(g_shown_caps, 0, sizeof g_shown_caps);
    g_hover_part = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void apple2_controllers_window_show(HWND parent, apple2session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (!IsWindowVisible(g_panel))
        show_panel();
    else
        SetForegroundWindow(g_panel);
}

void apple2_controllers_window_toggle(HWND parent, apple2session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

int apple2_controllers_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void apple2_controllers_window_refresh(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}

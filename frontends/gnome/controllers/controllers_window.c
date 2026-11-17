/*
 * The Controllers window -- the Apple II's game port drawn as a player sees
 * it. In Joystick mode the Apple Joystick IIe or IIc: a square case with two
 * square orange buttons, a raised block with a pyramid well and the stick
 * tilting in it, in the beige of the IIe's own joystick or the IIc's platinum.
 * In Paddles mode a pair of Apple Hand Controllers: a ribbed wedge each, a
 * ribbed knob and an orange button. Under them the console's two buttons
 * (Reset, Power Cycle) and, when asked for, the Apple IIe keyboard. Everything
 * is drawn from the session's layout (apple2session_controls_layout and
 * apple2session_keyboard_layout), so every frontend shows the same controls.
 *
 * Mouse: a press on a button holds it; a press in the stick's well tilts the
 * stick toward the pointer, proportionally (the Apple joystick is analog), and
 * a drag follows; a press on a knob turns it to the pointer's angle and a drag
 * follows (the scroll wheel nudges it); a click on a key of the keyboard types
 * it (Shift and Control latch, Caps Lock toggles, the Apple keys are held).
 * Every press is the raw press and release (a GtkGestureDrag, not a click: a
 * click gives up when the pointer wobbles), because the machine samples its
 * game port once per frame. Losing the focus, hiding and the pointer leaving a
 * held button let go of whatever the mouse holds here.
 *
 * What the machine sees held -- keyboard, gamepads, the Alt keys and this
 * window together (apple2session_target_held, apple2session_axes_get) -- is
 * polled every frame and painted in the accent, so the window doubles as a
 * gamepad tester: the stick cap follows the machine's PDL0/PDL1.
 *
 * Keys typed with the window active go through the main window
 * (apple2_window_key), so the machine, F9 and the shortcuts behave the same
 * whichever window has the focus, and the Alt keys still reach the machine as
 * Open Apple and Closed Apple.
 *
 * FIXED SIZE, FLOATING: the drawing is one size per keyboard setting, the
 * labels ellipsize rather than grow, and the window is not resizable, so
 * neither Map mode, the Joystick/Paddles switch nor a long gamepad name changes
 * its size and tiling compositors float it. It is hidden at startup (F9 or the
 * menu shows it). SINGLETON, hidden rather than destroyed, so a remap survives
 * closing it. Its timers run only while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a control to pick
 * the target (the four walls of the well are the stick's four directions),
 * then press a keyboard key OR a gamepad input (a knob takes an axis). While
 * armed the same gestures pick the target instead of pressing, every control
 * shows the input bound to it, and typed keys are not typed into the machine;
 * once a target is picked the other controls are greyed. Rebinding steals the
 * input from whatever held it. Escape cancels. Clear takes the picked target's
 * key and input away, Defaults puts every binding back.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../help/help_window.h"
#include "../window.h"
#include "bindings.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define UNIT 34.0               /* pixels per layout unit (the photo's scale) */
#define MARGIN 14.0             /* the desk round the controls, pixels */
#define STAGE_H 8.5             /* the stage: the joystick's height, units */
#define GAP 0.55                /* between the stage, the strip and the keyboard */
#define STRIP_H 0.9             /* the console strip, units (core layout) */
#define MAX_CTL 16
#define MAX_CAPS 80
#define KNOB_NUDGE 0.08f        /* one scroll step turns a knob by this much */
#define MAX_PADS 8              /* pads listed (SDL opens few more) */
#define PAD_ROWS 2              /* pad rows the window reserves; more scroll */
#define PAD_ROW_H 34            /* one row's height, px */
#define PAD_GAP 4
#define U(v) ((v) * UNIT)

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the Apple2Window: typed keys go through it */
static apple2session *g_session;
static GtkWidget *g_area;       /* the controls' drawing area */

static const apple2_control *g_lay[APPLE2_VIEW_COUNT];  /* apple2session_controls_layout */
static int g_cnt[APPLE2_VIEW_COUNT];
static float g_vw[APPLE2_VIEW_COUNT], g_vh[APPLE2_VIEW_COUNT];
static const apple2_keycap *g_kb;
static int g_kbn;
static float g_kbw, g_kbh;

static int g_mode;              /* APPLE2_MODE_* as last drawn */
static int g_model;             /* APPLE2_MODEL_* as last drawn */
static gboolean g_kb_on;        /* the keyboard view shown */
static float g_axx, g_axy;      /* what the machine's PDL0/PDL1 are driven with */
static float g_knob[APPLE2SESSION_PADDLES];
static gboolean g_lit[APPLE2_VIEW_COUNT][MAX_CTL];
static gboolean g_turn_lit[APPLE2SESSION_PADDLES * 2];
static int g_cap_held[MAX_CAPS];
static gboolean g_syncing;      /* a refresh, not the user, is moving a widget */

/* What the pointer is over / holds. */
enum { S_NONE = 0, S_BTN, S_STICK, S_KNOB, S_TURN, S_SYS, S_KEY };
typedef struct {
    int sect;                   /* S_* */
    int idx;                    /* layout index; S_STICK: the wall 0..3 or -1; S_TURN: n*2+cw; S_KEY: cap */
    int n;                      /* a paddle's number */
    int target;                 /* what Map binds, or -1 */
} Hit;
static Hit g_hover = { S_NONE, -1, -1, -1 };
static Hit g_press = { S_NONE, -1, -1, -1 };
static double g_ptr_x, g_ptr_y; /* the pointer, for the scroll wheel */

/* What the mouse is dragging: the stick or a knob's dial. */
static struct {
    int kind;                   /* 0 none, 1 stick, 2 knob */
    int n;
    double cenx, ceny, rad;     /* the dial, pixels */
} g_drag;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad input to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button, *g_map_hint, *g_show_bindings;
static GtkWidget *g_mode_btn[2], *g_model_btn[2], *g_kb_check;
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_eff[MAX_PADS];  /* each pad's "plays ..." */
static int g_pad_row_count;
static unsigned g_pad_generation = (unsigned)-1;
static int g_pad_rows_mode = -1;        /* the mode the pad rows were built for */
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_info_timer;

static void refresh_all(void);

/* "controllers_show_bindings"; the older spellings are read when it is absent. */
static gboolean show_bindings_saved(void)
{
    static const char *const older[] = { "controllers_bindings", "controllers_key_names",
                                         "show_bindings" };
    int val = apple2session_get_int(g_session, "controllers_show_bindings", -1);
    size_t idx;
    for (idx = 0; val < 0 && idx < G_N_ELEMENTS(older); idx++)
        val = apple2session_get_int(g_session, older[idx], -1);
    return val > 0;
}

/* ---- small helpers ---------------------------------------------------------- */

static void rgb(cairo_t *cctx, guint32 col)
{
    cairo_set_source_rgb(cctx, ((col >> 16) & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0,
                         (col & 0xFF) / 255.0);
}

static guint32 mix(guint32 from, guint32 dest, double frac)
{
    guint32 out = 0;
    int shift;
    for (shift = 16; shift >= 0; shift -= 8) {
        double chan_from = (from >> shift) & 0xFF, chan_dest = (dest >> shift) & 0xFF;
        out |= (guint32)(chan_from + (chan_dest - chan_from) * frac + 0.5) << shift;
    }
    return out;
}

static void rrect(cairo_t *cctx, double xpos, double ypos, double wid, double hgt, double rad)
{
    if (rad > wid / 2) rad = wid / 2;
    if (rad > hgt / 2) rad = hgt / 2;
    cairo_new_sub_path(cctx);
    cairo_arc(cctx, xpos + wid - rad, ypos + rad, rad, -G_PI / 2, 0);
    cairo_arc(cctx, xpos + wid - rad, ypos + hgt - rad, rad, 0, G_PI / 2);
    cairo_arc(cctx, xpos + rad, ypos + hgt - rad, rad, G_PI / 2, G_PI);
    cairo_arc(cctx, xpos + rad, ypos + rad, rad, G_PI, 3 * G_PI / 2);
    cairo_close_path(cctx);
}

static void stop_rgb(cairo_pattern_t *pat, double offset, guint32 col)
{
    cairo_pattern_add_color_stop_rgb(pat, offset, ((col >> 16) & 0xFF) / 255.0,
                                     ((col >> 8) & 0xFF) / 255.0, (col & 0xFF) / 255.0);
}

/* Text centred in a box, in an absolute pixel size, ellipsized to the box. */
static void legend(cairo_t *cctx, GtkWidget *widget, const char *text, double pxsize,
                   gboolean bold, guint32 colour, double xpos, double ypos, double boxw,
                   double boxh)
{
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, text);
    PangoFontDescription *fdesc = pango_font_description_new();
    int textw, texth;

    pango_font_description_set_family(fdesc, "Sans");
    pango_font_description_set_weight(fdesc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(fdesc, pxsize * PANGO_SCALE);
    pango_layout_set_font_description(lay, fdesc);
    pango_layout_set_width(lay, (int)(boxw * PANGO_SCALE));
    pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(lay, PANGO_ALIGN_CENTER);
    pango_layout_get_pixel_size(lay, &textw, &texth);
    rgb(cctx, colour);
    cairo_move_to(cctx, xpos, ypos + (boxh - texth) / 2.0);
    pango_cairo_show_layout(cctx, lay);
    /* the move_to above is still the current point, and the next arc would
     * join it with a line */
    cairo_new_path(cctx);
    pango_font_description_free(fdesc);
    g_object_unref(lay);
}

/* A host key's name as a small legend: the long ones shortened to fit. */
static void legend_name(guint32 keysym, char *dst, int dstsz)
{
    char name[32];
    apple2session_keysym_name(keysym, name, sizeof name);
    if (!strcmp(name, "Keypad Enter"))
        g_snprintf(dst, dstsz, "KP Ent");
    else if (g_str_has_prefix(name, "Keypad "))
        g_snprintf(dst, dstsz, "KP %s", name + 7);
    else if (g_str_has_prefix(name, "Left "))
        g_snprintf(dst, dstsz, "L %s", name + 5);
    else if (g_str_has_prefix(name, "Right "))
        g_snprintf(dst, dstsz, "R %s", name + 6);
    else if (!strcmp(name, "Backspace"))
        g_snprintf(dst, dstsz, "BkSp");
    else if (!strcmp(name, "Escape"))
        g_snprintf(dst, dstsz, "Esc");
    else
        g_snprintf(dst, dstsz, "%s", name);
}

static gboolean is_knob_target(int target)
{
    return target >= APPLE2_TARGET_PADDLES &&
           (target - APPLE2_TARGET_PADDLES) % APPLE2_PADDLE_ACTS == APPLE2_PACT_KNOB;
}

static gboolean is_sysact(int target)
{
    return target >= APPLE2_TARGET_SYSACT_BASE && target < APPLE2_TARGET_PADDLES;
}

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings));
}

/* The binding legend for a target, or "" when none or not shown. A knob's is
 * the pad axis that turns it, every other target's the key. */
static void bound_text(int target, char *dst, int dstsz)
{
    dst[0] = '\0';
    if (target < 0 || !showing_bindings()) return;
    if (is_knob_target(target)) {
        const int pad = bindings_target_pad(target);
        if (pad >= 0) {
            /* "Right Stick X" does not fit a knob's face: "RS X" */
            char name[48];
            apple2session_pad_input_name(pad, name, sizeof name);
            if (g_str_has_prefix(name, "Left Stick "))
                g_snprintf(dst, dstsz, "LS %s", name + 11);
            else if (g_str_has_prefix(name, "Right Stick "))
                g_snprintf(dst, dstsz, "RS %s", name + 12);
            else if (!strcmp(name, "Left Trigger"))
                g_snprintf(dst, dstsz, "LT");
            else if (!strcmp(name, "Right Trigger"))
                g_snprintf(dst, dstsz, "RT");
            else
                g_snprintf(dst, dstsz, "%s", name);
        } else
            g_snprintf(dst, dstsz, "\xe2\x80\x94");
        return;
    }
    if (bindings_target_keysym(target))
        legend_name(bindings_target_keysym(target), dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

/* ---- geometry ---------------------------------------------------------------- */

/* The drawing is as wide as the widest thing on it: the paddles (the keyboard
 * when it is shown). The stage holds the joystick or the paddles, centred. */
static double content_units(void)
{
    return g_kb_on ? MAX((double)g_kbw, (double)g_vw[APPLE2_VIEW_PADDLES])
                   : (double)g_vw[APPLE2_VIEW_PADDLES];
}

static double stage_y(void) { return MARGIN; }
static double strip_y(void) { return MARGIN + (STAGE_H + GAP) * UNIT; }
static double kb_y(void) { return strip_y() + (STRIP_H + GAP) * UNIT; }

static int area_width(void)
{
    return (int)(2 * MARGIN + content_units() * UNIT + 0.5);
}

static int area_height(void)
{
    const double bottom = g_kb_on ? kb_y() + g_kbh * UNIT : strip_y() + STRIP_H * UNIT;
    return (int)(bottom + MARGIN + 0.5);
}

/* The pixel origin of a view of the layout. */
static void view_origin(int view, double *oxp, double *oyp)
{
    *oxp = MARGIN + (content_units() - g_vw[view]) * UNIT / 2;
    if (view == APPLE2_VIEW_CONSOLE)
        *oyp = strip_y();
    else
        *oyp = stage_y() + (STAGE_H - g_vh[view]) * UNIT / 2;
}

static void kb_origin(double *oxp, double *oyp)
{
    *oxp = MARGIN + (content_units() - g_kbw) * UNIT / 2;
    *oyp = kb_y();
}

/* The pyramid well: its outer square and its floor, from the core's layout
 * (controls.c). The four walls are the stick's four directions, in the order
 * of apple2_joy_act: left, right, up, down. */
static const float k_walls[4][4][2] = {
    { { 2.6f, 2.7f }, { 2.6f, 7.7f }, { 4.5f, 6.6f }, { 4.5f, 5.6f } },   /* left */
    { { 7.5f, 2.7f }, { 7.5f, 7.7f }, { 5.5f, 6.6f }, { 5.5f, 5.6f } },   /* right */
    { { 2.6f, 2.7f }, { 7.5f, 2.7f }, { 5.5f, 5.6f }, { 4.5f, 5.6f } },   /* up */
    { { 2.6f, 7.7f }, { 7.5f, 7.7f }, { 5.5f, 6.6f }, { 4.5f, 6.6f } },   /* down */
};

static gboolean in_poly(const float poly[4][2], double xpos, double ypos)
{
    gboolean inside = FALSE;
    int cur, prev;
    for (cur = 0, prev = 3; cur < 4; prev = cur++) {
        if (((poly[cur][1] > ypos) != (poly[prev][1] > ypos)) &&
            (xpos < (poly[prev][0] - poly[cur][0]) * (ypos - poly[cur][1]) /
                            (poly[prev][1] - poly[cur][1]) + poly[cur][0]))
            inside = !inside;
    }
    return inside;
}

static int wall_at(double unitx, double unity)
{
    int wall;
    for (wall = 0; wall < 4; wall++)
        if (in_poly(k_walls[wall], unitx, unity))
            return wall;
    return -1;
}

/* The two small buttons under each paddle that turn the knob while held (the
 * CCW and CW targets; the core's layout has no part for them). Pixels. */
static void turn_rect(int slot, double *xpos, double *ypos, double *wid, double *hgt)
{
    double oxp, oyp;
    const int pad = slot / 2, cw = slot % 2;
    view_origin(APPLE2_VIEW_PADDLES, &oxp, &oyp);
    *xpos = oxp + U(pad * 6.8f + (cw ? 4.55 : 0.95));
    *ypos = oyp + U(3.05);
    *wid = U(0.5);
    *hgt = U(0.4);
}

static int turn_target(int slot)
{
    return APPLE2_TARGET_PADDLE(slot / 2, slot % 2 ? APPLE2_PACT_CW : APPLE2_PACT_CCW);
}

static Hit hit_at(double xpos, double ypos)
{
    Hit hit = { S_NONE, -1, -1, -1 };
    double oxp, oyp;
    int idx;

    if (g_kb_on) {
        kb_origin(&oxp, &oyp);
        for (idx = g_kbn - 1; idx >= 0; idx--) {
            const apple2_keycap *cap = &g_kb[idx];
            if (xpos >= oxp + U(cap->xpos) && xpos < oxp + U(cap->xpos + cap->width) &&
                ypos >= oyp + U(cap->ypos) && ypos < oyp + U(cap->ypos + cap->height)) {
                hit.sect = S_KEY;
                hit.idx = idx;
                return hit;
            }
        }
    }
    view_origin(APPLE2_VIEW_CONSOLE, &oxp, &oyp);
    idx = apple2session_controls_hit(APPLE2_VIEW_CONSOLE, (float)((xpos - oxp) / UNIT),
                                     (float)((ypos - oyp) / UNIT));
    if (idx >= 0) {
        hit.sect = S_SYS;
        hit.idx = idx;
        hit.target = g_lay[APPLE2_VIEW_CONSOLE][idx].target;
        return hit;
    }
    if (g_mode == APPLE2_MODE_JOYSTICK) {
        float unitx, unity;
        view_origin(APPLE2_VIEW_JOYSTICK, &oxp, &oyp);
        unitx = (float)((xpos - oxp) / UNIT);
        unity = (float)((ypos - oyp) / UNIT);
        idx = apple2session_controls_hit(APPLE2_VIEW_JOYSTICK, unitx, unity);
        if (idx < 0)
            return hit;
        if (g_lay[APPLE2_VIEW_JOYSTICK][idx].kind == APPLE2_CTL_BUTTON) {
            hit.sect = S_BTN;
            hit.idx = idx;
            hit.target = g_lay[APPLE2_VIEW_JOYSTICK][idx].target;
        } else {
            const int wall = wall_at(unitx, unity);
            hit.sect = S_STICK;
            hit.idx = wall;
            hit.target = wall >= 0 ? APPLE2_TARGET_JOY(wall) : -1;
        }
        return hit;
    }
    {
        int slot;
        for (slot = 0; slot < APPLE2SESSION_PADDLES * 2; slot++) {
            double bx, by, bw, bh;
            turn_rect(slot, &bx, &by, &bw, &bh);
            if (xpos >= bx && xpos < bx + bw && ypos >= by && ypos < by + bh) {
                hit.sect = S_TURN;
                hit.idx = slot;
                hit.n = slot / 2;
                hit.target = turn_target(slot);
                return hit;
            }
        }
    }
    view_origin(APPLE2_VIEW_PADDLES, &oxp, &oyp);
    idx = apple2session_controls_hit(APPLE2_VIEW_PADDLES, (float)((xpos - oxp) / UNIT),
                                     (float)((ypos - oyp) / UNIT));
    if (idx >= 0) {
        const apple2_control *ctl = &g_lay[APPLE2_VIEW_PADDLES][idx];
        hit.idx = idx;
        hit.n = ctl->port;
        if (ctl->kind == APPLE2_CTL_KNOB) {
            hit.sect = S_KNOB;
            hit.target = APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_KNOB);
        } else {
            hit.sect = S_BTN;
            hit.target = ctl->target;
        }
    }
    return hit;
}

static gboolean same_hit(const Hit *aa, const Hit *bb)
{
    return aa->sect == bb->sect && aa->idx == bb->idx && aa->n == bb->n;
}

/* ---- the plastic ------------------------------------------------------------------ */

/* The Apple joystick's two looks (DESIGN-apple2.md): the IIe's beige, from the
 * user's own photograph, and the IIc's platinum, from the MITH photograph. The
 * paddles take the same two schemes. */
typedef struct {
    guint32 case_top, case_shade, wall_tb, wall_side, floor, shaft, cap, btn, btn_down,
            btn_frame, logo;
    guint32 pad_body, pad_rib, knob, knob_rib, knob_idx, pad_btn, pad_btn_down;
} Scheme;

static const Scheme k_beige = {
    0xE6DBBF, 0xCFC3A4, 0x4A4A4E, 0x5C5C60, 0x2A2A2E, 0x1D1D26, 0x2A2A34, 0xF26A1F, 0xC9500F,
    0xB9AC8B, 0xD6CAAC,
    0xC9C3A8, 0xB3AD92, 0x6F6C54, 0x5C5A44, 0xE9E5D0, 0xEB6A25, 0xC24E10,
};
static const Scheme k_platinum = {
    0xEBE9E1, 0xCFCDC4, 0xBDBBB2, 0xCBC9C0, 0xA9A79E, 0xA6A499, 0xB4B2A7, 0xF0641C, 0xC54B0D,
    0xB6B4AB, 0xD7D5CC,
    0xD5D3CA, 0xBFBDB4, 0x8A8878, 0x6E6C60, 0xE9E5D0, 0xF0641C, 0xC54B0D,
};

#define COL_DESK_TOP 0x57544D
#define COL_DESK_BOT 0x3D3B36
#define COL_DESK_EDGE 0x24231F
#define COL_ACCENT APPLE2SESSION_ACCENT_RGB
#define COL_HELD_INK 0x2A0A06
#define COL_BOUND_INK 0xF4EEDD
#define COL_CAPTION 0xE6E2D4
/* the strip's buttons and the keyboard: platinum plastic whatever the joystick */
#define COL_STRIP 0xD9D6CB
#define COL_STRIP_HOT 0xE9E6DC
#define COL_STRIP_INK 0x3A3A3C
#define COL_KB_CASE_TOP 0xD8D6CD
#define COL_KB_CASE_BOT 0xC4C2B8
#define COL_KB_CASE_EDGE 0x8E8C82
#define COL_KB_CAP 0xE4E2DA
#define COL_KB_MOD 0xCCCABF
#define COL_KB_RESET 0xF4F3EE
#define COL_KB_INK 0x2B2B2D
#define COL_KB_SHADOW 0x8F8D84

static const Scheme *scheme(void)
{
    return g_model == APPLE2_MODEL_IIC ? &k_platinum : &k_beige;
}

/* Everything a control draws between begin and end is dimmed to 40% in Map
 * mode when it is not the picked target. */
static gboolean dimmed(int target)
{
    return g_map_state != -2 && (g_map_state < 0 ? FALSE : g_map_state != target);
}

static void dim_begin(cairo_t *cctx, gboolean dim)
{
    if (dim) cairo_push_group(cctx);
}

static void dim_end(cairo_t *cctx, gboolean dim)
{
    if (dim) {
        cairo_pop_group_to_source(cctx);
        cairo_paint_with_alpha(cctx, 0.4);
    }
}

static void poly_path(cairo_t *cctx, const float poly[4][2])
{
    int idx;
    cairo_move_to(cctx, U(poly[0][0]), U(poly[0][1]));
    for (idx = 1; idx < 4; idx++)
        cairo_line_to(cctx, U(poly[idx][0]), U(poly[idx][1]));
    cairo_close_path(cctx);
}

/* The body of an apple: two lobes and a narrower foot. `shrink` takes that many
 * pixels off every circle's radius, so an outline has an even thickness. */
static void apple_body(cairo_t *cctx, double cenx, double ceny, double size, double shrink)
{
    cairo_new_path(cctx);
    cairo_arc(cctx, cenx - 0.17 * size, ceny + 0.08 * size, 0.31 * size - shrink, 0, 2 * G_PI);
    cairo_new_sub_path(cctx);
    cairo_arc(cctx, cenx + 0.17 * size, ceny + 0.08 * size, 0.31 * size - shrink, 0, 2 * G_PI);
    cairo_new_sub_path(cctx);
    cairo_arc(cctx, cenx, ceny + 0.2 * size, 0.30 * size - shrink, 0, 2 * G_PI);
}

/* A tiny Apple: filled (the Closed Apple, Option) or an outline (the Open
 * Apple), with its leaf and the bite out of the right side. `bg` is the plastic
 * under it. */
static void apple_glyph(cairo_t *cctx, double cenx, double ceny, double size, guint32 colour,
                        gboolean filled, guint32 bg)
{
    apple_body(cctx, cenx, ceny, size, 0);
    rgb(cctx, colour);
    cairo_fill(cctx);
    if (!filled) {
        apple_body(cctx, cenx, ceny, size, MAX(1.3, size * 0.07));
        rgb(cctx, bg);
        cairo_fill(cctx);
    }
    /* the bite, out of the upper right */
    cairo_arc(cctx, cenx + 0.5 * size, ceny - 0.02 * size, 0.17 * size, 0, 2 * G_PI);
    rgb(cctx, bg);
    cairo_fill(cctx);
    /* the leaf, leaning over the top */
    cairo_save(cctx);
    cairo_translate(cctx, cenx + 0.07 * size, ceny - 0.36 * size);
    cairo_rotate(cctx, 0.7);
    cairo_scale(cctx, 0.07 * size, 0.15 * size);
    cairo_arc(cctx, 0, 0, 1, 0, 2 * G_PI);
    cairo_restore(cctx);
    rgb(cctx, colour);
    cairo_fill(cctx);
}

/* ---- the joystick -------------------------------------------------------------------- */

static void draw_wall_overlays(cairo_t *cctx, GtkWidget *widget)
{
    const gboolean armed = g_map_state != -2;
    const gboolean wall_lit[4] = {
        g_mode == APPLE2_MODE_JOYSTICK && g_axx < -0.5f, g_mode == APPLE2_MODE_JOYSTICK && g_axx > 0.5f,
        g_mode == APPLE2_MODE_JOYSTICK && g_axy < -0.5f, g_mode == APPLE2_MODE_JOYSTICK && g_axy > 0.5f,
    };
    int wall;

    for (wall = 0; wall < 4; wall++) {
        const int target = APPLE2_TARGET_JOY(wall);
        const gboolean sel = g_map_state >= 0 && g_map_state == target;
        const gboolean hot = armed && g_hover.sect == S_STICK && g_hover.idx == wall;
        double cenx = 0, ceny = 0;
        char bound[40];
        int pt;

        for (pt = 0; pt < 4; pt++) {
            cenx += k_walls[wall][pt][0] / 4;
            ceny += k_walls[wall][pt][1] / 4;
        }
        if (wall_lit[wall] || sel || hot) {
            poly_path(cctx, k_walls[wall]);
            cairo_set_source_rgba(cctx, ((COL_ACCENT >> 16) & 0xFF) / 255.0,
                                  ((COL_ACCENT >> 8) & 0xFF) / 255.0,
                                  (COL_ACCENT & 0xFF) / 255.0, sel ? 0.7 : wall_lit[wall] ? 0.55 : 0.35);
            cairo_fill(cctx);
        }
        bound_text(target, bound, sizeof bound);
        if (bound[0]) {
            legend(cctx, widget, bound, 10, TRUE, COL_BOUND_INK, U(cenx) - 28, U(ceny) - 8, 56, 16);
        } else if (armed) {
            /* an arrow pointing the way the stick tilts */
            const double delx = wall == 0 ? -1 : wall == 1 ? 1 : 0;
            const double dely = wall == 2 ? -1 : wall == 3 ? 1 : 0;
            const double px = U(cenx), py = U(ceny);
            cairo_move_to(cctx, px + delx * 9, py + dely * 9);
            cairo_line_to(cctx, px - delx * 6 - dely * 8, py - dely * 6 + delx * 8);
            cairo_line_to(cctx, px - delx * 6 + dely * 8, py - dely * 6 - delx * 8);
            cairo_close_path(cctx);
            rgb(cctx, sel ? COL_HELD_INK : COL_BOUND_INK);
            cairo_fill(cctx);
        }
    }
}

static void draw_stick(cairo_t *cctx, const Scheme *sch, const apple2_control *stick)
{
    const double sockx = stick->cx, socky = stick->cy;
    const double capx = stick->capx + g_axx * stick->reach;
    const double capy = stick->capy + g_axy * stick->reach;
    const double angle = atan2(capy - socky, capx - sockx);
    const gboolean lit = fabsf(g_axx) > 0.5f || fabsf(g_axy) > 0.5f || g_drag.kind == 1;
    cairo_pattern_t *grad;

    /* the socket */
    cairo_arc(cctx, U(sockx), U(socky), U(0.3), 0, 2 * G_PI);
    rgb(cctx, mix(sch->floor, 0x000000, 0.45));
    cairo_fill(cctx);
    /* the ball joint and the shaft */
    cairo_arc(cctx, U(sockx), U(socky), U(0.19), 0, 2 * G_PI);
    rgb(cctx, mix(sch->shaft, 0x000000, 0.25));
    cairo_fill(cctx);
    cairo_move_to(cctx, U(sockx), U(socky));
    cairo_line_to(cctx, U(capx), U(capy));
    rgb(cctx, sch->shaft);
    cairo_set_line_width(cctx, U(0.24));
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cctx);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_BUTT);
    /* the cap, a rounded bar along the shaft */
    cairo_save(cctx);
    cairo_translate(cctx, U(capx), U(capy));
    cairo_rotate(cctx, angle);
    grad = cairo_pattern_create_linear(0, -U(0.25), 0, U(0.25));
    stop_rgb(grad, 0, mix(sch->cap, 0xFFFFFF, 0.3));
    stop_rgb(grad, 0.5, sch->cap);
    stop_rgb(grad, 1, mix(sch->cap, 0x000000, 0.35));
    rrect(cctx, -U(0.6), -U(0.25), U(1.2), U(0.5), U(0.25));
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, lit ? COL_ACCENT : mix(sch->cap, 0x000000, 0.55));
    cairo_set_line_width(cctx, lit ? 2.5 : 1);
    cairo_stroke(cctx);
    cairo_restore(cctx);
}

static void draw_joy_button(cairo_t *cctx, GtkWidget *widget, const Scheme *sch, int idx)
{
    const apple2_control *ctl = &g_lay[APPLE2_VIEW_JOYSTICK][idx];
    const gboolean held = g_lit[APPLE2_VIEW_JOYSTICK][idx];
    const gboolean sel = g_map_state >= 0 && g_map_state == ctl->target;
    const gboolean hot = g_hover.sect == S_BTN && g_hover.idx == idx && !held;
    const gboolean dim = dimmed(ctl->target);
    const double xpos = U(ctl->xpos), ypos = U(ctl->ypos);
    const double wid = U(ctl->width), hgt = U(ctl->height);
    const double drop = held ? U(0.08) : 0;
    char bound[40];

    dim_begin(cctx, dim);
    rrect(cctx, xpos - U(0.12), ypos - U(0.12), wid + U(0.24), hgt + U(0.24), U(0.16));
    rgb(cctx, sch->btn_frame);
    cairo_fill_preserve(cctx);
    if (held || sel) {
        rgb(cctx, COL_ACCENT);
        cairo_set_line_width(cctx, sel ? 3 : 2.5);
    } else {
        rgb(cctx, mix(sch->btn_frame, 0x000000, 0.35));
        cairo_set_line_width(cctx, 1);
    }
    cairo_stroke(cctx);
    rrect(cctx, xpos, ypos + drop, wid, hgt, U(0.12));
    rgb(cctx, held ? sch->btn_down : hot ? mix(sch->btn, 0xFFFFFF, 0.18) : sch->btn);
    cairo_fill_preserve(cctx);
    rgb(cctx, mix(sch->btn, 0x000000, 0.45));
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0])
        legend(cctx, widget, bound, 10, TRUE, held ? COL_BOUND_INK : COL_HELD_INK, xpos, ypos + drop,
               wid, hgt);
    dim_end(cctx, dim);
}

static void draw_joystick(cairo_t *cctx, GtkWidget *widget)
{
    const Scheme *sch = scheme();
    const gboolean armed = g_map_state != -2;
    const apple2_control *stick = NULL;
    cairo_pattern_t *grad;
    double oxp, oyp;
    int idx;

    view_origin(APPLE2_VIEW_JOYSTICK, &oxp, &oyp);
    cairo_save(cctx);
    cairo_translate(cctx, oxp, oyp);

    if (armed) cairo_push_group(cctx);
    /* a soft shadow on the desk */
    rrect(cctx, U(0.12), U(0.18), U(8.0), U(8.3), U(0.4));
    cairo_set_source_rgba(cctx, 0, 0, 0, 0.3);
    cairo_fill(cctx);
    /* the base */
    grad = cairo_pattern_create_linear(0, 0, 0, U(8.3));
    stop_rgb(grad, 0, mix(sch->case_top, 0xFFFFFF, 0.12));
    stop_rgb(grad, 1, sch->case_top);
    rrect(cctx, 0, 0, U(8.0), U(8.3), U(0.35));
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, mix(sch->case_shade, 0x000000, 0.25));
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    /* the seam of the lower shell */
    cairo_move_to(cctx, U(0.12), U(7.7));
    cairo_line_to(cctx, U(7.88), U(7.7));
    rgb(cctx, sch->case_shade);
    cairo_set_line_width(cctx, 1.6);
    cairo_stroke(cctx);
    /* the raised block: lighter along its top, a shadow down its left */
    rrect(cctx, U(1.78), U(1.98), U(0.34), U(6.3), U(0.15));
    cairo_set_source_rgba(cctx, 0, 0, 0, 0.18);
    cairo_fill(cctx);
    grad = cairo_pattern_create_linear(0, U(1.9), 0, U(8.3));
    stop_rgb(grad, 0, mix(sch->case_top, 0xFFFFFF, 0.28));
    stop_rgb(grad, 0.12, mix(sch->case_top, 0xFFFFFF, 0.1));
    stop_rgb(grad, 1, sch->case_top);
    rrect(cctx, U(2.0), U(1.9), U(6.1), U(6.4), U(0.3));
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, mix(sch->case_shade, 0x000000, 0.3));
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    /* the Apple logo, an outlined rounded square with a small apple in it */
    rrect(cctx, U(0.5), U(6.6), U(0.8), U(1.0), U(0.16));
    rgb(cctx, mix(sch->logo, 0x000000, 0.08));
    cairo_fill_preserve(cctx);
    rgb(cctx, mix(sch->logo, 0x000000, 0.28));
    cairo_set_line_width(cctx, 1.2);
    cairo_stroke(cctx);
    apple_glyph(cctx, U(0.9), U(7.12), U(0.58), mix(sch->logo, 0x000000, 0.3), TRUE,
                mix(sch->logo, 0x000000, 0.08));
    /* the pyramid well: four sloping walls (top and bottom darker) and the floor */
    for (idx = 0; idx < 4; idx++) {
        poly_path(cctx, k_walls[idx]);
        rgb(cctx, idx >= 2 ? sch->wall_tb : sch->wall_side);
        cairo_fill_preserve(cctx);
        rgb(cctx, mix(idx >= 2 ? sch->wall_tb : sch->wall_side, 0x000000, 0.35));
        cairo_set_line_width(cctx, 1);
        cairo_stroke(cctx);
    }
    rrect(cctx, U(4.5), U(5.6), U(1.0), U(1.0), U(0.04));
    rgb(cctx, sch->floor);
    cairo_fill(cctx);
    if (armed) {
        cairo_pop_group_to_source(cctx);
        cairo_paint_with_alpha(cctx, 0.55);
    }

    draw_wall_overlays(cctx, widget);

    for (idx = 0; idx < g_cnt[APPLE2_VIEW_JOYSTICK]; idx++) {
        const apple2_control *ctl = &g_lay[APPLE2_VIEW_JOYSTICK][idx];
        if (ctl->kind == APPLE2_CTL_STICK)
            stick = ctl;
        else if (ctl->kind == APPLE2_CTL_BUTTON)
            draw_joy_button(cctx, widget, sch, idx);
    }
    if (stick) {
        if (armed) cairo_push_group(cctx);
        draw_stick(cctx, sch, stick);
        if (armed) {
            cairo_pop_group_to_source(cctx);
            cairo_paint_with_alpha(cctx, 0.5);
        }
    }
    cairo_restore(cctx);
}

/* ---- the paddles --------------------------------------------------------------------- */

static void draw_paddle_body(cairo_t *cctx, const Scheme *sch, double oxp)
{
    cairo_pattern_t *grad;
    int rib;

    cairo_save(cctx);
    cairo_translate(cctx, oxp, 0);
    cairo_move_to(cctx, U(0.0), U(0.6));
    cairo_line_to(cctx, U(5.2), U(0.0));
    cairo_line_to(cctx, U(6.0), U(0.9));
    cairo_line_to(cctx, U(6.0), U(2.4));
    cairo_line_to(cctx, U(5.0), U(3.0));
    cairo_line_to(cctx, U(0.0), U(2.6));
    cairo_close_path(cctx);
    /* the shadow */
    cairo_save(cctx);
    cairo_translate(cctx, U(0.1), U(0.16));
    cairo_set_source_rgba(cctx, 0, 0, 0, 0.3);
    cairo_fill_preserve(cctx);
    cairo_restore(cctx);
    grad = cairo_pattern_create_linear(0, 0, 0, U(3.0));
    stop_rgb(grad, 0, mix(sch->pad_body, 0xFFFFFF, 0.2));
    stop_rgb(grad, 1, sch->pad_body);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    cairo_save(cctx);
    cairo_clip_preserve(cctx);
    /* the fine ribs down the lower side */
    for (rib = 0; rib < 10; rib++) {
        const double ypos = U(2.0 + rib * 0.1);
        cairo_move_to(cctx, U(0.2), ypos);
        cairo_line_to(cctx, U(5.6), ypos);
    }
    rgb(cctx, sch->pad_rib);
    cairo_set_line_width(cctx, 1.1);
    cairo_stroke(cctx);
    cairo_restore(cctx);
    rgb(cctx, mix(sch->pad_rib, 0x000000, 0.3));
    cairo_set_line_width(cctx, 1.2);
    cairo_set_line_join(cctx, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cctx);
    cairo_restore(cctx);
}

static void draw_knob(cairo_t *cctx, GtkWidget *widget, const Scheme *sch, int idx)
{
    const apple2_control *ctl = &g_lay[APPLE2_VIEW_PADDLES][idx];
    const int pad = ctl->port;
    const int target = APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_KNOB);
    const gboolean sel = g_map_state >= 0 && g_map_state == target;
    const gboolean dim = dimmed(target);
    const double cenx = U(ctl->cx), ceny = U(ctl->cy), rad = U(ctl->reach);
    const double ang = apple2session_knob_angle(g_knob[pad]) * G_PI / 180.0;
    cairo_pattern_t *grad;
    char bound[40];
    int rib;

    dim_begin(cctx, dim);
    /* the collar under the knob */
    cairo_arc(cctx, cenx, ceny, rad + 3, 0, 2 * G_PI);
    rgb(cctx, mix(sch->knob, 0x000000, 0.5));
    cairo_fill(cctx);
    /* the ribbed rim */
    cairo_arc(cctx, cenx, ceny, rad, 0, 2 * G_PI);
    rgb(cctx, sch->knob);
    cairo_fill(cctx);
    for (rib = 0; rib < 24; rib++) {
        const double ribang = rib * G_PI / 12;
        cairo_move_to(cctx, cenx + cos(ribang) * rad * 0.82, ceny + sin(ribang) * rad * 0.82);
        cairo_line_to(cctx, cenx + cos(ribang) * rad, ceny + sin(ribang) * rad);
    }
    rgb(cctx, sch->knob_rib);
    cairo_set_line_width(cctx, 1.6);
    cairo_stroke(cctx);
    /* the flat top */
    grad = cairo_pattern_create_radial(cenx - rad * 0.3, ceny - rad * 0.3, rad * 0.1, cenx, ceny,
                                       rad * 0.82);
    stop_rgb(grad, 0, mix(sch->knob, 0xFFFFFF, 0.22));
    stop_rgb(grad, 1, sch->knob);
    cairo_arc(cctx, cenx, ceny, rad * 0.8, 0, 2 * G_PI);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, sel ? COL_ACCENT : mix(sch->knob_rib, 0x000000, 0.2));
    cairo_set_line_width(cctx, sel ? 3 : 1);
    cairo_stroke(cctx);
    /* the index mark: where it is turned to */
    cairo_move_to(cctx, cenx + sin(ang) * rad * 0.2, ceny - cos(ang) * rad * 0.2);
    cairo_line_to(cctx, cenx + sin(ang) * rad * 0.76, ceny - cos(ang) * rad * 0.76);
    rgb(cctx, sch->knob_idx);
    cairo_set_line_width(cctx, 3);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cctx);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_BUTT);
    bound_text(target, bound, sizeof bound);
    if (bound[0])
        legend(cctx, widget, bound, 9, TRUE, COL_BOUND_INK, cenx - rad * 0.5, ceny + rad * 0.15, rad,
               14);
    dim_end(cctx, dim);
}

static void draw_paddle_button(cairo_t *cctx, GtkWidget *widget, const Scheme *sch, int idx)
{
    const apple2_control *ctl = &g_lay[APPLE2_VIEW_PADDLES][idx];
    const gboolean held = g_lit[APPLE2_VIEW_PADDLES][idx];
    const gboolean sel = g_map_state >= 0 && g_map_state == ctl->target;
    const gboolean hot = g_hover.sect == S_BTN && g_hover.idx == idx && !held;
    const gboolean dim = dimmed(ctl->target);
    const double xpos = U(ctl->xpos), ypos = U(ctl->ypos);
    const double wid = U(ctl->width), hgt = U(ctl->height);
    const double drop = held ? U(0.06) : 0;
    char bound[40];

    dim_begin(cctx, dim);
    rrect(cctx, xpos - U(0.08), ypos - U(0.08), wid + U(0.16), hgt + U(0.16), U(0.14));
    rgb(cctx, mix(sch->pad_body, 0x000000, 0.3));
    cairo_fill_preserve(cctx);
    rgb(cctx, held || sel ? COL_ACCENT : mix(sch->pad_body, 0x000000, 0.5));
    cairo_set_line_width(cctx, held || sel ? 2.5 : 1);
    cairo_stroke(cctx);
    rrect(cctx, xpos, ypos + drop, wid, hgt, U(0.1));
    rgb(cctx, held ? sch->pad_btn_down : hot ? mix(sch->pad_btn, 0xFFFFFF, 0.18) : sch->pad_btn);
    cairo_fill(cctx);
    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0])
        legend(cctx, widget, bound, 8, TRUE, COL_HELD_INK, xpos - 6, ypos + drop, wid + 12, hgt);
    dim_end(cctx, dim);
}

static void draw_turn_button(cairo_t *cctx, GtkWidget *widget, int slot)
{
    double xpos, ypos, wid, hgt;
    const int target = turn_target(slot);
    const gboolean lit = g_turn_lit[slot] || (g_press.sect == S_TURN && g_press.idx == slot);
    const gboolean sel = g_map_state >= 0 && g_map_state == target;
    const gboolean hot = g_hover.sect == S_TURN && g_hover.idx == slot;
    const gboolean dim = dimmed(target);
    const double dirx = slot % 2 ? 1 : -1;
    char bound[40];

    turn_rect(slot, &xpos, &ypos, &wid, &hgt);
    /* drawn in view-less pixels: undo the stage translation by using the window's own */
    dim_begin(cctx, dim);
    rrect(cctx, xpos, ypos + 2, wid, hgt, 5);
    rgb(cctx, lit || sel ? mix(COL_ACCENT, 0x000000, 0.45) : 0x26241F);
    cairo_fill(cctx);
    rrect(cctx, xpos, ypos, wid, hgt - 1, 5);
    rgb(cctx, lit || sel ? COL_ACCENT : hot ? 0x6A675E : 0x55524A);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit || sel ? COL_HELD_INK : 0x1A1914);
    cairo_set_line_width(cctx, lit || sel ? 2 : 1);
    cairo_stroke(cctx);
    bound_text(target, bound, sizeof bound);
    if (bound[0]) {
        legend(cctx, widget, bound, 8, TRUE, lit || sel ? COL_HELD_INK : COL_BOUND_INK, xpos - 3, ypos,
               wid + 6, hgt - 1);
    } else {
        const double cenx = xpos + wid / 2, ceny = ypos + (hgt - 1) / 2;
        cairo_move_to(cctx, cenx + dirx * 5, ceny);
        cairo_line_to(cctx, cenx - dirx * 3, ceny - 5);
        cairo_line_to(cctx, cenx - dirx * 3, ceny + 5);
        cairo_close_path(cctx);
        rgb(cctx, lit || sel ? COL_HELD_INK : COL_BOUND_INK);
        cairo_fill(cctx);
    }
    dim_end(cctx, dim);
}

static void draw_paddles(cairo_t *cctx, GtkWidget *widget)
{
    const Scheme *sch = scheme();
    double oxp, oyp;
    int idx, slot;

    view_origin(APPLE2_VIEW_PADDLES, &oxp, &oyp);
    cairo_save(cctx);
    cairo_translate(cctx, oxp, oyp);
    for (idx = 0; idx < g_cnt[APPLE2_VIEW_PADDLES]; idx++) {
        const apple2_control *ctl = &g_lay[APPLE2_VIEW_PADDLES][idx];
        gboolean armed = g_map_state != -2;
        switch (ctl->kind) {
        case APPLE2_CTL_PART:
            if (armed) cairo_push_group(cctx);
            draw_paddle_body(cctx, sch, U(ctl->xpos));
            if (armed) {
                cairo_pop_group_to_source(cctx);
                cairo_paint_with_alpha(cctx, 0.55);
            }
            break;
        case APPLE2_CTL_KNOB:
            draw_knob(cctx, widget, sch, idx);
            break;
        case APPLE2_CTL_BUTTON:
            draw_paddle_button(cctx, widget, sch, idx);
            break;
        case APPLE2_CTL_CAPTION:
            legend(cctx, widget, ctl->label, 11, TRUE, COL_CAPTION, U(ctl->xpos), U(ctl->ypos),
                   U(ctl->width), U(ctl->height));
            break;
        default:
            break;
        }
    }
    cairo_restore(cctx);
    /* the turn keys are placed in window pixels */
    for (slot = 0; slot < APPLE2SESSION_PADDLES * 2; slot++)
        draw_turn_button(cctx, widget, slot);
}

/* ---- the console strip ------------------------------------------------------------- */

static void draw_sysbutton(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const apple2_control *ctl = &g_lay[APPLE2_VIEW_CONSOLE][idx];
    const gboolean held = g_lit[APPLE2_VIEW_CONSOLE][idx] ||
                          (g_press.sect == S_SYS && g_press.idx == idx);
    const gboolean sel = g_map_state >= 0 && g_map_state == ctl->target;
    const gboolean hot = g_hover.sect == S_SYS && g_hover.idx == idx && !held;
    const gboolean dim = dimmed(ctl->target);
    const gboolean lit = held || sel;
    const double xpos = U(ctl->xpos), ypos = U(ctl->ypos);
    const double wid = U(ctl->width), hgt = U(ctl->height) - 3;
    char bound[40], text[120];

    dim_begin(cctx, dim);
    rrect(cctx, xpos, ypos + 3, wid, hgt, 7);
    rgb(cctx, lit ? mix(COL_ACCENT, 0x000000, 0.45) : 0x7D7B72);
    cairo_fill(cctx);
    rrect(cctx, xpos, ypos, wid, hgt, 7);
    rgb(cctx, lit ? COL_ACCENT : hot ? COL_STRIP_HOT : COL_STRIP);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD_INK : 0x8E8C82);
    cairo_set_line_width(cctx, lit ? 2 : 1);
    cairo_stroke(cctx);
    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0] && strcmp(bound, "\xe2\x80\x94") != 0)   /* nothing bound: no suffix */
        g_snprintf(text, sizeof text, "%s  \xc2\xb7  %s", ctl->label, bound);
    else
        g_snprintf(text, sizeof text, "%s", ctl->label);
    legend(cctx, widget, text, 11, TRUE, lit ? COL_HELD_INK : COL_STRIP_INK, xpos + 2, ypos, wid - 4,
           hgt);
    dim_end(cctx, dim);
}

static void draw_strip(cairo_t *cctx, GtkWidget *widget)
{
    double oxp, oyp;
    int idx;

    view_origin(APPLE2_VIEW_CONSOLE, &oxp, &oyp);
    cairo_save(cctx);
    cairo_translate(cctx, oxp, oyp);
    for (idx = 0; idx < g_cnt[APPLE2_VIEW_CONSOLE]; idx++)
        if (g_lay[APPLE2_VIEW_CONSOLE][idx].kind == APPLE2_CTL_SYSBUTTON)
            draw_sysbutton(cctx, widget, idx);
    cairo_restore(cctx);
}

/* ---- the keyboard ------------------------------------------------------------------- */

static void draw_cap(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const apple2_keycap *cap = &g_kb[idx];
    const int held = idx < MAX_CAPS ? g_cap_held[idx] : 0;
    const gboolean down = held & 1, latched = (held & 2) != 0;
    const gboolean hot = g_hover.sect == S_KEY && g_hover.idx == idx && !down;
    const double xpos = U(cap->xpos), ypos = U(cap->ypos);
    const double wid = U(cap->width), hgt = U(cap->height);
    const double drop = down ? 2 : 0;
    guint32 face = cap->kind == APPLE2_KEYCAP_RESET ? COL_KB_RESET
                   : cap->kind == APPLE2_KEYCAP_CHAR || cap->kind == APPLE2_KEYCAP_SPACE
                         ? COL_KB_CAP : COL_KB_MOD;
    guint32 ink;

    if (hot) face = mix(face, 0xFFFFFF, 0.35);
    if (down) face = COL_ACCENT;
    ink = down ? COL_HELD_INK : COL_KB_INK;

    rrect(cctx, xpos, ypos + 3, wid, hgt - 1, U(0.12));
    rgb(cctx, down ? mix(COL_ACCENT, 0x000000, 0.45) : COL_KB_SHADOW);
    cairo_fill(cctx);
    rrect(cctx, xpos, ypos + drop, wid, hgt - 2, U(0.12));
    rgb(cctx, face);
    cairo_fill_preserve(cctx);
    rgb(cctx, down ? COL_HELD_INK : mix(face, 0x000000, 0.32));
    cairo_set_line_width(cctx, down ? 2 : 1);
    cairo_stroke(cctx);

    switch (cap->kind) {
    case APPLE2_KEYCAP_APPLE: {
        const gboolean closed = cap->keysym == 1;
        apple_glyph(cctx, xpos + wid / 2, ypos + hgt * 0.38 + drop, U(0.62), ink, closed, face);
        legend(cctx, widget, closed ? "Option" : "Open", 7, FALSE, ink, xpos, ypos + hgt * 0.62 + drop,
               wid, hgt * 0.3);
        break;
    }
    case APPLE2_KEYCAP_RESET:
        legend(cctx, widget, cap->legend, 6, TRUE, ink, xpos, ypos + drop, wid, hgt - 2);
        break;
    case APPLE2_KEYCAP_CHAR:
        if (cap->shifted && cap->shifted[0]) {
            legend(cctx, widget, cap->shifted, 9, TRUE, ink, xpos, ypos + 2 + drop, wid, hgt * 0.46);
            legend(cctx, widget, cap->legend, 11, TRUE, ink, xpos, ypos + hgt * 0.42 + drop, wid,
                   hgt * 0.5);
        } else {
            legend(cctx, widget, cap->legend, 13, TRUE, ink, xpos, ypos + drop, wid, hgt - 2);
        }
        break;
    case APPLE2_KEYCAP_SPECIAL:
        legend(cctx, widget, cap->legend, strlen(cap->legend) > 3 ? 8 : 14, TRUE, ink, xpos,
               ypos + drop, wid, hgt - 2);
        break;
    default:   /* the space bar, Shift, Control, Caps Lock */
        if (cap->legend[0])
            legend(cctx, widget, cap->legend, 8, TRUE, ink, xpos, ypos + drop, wid, hgt - 2);
        break;
    }
    if (latched || (cap->kind == APPLE2_KEYCAP_LOCK && latched)) {
        /* a latch that is on: a dot in the corner */
        cairo_arc(cctx, xpos + U(0.17), ypos + U(0.17), 3.5, 0, 2 * G_PI);
        rgb(cctx, COL_ACCENT);
        cairo_fill_preserve(cctx);
        rgb(cctx, COL_HELD_INK);
        cairo_set_line_width(cctx, 1);
        cairo_stroke(cctx);
    }
}

static void draw_keyboard(cairo_t *cctx, GtkWidget *widget)
{
    const gboolean armed = g_map_state != -2;
    cairo_pattern_t *grad;
    double oxp, oyp;
    int idx;

    kb_origin(&oxp, &oyp);
    cairo_save(cctx);
    cairo_translate(cctx, oxp, oyp);
    if (armed) cairo_push_group(cctx);
    grad = cairo_pattern_create_linear(0, -U(0.3), 0, U(g_kbh + 0.3));
    stop_rgb(grad, 0, COL_KB_CASE_TOP);
    stop_rgb(grad, 1, COL_KB_CASE_BOT);
    rrect(cctx, -U(0.3), -U(0.3), U(g_kbw + 0.6), U(g_kbh + 0.6), U(0.3));
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_KB_CASE_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    for (idx = 0; idx < g_kbn; idx++)
        draw_cap(cctx, widget, idx);
    if (armed) {
        cairo_pop_group_to_source(cctx);
        cairo_paint_with_alpha(cctx, 0.4);
    }
    cairo_restore(cctx);
}

static void draw_area(GtkDrawingArea *area, cairo_t *cctx, int wid, int hgt, gpointer udata)
{
    GtkWidget *widget = GTK_WIDGET(area);
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, hgt);
    (void)udata;

    /* the desk the controllers lie on */
    stop_rgb(grad, 0, COL_DESK_TOP);
    stop_rgb(grad, 1, COL_DESK_BOT);
    rrect(cctx, 0.5, 0.5, wid - 1, hgt - 1, 14);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_DESK_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);

    if (!g_lay[APPLE2_VIEW_JOYSTICK])
        return;
    if (g_mode == APPLE2_MODE_PADDLES)
        draw_paddles(cctx, widget);
    else
        draw_joystick(cctx, widget);
    draw_strip(cctx, widget);
    if (g_kb_on)
        draw_keyboard(cctx, widget);
}

/* ---- pointer ---------------------------------------------------------------------- */

/* Lets go of everything the mouse holds on the controls. The keyboard's latches
 * stay: they are the user's, and a window hiding or losing focus lets them go
 * through release_all_holds. */
static void mouse_release(void)
{
    const gboolean any = g_drag.kind || g_press.sect != S_NONE;
    if (g_press.sect == S_KEY)
        apple2session_keyboard_press(g_session, g_press.idx, 0);
    apple2session_pointer_release(g_session);
    g_drag.kind = 0;
    g_press.sect = S_NONE;
    g_press.idx = g_press.n = g_press.target = -1;
    if (any && g_area)
        gtk_widget_queue_draw(g_area);
}

/* Everything the window holds: the mouse's, the keyboard view's latches too. */
static void release_all_holds(void)
{
    mouse_release();
    apple2session_keyboard_release(g_session);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    apple2session_gamepad_capture_cancel(g_session);
}

static void say_bound(const char *what, int stolen)
{
    char msg[220];
    if (stolen >= 0)
        g_snprintf(msg, sizeof msg, "Bound %s (was %s)", what, bindings_target_label(stolen));
    else
        g_snprintf(msg, sizeof msg, "Bound %s", what);
    gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer udata)
{
    int input;
    (void)udata;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (apple2session_gamepad_capture_poll(g_session, &input)) {
        char name[48];
        int stolen = -1;
        bindings_set_pad(g_session, g_map_state, input, &stolen);
        apple2session_pad_input_name(input, name, sizeof name);
        g_capture_timer = 0;
        set_map_state(-1);     /* stay armed: remapping several in a row is normal */
        say_bound(name, stolen);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    gtk_widget_set_sensitive(g_clear_button, state >= 0);
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "apple2-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Hold a button with the mouse, drag the stick or a knob. Map rebinds one.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "apple2-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Click a control to remap it (a wall of the well is a direction)");
    } else {
        char msg[200];
        mouse_release();
        if (is_knob_target(state)) {
            g_snprintf(msg, sizeof msg, "Move a gamepad stick or trigger for %s (Esc cancels)",
                       bindings_target_label(state));
            apple2session_gamepad_capture_begin_axis(g_session);
        } else {
            g_snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s (Esc cancels)",
                       bindings_target_label(state));
            apple2session_gamepad_capture_begin(g_session);
        }
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_all();
}

static void run_sysaction(int sysact)
{
    if (g_parent)
        apple2_window_run_sysaction(APPLE2_WINDOW(g_parent), sysact);
    else
        apple2session_sysaction_fire(g_session, (apple2session_sysaction)sysact);
}

/* The stick under a drag: the cap follows the pointer. */
static void stick_follow(double xpos, double ypos)
{
    double oxp, oyp;
    float xval, yval;
    view_origin(APPLE2_VIEW_JOYSTICK, &oxp, &oyp);
    apple2session_joystick_from_point((float)((xpos - oxp) / UNIT), (float)((ypos - oyp) / UNIT),
                                      &xval, &yval);
    apple2session_pointer_set(g_session, xval, yval);
    gtk_widget_queue_draw(g_area);
}

/* The knob under a drag: the pointer's angle from the dial's centre. */
static void knob_follow(double xpos, double ypos)
{
    float val;
    if (apple2session_knob_from_point(xpos - g_drag.cenx, ypos - g_drag.ceny, g_drag.rad, &val))
        apple2session_knob_set(g_session, g_drag.n, val);
    gtk_widget_queue_draw(g_area);
}

static void drag_follow(double xpos, double ypos)
{
    if (g_drag.kind == 1)
        stick_follow(xpos, ypos);
    else if (g_drag.kind == 2)
        knob_follow(xpos, ypos);
}

static void on_area_begin(GtkGestureDrag *gest, double xpos, double ypos, gpointer udata)
{
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gest));
    const Hit hit = hit_at(xpos, ypos);
    (void)udata;

    if (button != GDK_BUTTON_PRIMARY || hit.sect == S_NONE)
        return;
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target; keys, the floor and the cap are no target */
        if (hit.target >= 0)
            set_map_state(hit.target);
        return;
    }
    mouse_release();
    switch (hit.sect) {
    case S_STICK:
        g_drag.kind = 1;
        g_press = hit;
        drag_follow(xpos, ypos);
        break;
    case S_KNOB: {
        double oxp, oyp;
        const apple2_control *ctl = &g_lay[APPLE2_VIEW_PADDLES][hit.idx];
        view_origin(APPLE2_VIEW_PADDLES, &oxp, &oyp);
        g_drag.kind = 2;
        g_drag.n = hit.n;
        g_drag.cenx = oxp + U(ctl->cx);
        g_drag.ceny = oyp + U(ctl->cy);
        g_drag.rad = U(ctl->reach);
        g_press = hit;
        drag_follow(xpos, ypos);
        break;
    }
    case S_BTN:
    case S_TURN:
        g_press = hit;
        apple2session_press(g_session, hit.target, 1);
        break;
    case S_SYS:
        g_press = hit;
        break;
    case S_KEY:
        g_press = hit;
        apple2session_keyboard_press(g_session, hit.idx, 1);
        break;
    default:
        return;
    }
    gtk_widget_queue_draw(g_area);
}

static void on_area_update(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    double startx, starty;
    (void)udata;
    if (!g_drag.kind) return;
    gtk_gesture_drag_get_start_point(gest, &startx, &starty);
    drag_follow(startx + offx, starty + offy);
}

/* A console button fires on release, like a real one: pressing and dragging
 * off must not reset the machine. */
static void on_area_end(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    const Hit was = g_press;
    double startx, starty;
    (void)udata;
    if (was.sect == S_SYS) {
        gtk_gesture_drag_get_start_point(gest, &startx, &starty);
        g_press.sect = S_NONE;
        {
            const Hit now = hit_at(startx + offx, starty + offy);
            if (now.sect == S_SYS && now.idx == was.idx && is_sysact(was.target))
                run_sysaction(was.target - APPLE2_TARGET_SYSACT_BASE);
        }
    }
    if (was.sect != S_NONE || g_drag.kind)
        mouse_release();
}

static void on_area_cancel(GtkGesture *gest, GdkEventSequence *seq, gpointer udata)
{
    (void)gest; (void)seq; (void)udata;
    mouse_release();
}

/* Dragging off a held button or key lets it go. */
static void on_area_motion(GtkEventControllerMotion *ctrl, double xpos, double ypos, gpointer udata)
{
    const Hit hit = hit_at(xpos, ypos);
    (void)ctrl; (void)udata;
    g_ptr_x = xpos;
    g_ptr_y = ypos;
    if ((g_press.sect == S_BTN || g_press.sect == S_TURN || g_press.sect == S_KEY) &&
        !same_hit(&hit, &g_press))
        mouse_release();
    if (!same_hit(&hit, &g_hover)) {
        g_hover = hit;
        gtk_widget_queue_draw(g_area);
    }
}

static void on_area_leave(GtkEventControllerMotion *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    if (g_press.sect == S_BTN || g_press.sect == S_TURN || g_press.sect == S_KEY)
        mouse_release();
    if (g_hover.sect != S_NONE) {
        g_hover.sect = S_NONE;
        g_hover.idx = -1;
        gtk_widget_queue_draw(g_area);
    }
}

/* The wheel over a knob (or a turn key) nudges it. */
static gboolean on_area_scroll(GtkEventControllerScroll *ctrl, double delx, double dely,
                               gpointer udata)
{
    Hit hit;
    (void)ctrl; (void)delx; (void)udata;
    if (g_map_state != -2 || g_drag.kind || g_mode != APPLE2_MODE_PADDLES)
        return FALSE;
    hit = hit_at(g_ptr_x, g_ptr_y);
    if (hit.sect != S_KNOB && hit.sect != S_TURN)
        return FALSE;
    apple2session_knob_nudge(g_session, hit.n, dely < 0 ? KNOB_NUDGE : -KNOB_NUDGE);
    gtk_widget_queue_draw(g_area);
    return TRUE;
}

static void binding_tip(int target, char *text, int textsz)
{
    char key[32], pad[48];
    if (is_knob_target(target)) {
        const int input = bindings_target_pad(target);
        if (input >= 0) apple2session_pad_input_name(input, pad, sizeof pad);
        else g_snprintf(pad, sizeof pad, "none");
        g_snprintf(text, textsz, "%s\nGamepad axis: %s", bindings_target_label(target), pad);
        return;
    }
    apple2session_keysym_name(bindings_target_keysym(target), key, sizeof key);
    if (bindings_target_pad(target) >= 0)
        apple2session_pad_input_name(bindings_target_pad(target), pad, sizeof pad);
    else
        g_snprintf(pad, sizeof pad, "none");
    g_snprintf(text, textsz, "%s\nKey: %s    Gamepad: %s", bindings_target_label(target),
               key[0] ? key : "none", pad);
}

static gboolean on_area_tooltip(GtkWidget *widget, int xpos, int ypos, gboolean kbmode,
                                GtkTooltip *tip, gpointer udata)
{
    Hit hit;
    char text[300];
    (void)widget; (void)udata;
    if (kbmode) return FALSE;
    hit = hit_at(xpos, ypos);
    switch (hit.sect) {
    case S_KEY: {
        const apple2_keycap *cap = &g_kb[hit.idx];
        if (cap->kind == APPLE2_KEYCAP_MODIFIER)
            g_snprintf(text, sizeof text, "%s: latches until the next key (click again to release)",
                       cap->legend);
        else if (cap->kind == APPLE2_KEYCAP_LOCK)
            g_snprintf(text, sizeof text, "Caps Lock: on at power-up, like the real //e");
        else if (cap->kind == APPLE2_KEYCAP_APPLE)
            g_snprintf(text, sizeof text, cap->keysym == 1 ? "Option (Closed Apple): held while pressed (button 1)"
                                                           : "Open Apple: held while pressed (button 0)");
        else if (cap->kind == APPLE2_KEYCAP_RESET)
            g_snprintf(text, sizeof text, "Reset (warm)");
        else
            return FALSE;
        break;
    }
    case S_STICK:
        if (hit.target >= 0)
            g_snprintf(text, sizeof text, "Stick (press and drag to tilt it)\nThis wall: %s",
                       bindings_target_short_label(hit.target));
        else
            g_snprintf(text, sizeof text, "Press and drag to tilt the stick");
        break;
    case S_KNOB:
        g_snprintf(text, sizeof text,
                   "Drag to turn the knob, scroll to nudge it (Map: its gamepad axis)");
        break;
    case S_TURN:
        g_snprintf(text, sizeof text, "Turns the knob while held\n");
        binding_tip(hit.target, text + strlen(text), (int)(sizeof text - strlen(text)));
        break;
    case S_BTN:
    case S_SYS:
        if (hit.target < 0) return FALSE;
        if (hit.sect == S_SYS && hit.target == APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_POWER)) {
            g_snprintf(text, sizeof text, "Power cycle: boots the FujiNet card in slot 7 into CONFIG\n");
            binding_tip(hit.target, text + strlen(text), (int)(sizeof text - strlen(text)));
        } else {
            binding_tip(hit.target, text, sizeof text);
        }
        break;
    default:
        return FALSE;
    }
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

/* ---- refresh ---------------------------------------------------------------------- */

static void refresh_all(void)
{
    if (g_area) gtk_widget_queue_draw(g_area);
}

static void relayout(void)
{
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), area_width());
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), area_height());
    gtk_widget_set_size_request(gtk_widget_get_parent(g_area), area_width(), -1);
    gtk_widget_queue_resize(g_area);
}

/* Every frame while the window is on screen: what the machine sees held,
 * painted in the accent, the stick's and knobs' positions, and the keyboard.
 * Redraws only when something moved. */
static gboolean frame_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer udata)
{
    gboolean changed = FALSE;
    float pdl0 = 0, pdl1 = 0;
    int view, idx, pad;
    (void)clock; (void)udata;

    apple2session_axes_get(g_session, &pdl0, &pdl1);
    if (g_mode == APPLE2_MODE_JOYSTICK) {
        if (fabsf(pdl0 - g_axx) > 0.004f || fabsf(pdl1 - g_axy) > 0.004f) {
            g_axx = pdl0;
            g_axy = pdl1;
            changed = TRUE;
        }
    } else if (g_axx != 0 || g_axy != 0) {
        g_axx = g_axy = 0;
        changed = TRUE;
    }
    for (pad = 0; pad < APPLE2SESSION_PADDLES; pad++) {
        const float val = apple2session_knob_get(g_session, pad);
        if (fabsf(val - g_knob[pad]) > 0.002f) {
            g_knob[pad] = val;
            changed = TRUE;
        }
    }
    for (view = 0; view < APPLE2_VIEW_COUNT; view++) {
        for (idx = 0; idx < g_cnt[view] && idx < MAX_CTL; idx++) {
            const apple2_control *ctl = &g_lay[view][idx];
            gboolean lit = FALSE;
            if ((ctl->kind == APPLE2_CTL_BUTTON || ctl->kind == APPLE2_CTL_SYSBUTTON) &&
                ctl->target >= 0 && !is_sysact(ctl->target))
                lit = apple2session_target_held(g_session, ctl->target) != 0;
            if (lit != g_lit[view][idx]) {
                g_lit[view][idx] = lit;
                changed = TRUE;
            }
        }
    }
    for (idx = 0; idx < APPLE2SESSION_PADDLES * 2; idx++) {
        const gboolean lit = apple2session_target_held(g_session, turn_target(idx)) != 0;
        if (lit != g_turn_lit[idx]) {
            g_turn_lit[idx] = lit;
            changed = TRUE;
        }
    }
    if (g_kb_on) {
        for (idx = 0; idx < g_kbn && idx < MAX_CAPS; idx++) {
            const int held = apple2session_keyboard_held(g_session, idx);
            if (held != g_cap_held[idx]) {
                g_cap_held[idx] = held;
                changed = TRUE;
            }
        }
    }
    if (changed)
        gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int idx;
    for (idx = 0; idx < g_pad_row_count; idx++) {
        const int eff = apple2session_gamepad_effective_port(g_session, idx);
        char text[32];
        if (eff < 0)
            g_snprintf(text, sizeof text, "plays nothing");
        else if (g_mode == APPLE2_MODE_PADDLES)
            g_snprintf(text, sizeof text, "plays paddle %d", eff);
        else
            g_snprintf(text, sizeof text, "plays the joystick");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_eff[idx])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_eff[idx]), text);
    }
}

static void on_assign_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    const int idx = GPOINTER_TO_INT(udata);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)pspec;
    if (g_syncing) return;
    /* 0 = automatic (connection order), then the ports: the joystick, or Paddle 0 and 1 */
    apple2session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter) or the
 * mode changes: one row per pad, its name, what it plays and the one it is set
 * to. The area is PAD_ROWS rows tall whatever is plugged in (more pads scroll),
 * so the window never changes size. */
static void refresh_pads(void)
{
    static const char *const joy_choices[] = { "Automatic", "Joystick", NULL };
    static const char *const pad_choices[] = { "Automatic", "Paddle 0", "Paddle 1", NULL };
    const char *const *choices = g_mode == APPLE2_MODE_PADDLES ? pad_choices : joy_choices;
    const int ports = apple2session_controller_ports(g_session);
    GtkWidget *child;
    int count, idx;

    g_syncing = TRUE;
    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_pad_row_count = 0;
    g_pad_rows_mode = g_mode;

    count = apple2session_gamepad_count(g_session);
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected - plug one in");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_widget_set_size_request(none, -1, PAD_ROW_H);
        gtk_widget_set_tooltip_text(none, "A gamepad is picked up as it appears");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        g_syncing = FALSE;
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PADS; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *drop, *eff;
        char text[128];
        const int assign = apple2session_gamepad_assignment(g_session, idx);

        apple2session_gamepad_name(g_session, idx, text, sizeof text);
        gtk_widget_set_size_request(row, -1, PAD_ROW_H);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        drop = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drop),
                                   assign < 0 || assign >= ports ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(drop, FALSE);
        gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
        g_signal_connect(drop, "notify::selected", G_CALLBACK(on_assign_changed),
                         GINT_TO_POINTER(idx));
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), drop);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_eff[g_pad_row_count++] = eff;
    }
    g_syncing = FALSE;
    refresh_pad_ports();
}

/* Keeps the mode, model and keyboard widgets in step with the session (the
 * Preferences page and the settings file move them too), and the pad rows. */
static void sync_widgets(void)
{
    const int mode = apple2session_controller_mode(g_session);
    const int model = apple2session_controller_model(g_session);
    const gboolean kbon = apple2session_get_int(g_session, "controllers_keyboard", 0) != 0;
    gboolean resized = FALSE;

    g_syncing = TRUE;
    if (mode != g_mode) {
        mouse_release();
        g_mode = mode;
        refresh_all();
    }
    if (model != g_model) {
        g_model = model;
        refresh_all();
    }
    if (kbon != g_kb_on) {
        g_kb_on = kbon;
        resized = TRUE;
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_mode_btn[g_mode == APPLE2_MODE_PADDLES]), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_model_btn[g_model == APPLE2_MODEL_IIC]), TRUE);
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(g_kb_check)) != g_kb_on)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(g_kb_check), g_kb_on);
    g_syncing = FALSE;
    if (resized)
        relayout();
    if (g_pad_rows_mode != g_mode)
        refresh_pads();
}

/* The pad rows (hot plug, automatic assignment changes) and the mode switches. */
static gboolean info_tick(gpointer udata)
{
    unsigned gen;
    (void)udata;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;

    sync_widgets();
    gen = apple2session_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

/* Map mode's share of a key, for every window's key path (the main window
 * asks too: Map can be armed with either window focused). While Map is armed
 * a press is the key to bind (once a target is picked) or is swallowed; Escape
 * steps back. The window's own keys (F9, F11, F12) are never Map's, and neither
 * is a Ctrl chord. Alt, Meta and Super are the Open and Closed Apple keys: Map
 * says so and does not bind them. Returns TRUE when Map took the key. A release
 * of a key Map took reaches the session as a release of nothing, which is
 * harmless. */
gboolean apple2_controllers_map_key(guint keyval, guint code, GdkModifierType mods, gboolean down)
{
    guint32 keysym;
    char name[32];
    int stolen = -1;

    if (!g_window || g_map_state == -2)
        return FALSE;
    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12 ||
        (mods & GDK_CONTROL_MASK))
        return FALSE;
    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    if (!down)
        return TRUE;
    if (keyval == GDK_KEY_Escape) {
        g_swallow_code = code;
        set_map_state(g_map_state >= 0 ? -1 : -2);
        return TRUE;
    }
    if (g_map_state < 0)
        return TRUE;              /* armed, no target yet: typing is not typed */
    if (is_knob_target(g_map_state)) {
        /* the knob follows a pad axis: a key cannot turn it */
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "The knob follows an axis: move a gamepad stick or trigger");
        return TRUE;
    }
    keysym = code >= 8 ? apple2session_keysym_from_evdev(code - 8) : 0;
    if (!keysym) keysym = keyval;
    if (keysym == APPLE2SESSION_KEYSYM_LALT || keysym == APPLE2SESSION_KEYSYM_RALT ||
        keysym == APPLE2SESSION_KEYSYM_LMETA || keysym == APPLE2SESSION_KEYSYM_RMETA ||
        keysym == APPLE2SESSION_KEYSYM_LSUPER || keysym == APPLE2SESSION_KEYSYM_RSUPER) {
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Alt keys are Open Apple and Closed Apple: they cannot be bound");
        return TRUE;
    }
    bindings_set_keysym(g_session, g_map_state, keysym, &stolen);
    apple2session_keysym_name(keysym, name, sizeof name);
    g_swallow_code = code;
    set_map_state(-1);            /* stay armed: remapping several in a row is normal */
    say_bound(name, stolen);
    return TRUE;
}

static gboolean on_key(guint keyval, guint code, GdkModifierType mods, gboolean down)
{
    /* an open popover has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    if (apple2_controllers_map_key(keyval, code, mods, down))
        return TRUE;
    /* Otherwise exactly what the main window does with it (Alt included). */
    if (!g_parent) return FALSE;
    return apple2_window_key(APPLE2_WINDOW(g_parent), keyval, code, mods, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    return on_key(keyval, code, mods, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                            guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    on_key(keyval, code, mods, FALSE);
}

/* Keys typed here went through the main window, so a key held into a focus
 * change must be let go there too (and the mouse's, here). */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    release_all_holds();
    if (g_parent)
        apple2_window_release_keys(APPLE2_WINDOW(g_parent));
    g_swallow_code = 0;     /* its release went elsewhere */
}

static void on_active_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    (void)pspec; (void)udata;
    if (!gtk_window_is_active(GTK_WINDOW(obj)))
        release_all_holds();
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    bindings_reset_defaults(g_session);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Bindings back to their defaults");
    refresh_all();
}

/* Takes the picked control's key and gamepad input away. */
static void on_clear_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    if (g_map_state < 0) return;
    bindings_clear(g_session, g_map_state);
    gtk_label_set_text(GTK_LABEL(g_map_hint),
                       "Cleared. Press a key or a gamepad input to bind it again");
    refresh_all();
}

static void on_show_bindings(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    apple2session_set_int(g_session, "controllers_show_bindings",
                          gtk_check_button_get_active(button) ? 1 : 0);
    refresh_all();
}

static void on_kb_toggled(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    if (g_syncing) return;
    apple2session_set_int(g_session, "controllers_keyboard",
                          gtk_check_button_get_active(button) ? 1 : 0);
    g_kb_on = gtk_check_button_get_active(button) != 0;
    apple2session_keyboard_release(g_session);
    set_map_state(-2);
    relayout();
    refresh_all();
}

static void on_mode_toggled(GtkToggleButton *button, gpointer udata)
{
    const int mode = GPOINTER_TO_INT(udata);
    if (g_syncing || !gtk_toggle_button_get_active(button) || mode == g_mode) return;
    set_map_state(-2);
    release_all_holds();
    apple2session_set_controller_mode(g_session, mode);
    g_mode = mode;
    refresh_pads();
    refresh_all();
}

static void on_model_toggled(GtkToggleButton *button, gpointer udata)
{
    const int model = GPOINTER_TO_INT(udata);
    if (g_syncing || !gtk_toggle_button_get_active(button) || model == g_model) return;
    apple2session_set_controller_model(g_session, model);
    g_model = model;
    refresh_all();
}

static void hide_window(void)
{
    set_map_state(-2);
    release_all_holds();
    g_swallow_code = 0;
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *window, gpointer udata)
{
    (void)window; (void)udata;
    hide_window();
    return TRUE;
}

/* The pad line's timer runs only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. (The per-frame tick
 * stops by itself while the drawing is unmapped.) */
static void on_window_map(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (!g_info_timer) g_info_timer = g_timeout_add(250, info_tick, NULL);
    info_tick(NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_info_timer) g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_destroy(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    g_area = NULL;
    g_window = NULL;
    g_pad_box = NULL;
    g_pad_row_count = 0;
    g_pad_rows_mode = -1;
    g_map_state = -2;
    g_hover.sect = g_press.sect = S_NONE;
    g_drag.kind = 0;
}

/* The Map "Done" button in the accent. */
static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "button.apple2-accent { background-image: none; background-color: #f44336;"
        " color: #2a0a06; font-weight: bold; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static GtkWidget *segment(const char *first, const char *second, GtkWidget **out, GCallback cb)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    int idx;
    gtk_widget_add_css_class(box, "linked");
    out[0] = gtk_toggle_button_new_with_label(first);
    out[1] = gtk_toggle_button_new_with_label(second);
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(out[1]), GTK_TOGGLE_BUTTON(out[0]));
    for (idx = 0; idx < 2; idx++) {
        gtk_widget_set_focus_on_click(out[idx], FALSE);
        g_signal_connect(out[idx], "toggled", cb, GINT_TO_POINTER(idx));
        gtk_box_append(GTK_BOX(box), out[idx]);
    }
    return box;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *modes, *maprow, *toolbar, *header, *defaults, *padscroll, *stage, *label;
    GtkEventController *keys, *focus, *motion, *scroll;
    GtkGesture *drag;
    int view;

    install_css();
    for (view = 0; view < APPLE2_VIEW_COUNT; view++)
        g_lay[view] = apple2session_controls_layout(view, &g_cnt[view], &g_vw[view], &g_vh[view]);
    g_kb = apple2session_keyboard_layout(&g_kbn, &g_kbw, &g_kbh);
    if (g_kbn > MAX_CAPS) g_kbn = MAX_CAPS;
    memset(g_lit, 0, sizeof g_lit);
    memset(g_turn_lit, 0, sizeof g_turn_lit);
    memset(g_cap_held, 0, sizeof g_cap_held);
    g_mode = apple2session_controller_mode(g_session);
    g_model = apple2session_controller_model(g_session);
    g_kb_on = apple2session_get_int(g_session, "controllers_keyboard", 0) != 0;
    g_axx = g_axy = 0;
    g_knob[0] = apple2session_knob_get(g_session, 0);
    g_knob[1] = apple2session_knob_get(g_session, 1);
    g_hover.sect = g_press.sect = S_NONE;
    g_drag.kind = 0;

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* min == max: Wayland compositors float it */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    /* the controls */
    g_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), area_width());
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), area_height());
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_area, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a control must not steal focus. */
    gtk_widget_set_focusable(g_area, FALSE);
    gtk_widget_set_has_tooltip(g_area, TRUE);
    g_signal_connect(g_area, "query-tooltip", G_CALLBACK(on_area_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_area_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_area_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_area_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_area_cancel), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_area_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_area_leave), NULL);
    gtk_widget_add_controller(g_area, motion);
    scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_area_scroll), NULL);
    gtk_widget_add_controller(g_area, scroll);
    gtk_widget_add_tick_callback(g_area, frame_tick, NULL, NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_size_request(root, area_width(), -1);
    stage = root;
    gtk_box_append(GTK_BOX(root), g_area);

    /* Joystick | Paddles, Beige | Platinum, and the Keyboard check */
    modes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    label = gtk_label_new("Mode");
    gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(modes), label);
    gtk_box_append(GTK_BOX(modes),
                   segment("Joystick", "Paddles", g_mode_btn, G_CALLBACK(on_mode_toggled)));
    label = gtk_label_new("Colour");
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_set_margin_start(label, 6);
    gtk_box_append(GTK_BOX(modes), label);
    gtk_box_append(GTK_BOX(modes),
                   segment("Beige", "Platinum", g_model_btn, G_CALLBACK(on_model_toggled)));
    gtk_widget_set_tooltip_text(g_mode_btn[0], "An Apple Joystick IIe or IIc on the game port");
    gtk_widget_set_tooltip_text(g_mode_btn[1], "Two Apple Hand Controllers (paddles) on the game port");
    gtk_widget_set_tooltip_text(g_model_btn[0], "The beige of the IIe's own joystick");
    gtk_widget_set_tooltip_text(g_model_btn[1], "The platinum of the IIc's joystick (looks only)");
    g_kb_check = gtk_check_button_new_with_label("Keyboard");
    gtk_widget_set_focus_on_click(g_kb_check, FALSE);
    gtk_widget_set_margin_start(g_kb_check, 6);
    gtk_widget_set_tooltip_text(g_kb_check, "Show the Apple IIe keyboard: click its keys to type");
    g_signal_connect(g_kb_check, "toggled", G_CALLBACK(on_kb_toggled), NULL);
    gtk_box_append(GTK_BOX(modes), g_kb_check);
    gtk_box_append(GTK_BOX(stage), modes);

    /* the pad rows: a fixed-height area, so the window never resizes */
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PAD_GAP);
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);
    g_pad_generation = apple2session_gamepad_generation(g_session);
    refresh_pads();

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    g_clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_focus_on_click(g_clear_button, FALSE);
    gtk_widget_set_tooltip_text(g_clear_button, "Take the picked control's key and gamepad input away");
    g_signal_connect(g_clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_bindings = gtk_check_button_new_with_label("Show bindings");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_bindings), show_bindings_saved());
    gtk_widget_set_focus_on_click(g_show_bindings, FALSE);
    g_signal_connect(g_show_bindings, "toggled", G_CALLBACK(on_show_bindings), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_show_bindings);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint wraps onto a second line rather than clip its "Esc cancels"
     * or widen the window; two lines are always reserved, so the window never
     * changes height either */
    gtk_label_set_wrap(GTK_LABEL(g_map_hint), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(g_map_hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(g_map_hint), 2);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_label_set_yalign(GTK_LABEL(g_map_hint), 0);
    {
        PangoLayout *twolines = gtk_widget_create_pango_layout(g_map_hint, "A\nA");
        int linesw, linesh;
        pango_layout_get_pixel_size(twolines, &linesw, &linesh);
        g_object_unref(twolines);
        gtk_widget_set_size_request(g_map_hint, -1, linesh);
    }
    gtk_box_append(GTK_BOX(root), g_map_hint);

    header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    adw_window_title_new("Controllers", "Apple II game port"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), apple2_help_button("controllers"));
    apple2_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    set_map_state(-2);
    sync_widgets();
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void ensure_window(GtkWindow *parent, apple2session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
}

void apple2_controllers_show(GtkWindow *parent, apple2session *session)
{
    ensure_window(parent, session);
    gtk_window_present(GTK_WINDOW(g_window));
}

void apple2_controllers_toggle(GtkWindow *parent, apple2session *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        gtk_window_present(GTK_WINDOW(g_window));
}

gboolean apple2_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

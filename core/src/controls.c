/*
 * controls -- the Controllers window's layout and hit test, one table for the
 * four toolkits (see apple2session.h). The geometry follows DESIGN-apple2.md:
 * the joystick's proportions are read off the user's photograph of the Apple
 * Joystick IIe (about 34 px per unit), the paddle's off the Apple Hand
 * Controllers II. Units are layout units; a toolkit adds its own margin and
 * scale and paints the PART entries itself.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stddef.h>

#include "apple2session.h"

#define NO_TARGET (-1)

#define PART(pt, lbl, x, y, w, h) \
    { APPLE2_CTL_PART, pt, lbl, -1, NO_TARGET, x, y, w, h, 0, 0, 0, 0, 0 }

/* ---- the joystick (8.3 x 8.5) ------------------------------------------------- */

#define STICK_WELL_X 2.6f
#define STICK_WELL_Y 2.7f
#define STICK_WELL_W 4.9f
#define STICK_WELL_H 5.0f
#define STICK_SOCKET_X 5.0f
#define STICK_SOCKET_Y 6.1f
#define STICK_CAP_X 5.2f           /* the cap at rest, a little up and right of its socket */
#define STICK_CAP_Y 4.9f
#define STICK_REACH 1.4f

static const apple2_control k_joystick[] = {
    PART(APPLE2_PART_JOY_BASE, "base", 0.0f, 0.0f, 8.0f, 8.3f),
    PART(APPLE2_PART_JOY_BLOCK, "block", 2.0f, 1.9f, 6.1f, 6.4f),
    PART(APPLE2_PART_JOY_WELL, "well", STICK_WELL_X, STICK_WELL_Y, STICK_WELL_W, STICK_WELL_H),
    PART(APPLE2_PART_JOY_FLOOR, "floor", 4.5f, 5.6f, 1.0f, 1.0f),
    PART(APPLE2_PART_JOY_LOGO, "logo", 0.5f, 6.6f, 0.8f, 1.0f),
    /* the left button is button 0 (Open Apple), the top one button 1 (Closed Apple) */
    { APPLE2_CTL_BUTTON, APPLE2_PART_NONE, "Button 0", -1, APPLE2_TARGET_JOY(APPLE2_ACT_BTN0),
      0.45f, 2.2f, 1.35f, 1.3f, 0, 0, 0, 0, 0 },
    { APPLE2_CTL_BUTTON, APPLE2_PART_NONE, "Button 1", -1, APPLE2_TARGET_JOY(APPLE2_ACT_BTN1),
      2.2f, 0.5f, 1.4f, 1.3f, 0, 0, 0, 0, 0 },
    { APPLE2_CTL_STICK, APPLE2_PART_NONE, "Stick", -1, NO_TARGET,
      STICK_WELL_X, STICK_WELL_Y, STICK_WELL_W, STICK_WELL_H,
      STICK_SOCKET_X, STICK_SOCKET_Y, STICK_CAP_X, STICK_CAP_Y, STICK_REACH },
};

/* ---- the paddles (12.9 x 3.5): two wedges 6 wide, 0.8 apart, captioned ----------- */

#define PADDLE_PITCH 6.8f

#define PADDLE(n, ox) \
    PART(APPLE2_PART_PADDLE_BODY, "body", (ox), 0.0f, 6.0f, 3.0f), \
    { APPLE2_CTL_KNOB, APPLE2_PART_NONE, "Knob", n, NO_TARGET, \
      (ox) + 0.7f, 0.4f, 2.0f, 2.0f, (ox) + 1.7f, 1.4f, 0, 0, 1.0f }, \
    { APPLE2_CTL_BUTTON, APPLE2_PART_NONE, "Button", n, \
      APPLE2_TARGET_PADDLE(n, APPLE2_PACT_BUTTON), \
      (ox) + 5.15f, 0.85f, 0.9f, 0.7f, 0, 0, 0, 0, 0 }, \
    { APPLE2_CTL_CAPTION, APPLE2_PART_NONE, "Paddle " #n, n, NO_TARGET, \
      (ox), 3.0f, 6.0f, 0.5f, 0, 0, 0, 0, 0 }

static const apple2_control k_paddles[] = {
    PADDLE(0, 0.0f),
    PADDLE(1, PADDLE_PITCH),
};

/* ---- the console strip (6.4 x 0.9) --------------------------------------------- */

static const apple2_control k_console[] = {
    { APPLE2_CTL_SYSBUTTON, APPLE2_PART_NONE, "Reset", -1,
      APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_RESET), 0.0f, 0.0f, 3.0f, 0.9f, 0, 0, 0, 0, 0 },
    { APPLE2_CTL_SYSBUTTON, APPLE2_PART_NONE, "Power Cycle", -1,
      APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_POWER), 3.4f, 0.0f, 3.0f, 0.9f, 0, 0, 0, 0, 0 },
};

typedef struct {
    const apple2_control *list;
    int count;
    float width, height;
} view_def;

#define VIEW(arr, w, h) { arr, (int)(sizeof arr / sizeof arr[0]), w, h }

static const view_def k_views[APPLE2_VIEW_COUNT] = {
    [APPLE2_VIEW_JOYSTICK] = VIEW(k_joystick, 8.3f, 8.5f),
    [APPLE2_VIEW_PADDLES] = VIEW(k_paddles, 12.9f, 3.5f),
    [APPLE2_VIEW_CONSOLE] = VIEW(k_console, 6.4f, 0.9f),
};

const apple2_control *apple2session_controls_layout(int view, int *count,
                                                    float *width, float *height)
{
    if (view < 0 || view >= APPLE2_VIEW_COUNT) {
        if (count) *count = 0;
        if (width) *width = 0;
        if (height) *height = 0;
        return NULL;
    }
    if (count) *count = k_views[view].count;
    if (width) *width = k_views[view].width;
    if (height) *height = k_views[view].height;
    return k_views[view].list;
}

static int inside_rect(const apple2_control *ctl, float xpos, float ypos)
{
    return xpos >= ctl->xpos && xpos <= ctl->xpos + ctl->width &&
           ypos >= ctl->ypos && ypos <= ctl->ypos + ctl->height;
}

int apple2session_controls_hit(int view, float xpos, float ypos)
{
    if (view < 0 || view >= APPLE2_VIEW_COUNT) return -1;
    /* topmost first: the table is in paint order */
    for (int idx = k_views[view].count - 1; idx >= 0; idx--) {
        const apple2_control *ctl = &k_views[view].list[idx];
        switch (ctl->kind) {
        case APPLE2_CTL_KNOB: {
            const float dx = xpos - ctl->cx, dy = ypos - ctl->cy;
            if (dx * dx + dy * dy <= ctl->reach * ctl->reach) return idx;
            break;
        }
        case APPLE2_CTL_STICK:
        case APPLE2_CTL_BUTTON:
        case APPLE2_CTL_SYSBUTTON:
            if (inside_rect(ctl, xpos, ypos)) return idx;
            break;
        default:
            break;
        }
    }
    return -1;
}

int apple2session_joystick_from_point(float xpos, float ypos, float *xval, float *yval)
{
    float dx = (xpos - STICK_CAP_X) / STICK_REACH;
    float dy = (ypos - STICK_CAP_Y) / STICK_REACH;
    if (dx < -1.0f) dx = -1.0f; else if (dx > 1.0f) dx = 1.0f;
    if (dy < -1.0f) dy = -1.0f; else if (dy > 1.0f) dy = 1.0f;
    if (xval) *xval = dx;
    if (yval) *yval = dy;
    return xpos >= STICK_WELL_X && xpos <= STICK_WELL_X + STICK_WELL_W &&
           ypos >= STICK_WELL_Y && ypos <= STICK_WELL_Y + STICK_WELL_H;
}

/*
 * controls_test -- the Controllers window's layout table: three views, every
 * control inside its view, no two pressable controls overlapping, exact-shape
 * hit tests (the dial is a circle, not its bounding box), targets in range,
 * and the stick's deflection from a pointer.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "apple2session.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 0.001f)

static int pressable(const apple2_control *ctl)
{
    return ctl->kind == APPLE2_CTL_STICK || ctl->kind == APPLE2_CTL_BUTTON ||
           ctl->kind == APPLE2_CTL_KNOB || ctl->kind == APPLE2_CTL_SYSBUTTON;
}

static int overlap(const apple2_control *a, const apple2_control *b)
{
    return a->xpos < b->xpos + b->width && b->xpos < a->xpos + a->width &&
           a->ypos < b->ypos + b->height && b->ypos < a->ypos + a->height;
}

static int find_kind(const apple2_control *list, int count, int kind, int nth)
{
    for (int idx = 0; idx < count; idx++)
        if (list[idx].kind == kind && nth-- == 0)
            return idx;
    return -1;
}

static void centre_hit(int view, const apple2_control *list, int idx, const char *what)
{
    const apple2_control *ctl = &list[idx];
    char msg[96];
    snprintf(msg, sizeof msg, "the centre of %s hits it", what);
    CHECK(apple2session_controls_hit(view, ctl->xpos + ctl->width / 2, ctl->ypos + ctl->height / 2) == idx, msg);
}

int main(void)
{
    int count;
    float width, height;
    const apple2_control *list;

    /* a bad view */
    list = apple2session_controls_layout(-1, &count, &width, &height);
    CHECK(list == NULL && count == 0 && width == 0 && height == 0, "a bad view has no layout");
    CHECK(apple2session_controls_layout(APPLE2_VIEW_COUNT, NULL, NULL, NULL) == NULL, "past the last view too");
    CHECK(apple2session_controls_hit(7, 1, 1) == -1 && apple2session_controls_hit(-1, 1, 1) == -1, "and no hit");

    /* every view: in bounds, targets in range, pressables do not overlap, paint order has parts first */
    for (int view = 0; view < APPLE2_VIEW_COUNT; view++) {
        list = apple2session_controls_layout(view, &count, &width, &height);
        CHECK(list && count > 0 && width > 0 && height > 0, "a view has controls and a size");
        for (int idx = 0; idx < count; idx++) {
            const apple2_control *ctl = &list[idx];
            CHECK(ctl->xpos >= -0.001f && ctl->ypos >= -0.001f &&
                  ctl->xpos + ctl->width <= width + 0.001f && ctl->ypos + ctl->height <= height + 0.001f,
                  "every control is inside its view");
            CHECK(ctl->target >= -1 && ctl->target < APPLE2_TARGET_COUNT, "targets are in range");
            CHECK(ctl->label && ctl->label[0], "every control has a label");
            if (ctl->kind == APPLE2_CTL_PART)
                CHECK(ctl->part != APPLE2_PART_NONE && ctl->target == -1, "a part names its shape and presses nothing");
            else
                CHECK(ctl->part == APPLE2_PART_NONE, "a control that is not a part names no shape");
            if (ctl->kind == APPLE2_CTL_BUTTON || ctl->kind == APPLE2_CTL_SYSBUTTON)
                CHECK(ctl->target >= 0, "buttons have targets");
            for (int other = idx + 1; other < count; other++)
                if (pressable(ctl) && pressable(&list[other]))
                    CHECK(!overlap(ctl, &list[other]), "no two pressable controls overlap");
        }
    }

    /* the joystick, 8.3 x 8.5 */
    list = apple2session_controls_layout(APPLE2_VIEW_JOYSTICK, &count, &width, &height);
    CHECK(NEAR(width, 8.3f) && NEAR(height, 8.5f), "the joystick view is 8.3 by 8.5");
    {
        const int b0 = find_kind(list, count, APPLE2_CTL_BUTTON, 0);
        const int b1 = find_kind(list, count, APPLE2_CTL_BUTTON, 1);
        const int stick = find_kind(list, count, APPLE2_CTL_STICK, 0);
        CHECK(find_kind(list, count, APPLE2_CTL_BUTTON, 2) == -1 && stick >= 0 &&
              find_kind(list, count, APPLE2_CTL_STICK, 1) == -1, "two buttons and one stick");
        CHECK(list[b0].target == APPLE2_TARGET_JOY(APPLE2_ACT_BTN0) &&
              list[b1].target == APPLE2_TARGET_JOY(APPLE2_ACT_BTN1), "the buttons are button 0 and button 1");
        CHECK(list[b0].xpos < list[b1].xpos && list[b0].ypos > list[b1].ypos, "button 0 is the left one, button 1 the top one");
        centre_hit(APPLE2_VIEW_JOYSTICK, list, b0, "button 0");
        centre_hit(APPLE2_VIEW_JOYSTICK, list, b1, "button 1");
        centre_hit(APPLE2_VIEW_JOYSTICK, list, stick, "the well");
        CHECK(NEAR(list[stick].cx, 5.0f) && NEAR(list[stick].cy, 6.1f) && NEAR(list[stick].reach, 1.4f) &&
              NEAR(list[stick].capx, 5.2f) && NEAR(list[stick].capy, 4.9f), "the stick's socket, rest and reach");
        CHECK(apple2session_controls_hit(APPLE2_VIEW_JOYSTICK, 0.2f, 0.2f) == -1, "the corner of the case is nothing");
        CHECK(apple2session_controls_hit(APPLE2_VIEW_JOYSTICK, 4.0f, 1.0f) == -1, "and so is the case above the block");
        CHECK(apple2session_controls_hit(APPLE2_VIEW_JOYSTICK, 7.9f, 2.0f) == -1, "and the block outside the well");
        CHECK(apple2session_controls_hit(APPLE2_VIEW_JOYSTICK, 100, 100) == -1 &&
              apple2session_controls_hit(APPLE2_VIEW_JOYSTICK, -1, -1) == -1, "and far away");
        CHECK(find_kind(list, count, APPLE2_CTL_PART, 0) == 0 && list[0].part == APPLE2_PART_JOY_BASE,
              "the base is painted first");
    }

    /* the stick's deflection */
    {
        float xval = 9, yval = 9;
        CHECK(apple2session_joystick_from_point(5.2f, 4.9f, &xval, &yval) && NEAR(xval, 0) && NEAR(yval, 0),
              "the pointer on the resting cap: centred, in the well");
        CHECK(apple2session_joystick_from_point(5.2f + 1.4f, 4.9f - 1.4f, &xval, &yval) &&
              NEAR(xval, 1) && NEAR(yval, -1), "a full reach right and up: the top right stop");
        CHECK(apple2session_joystick_from_point(5.2f - 0.7f, 4.9f + 0.7f, &xval, &yval) &&
              NEAR(xval, -0.5f) && NEAR(yval, 0.5f), "half a reach left and down: proportional");
        CHECK(apple2session_joystick_from_point(5.0f, 6.1f, &xval, &yval) &&
              NEAR(xval, -0.2f / 1.4f) && NEAR(yval, 1.2f / 1.4f), "on the socket the cap is dragged to it");
        CHECK(apple2session_joystick_from_point(7.4f, 7.6f, &xval, &yval) && NEAR(xval, 1) && NEAR(yval, 1),
              "past the reach it clamps at the stops");
        CHECK(!apple2session_joystick_from_point(1.0f, 1.0f, &xval, &yval) && NEAR(xval, -1) && NEAR(yval, -1),
              "outside the well it reports so, with the clamped value");
        CHECK(apple2session_joystick_from_point(5.0f, 5.0f, NULL, NULL), "and the outputs are optional");
    }

    /* the paddles, 12.9 x 3.5 */
    list = apple2session_controls_layout(APPLE2_VIEW_PADDLES, &count, &width, &height);
    CHECK(NEAR(width, 12.9f) && NEAR(height, 3.5f), "the paddles view is 12.9 by 3.5");
    {
        int knobs = 0, buttons = 0, bodies = 0, captions = 0;
        for (int idx = 0; idx < count; idx++) {
            switch (list[idx].kind) {
            case APPLE2_CTL_KNOB:
                CHECK(list[idx].port == knobs && NEAR(list[idx].reach, 1.0f), "knob n is paddle n's, radius 1");
                knobs++;
                break;
            case APPLE2_CTL_BUTTON:
                CHECK(list[idx].port == buttons &&
                      list[idx].target == APPLE2_TARGET_PADDLE(buttons, APPLE2_PACT_BUTTON), "button n is paddle n's button");
                buttons++;
                break;
            case APPLE2_CTL_PART: bodies += list[idx].part == APPLE2_PART_PADDLE_BODY; break;
            case APPLE2_CTL_CAPTION: captions++; break;
            default: break;
            }
        }
        CHECK(knobs == 2 && buttons == 2 && bodies == 2 && captions == 2, "two of each: knob, button, body, caption");
        {
            const int knob = find_kind(list, count, APPLE2_CTL_KNOB, 0);
            const int knob1 = find_kind(list, count, APPLE2_CTL_KNOB, 1);
            const int btn = find_kind(list, count, APPLE2_CTL_BUTTON, 1);
            CHECK(apple2session_controls_hit(APPLE2_VIEW_PADDLES, list[knob].cx, list[knob].cy) == knob, "the dial's centre hits it");
            CHECK(apple2session_controls_hit(APPLE2_VIEW_PADDLES, list[knob].cx + 0.9f, list[knob].cy) == knob, "and its edge");
            CHECK(apple2session_controls_hit(APPLE2_VIEW_PADDLES, list[knob].cx + 0.95f, list[knob].cy + 0.95f) == -1,
                  "but not the corner of its bounding box: the dial is a circle");
            CHECK(apple2session_controls_hit(APPLE2_VIEW_PADDLES, list[knob1].cx, list[knob1].cy) == knob1, "paddle 1's dial");
            centre_hit(APPLE2_VIEW_PADDLES, list, btn, "paddle 1's button");
            CHECK(list[knob1].cx - list[knob].cx > 6.0f, "paddle 1 sits to the right of paddle 0");
        }
    }

    /* the console strip */
    list = apple2session_controls_layout(APPLE2_VIEW_CONSOLE, &count, &width, &height);
    CHECK(count == 2 && list[0].kind == APPLE2_CTL_SYSBUTTON && list[1].kind == APPLE2_CTL_SYSBUTTON,
          "the console strip has two buttons");
    CHECK(list[0].target == APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_RESET) &&
          list[1].target == APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_POWER), "Reset and Power Cycle");
    centre_hit(APPLE2_VIEW_CONSOLE, list, 0, "Reset");
    centre_hit(APPLE2_VIEW_CONSOLE, list, 1, "Power Cycle");
    CHECK(apple2session_controls_hit(APPLE2_VIEW_CONSOLE, 3.2f, 0.4f) == -1, "the gap between them is nothing");

    CHECK(APPLE2SESSION_ACCENT_RGB != 0, "an accent colour");

    fprintf(stderr, "controls_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

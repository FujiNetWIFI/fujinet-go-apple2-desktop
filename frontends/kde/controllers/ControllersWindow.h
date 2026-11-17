/*
 * The Controllers window (F9): the Apple Joystick IIe/IIc (beige or platinum)
 * or the two Apple Hand Controllers (paddles), the console's Reset and Power
 * Cycle buttons, the gamepads with the port each plays, Map mode for rebinding
 * any control, and an optional Apple //e keyboard. Hidden at start; a
 * fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QPointF>
#include <QWidget>

#include "apple2session.h"

namespace apple2ctl {
/* The drawing's metrics, shared with the offscreen test: pixels per layout
 * unit for the joystick, the console strip and the keyboard (a key's width);
 * the paddles are drawn a little larger, in the room the joystick's height
 * leaves. The stage (controller + strip) is the same size in both modes, so
 * switching mode never resizes the window. */
constexpr double kUnit = 34.0;
constexpr double kPaddleUnit = 40.0;
constexpr int kMargin = 16;                      /* round the keyboard's width */
constexpr int kStageW = 590;                     /* ceil(16.4 * kUnit) + 2 * kMargin */
constexpr int kTopPad = 16;
constexpr int kJoyH = 289;                       /* ceil(8.5 * kUnit) */
constexpr int kGap = 14;
constexpr int kStripH = 31;                      /* ceil(0.9 * kUnit) */
constexpr int kStageH = kTopPad + kJoyH + kGap + kStripH + kGap;

/* where each view's layout origin sits in the stage, in pixels */
inline QPointF joystickOrigin() { return QPointF((kStageW - 8.3 * kUnit) / 2, kTopPad); }
inline QPointF paddlesOrigin()
{
    return QPointF((kStageW - 12.9 * kPaddleUnit) / 2, kTopPad + (kJoyH - 3.5 * kPaddleUnit) / 2);
}
inline QPointF consoleOrigin() { return QPointF((kStageW - 6.4 * kUnit) / 2, kTopPad + kJoyH + kGap); }
} // namespace apple2ctl

/* Shows the window, or hides it when it is showing. */
void apple2ControllersToggle(QWidget *parent, apple2session *session);
/* Shows (and raises) the window; leaves it showing if it already is. */
void apple2ControllersShow(QWidget *parent, apple2session *session);
bool apple2ControllersVisible();
/* The Controllers window has the keyboard focus (the main window then keeps
 * its keys down when it loses activation). */
bool apple2ControllersActive();
/* Preferences closed or the session restarted: redraw, and follow a mode
 * changed there. */
void apple2ControllersRefresh();

/*
 * Apple2KeyForward -- one key path for the machine window and the Controllers
 * panel: Map capture first, then the bindings (a bound key drives the joystick
 * or a paddle, or posts a console action), and only what the table did not take
 * is typed on the Apple II.
 *
 * The physical key (its macOS virtual key code) decides the binding, so the
 * numeric keypad's keys steer whatever the layout or Num Lock. The typed
 * character is the one the key produced.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "apple2session.h"

/* Forward a key press or release. YES when the key was taken (bound, or
 * typed). A key typed with Command held is never forwarded: it is a menu
 * equivalent and no key-up arrives for it. */
BOOL Apple2ForwardKeyEvent(apple2session *session, NSEvent *event, int down);

/* Shift, Control and Caps Lock arrive as flagsChanged, not keyDown: they can be
 * bound too. (Option is Open and Closed Apple, which the owning view tracks
 * itself; Command is the menus'.) */
void Apple2ForwardModifierEvent(apple2session *session, NSEvent *event);

/* The keysym of an NSEvent's physical key, 0 when unknown. */
uint32_t Apple2KeysymFromEvent(NSEvent *event);

/* Implemented by controllers/ControllersWindow.m: while Map waits for a key,
 * binds it and returns YES. F9, F11, F12 and the Apple keys are never taken. */
BOOL Apple2ControllersMapKey(uint32_t keysym);

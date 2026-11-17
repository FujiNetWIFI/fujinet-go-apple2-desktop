/*
 * The Win32 Controllers window (F9): the Apple Joystick IIe/IIc or the two
 * Apple Hand Controllers (paddles) drawn as a player sees them, the console's
 * Reset and Power Cycle buttons, the gamepads and the port each drives, Map
 * mode, and the Apple II keyboard, in a fixed-size floating tool window. Hidden
 * at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "apple2session.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void apple2_controllers_window_toggle(HWND parent, apple2session *session);
/* Show it (and raise it) if it is not showing. */
void apple2_controllers_window_show(HWND parent, apple2session *session);
int  apple2_controllers_window_visible(void);

/* The gamepad set, the bindings, the mode or the machine changed: refresh. */
void apple2_controllers_window_refresh(void);

/* Map mode is armed: the next host key is a binding, not typing. The main
 * window gives its keys here first; 1 if consumed. */
int apple2_controllers_map_key(UINT msg, WPARAM wparam, LPARAM lparam);

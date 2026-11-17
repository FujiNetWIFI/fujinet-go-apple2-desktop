/*
 * Keyboard path shared by the main window and the Controllers window.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_WIN_KEYS_H
#define APPLE2_WIN_KEYS_H

#include <stdint.h>
#include <windows.h>

#include "apple2session.h"

/* The keysym of the PHYSICAL key a WM_KEYDOWN/WM_KEYUP reports (its scan code,
 * so Map mode and the bindings see one key the same way whatever the layout or
 * NumLock say), or 0 if the key has none. */
uint32_t apple2_win_keysym(WPARAM vk, LPARAM lp);

/* Runs a WM_KEYDOWN / WM_SYSKEYDOWN / WM_KEYUP / WM_SYSKEYUP through the
 * session: apple2session_input_key first (a bound key drives a controller
 * target and is not typed), else the Apple II's keyboard, where Alt (Open
 * Apple) and the right Alt (Closed Apple) are held buttons. Returns 1 when the
 * key was consumed (the window proc returns 0), 0 to let default processing go
 * on (Alt+F4). Auto-repeat presses are swallowed. */
int apple2_win_key_msg(apple2session *session, UINT msg, WPARAM wp, LPARAM lp);

#endif

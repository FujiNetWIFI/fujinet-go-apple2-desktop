/*
 * The Win32 frontend's debugger window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_WIN_DBG_WINDOW_H
#define APPLE2_WIN_DBG_WINDOW_H

#include <windows.h>

#include "apple2session.h"

/* One window per process; presents the existing one if there is one. */
void apple2_debugger_show(HWND parent, apple2session *session);

/* Give the debugger's F5/F7/F8 accelerators first refusal on a message
 * bound for its window. Returns 1 when the message was consumed. Called
 * from the main message loop. */
int apple2_debugger_pretranslate(MSG *msg);

#endif

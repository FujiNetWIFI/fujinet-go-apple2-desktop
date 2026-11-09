/*
 * The Win32 debugger window over the 6502 debugger engine (apple2debug.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_WIN_DBG_WINDOW_H
#define APPLE2_WIN_DBG_WINDOW_H

#include <windows.h>

#include "apple2session.h"

/* Show: created on first use, hidden (and detached, so the machine runs on)
 * when closed. Showing it attaches the engine, which stops the machine, as
 * the other frontends do, so there is something to look at. */
void apple2_debugger_show(HWND parent, apple2session *session);

/* F12 / View > Debugger: show, or hide and let the machine run. */
void apple2_debugger_toggle(HWND parent, apple2session *session);
int  apple2_debugger_visible(void);

/* Accelerators (F5/F7/F8/Shift+F8/Shift+F7/F9/F12) for the main loop's
 * pretranslate hook, so they work while a child control (an edit field,
 * say) has the focus. Returns 1 when the message was consumed. */
int apple2_debugger_pretranslate(MSG *msg);

#endif /* APPLE2_WIN_DBG_WINDOW_H */

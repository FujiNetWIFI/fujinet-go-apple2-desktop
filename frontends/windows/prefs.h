/*
 * The Input settings window -- see prefs.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_WIN_PREFS_H
#define APPLE2_WIN_PREFS_H

#include "win_common.h"
#include "apple2session.h"

/* What the window asks of the main window. */
typedef struct {
    /* the window closed; dirty = a setting that needs a restart changed */
    void (*closed)(int dirty);
} apple2_prefs_hooks;

/* Shows the window, or raises it when it is open. */
void apple2_prefs_show(HWND parent, apple2session *session, const apple2_prefs_hooks *hooks);
/* Closing without applying anything (quitting): the settings are saved. */
void apple2_prefs_abandon(void);
/* The gamepad set changed. */
void apple2_prefs_gamepads_changed(void);
/* Tab and the arrow keys move between its controls; 1 if the message was used. */
int apple2_prefs_pretranslate(MSG *msg);

#endif

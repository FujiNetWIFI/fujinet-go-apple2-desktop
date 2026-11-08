/*
 * The Help window (Ctrl+Shift+H): the topics of data/help/help.md, built in
 * (apple2help.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Shows the Help at id: a heading's slug or a context ("debugger/cpu",
 * "prefs/ROMs"), or "?words" to search; NULL stays where it was. */
void apple2_help_show(GtkWindow *parent, const char *id);

/* What app.help (Ctrl+Shift+H; F1 outside the main window) opens for a
 * window: set by the windows that have one; the main window's own is
 * worked out by apple2_window_help_context. */
void apple2_help_set_context(GtkWindow *window, const char *context);
const char *apple2_help_get_context(GtkWindow *window);

/* Opens the Help for window (or, in the Help, finds). */
void apple2_help_for(GtkWindow *window);

/* A Help button: opens context, or with NULL its window's. */
GtkWidget *apple2_help_button(const char *context);

G_END_DECLS

/*
 * Apple2Window: main window of the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "apple2session.h"

G_BEGIN_DECLS

#define APPLE2_TYPE_WINDOW (apple2_window_get_type())
G_DECLARE_FINAL_TYPE(Apple2Window, apple2_window, APPLE2, WINDOW,
                     AdwApplicationWindow)

GtkWidget *apple2_window_new(AdwApplication *app, apple2session *session);
void apple2_window_toast(Apple2Window *self, const char *message);
void apple2_window_restart_session(Apple2Window *self);

/* main.c: the icon name to use, which differs between an installed app and
 * one running out of the build tree. */
const char *apple2_icon_name(void);

/* The Help context the main window shows now: "main", or "prefs/<page>"
 * while Preferences are open. */
const char *apple2_window_help_context(Apple2Window *self);

/* The one key path: the main window and the Controllers window both end here,
 * so Map mode, the bindings, F9 (Controllers), F10, F11, F12 and the machine's
 * keys behave the same whichever has the focus. The Alt keys are the Open and
 * Closed Apple keys and reach the machine. Returns whether the key was
 * consumed. */
gboolean apple2_window_key(Apple2Window *self, guint keyval, guint keycode,
                           GdkModifierType state, gboolean down);
/* Lets go of every key the keyboard holds (focus lost). */
void apple2_window_release_keys(Apple2Window *self);
/* A console action (Reset, Power Cycle, Stop in Debugger) from a key, a pad
 * button or the Controllers window, with the same feedback as the Machine menu.
 * UI thread. */
void apple2_window_run_sysaction(Apple2Window *self, int sysact);

G_END_DECLS

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

G_END_DECLS

/*
 * The GNOME frontend's debugger window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "apple2session.h"

G_BEGIN_DECLS

/* Presents the debugger window, creating it on first call. Safe to call
 * repeatedly; the window is a singleton per process. */
void apple2_debugger_show(GtkWindow *parent, apple2session *session);

G_END_DECLS

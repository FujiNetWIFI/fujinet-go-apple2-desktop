/*
 * Preferences dialog for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "apple2session.h"

G_BEGIN_DECLS

typedef struct _Apple2Window Apple2Window;

/* Shows the preferences dialog. display_changed is invoked immediately when
 * a display-only option changes; machine options restart the session when
 * the dialog closes. */
void apple2_prefs_show(Apple2Window *parent, apple2session *session,
                       void (*display_changed)(Apple2Window *parent));

G_END_DECLS

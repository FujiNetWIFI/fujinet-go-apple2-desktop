/*
 * Apple2Display: the emulator video widget for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "apple2session.h"

G_BEGIN_DECLS

#define APPLE2_TYPE_DISPLAY (apple2_display_get_type())
G_DECLARE_FINAL_TYPE(Apple2Display, apple2_display, APPLE2, DISPLAY, GtkWidget)

typedef enum {
    /* Default: what the machine looked like on a real monitor. The core
     * renders 560x384 (or 640x400 with VidHD), which is not 4:3 -- Apple II
     * pixels are not square. */
    APPLE2_ASPECT_TV_4_3 = 0,
    /* One core pixel per display pixel, i.e. the frame's own ratio. */
    APPLE2_ASPECT_SQUARE_PIXELS = 1,
    APPLE2_ASPECT_INTEGER = 2,
} Apple2AspectMode;

GtkWidget *apple2_display_new(apple2session *session);
void apple2_display_set_aspect_mode(Apple2Display *self, Apple2AspectMode mode);
void apple2_display_set_smooth(Apple2Display *self, gboolean smooth);

G_END_DECLS

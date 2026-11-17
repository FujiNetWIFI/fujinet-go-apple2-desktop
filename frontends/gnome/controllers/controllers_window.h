/*
 * The Controllers window -- the Apple II's game port drawn as a player sees it:
 * the Apple Joystick IIe and IIc (beige or platinum, a stick in its pyramid
 * well and two square buttons) or a pair of Apple Hand Controllers (a ribbed
 * knob and a button each), the console's Reset and Power Cycle buttons, and,
 * on request, the Apple IIe keyboard. Held controls light up in the accent
 * whoever holds them, the pointer works every control (drag the stick, turn
 * the knobs, click the keys) and Map mode rebinds any of them to a key or a
 * gamepad input.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "apple2session.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is the main window; it is
 * only used the first time, to set the transient-for relationship, and keys
 * typed in this window go through it. */
void apple2_controllers_toggle(GtkWindow *parent, apple2session *session);
/* Shows it (or raises it). */
void apple2_controllers_show(GtkWindow *parent, apple2session *session);
gboolean apple2_controllers_visible(void);

/* Map mode's share of a key, for the main window's key path as well (Map can
 * be armed with either window focused): while Map is armed this takes the key
 * and returns TRUE. F9, F11, F12 and Ctrl chords are never Map's; Alt, Meta and
 * Super are the Open and Closed Apple keys and cannot be bound. */
gboolean apple2_controllers_map_key(guint keyval, guint code, GdkModifierType mods,
                                    gboolean down);

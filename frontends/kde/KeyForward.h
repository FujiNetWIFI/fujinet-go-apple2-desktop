/*
 * Key handling shared by the GPU and software displays.
 *
 * Key events are translated to the X11 keysyms apple2_key_from_event expects,
 * so this frontend and the GNOME one share one mapping table. Both press and
 * release are forwarded: the core tracks Open Apple and Closed Apple as held
 * modifiers, so a missed release would leave one stuck down.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QEvent>
#include <QKeyEvent>

#include "apple2session.h"

namespace keyforward {

/* Claim every key except the reserved chords (F10 menu, F11 fullscreen,
 * F12 debugger) while focused, so menu accelerators cannot shadow what the
 * Apple II should receive. Alt in particular must NOT reach the menu bar: it
 * is Open/Closed Apple. Returns true when the event was accepted. */
bool shortcutOverride(QEvent *event);

/* Auto-repeat is swallowed (the core latches a key until it is read; a repeat
 * storm would jam it). */
void keyPress(apple2session *session, QKeyEvent *event);
void keyRelease(apple2session *session, QKeyEvent *event);

} // namespace keyforward

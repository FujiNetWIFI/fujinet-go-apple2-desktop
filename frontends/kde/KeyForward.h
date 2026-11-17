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

/* The key's keysym in the session's binding space, or 0: the PHYSICAL key
 * through its evdev code (so Shift cannot turn 8 into '*', and NumLock cannot
 * turn Keypad 8 into Up), else Qt's own key code. */
quint32 physicalKeysym(const QKeyEvent *event);

/* The app's own keys: F9 (Controllers), F10 (menu), F11 (fullscreen) and F12
 * (Debugger). They are never typed on the Apple II and cannot be bound. */
bool isAppKey(const QKeyEvent *event);

/* Claim every key except the reserved chords (F9 Controllers, F10 menu, F11
 * fullscreen, F12 debugger) while focused, so menu accelerators cannot shadow what the
 * Apple II should receive. Alt in particular must NOT reach the menu bar: it
 * is Open/Closed Apple. Returns true when the event was accepted. */
bool shortcutOverride(QEvent *event);

/* The session's controller bindings first (a numpad key that steers the
 * joystick, a key bound to a console action, which is posted for the main
 * window's timer to run), then what the Apple II types. Alt, Meta and Super
 * are the Open and Closed Apple keys and always go to the machine.
 * Auto-repeat is swallowed (the core latches a key until it is read; a repeat
 * storm would jam it). */
void keyPress(apple2session *session, QKeyEvent *event);
void keyRelease(apple2session *session, QKeyEvent *event);

} // namespace keyforward

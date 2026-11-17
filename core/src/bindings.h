/*
 * bindings -- the remappable key / pad-input binding table, shared by the
 * session (seed at start), input.c (resolve a key), gamepad_sdl.c (resolve a
 * pad button or axis on its polling thread) and the Controllers window's Map
 * mode. A target (apple2session.h: APPLE2_TARGET_*) holds at most one keysym
 * and one pad input id; binding one steals it from whoever held it, so no two
 * targets ever fire from the same input.
 *
 * Keys are global. A pad input is scoped: a system action is reachable from
 * every pad, the joystick's targets from the joystick's pad, a paddle's
 * targets only from that paddle's pad, so two paddles can both use South as
 * their button.
 *
 * Process-global storage (like the machine itself, a process singleton).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_BINDINGS_H
#define APPLE2_BINDINGS_H

#include "apple2session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reload the table: every target to its default, then overridden by whatever
 * "bindings" holds in s's settings store. Called once per apple2session_new. */
void bindings_init(apple2session *s);

/* 1 if the target acts in `mode` (APPLE2_MODE_*): a joystick target in
 * joystick mode, a paddle target in paddles mode, a system action always. */
int bindings_target_in_mode(int target, int mode);

/* The target a keysym drives now, or -1. F9, F11, F12 and the Open/Closed
 * Apple keys (Alt, Meta, Super) are never bound. The target may belong to the
 * other mode: see bindings_target_in_mode. */
int bindings_target_for_key(uint32_t keysym);
/* The target pad input `id` (a button of apple2session.h's bands) drives in
 * `mode` for port `port`'s pad (0, or 0..1 for paddles), or -1. A paddle's
 * knob axis is not a button: see bindings_knob_axis. */
int bindings_target_for_pad(int mode, int port, int id);
/* The pad axis that sets paddle `paddle`'s knob, or -1. */
int bindings_knob_axis(int paddle);

/* "Joystick Left", "Paddle 1 Clockwise", "Power Cycle". */
const char *bindings_target_label(int target);
/* Just the control: "Left", "Clockwise", "Button 0". */
const char *bindings_target_short_label(int target);

/* What a target has now: its keysym (0 none) and pad input id (-1 none). */
uint32_t bindings_target_keysym(int target);
int      bindings_target_pad(int target);

/* Rebind, stealing from any other target that held it (`stolen` receives the
 * target it was taken from, or -1). keysym 0 / id -1 clears. Persists. A pad
 * AXIS can only be bound to a knob target and a button only to the others. */
void bindings_set_keysym(apple2session *s, int target, uint32_t keysym, int *stolen);
void bindings_set_pad(apple2session *s, int target, int id, int *stolen);
/* Clear one target's key and pad input. */
void bindings_clear(apple2session *s, int target);
/* Every target back to its default. Persists. */
void bindings_reset_defaults(apple2session *s);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_BINDINGS_H */

/*
 * gamepad_sdl -- private interface: the pure helpers unit-tested without
 * hardware, and the start/stop the session calls. The public gamepad API is
 * in apple2session.h (and lives in gamepad_sdl.c).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_GAMEPAD_SDL_H
#define APPLE2_GAMEPAD_SDL_H

#include <stdint.h>

#include "apple2session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Direction bits a hat resolves to. */
#define APPLE2_DIR_UP    1
#define APPLE2_DIR_DOWN  2
#define APPLE2_DIR_LEFT  4
#define APPLE2_DIR_RIGHT 8

/* The stick's live travel starts past this: applied by apple2_paddle_from_axis,
 * which rescales the rest so the stick still reaches its stops. */
#define APPLE2_STICK_DEADZONE 0.18f

/* The direction bits as a mask over apple2_joy_act (LEFT/RIGHT/UP/DOWN).
 * Opposite directions cancel. Pure. */
unsigned apple2_acts_from_dir(int dir);

/* Which pad plays `port` (0 or 1): an explicit assignment wins; among the
 * rest, connection order (the first unassigned pad plays the first port nobody
 * is assigned to, and so on). `assign[idx]` is pad idx's assignment (-1 =
 * automatic). Returns the pad index or -1. Pure. */
int apple2_pad_for_port(const int *assign, int npads, int port);

/* The knob value (-1..1) an axis position gives: a gamepad trigger sweeps
 * 0..1 (mapped to -1..1), a stick (or any axis of a raw joystick) -1..1 with
 * the dead zone taken out so a centred stick reads exactly 0. Pure. */
float apple2_knob_from_axis(float value, int is_trigger);

/* SDL3's SDL_GamepadButton (a uint8_t) to the public numbering, and back.
 * Explicit tables, never a cast, so an SDL reordering cannot desync them. */
int apple2_pad_button_from_sdl(uint8_t button);
int apple2_sdl_button_from_pad(int button);

/* Start SDL's gamepad subsystem and the poll thread (a no-op if already
 * running). System actions are POSTED to the session from the poll thread
 * (never fired: firing joins threads). Returns 0 on success. */
int  apple2_gamepad_start(apple2session *s);
void apple2_gamepad_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_GAMEPAD_SDL_H */

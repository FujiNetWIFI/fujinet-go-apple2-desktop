/*
 * input -- the merged input layer: private to the session, the Controllers
 * window's entry points (apple2session.h) and the gamepad poll thread. Every
 * input source holds targets by its own bit, one word per target, so letting
 * go of one source never releases another's: the keyboard, the window's
 * mouse, and for each gamepad its buttons and its stick separately (two pads,
 * or a pad's button and its stick, bound to the same action cannot cancel each
 * other). The machine sees the union. The analog sources (the mouse's stick,
 * a pad's left stick) merge with the digital ones by strongest deflection.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_INPUT_H
#define APPLE2_INPUT_H

#include <stdint.h>

#include "apple2session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pads get a stable "owner" number, 0..APPLE2_MAX_PADS-1, while they are
 * connected, whatever their slot becomes when another is unplugged. */
#define APPLE2_MAX_PADS 8

#define APPLE2_SRC_KEYBOARD  (1u << 0)
#define APPLE2_SRC_POINTER   (1u << 1)
#define APPLE2_SRC_PAD(o)    (1u << (2 + (o)))
#define APPLE2_SRC_STICK(o)  (1u << (2 + APPLE2_MAX_PADS + (o)))

/* The session the machine is driven through (set by apple2session_new, cleared
 * by apple2session_free); the layer is process-global like the machine. Reads
 * the controller mode from the settings. */
void apple2_input_attach(apple2session *s);
void apple2_input_detach(apple2session *s);

/* The mode now (APPLE2_MODE_*), and a counter that changes every time it does,
 * so the pad thread knows to let go and look again. */
int      apple2_input_mode(void);
unsigned apple2_input_mode_generation(void);
/* Switch mode: lets go of every source, centres the paddles and the machine's
 * inputs. A no-op if it is the mode already. */
void apple2_input_set_mode(int mode);

/* The pad thread's entry points. press: a pad BUTTON holds or releases a
 * target (its APPLE2_SRC_PAD source). Targets that are not held inputs
 * (system actions) are ignored. */
void apple2_input_pad_press(int owner, int target, int down);
/* A hat or a raw direction steering `owner`'s pad on `port`: act is an
 * apple2_joy_act LEFT/RIGHT/UP/DOWN. In joystick mode it holds that joystick
 * target; in paddles mode LEFT and RIGHT turn the paddle's knob (CCW/CW) and
 * UP/DOWN do nothing. The APPLE2_SRC_STICK source. */
void apple2_input_pad_steer(int owner, int port, int act, int down);
/* The left analog stick of a pad that plays the joystick: x and y in -1..1
 * (+y down), the deadzone already applied. */
void apple2_input_pad_analog(int owner, float x, float y);
/* A pad axis moved paddle `paddle`'s knob to `value` (-1..1): sets it when the
 * axis changed since last time. */
void apple2_input_pad_knob(int owner, int paddle, float value);
/* The pad is gone (or moved to another port): let go of everything it held. */
void apple2_input_pad_clear(int owner);

/* Advance the keys' knob ramp by dt_ms (the pad poll thread calls it). */
void apple2_input_tick(int dt_ms);
/* 1 while some knob key is held (the poll thread then wakes every 16 ms). */
int  apple2_input_knob_active(void);

/* The Open or Closed Apple key (Alt / Meta / Super, left or right) went down
 * or up through apple2session_key: for the held lights only; the core presses
 * the button itself. */
void apple2_input_apple_key(int button, int down);

/* After the machine (re)starts: push every merged value to it again. */
void apple2_input_resync(void);
/* Drop every source's holds (a stop) and tell the machine. */
void apple2_input_clear_all(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_INPUT_H */

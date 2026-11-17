/*
 * keyboard -- private interface to keyboard.c: the sink the on-screen
 * keyboard types through, so a test can record what a click would type
 * without a machine. The public half is in apple2session.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_KEYBOARD_H
#define APPLE2_KEYBOARD_H

#include <stdint.h>

#include "apple2session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What apple2session_key takes: down, keysym, unicode, ctrl, shift. */
typedef void (*apple2_key_sink)(apple2session *s, int down, uint32_t keysym,
                                uint32_t unicode, int ctrl, int shift);

/* Replace the sink (NULL restores apple2session_key). */
void apple2_keyboard_set_sink(apple2_key_sink sink);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_KEYBOARD_H */

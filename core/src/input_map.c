/*
 * Key and paddle mapping: X11/xkb keysym -> the libretro (keycode, character,
 * mods) triple AppleWin's core expects, and gamepad stick -> Apple II paddle.
 *
 * Every frontend translates its toolkit's key events to X11/xkb keysyms (GDK
 * keyvals already are; Qt and Win32 map through a small table) and then calls
 * apple2session_key, so this one table serves all four. Ported from the
 * Android app's input/AppleInput.kt + input/HardwareKeyboard.kt +
 * input/GameControllerMapper.kt.
 *
 * The core resolves a printable key from `character`; `keycode` is only
 * consulted for Ctrl combos and non-printing keys. That is why Ctrl forces
 * character to 0, and why the keycode for a printable key must be the
 * UNSHIFTED value -- RETROK_a for 'A', RETROK_1 for '!'.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>

#include "apple2session.h"

/* --- libretro key codes (libretro.h RETROK_*) ---------------------------- */
#define RK_BACKSPACE 8
#define RK_TAB       9
#define RK_RETURN    13
#define RK_ESCAPE    27
#define RK_SPACE     32
#define RK_0         48
#define RK_a         97
#define RK_DELETE    127
#define RK_UP        273
#define RK_DOWN      274
#define RK_RIGHT     275
#define RK_LEFT      276
#define RK_RALT      307   /* Closed (Solid) Apple */
#define RK_LALT      308   /* Open Apple */

/* --- libretro modifier mask (RETROKMOD_*) -------------------------------- */
#define RKMOD_NONE  0x0000
#define RKMOD_SHIFT 0x0001
#define RKMOD_CTRL  0x0002

/* --- X11/xkb keysyms ----------------------------------------------------- */
#define XKS_BACKSPACE 0xFF08
#define XKS_TAB       0xFF09
#define XKS_RETURN    0xFF0D
#define XKS_ESCAPE    0xFF1B
#define XKS_DELETE    0xFFFF
#define XKS_KP_ENTER  0xFF8D
#define XKS_LEFT      0xFF51
#define XKS_UP        0xFF52
#define XKS_RIGHT     0xFF53
#define XKS_DOWN      0xFF54
#define XKS_ALT_L     0xFFE9
#define XKS_ALT_R     0xFFEA
#define XKS_META_L    0xFFE7   /* some layouts report the Apple keys here */
#define XKS_META_R    0xFFE8
#define XKS_SUPER_L   0xFFEB
#define XKS_SUPER_R   0xFFEC

/* Keys with no printable character, resolved by keycode alone. Returns 0 for
 * anything not in the table. */
static unsigned special_keycode(uint32_t keysym)
{
    switch (keysym) {
    case XKS_RETURN:
    case XKS_KP_ENTER:  return RK_RETURN;
    case XKS_ESCAPE:    return RK_ESCAPE;
    case XKS_TAB:       return RK_TAB;
    case XKS_BACKSPACE: return RK_BACKSPACE;
    case XKS_DELETE:    return RK_DELETE;
    case XKS_UP:        return RK_UP;
    case XKS_DOWN:      return RK_DOWN;
    case XKS_LEFT:      return RK_LEFT;
    case XKS_RIGHT:     return RK_RIGHT;
    /* Open Apple / Closed Apple. Alt is the natural home for them on a PC
     * keyboard; Meta/Super are accepted too because a Mac keyboard reports
     * its Command keys there and they are physically the Apple keys. */
    case XKS_ALT_L:
    case XKS_META_L:
    case XKS_SUPER_L:   return RK_LALT;
    case XKS_ALT_R:
    case XKS_META_R:
    case XKS_SUPER_R:   return RK_RALT;
    default:            return 0;
    }
}

/* The unshifted character on the same physical key, for a US layout -- which
 * is what an Apple II keyboard is. Only the keycode is derived this way; the
 * character actually typed is passed through untouched, so a non-US layout
 * still types correctly and only Ctrl combos would resolve oddly. */
static uint32_t unshift_us(uint32_t ch)
{
    switch (ch) {
    case '!': return '1';
    case '@': return '2';
    case '#': return '3';
    case '$': return '4';
    case '%': return '5';
    case '^': return '6';
    case '&': return '7';
    case '*': return '8';
    case '(': return '9';
    case ')': return '0';
    case '_': return '-';
    case '+': return '=';
    case '{': return '[';
    case '}': return ']';
    case '|': return '\\';
    case ':': return ';';
    case '"': return '\'';
    case '<': return ',';
    case '>': return '.';
    case '?': return '/';
    case '~': return '`';
    default:
        if (ch >= 'A' && ch <= 'Z') return ch + 32; /* RETROK_a..RETROK_z */
        return ch;
    }
}

int apple2_key_from_event(uint32_t keysym, uint32_t unicode, int ctrl_down,
                          int shift_down, unsigned *keycode_out,
                          uint32_t *char_out, uint16_t *mods_out)
{
    unsigned keycode;
    uint32_t ch;
    uint16_t mods = RKMOD_NONE;

    if (ctrl_down) mods |= RKMOD_CTRL;
    if (shift_down) mods |= RKMOD_SHIFT;

    keycode = special_keycode(keysym);
    if (keycode) {
        ch = 0;
    } else {
        /* Printable path. Prefer the frontend's translated character; fall
         * back to the keysym, which for ASCII printables IS the character. */
        ch = unicode ? unicode : keysym;
        if (ch < 0x20 || ch > 0x7E) return 0; /* not a key the II has */
        keycode = (unsigned)unshift_us(ch);
        if (ch == ' ') keycode = RK_SPACE;
        /* Ctrl combos: the core resolves them from the keycode, and a stray
         * control character would be interpreted as text. */
        if (ctrl_down) ch = 0;
    }

    if (keycode_out) *keycode_out = keycode;
    if (char_out) *char_out = ch;
    if (mods_out) *mods_out = mods;
    return 1;
}

float apple2_paddle_from_axis(float v, float deadzone)
{
    float mag, scaled;

    if (deadzone < 0.0f) deadzone = 0.0f;
    if (deadzone > 0.95f) deadzone = 0.95f;

    mag = v < 0.0f ? -v : v;
    if (mag <= deadzone) return 0.0f;

    /* Rescale the live part of the travel back to a full 0..1 so the paddle
     * still reaches its stops despite the deadzone. */
    scaled = (mag - deadzone) / (1.0f - deadzone);
    if (scaled > 1.0f) scaled = 1.0f;
    return v < 0.0f ? -scaled : scaled;
}

/*
 * bindings.c -- see bindings.h.
 *
 * Two tables: s_defaults (computed once, never mutated) and s_table (live).
 * Diffing them makes persistence (only non-default entries written) and reset
 * trivial. Ported from the ADAM and Astrocade desktops' bindings.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bindings.h"
#include "hid_keys.h"

typedef struct {
    uint32_t keysym;   /* 0 = unbound */
    int pad;           /* pad input id, -1 = unbound */
} slot;

static slot s_defaults[APPLE2_TARGET_COUNT];
static slot s_table[APPLE2_TARGET_COUNT];
static int  s_ready;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

#define PAD_AXIS_LEFTX (APPLE2_PAD_AXIS_BASE + 0)

/* The pad scopes of a target: -1 reaches every pad (system actions), SCOPE_JOY
 * is the joystick's pad, 0 and 1 are a paddle's pad. Two targets only compete
 * for a pad input when their scopes meet, so the joystick and the paddles can
 * each use South as a button, and so can both paddles. */
#define SCOPE_JOY 100

/* The physical key, so "5" is the key that prints 5 whatever Shift or NumLock
 * does. The joystick sits on the numeric keypad, where a laptop's arrow-less
 * keyboards can still reach it; the arrow keys stay the Apple II's own. */
static void compute_defaults(void)
{
    for (int t = 0; t < APPLE2_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].pad = -1;
    }

    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_LEFT)].keysym = APPLE2SESSION_KEYSYM_KP_4;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_RIGHT)].keysym = APPLE2SESSION_KEYSYM_KP_6;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_UP)].keysym = APPLE2SESSION_KEYSYM_KP_8;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_DOWN)].keysym = APPLE2SESSION_KEYSYM_KP_2;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_BTN0)].keysym = APPLE2SESSION_KEYSYM_KP_0;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_BTN1)].keysym = APPLE2SESSION_KEYSYM_KP_ENTER;
    /* the D-pad steers to the stops, South is button 0 (Open Apple) and East
     * button 1 (Closed Apple); the left stick always steers proportionally and
     * an unbound West / North stand in for South / East (gamepad_sdl.c) */
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_LEFT)].pad = APPLE2_PAD_BTN_DPAD_LEFT;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_RIGHT)].pad = APPLE2_PAD_BTN_DPAD_RIGHT;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_UP)].pad = APPLE2_PAD_BTN_DPAD_UP;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_DOWN)].pad = APPLE2_PAD_BTN_DPAD_DOWN;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_BTN0)].pad = APPLE2_PAD_BTN_SOUTH;
    s_defaults[APPLE2_TARGET_JOY(APPLE2_ACT_BTN1)].pad = APPLE2_PAD_BTN_EAST;

    /* paddle 0 on the keypad's 7 9 5, paddle 1 on its 1 3 and '.' */
    s_defaults[APPLE2_TARGET_PADDLE(0, APPLE2_PACT_CCW)].keysym = APPLE2SESSION_KEYSYM_KP_7;
    s_defaults[APPLE2_TARGET_PADDLE(0, APPLE2_PACT_CW)].keysym = APPLE2SESSION_KEYSYM_KP_9;
    s_defaults[APPLE2_TARGET_PADDLE(0, APPLE2_PACT_BUTTON)].keysym = APPLE2SESSION_KEYSYM_KP_5;
    s_defaults[APPLE2_TARGET_PADDLE(1, APPLE2_PACT_CCW)].keysym = APPLE2SESSION_KEYSYM_KP_1;
    s_defaults[APPLE2_TARGET_PADDLE(1, APPLE2_PACT_CW)].keysym = APPLE2SESSION_KEYSYM_KP_3;
    s_defaults[APPLE2_TARGET_PADDLE(1, APPLE2_PACT_BUTTON)].keysym = APPLE2SESSION_KEYSYM_KP_PERIOD;
    /* the D-pad ramps the knob, South is its button, the left stick's X sets
     * it (a pad plays one paddle, so both paddles use the same inputs) */
    for (int n = 0; n < APPLE2SESSION_PADDLES; n++) {
        s_defaults[APPLE2_TARGET_PADDLE(n, APPLE2_PACT_CCW)].pad = APPLE2_PAD_BTN_DPAD_LEFT;
        s_defaults[APPLE2_TARGET_PADDLE(n, APPLE2_PACT_CW)].pad = APPLE2_PAD_BTN_DPAD_RIGHT;
        s_defaults[APPLE2_TARGET_PADDLE(n, APPLE2_PACT_BUTTON)].pad = APPLE2_PAD_BTN_SOUTH;
        s_defaults[APPLE2_TARGET_PADDLE(n, APPLE2_PACT_KNOB)].pad = PAD_AXIS_LEFTX;
    }
}

static const char *const k_joy_label[APPLE2_JOY_ACTS] = {
    "Left", "Right", "Up", "Down", "Button 0", "Button 1",
};
static const char *const k_sysact_label[APPLE2SESSION_SYSACT_COUNT] = {
    "Reset", "Power Cycle", "Stop in Debugger",
};
static const char *const k_paddle_label[APPLE2_PADDLE_ACTS] = {
    "Counter-clockwise", "Clockwise", "Button", "Knob",
};

static int is_joy_target(int t) { return t >= 0 && t < APPLE2_JOY_ACTS; }
static int is_sysact_target(int t)
{
    return t >= APPLE2_TARGET_SYSACT_BASE && t < APPLE2_TARGET_PADDLES;
}
static int is_paddle_target(int t)
{
    return t >= APPLE2_TARGET_PADDLES && t < APPLE2_TARGET_COUNT;
}
static int is_knob_target(int t)
{
    return is_paddle_target(t) &&
           (t - APPLE2_TARGET_PADDLES) % APPLE2_PADDLE_ACTS == APPLE2_PACT_KNOB;
}

static int scope_of(int target)
{
    if (is_joy_target(target)) return SCOPE_JOY;
    if (is_paddle_target(target))
        return (target - APPLE2_TARGET_PADDLES) / APPLE2_PADDLE_ACTS;
    return -1;
}

static int scopes_meet(int a, int b)
{
    return a == -1 || b == -1 || a == b;
}

int bindings_target_in_mode(int target, int mode)
{
    if (is_sysact_target(target)) return 1;
    if (mode == APPLE2_MODE_PADDLES) return is_paddle_target(target);
    return is_joy_target(target);
}

const char *bindings_target_short_label(int target)
{
    if (is_joy_target(target)) return k_joy_label[target];
    if (is_sysact_target(target)) return k_sysact_label[target - APPLE2_TARGET_SYSACT_BASE];
    if (is_paddle_target(target))
        return k_paddle_label[(target - APPLE2_TARGET_PADDLES) % APPLE2_PADDLE_ACTS];
    return "?";
}

const char *bindings_target_label(int target)
{
    static char buf[APPLE2_TARGET_COUNT][48];
    if (target < 0 || target >= APPLE2_TARGET_COUNT)
        return "?";
    if (!buf[target][0]) {
        if (is_joy_target(target))
            snprintf(buf[target], sizeof buf[target], "Joystick %s",
                     bindings_target_short_label(target));
        else if (is_sysact_target(target))
            snprintf(buf[target], sizeof buf[target], "%s",
                     bindings_target_short_label(target));
        else
            snprintf(buf[target], sizeof buf[target], "Paddle %d %s",
                     (target - APPLE2_TARGET_PADDLES) / APPLE2_PADDLE_ACTS,
                     bindings_target_short_label(target));
    }
    return buf[target];
}

static void ensure_ready(void)
{
    if (s_ready)
        return;
    compute_defaults();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_ready = 1;
}

/* ---- persistence: "<target>.k:<keysym>" / ".p:<pad id>", space-separated -- */

static void persist(apple2session *s)
{
    char buf[2048];
    size_t n = 0;
    buf[0] = '\0';
    for (int t = 0; t < APPLE2_TARGET_COUNT && n + 48 < sizeof buf; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%d.k:%u",
                                  n ? " " : "", t, s_table[t].keysym);
        if (n + 48 < sizeof buf && s_table[t].pad != s_defaults[t].pad)
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%d.p:%d",
                                  n ? " " : "", t, s_table[t].pad);
    }
    apple2session_set_str(s, "bindings", buf);
}

/* F9, F11 and F12 belong to the app (the Controllers window, full screen, the
 * debugger); Alt, Meta and Super are the Open and Closed Apple keys, which the
 * core hard-wires to buttons 0 and 1 (input_map.c). Never bindable. */
static int keysym_reserved(uint32_t keysym)
{
    switch (keysym) {
    case APPLE2SESSION_KEYSYM_F9:
    case APPLE2SESSION_KEYSYM_F11:
    case APPLE2SESSION_KEYSYM_F12:
    case APPLE2SESSION_KEYSYM_LALT:
    case APPLE2SESSION_KEYSYM_RALT:
    case APPLE2SESSION_KEYSYM_LMETA:
    case APPLE2SESSION_KEYSYM_RMETA:
    case APPLE2SESSION_KEYSYM_LSUPER:
    case APPLE2SESSION_KEYSYM_RSUPER:
        return 1;
    default:
        return 0;
    }
}

static void load_overrides(apple2session *s)
{
    const char *str = apple2session_get_str(s, "bindings", "");
    uint8_t key_set[APPLE2_TARGET_COUNT] = { 0 }, pad_set[APPLE2_TARGET_COUNT] = { 0 };

    for (const char *p = str; p && *p; ) {
        int target = 0, consumed = 0;
        char kind = 0;
        long val = 0;
        if (sscanf(p, "%d.%c:%ld%n", &target, &kind, &val, &consumed) == 3 &&
            target >= 0 && target < APPLE2_TARGET_COUNT) {
            if (kind == 'k') {
                if (!keysym_reserved((uint32_t)val)) {  /* a reserved one keeps the default */
                    s_table[target].keysym = (uint32_t)val;
                    key_set[target] = 1;
                }
            } else if (kind == 'p') {
                /* a knob follows an axis, every other target a button */
                if (val < 0 || APPLE2_PAD_IS_AXIS((int)val) == is_knob_target(target)) {
                    s_table[target].pad = (int)val;
                    pad_set[target] = 1;
                }
            }
        }
        const char *next = consumed > 0 ? p + consumed : p + 1;
        while (*next == ' ' || *next == ';') next++;
        p = next;
    }

    /* A binding the file sets displaces the DEFAULT holder of the same input
     * (a default added since the file was written, say): one input, one
     * target, as the Map window keeps it. */
    for (int t = 0; t < APPLE2_TARGET_COUNT; t++) {
        for (int u = 0; u < APPLE2_TARGET_COUNT; u++) {
            if (u == t) continue;
            if (key_set[t] && !key_set[u] && s_table[t].keysym &&
                s_table[u].keysym == s_table[t].keysym)
                s_table[u].keysym = 0;
            if (pad_set[t] && !pad_set[u] && s_table[t].pad >= 0 &&
                s_table[u].pad == s_table[t].pad &&
                scopes_meet(scope_of(t), scope_of(u)))
                s_table[u].pad = -1;
        }
    }
}

void bindings_init(apple2session *s)
{
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    memcpy(s_table, s_defaults, sizeof s_table);
    load_overrides(s);
    pthread_mutex_unlock(&s_mtx);
}

int bindings_target_for_key(uint32_t keysym)
{
    int r = -1;
    if (!keysym || keysym_reserved(keysym)) return -1;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    for (int t = 0; t < APPLE2_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) { r = t; break; }
    pthread_mutex_unlock(&s_mtx);
    return r;
}

int bindings_target_for_pad(int mode, int port, int id)
{
    int r = -1;
    if (id < 0) return -1;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    for (int t = 0; t < APPLE2_TARGET_COUNT; t++) {
        if (s_table[t].pad != id || !bindings_target_in_mode(t, mode)) continue;
        const int sc = scope_of(t);
        if (sc == -1 || (mode == APPLE2_MODE_PADDLES ? sc == port
                                                      : port == 0)) {
            r = t;
            break;
        }
    }
    pthread_mutex_unlock(&s_mtx);
    return r;
}

int bindings_knob_axis(int paddle)
{
    int r = -1;
    if (paddle < 0 || paddle >= APPLE2SESSION_PADDLES) return -1;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    r = s_table[APPLE2_TARGET_PADDLE(paddle, APPLE2_PACT_KNOB)].pad;
    pthread_mutex_unlock(&s_mtx);
    return r >= APPLE2_PAD_AXIS_BASE ? r : -1;
}

uint32_t bindings_target_keysym(int target)
{
    uint32_t r = 0;
    if (target < 0 || target >= APPLE2_TARGET_COUNT) return 0;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    r = s_table[target].keysym;
    pthread_mutex_unlock(&s_mtx);
    return r;
}

int bindings_target_pad(int target)
{
    int r = -1;
    if (target < 0 || target >= APPLE2_TARGET_COUNT) return -1;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    r = s_table[target].pad;
    pthread_mutex_unlock(&s_mtx);
    return r;
}

void bindings_set_keysym(apple2session *s, int target, uint32_t keysym, int *stolen)
{
    if (stolen) *stolen = -1;
    if (target < 0 || target >= APPLE2_TARGET_COUNT) return;
    if (keysym_reserved(keysym)) return;       /* the app's and the Apple keys' */
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    if (keysym)
        for (int t = 0; t < APPLE2_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                if (stolen) *stolen = t;
            }
    s_table[target].keysym = keysym;
    persist(s);
    pthread_mutex_unlock(&s_mtx);
}

void bindings_set_pad(apple2session *s, int target, int id, int *stolen)
{
    if (stolen) *stolen = -1;
    if (target < 0 || target >= APPLE2_TARGET_COUNT) return;
    /* a knob follows an axis, every other target takes a button */
    if (id >= 0 && APPLE2_PAD_IS_AXIS(id) != is_knob_target(target)) return;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    if (id >= 0)
        for (int t = 0; t < APPLE2_TARGET_COUNT; t++) {
            if (t == target || s_table[t].pad != id) continue;
            if (scopes_meet(scope_of(t), scope_of(target))) {
                s_table[t].pad = -1;
                if (stolen) *stolen = t;
            }
        }
    s_table[target].pad = id;
    persist(s);
    pthread_mutex_unlock(&s_mtx);
}

void bindings_clear(apple2session *s, int target)
{
    if (target < 0 || target >= APPLE2_TARGET_COUNT) return;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    s_table[target].keysym = 0;
    s_table[target].pad = -1;
    persist(s);
    pthread_mutex_unlock(&s_mtx);
}

void bindings_reset_defaults(apple2session *s)
{
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    memcpy(s_table, s_defaults, sizeof s_table);
    persist(s);
    pthread_mutex_unlock(&s_mtx);
}

int apple2session_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case APPLE2SESSION_KEYSYM_UP: name = "Up"; break;
    case APPLE2SESSION_KEYSYM_DOWN: name = "Down"; break;
    case APPLE2SESSION_KEYSYM_LEFT: name = "Left"; break;
    case APPLE2SESSION_KEYSYM_RIGHT: name = "Right"; break;
    case APPLE2SESSION_KEYSYM_ESCAPE: name = "Escape"; break;
    case APPLE2SESSION_KEYSYM_RETURN: name = "Return"; break;
    case APPLE2SESSION_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case APPLE2SESSION_KEYSYM_TAB: name = "Tab"; break;
    case APPLE2SESSION_KEYSYM_SPACE: name = "Space"; break;
    case APPLE2SESSION_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case APPLE2SESSION_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case APPLE2SESSION_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case APPLE2SESSION_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case APPLE2SESSION_KEYSYM_LALT: name = "Left Alt"; break;
    case APPLE2SESSION_KEYSYM_RALT: name = "Right Alt"; break;
    case APPLE2SESSION_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case APPLE2SESSION_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case APPLE2SESSION_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case APPLE2SESSION_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    case 0xffad: name = "Keypad -"; break;
    case 0xffab: name = "Keypad +"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= APPLE2SESSION_KEYSYM_F1 && keysym <= APPLE2SESSION_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - APPLE2SESSION_KEYSYM_F1 + 1);
    if (keysym >= APPLE2SESSION_KEYSYM_KP_0 && keysym <= APPLE2SESSION_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - APPLE2SESSION_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= APPLE2SESSION_KEYSYM_HID_BASE
        && keysym <= APPLE2SESSION_KEYSYM_HID_BASE + APPLE2SESSION_HID_USAGE_MAX) {
        const char *hid = apple2_hid_usage_name(keysym - APPLE2SESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - APPLE2SESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}

/*
 * input.c -- see input.h, and apple2session.h for the public half.
 *
 * Process-global, like the machine it drives (a process singleton): one
 * mutex, one table of "who holds each target". After every change the union
 * is recomputed and what changed is handed to the machine through
 * apple2session_paddle / apple2session_paddle_button.
 *
 * The machine has two analog inputs (PDL0, PDL1) and two buttons (PB0, PB1).
 * In JOYSTICK mode PDL0/PDL1 are the stick's X and Y: each is the strongest of
 * the digital holds (a key, a D-pad button, a hat: they drive the stop, and a
 * pair held both ways holds neither) and the analog sources (the window's
 * mouse, a pad's left stick). In PADDLES mode PDL0/PDL1 are two knobs, each an
 * absolute position that holds where it was left: the turn keys ramp it, a pad
 * axis, the window's dial or its wheel set it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "bindings.h"
#include "input.h"
#include "session_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define KNOB_RATE 0.002f     /* units per ms: a full sweep (-1 to 1) in a second */
#define KNOB_MIN_MOVE 0.02f  /* a pad axis must move this far to take the knob */
#define HELD_AT 0.5f         /* a direction target reads as held past half deflection */
#define ANA_COUNT (1 + APPLE2_MAX_PADS)   /* analog sources: the pointer, then each pad */

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static apple2session *g_sess;
static atomic_int  g_mode;
static atomic_uint g_mode_gen;
/* who holds each target (APPLE2_SRC_* bits); the system actions are not held */
static uint32_t g_bits[APPLE2_TARGET_COUNT];
/* the analog stick positions of the pointer (slot 0) and of each pad that
 * plays the joystick (1 + owner), +y toward the player */
static float g_ana_x[ANA_COUNT], g_ana_y[ANA_COUNT];
/* the paddle knobs, -1..1 */
static float g_pdl[APPLE2SESSION_PADDLES];
static float g_pad_axis_last[APPLE2_MAX_PADS];
static uint8_t g_pad_axis_seen[APPLE2_MAX_PADS];
/* what the machine is being driven with, and what was last handed to it */
static float g_out_x, g_out_y;
static uint8_t g_out_b[2];
static int g_push_valid;
static float g_pushed_x, g_pushed_y;
static uint8_t g_pushed_b[2];
/* the Open and Closed Apple keys, for the held lights only */
static uint8_t g_alt[2];
/* the keysym that is holding each target (ignores auto-repeat, and a release
 * clears exactly what its press set) */
static uint32_t g_held_keysym[APPLE2_TARGET_COUNT];

static int is_held_target(int target)
{
    return target >= 0 && target < APPLE2_TARGET_COUNT &&
           !(target >= APPLE2_TARGET_SYSACT_BASE && target < APPLE2_TARGET_PADDLES);
}

static float clampf(float val, float lo, float hi)
{
    return val < lo ? lo : val > hi ? hi : val;
}

static int held_locked(int target)
{
    return g_bits[target] != 0;
}

/* The machine's values from the sources. g_mtx held. */
static void compute_locked(float *px, float *py, uint8_t *pb0, uint8_t *pb1)
{
    if (atomic_load(&g_mode) == APPLE2_MODE_PADDLES) {
        *px = g_pdl[0];
        *py = g_pdl[1];
        *pb0 = held_locked(APPLE2_TARGET_PADDLE(0, APPLE2_PACT_BUTTON));
        *pb1 = held_locked(APPLE2_TARGET_PADDLE(1, APPLE2_PACT_BUTTON));
        return;
    }
    float xval = (float)(held_locked(APPLE2_TARGET_JOY(APPLE2_ACT_RIGHT)) -
                         held_locked(APPLE2_TARGET_JOY(APPLE2_ACT_LEFT)));
    float yval = (float)(held_locked(APPLE2_TARGET_JOY(APPLE2_ACT_DOWN)) -
                         held_locked(APPLE2_TARGET_JOY(APPLE2_ACT_UP)));
    for (int src = 0; src < ANA_COUNT; src++) {
        if (fabsf(g_ana_x[src]) > fabsf(xval)) xval = g_ana_x[src];
        if (fabsf(g_ana_y[src]) > fabsf(yval)) yval = g_ana_y[src];
    }
    *px = xval;
    *py = yval;
    *pb0 = held_locked(APPLE2_TARGET_JOY(APPLE2_ACT_BTN0));
    *pb1 = held_locked(APPLE2_TARGET_JOY(APPLE2_ACT_BTN1));
}

/* Recompute and tell the machine what changed (everything when forced). */
static void push_locked(int force)
{
    float xval, yval;
    uint8_t btn0, btn1;
    compute_locked(&xval, &yval, &btn0, &btn1);
    g_out_x = xval;
    g_out_y = yval;
    g_out_b[0] = btn0;
    g_out_b[1] = btn1;
    if (!g_sess) return;
    force = force || !g_push_valid;
    if (force || xval != g_pushed_x || yval != g_pushed_y) {
        g_pushed_x = xval;
        g_pushed_y = yval;
        apple2session_paddle(g_sess, xval, yval);
    }
    if (force || btn0 != g_pushed_b[0]) {
        g_pushed_b[0] = btn0;
        apple2session_paddle_button(g_sess, 0, btn0);
    }
    if (force || btn1 != g_pushed_b[1]) {
        g_pushed_b[1] = btn1;
        apple2session_paddle_button(g_sess, 1, btn1);
    }
    g_push_valid = 1;
}

static void source_locked(int target, uint32_t src, int down)
{
    if (!is_held_target(target)) return;
    if (down) g_bits[target] |= src; else g_bits[target] &= ~src;
}

static void clear_sources_locked(uint32_t mask)
{
    for (int t = 0; t < APPLE2_TARGET_COUNT; t++)
        g_bits[t] &= ~mask;
}

static int knob_dir_locked(int paddle)
{
    const int ccw = held_locked(APPLE2_TARGET_PADDLE(paddle, APPLE2_PACT_CCW));
    const int cw = held_locked(APPLE2_TARGET_PADDLE(paddle, APPLE2_PACT_CW));
    return cw - ccw;
}

/* ---- attach, mode ------------------------------------------------------------- */

static void reset_state_locked(void)
{
    memset(g_bits, 0, sizeof g_bits);
    memset(g_ana_x, 0, sizeof g_ana_x);
    memset(g_ana_y, 0, sizeof g_ana_y);
    memset(g_pdl, 0, sizeof g_pdl);
    memset(g_pad_axis_seen, 0, sizeof g_pad_axis_seen);
    memset(g_held_keysym, 0, sizeof g_held_keysym);
}

void apple2_input_attach(apple2session *sess)
{
    const int mode = apple2session_get_int(sess, "controller_mode", APPLE2_MODE_JOYSTICK);
    pthread_mutex_lock(&g_mtx);
    g_sess = sess;
    reset_state_locked();
    memset(g_alt, 0, sizeof g_alt);
    atomic_store(&g_mode, mode == APPLE2_MODE_PADDLES ? APPLE2_MODE_PADDLES
                                                      : APPLE2_MODE_JOYSTICK);
    g_push_valid = 0;
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void apple2_input_detach(apple2session *sess)
{
    pthread_mutex_lock(&g_mtx);
    if (g_sess == sess) g_sess = NULL;
    pthread_mutex_unlock(&g_mtx);
}

int apple2_input_mode(void)
{
    return atomic_load(&g_mode);
}

unsigned apple2_input_mode_generation(void)
{
    return atomic_load(&g_mode_gen);
}

void apple2_input_set_mode(int mode)
{
    if (mode != APPLE2_MODE_PADDLES) mode = APPLE2_MODE_JOYSTICK;
    pthread_mutex_lock(&g_mtx);
    if (mode != atomic_load(&g_mode)) {
        reset_state_locked();
        atomic_store(&g_mode, mode);
        atomic_fetch_add(&g_mode_gen, 1);
        push_locked(1);
    }
    pthread_mutex_unlock(&g_mtx);
}

/* ---- public: the window's mouse ----------------------------------------------- */

void apple2session_press(apple2session *s, int target, int down)
{
    (void)s;
    if (!is_held_target(target)) return;
    pthread_mutex_lock(&g_mtx);
    source_locked(target, APPLE2_SRC_POINTER, down != 0);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void apple2session_pointer_set(apple2session *s, float xval, float yval)
{
    (void)s;
    pthread_mutex_lock(&g_mtx);
    g_ana_x[0] = clampf(xval, -1.0f, 1.0f);
    g_ana_y[0] = clampf(yval, -1.0f, 1.0f);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void apple2session_pointer_release(apple2session *s)
{
    (void)s;
    pthread_mutex_lock(&g_mtx);
    clear_sources_locked(APPLE2_SRC_POINTER);
    g_ana_x[0] = g_ana_y[0] = 0.0f;
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

int apple2session_target_held(apple2session *s, int target)
{
    int held = 0;
    (void)s;
    if (!is_held_target(target)) return 0;
    pthread_mutex_lock(&g_mtx);
    if (atomic_load(&g_mode) == APPLE2_MODE_JOYSTICK && target < APPLE2_JOY_ACTS) {
        switch (target) {
        case APPLE2_ACT_LEFT:  held = g_out_x <= -HELD_AT; break;
        case APPLE2_ACT_RIGHT: held = g_out_x >= HELD_AT; break;
        case APPLE2_ACT_UP:    held = g_out_y <= -HELD_AT; break;
        case APPLE2_ACT_DOWN:  held = g_out_y >= HELD_AT; break;
        case APPLE2_ACT_BTN0:  held = g_out_b[0] || g_alt[0]; break;
        case APPLE2_ACT_BTN1:  held = g_out_b[1] || g_alt[1]; break;
        default: break;
        }
    } else if (atomic_load(&g_mode) == APPLE2_MODE_PADDLES &&
               target >= APPLE2_TARGET_PADDLES) {
        const int rel = target - APPLE2_TARGET_PADDLES;
        const int pdl = rel / APPLE2_PADDLE_ACTS, act = rel % APPLE2_PADDLE_ACTS;
        if (act == APPLE2_PACT_CCW)
            held = knob_dir_locked(pdl) < 0;
        else if (act == APPLE2_PACT_CW)
            held = knob_dir_locked(pdl) > 0;
        else if (act == APPLE2_PACT_BUTTON)
            held = g_out_b[pdl] || g_alt[pdl];
    }
    pthread_mutex_unlock(&g_mtx);
    return held;
}

void apple2session_axes_get(apple2session *s, float *pdl0, float *pdl1)
{
    (void)s;
    pthread_mutex_lock(&g_mtx);
    if (pdl0) *pdl0 = g_out_x;
    if (pdl1) *pdl1 = g_out_y;
    pthread_mutex_unlock(&g_mtx);
}

void apple2session_release_all(apple2session *s)
{
    (void)s;
    pthread_mutex_lock(&g_mtx);
    clear_sources_locked(APPLE2_SRC_KEYBOARD);
    memset(g_held_keysym, 0, sizeof g_held_keysym);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void apple2_input_apple_key(int button, int down)
{
    if (button < 0 || button > 1) return;
    pthread_mutex_lock(&g_mtx);
    g_alt[button] = down != 0;
    pthread_mutex_unlock(&g_mtx);
}

/* ---- public: the keyboard ----------------------------------------------------- */

int apple2session_input_key(apple2session *s, uint32_t keysym, int down,
                            int ctrl_down)
{
    const int mode = atomic_load(&g_mode);
    int consumed = 0;
    if (down) {
        const int t = ctrl_down ? -1 : bindings_target_for_key(keysym);
        if (t >= 0 && bindings_target_in_mode(t, mode)) {
            consumed = 1;
            pthread_mutex_lock(&g_mtx);
            if (g_held_keysym[t] != keysym) {     /* not an auto-repeat */
                g_held_keysym[t] = keysym;
                if (!is_held_target(t)) {
                    pthread_mutex_unlock(&g_mtx);
                    apple2session_sysaction_post(
                        s, (apple2session_sysaction)(t - APPLE2_TARGET_SYSACT_BASE));
                    return 1;
                }
                source_locked(t, APPLE2_SRC_KEYBOARD, 1);
                push_locked(0);
            }
            pthread_mutex_unlock(&g_mtx);
        }
    } else {
        /* a release clears exactly what the press set, whatever has been
         * rebound or pressed (Ctrl) since */
        pthread_mutex_lock(&g_mtx);
        for (int t = 0; t < APPLE2_TARGET_COUNT; t++)
            if (g_held_keysym[t] == keysym && keysym) {
                g_held_keysym[t] = 0;
                consumed = 1;
                if (is_held_target(t)) {
                    source_locked(t, APPLE2_SRC_KEYBOARD, 0);
                    push_locked(0);
                }
            }
        pthread_mutex_unlock(&g_mtx);
        if (!consumed) {
            const int t = bindings_target_for_key(keysym);
            if (t >= 0 && bindings_target_in_mode(t, mode))
                consumed = 1;
        }
    }
    return consumed;
}

void apple2session_sysaction_drain(apple2session *s)
{
    unsigned pending = apple2session_sysaction_take(s);
    for (int a = 0; a < APPLE2SESSION_SYSACT_COUNT; a++)
        if (pending & (1u << a))
            apple2session_sysaction_fire(s, (apple2session_sysaction)a);
}

/* ---- public: the paddle knobs --------------------------------------------------- */

void apple2session_knob_set(apple2session *s, int paddle, float value)
{
    (void)s;
    if (paddle < 0 || paddle >= APPLE2SESSION_PADDLES) return;
    pthread_mutex_lock(&g_mtx);
    g_pdl[paddle] = clampf(value, -1.0f, 1.0f);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

float apple2session_knob_get(apple2session *s, int paddle)
{
    float val;
    (void)s;
    if (paddle < 0 || paddle >= APPLE2SESSION_PADDLES) return 0.0f;
    pthread_mutex_lock(&g_mtx);
    val = g_pdl[paddle];
    pthread_mutex_unlock(&g_mtx);
    return val;
}

void apple2session_knob_nudge(apple2session *s, int paddle, float delta)
{
    (void)s;
    if (paddle < 0 || paddle >= APPLE2SESSION_PADDLES) return;
    pthread_mutex_lock(&g_mtx);
    g_pdl[paddle] = clampf(g_pdl[paddle] + delta, -1.0f, 1.0f);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

/* The dial: 270 degrees, -1 at the lower left (-135), +1 at the lower right
 * (+135), 0 straight up. */
double apple2session_knob_angle(float value)
{
    return (double)clampf(value, -1.0f, 1.0f) * 135.0;
}

int apple2session_knob_from_point(double dx, double dy, double radius, float *value)
{
    if (radius <= 0 || sqrt(dx * dx + dy * dy) < radius * 0.2)
        return 0;
    double deg = atan2(dx, -dy) * 180.0 / M_PI;      /* 0 = up, clockwise */
    if (deg > 135.0) deg = 135.0;
    if (deg < -135.0) deg = -135.0;
    if (value) *value = (float)(deg / 135.0);
    return 1;
}

/* ---- pad side ----------------------------------------------------------------- */

void apple2_input_pad_press(int owner, int target, int down)
{
    if (owner < 0 || owner >= APPLE2_MAX_PADS || !is_held_target(target)) return;
    pthread_mutex_lock(&g_mtx);
    source_locked(target, APPLE2_SRC_PAD(owner), down != 0);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void apple2_input_pad_steer(int owner, int port, int act, int down)
{
    int target = -1;
    if (owner < 0 || owner >= APPLE2_MAX_PADS || act < 0 || act > APPLE2_ACT_DOWN)
        return;
    pthread_mutex_lock(&g_mtx);
    if (atomic_load(&g_mode) == APPLE2_MODE_PADDLES) {
        if (port >= 0 && port < APPLE2SESSION_PADDLES) {
            if (act == APPLE2_ACT_LEFT)
                target = APPLE2_TARGET_PADDLE(port, APPLE2_PACT_CCW);
            else if (act == APPLE2_ACT_RIGHT)
                target = APPLE2_TARGET_PADDLE(port, APPLE2_PACT_CW);
        }
    } else {
        target = APPLE2_TARGET_JOY(act);
    }
    if (target >= 0) {
        source_locked(target, APPLE2_SRC_STICK(owner), down != 0);
        push_locked(0);
    }
    pthread_mutex_unlock(&g_mtx);
}

void apple2_input_pad_analog(int owner, float xval, float yval)
{
    if (owner < 0 || owner >= APPLE2_MAX_PADS) return;
    pthread_mutex_lock(&g_mtx);
    g_ana_x[1 + owner] = clampf(xval, -1.0f, 1.0f);
    g_ana_y[1 + owner] = clampf(yval, -1.0f, 1.0f);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void apple2_input_pad_knob(int owner, int paddle, float value)
{
    if (owner < 0 || owner >= APPLE2_MAX_PADS ||
        paddle < 0 || paddle >= APPLE2SESSION_PADDLES) return;
    value = clampf(value, -1.0f, 1.0f);
    pthread_mutex_lock(&g_mtx);
    if (!g_pad_axis_seen[owner]) {
        g_pad_axis_seen[owner] = 1;          /* the first sample only records */
        g_pad_axis_last[owner] = value;
    } else if (fabsf(value - g_pad_axis_last[owner]) >= KNOB_MIN_MOVE) {
        g_pad_axis_last[owner] = value;
        g_pdl[paddle] = value;
        push_locked(0);
    }
    pthread_mutex_unlock(&g_mtx);
}

void apple2_input_pad_clear(int owner)
{
    if (owner < 0 || owner >= APPLE2_MAX_PADS) return;
    pthread_mutex_lock(&g_mtx);
    clear_sources_locked(APPLE2_SRC_PAD(owner) | APPLE2_SRC_STICK(owner));
    g_ana_x[1 + owner] = g_ana_y[1 + owner] = 0.0f;
    g_pad_axis_seen[owner] = 0;
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void apple2_input_tick(int dt_ms)
{
    pthread_mutex_lock(&g_mtx);
    if (atomic_load(&g_mode) == APPLE2_MODE_PADDLES) {
        for (int pdl = 0; pdl < APPLE2SESSION_PADDLES; pdl++) {
            const int dir = knob_dir_locked(pdl);
            if (!dir) continue;
            g_pdl[pdl] = clampf(g_pdl[pdl] + (float)dir * KNOB_RATE * (float)dt_ms,
                                -1.0f, 1.0f);
        }
        push_locked(0);
    }
    pthread_mutex_unlock(&g_mtx);
}

int apple2_input_knob_active(void)
{
    int on = 0;
    pthread_mutex_lock(&g_mtx);
    if (atomic_load(&g_mode) == APPLE2_MODE_PADDLES)
        for (int pdl = 0; pdl < APPLE2SESSION_PADDLES; pdl++)
            if (knob_dir_locked(pdl)) on = 1;
    pthread_mutex_unlock(&g_mtx);
    return on;
}

void apple2_input_resync(void)
{
    pthread_mutex_lock(&g_mtx);
    push_locked(1);
    pthread_mutex_unlock(&g_mtx);
}

void apple2_input_clear_all(void)
{
    pthread_mutex_lock(&g_mtx);
    reset_state_locked();
    push_locked(1);
    pthread_mutex_unlock(&g_mtx);
}

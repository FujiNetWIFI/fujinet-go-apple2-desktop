/*
 * input_merge_test -- the merged input layer with no machine: every source
 * holds a target by its own bit (the keyboard, the window's mouse, each pad's
 * buttons and stick), the machine sees the union, an analog source and a
 * digital one merge by strongest deflection, opposite holds cancel, and the
 * Open and Closed Apple keys light buttons 0 and 1. Joystick mode drives PDL0
 * and PDL1 as a springing stick, paddles mode as two knobs that hold where they
 * were left and ramp from the turn keys. The key path: the mode decides what
 * a key reaches, Ctrl and the reserved keys are never consumed, a release
 * always clears what its press set, a key on a console action posts it.
 *
 * What the machine is driven with is read back through
 * apple2session_axes_get and apple2session_target_held.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apple2session.h"
#include "bindings.h"
#include "input.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 0.001f)

#define JOY(act) APPLE2_TARGET_JOY(act)
#define PDL(n, act) APPLE2_TARGET_PADDLE(n, act)

static apple2session *g_s;

static void axes(float *xval, float *yval)
{
    apple2session_axes_get(g_s, xval, yval);
}

static int axes_are(float want_x, float want_y)
{
    float xval, yval;
    axes(&xval, &yval);
    return NEAR(xval, want_x) && NEAR(yval, want_y);
}

static int held(int target) { return apple2session_target_held(g_s, target); }

static void joystick(void)
{
    apple2session_pointer_release(g_s);
    apple2session_release_all(g_s);
    apple2session_set_controller_mode(g_s, APPLE2_MODE_JOYSTICK);
}

static void test_joystick(void)
{
    float xval, yval;
    joystick();

    CHECK(apple2session_controller_mode(g_s) == APPLE2_MODE_JOYSTICK, "joystick mode");
    CHECK(apple2session_controller_ports(g_s) == 1, "the joystick has one port");
    CHECK(axes_are(0, 0), "nothing held: the stick is centred");

    /* the mouse's digital hold drives the stop */
    apple2session_press(g_s, JOY(APPLE2_ACT_LEFT), 1);
    CHECK(axes_are(-1, 0) && held(JOY(APPLE2_ACT_LEFT)) && !held(JOY(APPLE2_ACT_RIGHT)), "left: x = -1");
    apple2session_press(g_s, JOY(APPLE2_ACT_DOWN), 1);
    CHECK(axes_are(-1, 1), "and down is +y: a diagonal");
    apple2session_press(g_s, JOY(APPLE2_ACT_LEFT), 0);
    apple2session_press(g_s, JOY(APPLE2_ACT_DOWN), 0);
    CHECK(axes_are(0, 0) && !held(JOY(APPLE2_ACT_LEFT)), "let go: centred");
    apple2session_press(g_s, JOY(APPLE2_ACT_UP), 1);
    CHECK(axes_are(0, -1), "up is -y");
    apple2session_press(g_s, JOY(APPLE2_ACT_UP), 0);

    /* opposite holds from two sources cancel */
    apple2session_press(g_s, JOY(APPLE2_ACT_LEFT), 1);
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_6, 1, 0) == 1, "KP 6 is bound: consumed");
    CHECK(axes_are(0, 0), "left from the mouse and right from the keyboard hold neither");
    apple2session_press(g_s, JOY(APPLE2_ACT_LEFT), 0);
    CHECK(axes_are(1, 0), "the keyboard's right is back once left lets go");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_6, 0, 0) == 1, "its release is consumed");
    CHECK(axes_are(0, 0), "and lets go");

    /* the mouse on the stick is proportional; a digital hold beats it */
    apple2session_pointer_set(g_s, 0.5f, -0.25f);
    CHECK(axes_are(0.5f, -0.25f), "the pointer's stick position is proportional");
    CHECK(!held(JOY(APPLE2_ACT_LEFT)) && held(JOY(APPLE2_ACT_RIGHT)) == 1, "half deflection reads as held");
    apple2session_press(g_s, JOY(APPLE2_ACT_RIGHT), 1);
    CHECK(axes_are(1, -0.25f), "a digital hold drives x to the stop");
    apple2session_press(g_s, JOY(APPLE2_ACT_RIGHT), 0);
    CHECK(axes_are(0.5f, -0.25f), "and the analog position is back");
    apple2session_pointer_set(g_s, 7.0f, -9.0f);
    CHECK(axes_are(1, -1), "a position past the stops is clamped");

    /* a pad's stick: strongest deflection wins across sources */
    apple2session_pointer_set(g_s, 0.5f, 0.0f);
    apple2_input_pad_analog(0, -0.75f, 0.25f);
    CHECK(axes_are(-0.75f, 0.25f), "the pad's stick beats the smaller pointer deflection");
    apple2_input_pad_analog(1, 0.9f, 0.0f);
    CHECK(axes_are(0.9f, 0.25f), "and a second pad's bigger one beats that");
    apple2_input_pad_clear(1);
    CHECK(axes_are(-0.75f, 0.25f), "a pad that goes away lets go of its stick");
    apple2_input_pad_clear(0);
    CHECK(axes_are(0.5f, 0), "and the other pad's too");
    apple2session_pointer_release(g_s);
    CHECK(axes_are(0, 0), "pointer_release centres the pointer's stick");

    /* the buttons: each source, the Apple keys' lights, and a release of one source */
    apple2session_press(g_s, JOY(APPLE2_ACT_BTN0), 1);
    apple2_input_pad_press(0, JOY(APPLE2_ACT_BTN0), 1);
    CHECK(held(JOY(APPLE2_ACT_BTN0)) && !held(JOY(APPLE2_ACT_BTN1)), "button 0 held by two sources");
    apple2session_press(g_s, JOY(APPLE2_ACT_BTN0), 0);
    CHECK(held(JOY(APPLE2_ACT_BTN0)), "the mouse letting go leaves the pad's hold");
    apple2_input_pad_press(0, JOY(APPLE2_ACT_BTN0), 0);
    CHECK(!held(JOY(APPLE2_ACT_BTN0)), "and then it is let go");
    apple2_input_apple_key(1, 1);
    CHECK(held(JOY(APPLE2_ACT_BTN1)) && !held(JOY(APPLE2_ACT_BTN0)), "the Closed Apple key lights button 1");
    apple2_input_apple_key(1, 0);
    CHECK(!held(JOY(APPLE2_ACT_BTN1)), "and lets go");
    apple2session_key(g_s, 1, APPLE2SESSION_KEYSYM_LALT, 0, 0, 0);
    CHECK(held(JOY(APPLE2_ACT_BTN0)), "Alt through apple2session_key lights button 0 (Open Apple)");
    apple2session_key(g_s, 0, APPLE2SESSION_KEYSYM_LALT, 0, 0, 0);
    apple2session_key(g_s, 1, APPLE2SESSION_KEYSYM_RSUPER, 0, 0, 0);
    CHECK(held(JOY(APPLE2_ACT_BTN1)) && !held(JOY(APPLE2_ACT_BTN0)), "Right Super is Closed Apple");
    apple2session_key(g_s, 0, APPLE2SESSION_KEYSYM_RSUPER, 0, 0, 0);
    CHECK(!held(JOY(APPLE2_ACT_BTN1)), "released");

    /* a stick's hat steers, in its own source */
    apple2_input_pad_steer(0, 0, APPLE2_ACT_RIGHT, 1);
    CHECK(axes_are(1, 0), "a hat steers to the stop");
    apple2_input_pad_steer(0, 0, APPLE2_ACT_LEFT, 1);
    CHECK(axes_are(0, 0), "left and right on one hat hold neither");
    apple2_input_pad_clear(0);
    CHECK(axes_are(0, 0), "cleared");
}

static void test_keys(void)
{
    int sys;
    joystick();

    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_8, 1, 0) == 1 && axes_are(0, -1), "KP 8 steers up");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_8, 1, 0) == 1 && axes_are(0, -1),
          "an auto-repeat is consumed and changes nothing");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_8, 0, 0) == 1 && axes_are(0, 0), "its release lets go");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_0, 1, 0) == 1 && held(JOY(APPLE2_ACT_BTN0)), "KP 0 is button 0");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_0, 0, 0) == 1 && !held(JOY(APPLE2_ACT_BTN0)), "released");
    CHECK(apple2session_input_key(g_s, 'a', 1, 0) == 0 && apple2session_input_key(g_s, 'a', 0, 0) == 0,
          "an unbound key is not consumed, press or release");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_LALT, 1, 0) == 0 &&
          apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_RALT, 1, 0) == 0 &&
          apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_LSUPER, 1, 0) == 0 &&
          apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_RMETA, 1, 0) == 0,
          "the Open and Closed Apple keys are never consumed");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_F9, 1, 0) == 0 &&
          apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_F12, 1, 0) == 0, "F9 and F12 are the app's");

    /* the other mode's keys are the Apple II's */
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_7, 1, 0) == 0, "KP 7 (paddle 0) is not consumed in joystick mode");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_7, 0, 0) == 0, "nor its release");

    /* Ctrl: a press is not consumed, a release clears what the press set */
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_4, 1, 1) == 0 && axes_are(0, 0), "Ctrl+KP 4 is not consumed and holds nothing");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_4, 1, 0) == 1 && axes_are(-1, 0), "KP 4 steers left");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_4, 0, 1) == 1 && axes_are(0, 0),
          "its release is taken whatever Ctrl is doing, and lets go");

    /* the keyboard lets go on focus loss; the mouse does not */
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_6, 1, 0);
    apple2session_press(g_s, JOY(APPLE2_ACT_DOWN), 1);
    CHECK(axes_are(1, 1), "a key and the mouse");
    apple2session_release_all(g_s);
    CHECK(axes_are(0, 1) && held(JOY(APPLE2_ACT_DOWN)), "release_all lets go of the keyboard's hold alone");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_6, 0, 0) == 1 && axes_are(0, 1),
          "the late key release is still consumed (no stray key-up reaches the Apple II) and holds nothing");
    apple2session_pointer_release(g_s);
    CHECK(axes_are(0, 0), "then the mouse's");

    /* a key bound to a console action posts it, once */
    bindings_set_keysym(g_s, APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_POWER), 'p', NULL);
    (void)apple2session_sysaction_take(g_s);
    CHECK(apple2session_input_key(g_s, 'p', 1, 0) == 1, "a key on Power Cycle is consumed");
    CHECK(apple2session_input_key(g_s, 'p', 1, 0) == 1, "and so is its repeat");
    sys = (int)apple2session_sysaction_take(g_s);
    CHECK(sys == (1 << APPLE2SESSION_SYSACT_POWER), "it posted Power Cycle, once");
    CHECK(apple2session_input_key(g_s, 'p', 0, 0) == 1 && apple2session_sysaction_take(g_s) == 0, "the release posts nothing");
    apple2session_sysaction_post(g_s, APPLE2SESSION_SYSACT_RESET);
    apple2session_sysaction_post(g_s, APPLE2SESSION_SYSACT_DEBUG_STOP);
    apple2session_sysaction_post(g_s, (apple2session_sysaction)99);
    CHECK(apple2session_sysaction_take(g_s) ==
          ((1u << APPLE2SESSION_SYSACT_RESET) | (1u << APPLE2SESSION_SYSACT_DEBUG_STOP)) &&
          apple2session_sysaction_take(g_s) == 0, "take returns what was posted, once, and ignores a bad action");
    bindings_reset_defaults(g_s);
}

static void test_paddles(void)
{
    float xval, yval;
    joystick();
    apple2session_set_controller_mode(g_s, APPLE2_MODE_PADDLES);
    CHECK(apple2session_controller_mode(g_s) == APPLE2_MODE_PADDLES, "paddles mode");
    CHECK(apple2session_controller_ports(g_s) == 2, "the paddles have two ports");
    CHECK(!strcmp(apple2session_get_str(g_s, "controller_mode", ""), "1"), "the mode is a setting");
    CHECK(axes_are(0, 0), "both knobs start centred");

    /* a knob is absolute and holds where it was left */
    apple2session_knob_set(g_s, 0, 0.5f);
    apple2session_knob_set(g_s, 1, -0.25f);
    CHECK(axes_are(0.5f, -0.25f) && NEAR(apple2session_knob_get(g_s, 0), 0.5f), "knob 0 is PDL0 and knob 1 is PDL1");
    apple2session_knob_nudge(g_s, 0, 0.75f);
    CHECK(NEAR(apple2session_knob_get(g_s, 0), 1.0f), "a nudge past the stop is clamped");
    apple2session_knob_nudge(g_s, 0, -0.5f);
    CHECK(NEAR(apple2session_knob_get(g_s, 0), 0.5f), "and back");
    apple2session_knob_set(g_s, 0, 5.0f);
    apple2session_knob_set(g_s, 1, -5.0f);
    CHECK(axes_are(1, -1), "a position past the stops is clamped");
    apple2session_knob_set(g_s, 7, 1.0f);                /* no such paddle: ignored */
    CHECK(NEAR(apple2session_knob_get(g_s, 7), 0.0f), "no such paddle reads 0");
    apple2session_knob_set(g_s, 0, 0.0f);
    apple2session_knob_set(g_s, 1, 0.0f);

    /* the joystick's keys and the pointer's stick do not move a paddle */
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_8, 1, 0) == 0, "KP 8 (the joystick's) is not consumed");
    apple2session_pointer_set(g_s, 1.0f, 1.0f);
    CHECK(axes_are(0, 0), "the pointer's stick does nothing in paddles mode");
    apple2session_pointer_release(g_s);

    /* the turn keys ramp the knob, and it holds where they leave it */
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_9, 1, 0) == 1, "KP 9 is paddle 0's clockwise");
    CHECK(held(PDL(0, APPLE2_PACT_CW)) && !held(PDL(0, APPLE2_PACT_CCW)) && apple2_input_knob_active(),
          "held, and the pad thread is told to wake");
    apple2_input_tick(100);
    CHECK(NEAR(apple2session_knob_get(g_s, 0), 0.2f), "100 ms turns the knob a tenth of its travel");
    apple2_input_tick(100);
    CHECK(NEAR(apple2session_knob_get(g_s, 0), 0.4f), "and another");
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_9, 0, 0);
    apple2_input_tick(500);
    CHECK(NEAR(apple2session_knob_get(g_s, 0), 0.4f) && !apple2_input_knob_active(), "let go: it stays where it is");
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_7, 1, 0);
    apple2_input_tick(100);
    CHECK(NEAR(apple2session_knob_get(g_s, 0), 0.2f), "KP 7 turns it back");
    apple2_input_tick(5000);
    CHECK(NEAR(apple2session_knob_get(g_s, 0), -1.0f), "to the stop");
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_9, 1, 0);
    apple2_input_tick(100);
    CHECK(NEAR(apple2session_knob_get(g_s, 0), -1.0f), "both held: they cancel");
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_7, 0, 0);
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_9, 0, 0);
    apple2session_knob_set(g_s, 0, 0);

    /* the buttons: paddle n's button is PB n */
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_5, 1, 0);
    CHECK(held(PDL(0, APPLE2_PACT_BUTTON)) && !held(PDL(1, APPLE2_PACT_BUTTON)), "KP 5 is paddle 0's button");
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_5, 0, 0);
    apple2session_press(g_s, PDL(1, APPLE2_PACT_BUTTON), 1);
    CHECK(held(PDL(1, APPLE2_PACT_BUTTON)) && !held(PDL(0, APPLE2_PACT_BUTTON)), "the mouse presses paddle 1's");
    apple2session_press(g_s, PDL(1, APPLE2_PACT_BUTTON), 0);
    apple2_input_apple_key(0, 1);
    CHECK(held(PDL(0, APPLE2_PACT_BUTTON)), "the Open Apple key lights paddle 0's button (PB0)");
    apple2_input_apple_key(0, 0);
    CHECK(!held(JOY(APPLE2_ACT_BTN0)), "and the joystick's targets read nothing in this mode");

    /* a pad axis sets a knob absolutely: the first sample only records */
    apple2_input_pad_knob(0, 1, 0.0f);
    CHECK(NEAR(apple2session_knob_get(g_s, 1), 0.0f), "the first axis sample only records");
    apple2_input_pad_knob(0, 1, 0.005f);
    CHECK(NEAR(apple2session_knob_get(g_s, 1), 0.0f), "noise under the minimum move is ignored");
    apple2_input_pad_knob(0, 1, 0.8f);
    CHECK(NEAR(apple2session_knob_get(g_s, 1), 0.8f), "an axis moves the knob to where it points");
    apple2session_knob_set(g_s, 1, 0.1f);                /* the dial takes it back */
    apple2_input_pad_knob(0, 1, 0.8f);
    CHECK(NEAR(apple2session_knob_get(g_s, 1), 0.1f), "a still axis does not fight the dial");
    apple2_input_pad_clear(0);

    /* a hat turns the knob by its keys */
    apple2_input_pad_steer(0, 1, APPLE2_ACT_RIGHT, 1);
    CHECK(held(PDL(1, APPLE2_PACT_CW)), "a hat's right is paddle 1's clockwise");
    apple2_input_pad_steer(0, 1, APPLE2_ACT_UP, 1);
    CHECK(!held(PDL(1, APPLE2_PACT_CCW)), "and up does nothing");
    apple2_input_pad_clear(0);
    CHECK(!held(PDL(1, APPLE2_PACT_CW)), "a pad that goes away lets go");

    /* the mode switch lets go of everything and centres */
    apple2session_knob_set(g_s, 0, 0.9f);
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_5, 1, 0);
    apple2session_set_controller_mode(g_s, APPLE2_MODE_JOYSTICK);
    CHECK(axes_are(0, 0) && !held(JOY(APPLE2_ACT_BTN0)), "switching to the joystick centres the machine and lets go");
    CHECK(NEAR(apple2session_knob_get(g_s, 0), 0.0f), "the knob is centred too");
    CHECK(apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_5, 0, 0) == 0, "the held key's late release is harmless");
    CHECK(apple2_input_mode_generation() > 0, "the mode generation moved");
    xval = yval = 7.0f;
    axes(&xval, &yval);
    CHECK(NEAR(xval, 0) && NEAR(yval, 0), "still centred");
}

static void test_dial(void)
{
    float value = 99.0f;
    CHECK(apple2session_knob_from_point(0, -10, 10, &value) && NEAR(value, 0.0f), "straight up is 0");
    CHECK(apple2session_knob_from_point(10, 0, 10, &value) && NEAR(value, 90.0f / 135.0f), "right is 90 degrees");
    CHECK(apple2session_knob_from_point(10, 10, 10, &value) && NEAR(value, 1.0f), "lower right is the clockwise stop");
    CHECK(apple2session_knob_from_point(-10, 10, 10, &value) && NEAR(value, -1.0f), "lower left is the counter-clockwise stop");
    CHECK(apple2session_knob_from_point(0, 10, 10, &value) && NEAR(value, 1.0f), "straight down clamps to a stop");
    CHECK(!apple2session_knob_from_point(1, 1, 10, &value), "the dead centre reports nothing");
    CHECK(!apple2session_knob_from_point(5, 5, 0, &value), "no radius reports nothing");
    CHECK(NEAR((float)apple2session_knob_angle(0.0f), 0.0f) && NEAR((float)apple2session_knob_angle(1.0f), 135.0f) &&
          NEAR((float)apple2session_knob_angle(-1.0f), -135.0f) && NEAR((float)apple2session_knob_angle(9.0f), 135.0f),
          "the dial sweeps 270 degrees, clamped");
}

static void test_model_and_mode_persist(const apple2session_paths *paths)
{
    apple2session *s2;
    CHECK(apple2session_controller_model(g_s) == APPLE2_MODEL_IIE, "the joystick model defaults to the IIe");
    apple2session_set_controller_model(g_s, APPLE2_MODEL_IIC);
    CHECK(apple2session_controller_model(g_s) == APPLE2_MODEL_IIC, "the IIc joystick");
    apple2session_set_controller_model(g_s, 77);
    CHECK(apple2session_controller_model(g_s) == APPLE2_MODEL_IIE, "an unknown model is the IIe");
    apple2session_set_controller_model(g_s, APPLE2_MODEL_IIC);
    apple2session_set_controller_mode(g_s, APPLE2_MODE_PADDLES);
    apple2session_free(g_s);
    s2 = apple2session_new(paths);
    g_s = s2;
    CHECK(s2 && apple2session_controller_model(s2) == APPLE2_MODEL_IIC, "the model persists");
    CHECK(s2 && apple2session_controller_mode(s2) == APPLE2_MODE_PADDLES, "and so does the mode");
    apple2session_set_controller_mode(s2, 12);
    CHECK(apple2session_controller_mode(s2) == APPLE2_MODE_JOYSTICK, "an unknown mode is the joystick");
}

int main(void)
{
    char tmpl[512];
    test_tmpl(tmpl, sizeof tmpl, "apple2_input_merge_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    apple2session_paths paths = { cfg, data };

    g_s = apple2session_new(&paths);
    if (!g_s) return 1;

    test_joystick();
    test_keys();
    test_paddles();
    test_dial();
    test_model_and_mode_persist(&paths);

    apple2session_free(g_s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "input_merge_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/*
 * bindings_test -- the remappable binding table: defaults resolve, the mode
 * decides which targets a key or pad input reaches, a remap steals the input
 * from whoever held it, an override persists across a reopen, pad inputs are
 * scoped per port (the joystick, each paddle), the Apple keys and F9/F11/F12
 * can never be bound, and reset restores the defaults.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apple2session.h"
#include "bindings.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

#define JOY(act) APPLE2_TARGET_JOY(act)
#define PDL(n, act) APPLE2_TARGET_PADDLE(n, act)
#define SYS(a) APPLE2_TARGET_SYSACT(a)

int main(void)
{
    char tmpl[512];
    test_tmpl(tmpl, sizeof tmpl, "apple2_bindings_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    apple2session_paths paths = { cfg, data };

    apple2session *s = apple2session_new(&paths);
    if (!s) return 1;

    int stole = -99;

    /* the target space */
    CHECK(APPLE2_TARGET_COUNT == 6 + 3 + 2 * 4, "6 joystick + 3 system + 2 x 4 paddle targets");
    CHECK(APPLE2_TARGET_SYSACT(0) == 6 && APPLE2_TARGET_PADDLES == 9, "the target layout");

    /* default keys: the joystick on the keypad's 8 2 4 6 with 0 and Enter */
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_8) == JOY(APPLE2_ACT_UP), "KP 8 -> up");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_2) == JOY(APPLE2_ACT_DOWN), "KP 2 -> down");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_4) == JOY(APPLE2_ACT_LEFT), "KP 4 -> left");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_6) == JOY(APPLE2_ACT_RIGHT), "KP 6 -> right");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_0) == JOY(APPLE2_ACT_BTN0), "KP 0 -> button 0");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_ENTER) == JOY(APPLE2_ACT_BTN1), "KP Enter -> button 1");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_7) == PDL(0, APPLE2_PACT_CCW), "KP 7 -> paddle 0 CCW");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_9) == PDL(0, APPLE2_PACT_CW), "KP 9 -> paddle 0 CW");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_5) == PDL(0, APPLE2_PACT_BUTTON), "KP 5 -> paddle 0 button");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_1) == PDL(1, APPLE2_PACT_CCW), "KP 1 -> paddle 1 CCW");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_3) == PDL(1, APPLE2_PACT_CW), "KP 3 -> paddle 1 CW");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_PERIOD) == PDL(1, APPLE2_PACT_BUTTON), "KP . -> paddle 1 button");
    CHECK(bindings_target_for_key('a') == -1 && bindings_target_for_key(APPLE2SESSION_KEYSYM_UP) == -1,
          "the typewriter keys and the arrows are the Apple II's own");

    /* never bound: the app's keys, and the Open and Closed Apple keys */
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_F9) == -1 &&
          bindings_target_for_key(APPLE2SESSION_KEYSYM_F11) == -1 &&
          bindings_target_for_key(APPLE2SESSION_KEYSYM_F12) == -1, "F9, F11 and F12 are unbound");
    bindings_set_keysym(s, JOY(APPLE2_ACT_LEFT), APPLE2SESSION_KEYSYM_F9, &stole);
    CHECK(stole == -1 && bindings_target_keysym(JOY(APPLE2_ACT_LEFT)) == APPLE2SESSION_KEYSYM_KP_4,
          "binding F9 is refused");
    {
        const uint32_t apple[] = { APPLE2SESSION_KEYSYM_LALT, APPLE2SESSION_KEYSYM_RALT,
                                   APPLE2SESSION_KEYSYM_LMETA, APPLE2SESSION_KEYSYM_RMETA,
                                   APPLE2SESSION_KEYSYM_LSUPER, APPLE2SESSION_KEYSYM_RSUPER,
                                   APPLE2SESSION_KEYSYM_F11, APPLE2SESSION_KEYSYM_F12 };
        for (size_t idx = 0; idx < sizeof apple / sizeof apple[0]; idx++) {
            bindings_set_keysym(s, JOY(APPLE2_ACT_BTN0), apple[idx], NULL);
            CHECK(bindings_target_keysym(JOY(APPLE2_ACT_BTN0)) == APPLE2SESSION_KEYSYM_KP_0 &&
                  bindings_target_for_key(apple[idx]) == -1,
                  "Alt, Meta, Super, F11 and F12 cannot be bound");
        }
    }

    /* the mode picks the targets a key reaches */
    CHECK(bindings_target_in_mode(JOY(APPLE2_ACT_UP), APPLE2_MODE_JOYSTICK) &&
          !bindings_target_in_mode(JOY(APPLE2_ACT_UP), APPLE2_MODE_PADDLES), "joystick targets act in joystick mode");
    CHECK(bindings_target_in_mode(PDL(1, APPLE2_PACT_CW), APPLE2_MODE_PADDLES) &&
          !bindings_target_in_mode(PDL(1, APPLE2_PACT_CW), APPLE2_MODE_JOYSTICK), "paddle targets act in paddles mode");
    CHECK(bindings_target_in_mode(SYS(APPLE2SESSION_SYSACT_RESET), APPLE2_MODE_JOYSTICK) &&
          bindings_target_in_mode(SYS(APPLE2SESSION_SYSACT_POWER), APPLE2_MODE_PADDLES), "system actions act in both");

    /* pad defaults, per mode and port */
    CHECK(bindings_target_for_pad(APPLE2_MODE_JOYSTICK, 0, APPLE2_PAD_BTN_SOUTH) == JOY(APPLE2_ACT_BTN0), "joystick: South -> button 0");
    CHECK(bindings_target_for_pad(APPLE2_MODE_JOYSTICK, 0, APPLE2_PAD_BTN_EAST) == JOY(APPLE2_ACT_BTN1), "joystick: East -> button 1");
    CHECK(bindings_target_for_pad(APPLE2_MODE_JOYSTICK, 0, APPLE2_PAD_BTN_DPAD_LEFT) == JOY(APPLE2_ACT_LEFT), "joystick: D-pad left steers left");
    CHECK(bindings_target_for_pad(APPLE2_MODE_JOYSTICK, 0, APPLE2_PAD_BTN_DPAD_DOWN) == JOY(APPLE2_ACT_DOWN), "joystick: D-pad down steers down");
    CHECK(bindings_target_for_pad(APPLE2_MODE_JOYSTICK, 1, APPLE2_PAD_BTN_SOUTH) == -1, "the joystick has one port");
    CHECK(bindings_target_for_pad(APPLE2_MODE_PADDLES, 0, APPLE2_PAD_BTN_SOUTH) == PDL(0, APPLE2_PACT_BUTTON), "paddles: South is paddle 0's button on port 0");
    CHECK(bindings_target_for_pad(APPLE2_MODE_PADDLES, 1, APPLE2_PAD_BTN_SOUTH) == PDL(1, APPLE2_PACT_BUTTON), "and paddle 1's on port 1");
    CHECK(bindings_target_for_pad(APPLE2_MODE_PADDLES, 1, APPLE2_PAD_BTN_DPAD_RIGHT) == PDL(1, APPLE2_PACT_CW), "paddles: the D-pad turns the knob");
    CHECK(bindings_target_for_pad(APPLE2_MODE_PADDLES, 0, APPLE2_PAD_BTN_EAST) == -1, "paddles: East is unbound (a paddle has one button)");
    CHECK(bindings_knob_axis(0) == APPLE2_PAD_AXIS_BASE && bindings_knob_axis(1) == APPLE2_PAD_AXIS_BASE,
          "each knob follows the left stick's X");
    CHECK(bindings_knob_axis(2) == -1 && bindings_knob_axis(-1) == -1, "no such paddle: no axis");

    /* a remap steals the key from whoever held it */
    bindings_set_keysym(s, JOY(APPLE2_ACT_BTN0), APPLE2SESSION_KEYSYM_KP_8, &stole);
    CHECK(stole == JOY(APPLE2_ACT_UP), "taking KP 8 reports the target it was taken from");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_8) == JOY(APPLE2_ACT_BTN0) &&
          bindings_target_keysym(JOY(APPLE2_ACT_UP)) == 0, "button 0 has KP 8 and up has nothing");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_0) == -1, "button 0's old key is free");
    /* a key may be given to a paddle target: keys are global across modes */
    bindings_set_keysym(s, PDL(0, APPLE2_PACT_BUTTON), APPLE2SESSION_KEYSYM_KP_8, &stole);
    CHECK(stole == JOY(APPLE2_ACT_BTN0), "keys are global: a paddle target takes it from a joystick target");

    /* pad inputs are scoped: the joystick and each paddle can share South */
    bindings_set_pad(s, JOY(APPLE2_ACT_BTN1), APPLE2_PAD_BTN_SOUTH, &stole);
    CHECK(stole == JOY(APPLE2_ACT_BTN0), "South moves to button 1, from button 0 (same scope)");
    CHECK(bindings_target_for_pad(APPLE2_MODE_PADDLES, 0, APPLE2_PAD_BTN_SOUTH) == PDL(0, APPLE2_PACT_BUTTON),
          "and a paddle's South binding is not touched");
    bindings_set_pad(s, PDL(1, APPLE2_PACT_BUTTON), APPLE2_PAD_BTN_NORTH, &stole);
    CHECK(stole == -1 && bindings_target_for_pad(APPLE2_MODE_PADDLES, 1, APPLE2_PAD_BTN_NORTH) == PDL(1, APPLE2_PACT_BUTTON),
          "a free pad button binds without stealing");
    bindings_set_pad(s, PDL(0, APPLE2_PACT_BUTTON), APPLE2_PAD_BTN_NORTH, &stole);
    CHECK(stole == -1 && bindings_target_for_pad(APPLE2_MODE_PADDLES, 1, APPLE2_PAD_BTN_NORTH) == PDL(1, APPLE2_PACT_BUTTON),
          "two paddles can share a pad button (they are different ports)");
    /* a system action is reachable from every pad, so it competes with all */
    bindings_set_pad(s, SYS(APPLE2SESSION_SYSACT_POWER), APPLE2_PAD_BTN_NORTH, &stole);
    CHECK(stole == PDL(1, APPLE2_PACT_BUTTON) || stole == PDL(0, APPLE2_PACT_BUTTON),
          "a console action takes a pad button from a paddle");
    CHECK(bindings_target_for_pad(APPLE2_MODE_JOYSTICK, 0, APPLE2_PAD_BTN_NORTH) == SYS(APPLE2SESSION_SYSACT_POWER),
          "and is reachable from the joystick's pad");

    /* an axis belongs to a knob, a button to everything else */
    {
        const int before = bindings_target_pad(JOY(APPLE2_ACT_LEFT));
        bindings_set_pad(s, JOY(APPLE2_ACT_LEFT), APPLE2_PAD_AXIS_BASE + 1, NULL);
        CHECK(bindings_target_pad(JOY(APPLE2_ACT_LEFT)) == before, "an axis cannot drive a button target");
        bindings_set_pad(s, PDL(0, APPLE2_PACT_KNOB), APPLE2_PAD_BTN_SOUTH, NULL);
        CHECK(bindings_knob_axis(0) == APPLE2_PAD_AXIS_BASE, "a button cannot drive a knob");
        bindings_set_pad(s, PDL(0, APPLE2_PACT_KNOB), APPLE2_PAD_AXIS_BASE + 2, NULL);
        CHECK(bindings_knob_axis(0) == APPLE2_PAD_AXIS_BASE + 2 && bindings_knob_axis(1) == APPLE2_PAD_AXIS_BASE,
              "an axis binds the knob of that paddle only");
        bindings_set_pad(s, PDL(1, APPLE2_PACT_KNOB), APPLE2_PAD_AXIS_BASE + 5, NULL);   /* a trigger */
        CHECK(bindings_knob_axis(1) == APPLE2_PAD_AXIS_BASE + 5, "a trigger axis can turn a knob");
    }
    /* raw and hat bands are buttons */
    bindings_set_pad(s, JOY(APPLE2_ACT_UP), APPLE2_PAD_BTN_RAW_BASE + 6, NULL);
    bindings_set_pad(s, JOY(APPLE2_ACT_DOWN), APPLE2_PAD_HAT_BASE + 4, NULL);
    CHECK(bindings_target_for_pad(APPLE2_MODE_JOYSTICK, 0, APPLE2_PAD_BTN_RAW_BASE + 6) == JOY(APPLE2_ACT_UP) &&
          bindings_target_for_pad(APPLE2_MODE_JOYSTICK, 0, APPLE2_PAD_HAT_BASE + 4) == JOY(APPLE2_ACT_DOWN),
          "raw buttons and hat positions are bindable");

    /* clear */
    bindings_clear(s, JOY(APPLE2_ACT_UP));
    CHECK(bindings_target_keysym(JOY(APPLE2_ACT_UP)) == 0 && bindings_target_pad(JOY(APPLE2_ACT_UP)) == -1,
          "Clear removes a target's key and pad input");

    /* persistence: the overrides survive a new session, the defaults are not written */
    bindings_set_keysym(s, JOY(APPLE2_ACT_RIGHT), 'l', NULL);
    bindings_set_pad(s, PDL(0, APPLE2_PACT_CW), APPLE2_PAD_BTN_RIGHT_SHOULDER, NULL);
    apple2session_free(s);
    s = apple2session_new(&paths);
    if (!s) return 1;
    CHECK(bindings_target_for_key('l') == JOY(APPLE2_ACT_RIGHT), "an overridden key persists");
    CHECK(bindings_target_for_pad(APPLE2_MODE_PADDLES, 0, APPLE2_PAD_BTN_RIGHT_SHOULDER) == PDL(0, APPLE2_PACT_CW),
          "an overridden pad input persists");
    CHECK(bindings_target_keysym(JOY(APPLE2_ACT_UP)) == 0, "a cleared target stays cleared");
    CHECK(bindings_knob_axis(0) == APPLE2_PAD_AXIS_BASE + 2 && bindings_knob_axis(1) == APPLE2_PAD_AXIS_BASE + 5,
          "knob axes persist");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_6) == -1, "KP 6 stays free once 'l' took right");
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_5) == PDL(0, APPLE2_PACT_BUTTON) ||
          bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_8) == PDL(0, APPLE2_PACT_BUTTON),
          "paddle 0's button key persists where it went");

    /* garbage in the setting is skipped; an out-of-range target and a reserved key are dropped */
    apple2session_set_str(s, "bindings", "xx 99.k:65 3.k:65478 0.k:97 4.p:129");
    bindings_init(s);
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_F9) == -1, "a reserved key in the file is ignored");
    CHECK(bindings_target_for_key('a') == JOY(APPLE2_ACT_LEFT), "a good entry after garbage still loads");
    CHECK(bindings_target_pad(JOY(APPLE2_ACT_BTN0)) != APPLE2_PAD_AXIS_BASE + 1, "an axis for a button target in the file is dropped");
    CHECK(bindings_target_pad(JOY(APPLE2_ACT_RIGHT)) == APPLE2_PAD_BTN_DPAD_RIGHT, "the default pad input stays where the file said nothing");

    /* reset */
    bindings_reset_defaults(s);
    CHECK(bindings_target_for_key(APPLE2SESSION_KEYSYM_KP_8) == JOY(APPLE2_ACT_UP) &&
          bindings_target_for_key('l') == -1 &&
          bindings_target_for_pad(APPLE2_MODE_PADDLES, 0, APPLE2_PAD_BTN_SOUTH) == PDL(0, APPLE2_PACT_BUTTON) &&
          bindings_knob_axis(0) == APPLE2_PAD_AXIS_BASE, "reset restores every default");
    CHECK(!strcmp(apple2session_get_str(s, "bindings", "x"), ""), "and persists nothing but the defaults");

    /* labels and names */
    CHECK(!strcmp(bindings_target_label(JOY(APPLE2_ACT_BTN0)), "Joystick Button 0"), "a joystick label");
    CHECK(!strcmp(bindings_target_label(PDL(1, APPLE2_PACT_CW)), "Paddle 1 Clockwise"), "a paddle label");
    CHECK(!strcmp(bindings_target_label(SYS(APPLE2SESSION_SYSACT_POWER)), "Power Cycle"), "a console label");
    CHECK(!strcmp(bindings_target_short_label(PDL(0, APPLE2_PACT_KNOB)), "Knob"), "a short label");
    CHECK(!strcmp(bindings_target_label(-1), "?") && !strcmp(bindings_target_label(APPLE2_TARGET_COUNT), "?"), "bad targets are '?'");
    {
        char nm[32];
        apple2session_keysym_name(APPLE2SESSION_KEYSYM_KP_5, nm, sizeof nm);
        CHECK(!strcmp(nm, "Keypad 5"), "Keypad 5 is named");
        apple2session_keysym_name(APPLE2SESSION_KEYSYM_KP_ENTER, nm, sizeof nm);
        CHECK(!strcmp(nm, "Keypad Enter"), "Keypad Enter is named");
        apple2session_keysym_name('q', nm, sizeof nm);
        CHECK(!strcmp(nm, "Q"), "a letter is upper case");
        apple2session_keysym_name(APPLE2SESSION_KEYSYM_HID_BASE + 0x68, nm, sizeof nm);
        CHECK(nm[0] != '\0', "a HID-band key has a name");
    }

    apple2session_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "bindings_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

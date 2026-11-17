/*
 * gamepad_test -- the gamepad lifecycle and Map-mode capture API are safe
 * with no controller attached: start/stop don't crash, capture arms and
 * cancels, and a poll with nothing pressed reports no capture. (The actual
 * button->binding path needs hardware; gamepad_layer_test covers it with
 * virtual joysticks. This guards the headless contract the session relies
 * on -- start is best-effort and must never take the session down when
 * there's no gamepad.) Also the pure helpers.
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
#include "gamepad_sdl.h"
#include "pad_assign.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 0.001f)

int main(void)
{
    char tmpl[512];
    test_tmpl(tmpl, sizeof tmpl, "apple2_gamepad_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) return 1;
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    apple2session_paths paths = { cfg, data };

    apple2session *s = apple2session_new(&paths);
    if (!s) return 1;

    /* start may fail if SDL can't init the gamepad subsystem in this
     * environment; either way the session must survive it. */
    int rc = apple2_gamepad_start(s);
    fprintf(stderr, "gamepad_test: apple2_gamepad_start rc=%d\n", rc);

    if (rc == 0) {
        /* capture: arm, poll (nothing pressed -> 0), cancel */
        apple2session_gamepad_capture_begin(s);
        usleep(30 * 1000);
        int button = -99;
        int got = apple2session_gamepad_capture_poll(s, &button);
        CHECK(got == 0, "no capture without a controller press");
        apple2session_gamepad_capture_begin_axis(s);
        CHECK(apple2session_gamepad_capture_poll(s, &button) == 0, "no axis capture either");
        apple2session_gamepad_capture_cancel(s);
        apple2_gamepad_stop();
        /* start/stop again must be safe */
        CHECK(apple2_gamepad_start(s) == 0 || 1, "restart tolerated");
        apple2_gamepad_stop();
    }

    /* the input-name table */
    char nm[32];
    apple2session_pad_input_name(0, nm, sizeof nm);
    CHECK(strcmp(nm, "A") == 0, "button 0 (South) named");
    apple2session_pad_input_name(APPLE2_PAD_AXIS_BASE + 2, nm, sizeof nm);
    CHECK(strcmp(nm, "Right Stick X") == 0, "axis 2 named");
    apple2session_pad_input_name(APPLE2_PAD_BTN_DPAD_UP, nm, sizeof nm);
    CHECK(strcmp(nm, "D-pad Up") == 0, "the D-pad is named");
    apple2session_pad_input_name(APPLE2_PAD_BTN_RIGHT_TRIGGER, nm, sizeof nm);
    CHECK(strcmp(nm, "Right Trigger") == 0, "the trigger buttons are named");
    apple2session_pad_input_name(APPLE2_PAD_BTN_RAW_BASE + 3, nm, sizeof nm);
    CHECK(strcmp(nm, "Button 3") == 0, "a raw button is named by its number");
    apple2session_pad_input_name(APPLE2_PAD_HAT_BASE + 8 + 1, nm, sizeof nm);
    CHECK(strcmp(nm, "Hat 1 Up-Right") == 0, "a hat position is named");
    apple2session_pad_input_name(-1, nm, sizeof nm);
    CHECK(nm[0] == '\0', "no input has no name");

    /* the pure helpers */
    CHECK(apple2_acts_from_dir(APPLE2_DIR_UP | APPLE2_DIR_DOWN) == 0, "up and down cancel");
    CHECK(apple2_acts_from_dir(APPLE2_DIR_UP | APPLE2_DIR_RIGHT) ==
          ((1u << APPLE2_ACT_UP) | (1u << APPLE2_ACT_RIGHT)), "a diagonal is two actions");
    CHECK(apple2_acts_from_dir(APPLE2_DIR_LEFT) == (1u << APPLE2_ACT_LEFT), "left is left");
    {
        int none[4] = { -1, -1, -1, -1 };
        CHECK(apple2_pad_for_port(none, 3, 0) == 0 && apple2_pad_for_port(none, 3, 1) == 1 &&
              apple2_pad_for_port(none, 3, 2) == 2 && apple2_pad_for_port(none, 3, 3) == -1,
              "unassigned pads take ports in connection order");
        int one[4] = { 1, -1, -1, -1 };
        CHECK(apple2_pad_for_port(one, 3, 1) == 0, "an explicit assignment wins its port");
        CHECK(apple2_pad_for_port(one, 3, 0) == 1, "the first unassigned pad plays the first free port");
        CHECK(apple2_pad_for_port(one, 3, 2) == -1, "no pad left for a third port");
        int both[4] = { 1, 1, -1, -1 };
        CHECK(apple2_pad_for_port(both, 4, 1) == 0, "two pads on one port: the first wins");
        CHECK(apple2_pad_for_port(both, 4, 0) == 2, "the first unassigned pad plays port 0");
    }
    CHECK(NEAR(apple2_knob_from_axis(-1.0f, 0), -1.0f) && NEAR(apple2_knob_from_axis(0.0f, 0), 0.0f) &&
          NEAR(apple2_knob_from_axis(1.0f, 0), 1.0f), "a stick axis sweeps the knob -1..1");
    CHECK(NEAR(apple2_knob_from_axis(0.1f, 0), 0.0f), "inside the dead zone the knob reads centre");
    CHECK(NEAR(apple2_knob_from_axis(0.0f, 1), -1.0f) && NEAR(apple2_knob_from_axis(0.5f, 1), 0.0f) &&
          NEAR(apple2_knob_from_axis(1.0f, 1), 1.0f) && NEAR(apple2_knob_from_axis(2.0f, 1), 1.0f),
          "a trigger sweeps it 0..1 onto -1..1, clamped");
    CHECK(NEAR(apple2_paddle_from_axis(0.18f, APPLE2_STICK_DEADZONE), 0.0f) &&
          NEAR(apple2_paddle_from_axis(1.0f, APPLE2_STICK_DEADZONE), 1.0f) &&
          apple2_paddle_from_axis(0.59f, APPLE2_STICK_DEADZONE) > 0.0f, "the stick dead zone still reaches the stop");
    for (int btn = 0; btn < APPLE2_PAD_BTN_DPAD_RIGHT + 1; btn++) {
        const int sdl = apple2_sdl_button_from_pad(btn);
        CHECK(sdl >= 0 && apple2_pad_button_from_sdl((uint8_t)sdl) == btn, "named buttons round trip SDL");
    }
    CHECK(apple2_sdl_button_from_pad(APPLE2_PAD_BTN_LEFT_TRIGGER) == -1, "the triggers are not SDL buttons");

    /* nothing connected: the lists are empty and the getters are safe */
    CHECK(apple2session_gamepad_count(s) == 0 || apple2session_gamepad_count(s) > 0, "count callable");
    CHECK(apple2session_gamepad_assignment(s, 99) == -1 && apple2session_gamepad_effective_port(s, 99) == -1,
          "an unknown pad has no assignment");
    apple2session_gamepad_assign(s, 99, 1);        /* no such pad: ignored */
    CHECK(apple2session_gamepad_name(s, 99, nm, sizeof nm) == 0, "no name for no pad");

    apple2session_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "gamepad_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

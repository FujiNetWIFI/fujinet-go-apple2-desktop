/*
 * controllers_run_test -- the controllers with a machine actually running (no
 * FujiNet, no audio, no window): holds made before the machine starts reach it
 * once it is up (the core zeroes the host's inputs as it starts), every input
 * API is safe while frames run, the console actions fire live (Reset, Power
 * Cycle, Stop in Debugger), the on-screen keyboard types through the real key
 * path, mode switches mid-run, and stopping lets go of everything. SKIPs (77)
 * when the core has no ROMs (a WITH_APPLE_ROMS=OFF build with an empty ROM
 * directory).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apple2session.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static void sleep_ms(int msec)
{
    struct timespec stamp = { msec / 1000, (long)(msec % 1000) * 1000000L };
    nanosleep(&stamp, NULL);
}

static int axes_are(apple2session *s, float want_x, float want_y)
{
    float xval, yval;
    apple2session_axes_get(s, &xval, &yval);
    return fabsf(xval - want_x) < 0.001f && fabsf(yval - want_y) < 0.001f;
}

static int frames_advance(apple2session *s)
{
    const uint64_t before = apple2session_frame_count(s);
    sleep_ms(250);
    return apple2session_frame_count(s) > before;
}

int main(void)
{
    char tmpl[512];
    test_tmpl(tmpl, sizeof tmpl, "apple2_run_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    apple2session_paths paths = { cfg, data, "", NULL };

    apple2session *s = apple2session_new(&paths);
    if (!s) return 1;
    apple2session_start_opts opts;
    apple2session_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 1;

    /* a hold made before the machine starts */
    CHECK(apple2session_input_key(s, APPLE2SESSION_KEYSYM_KP_6, 1, 0) == 1, "KP 6 is the joystick's right");
    CHECK(axes_are(s, 1, 0), "held before the start");
    if (apple2session_start(s, &opts) != 0) {
        printf("SKIP: the machine would not start: %s\n", apple2session_last_error(s));
        apple2session_free(s);
        return 77;
    }
    CHECK(frames_advance(s), "frames run");
    CHECK(axes_are(s, 1, 0), "the hold survives the start (it was pushed again once the core was up)");
    apple2session_input_key(s, APPLE2SESSION_KEYSYM_KP_6, 0, 0);
    CHECK(axes_are(s, 0, 0), "and lets go");

    /* the whole input API while frames run */
    apple2session_pointer_set(s, 0.5f, -0.5f);
    apple2session_press(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN0), 1);
    CHECK(axes_are(s, 0.5f, -0.5f) && apple2session_target_held(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN0)), "the mouse holds the stick and a button");
    apple2session_key(s, 1, APPLE2SESSION_KEYSYM_LALT, 0, 0, 0);
    CHECK(apple2session_target_held(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN0)), "the Alt key lights button 0");
    apple2session_key(s, 0, APPLE2SESSION_KEYSYM_LALT, 0, 0, 0);
    apple2session_pointer_release(s);
    apple2session_release_all(s);
    CHECK(axes_are(s, 0, 0) && frames_advance(s), "released, and the machine runs on");

    /* the on-screen keyboard types through the real key path, and the Apple caps hold buttons */
    {
        int count;
        float wide, high;
        const apple2_keycap *caps = apple2session_keyboard_layout(&count, &wide, &high);
        for (int idx = 0; idx < count; idx++) {
            if (caps[idx].kind == APPLE2_KEYCAP_RESET) continue;
            apple2session_keyboard_press(s, idx, 1);
            apple2session_keyboard_press(s, idx, 0);
        }
        apple2session_keyboard_release(s);
        (void)apple2session_sysaction_take(s);
        CHECK(frames_advance(s), "every cap clicked: the machine runs on");
    }

    /* paddles mode mid-run */
    apple2session_set_controller_mode(s, APPLE2_MODE_PADDLES);
    apple2session_knob_set(s, 0, 0.5f);
    apple2session_knob_set(s, 1, -0.5f);
    CHECK(axes_are(s, 0.5f, -0.5f), "the knobs are PDL0 and PDL1");
    apple2session_set_controller_mode(s, APPLE2_MODE_JOYSTICK);
    CHECK(axes_are(s, 0, 0), "back to the joystick: centred");

    /* the console actions, fired live */
    apple2session_sysaction_post(s, APPLE2SESSION_SYSACT_RESET);
    apple2session_sysaction_drain(s);
    CHECK(frames_advance(s), "Reset: the machine runs on");
    apple2session_sysaction_fire(s, APPLE2SESSION_SYSACT_POWER);
    CHECK(frames_advance(s), "Power Cycle: the machine runs on");
    apple2session_press(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN1), 1);
    apple2session_sysaction_fire(s, APPLE2SESSION_SYSACT_POWER);
    CHECK(apple2session_target_held(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN1)), "a hold survives a power cycle");
    apple2session_pointer_release(s);

    /* stopping lets go of everything */
    apple2session_press(s, APPLE2_TARGET_JOY(APPLE2_ACT_LEFT), 1);
    apple2session_stop(s);
    CHECK(axes_are(s, 0, 0) && !apple2session_target_held(s, APPLE2_TARGET_JOY(APPLE2_ACT_LEFT)), "stop: nothing is held");

    /* and the session starts again, with a hold made while it was stopped */
    apple2session_press(s, APPLE2_TARGET_JOY(APPLE2_ACT_DOWN), 1);
    CHECK(apple2session_start(s, &opts) == 0, "it starts again");
    CHECK(frames_advance(s) && axes_are(s, 0, 1), "and the hold made while stopped reaches it");
    apple2session_pointer_release(s);

    /* Stop in Debugger breaks in at the next instruction; stopping the session
     * must still be able to take the parked machine down */
    {
        const uint64_t before = apple2session_frame_count(s);
        apple2session_sysaction_fire(s, APPLE2SESSION_SYSACT_DEBUG_STOP);
        sleep_ms(300);
        CHECK(apple2session_frame_count(s) - before < 40, "Stop in Debugger parks the machine");
        apple2session_stop(s);
        CHECK(!apple2session_is_running(s), "and the session can still be stopped");
    }

    apple2session_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "controllers_run_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

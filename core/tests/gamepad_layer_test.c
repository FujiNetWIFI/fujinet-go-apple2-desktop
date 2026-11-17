/*
 * gamepad_layer_test -- the gamepad layer end to end with SDL3 virtual
 * joysticks, no hardware, no human and no machine: pads taking the game port in
 * connection order (the joystick has one port, the paddles two), explicit
 * assignment to Joystick / Paddle 0 / Paddle 1, hot-unplug releasing exactly
 * what that pad held, the default mapping (the D-pad steers to the stops, the
 * left stick is proportional, South and West are button 0, East and North
 * button 1), a trigger axis as a bound button, a paddle's knob following one
 * axis absolutely, map-mode capture of buttons and of the knob's axis, a raw
 * HID adapter (button 0 stands in for South, a hat that steers and is
 * bindable, axes 0/1 the stick), the mode changing under a held pad, and the
 * assignment: written to the settings ("pad_assign") when made, restored when
 * a new session finds the pad again, found through a stale GUID by the name,
 * kept through garbage, removed by Automatic, with the toasts' text and
 * generation.
 *
 * What a pad holds is read back through apple2session_target_held and
 * apple2session_axes_get, which report what the machine is driven with. Every
 * wait polls with a ten second ceiling, so a slow runner is merely slow. SKIPs
 * (77) where SDL has no virtual joysticks or real pads are attached.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apple2session.h"
#include "bindings.h"
#include "gamepad_sdl.h"
#include "input.h"
#include "test_tmpdir.h"

#define NAME_FMT "apple2test pad %c"
#define RAW_NAME "apple2test raw adapter"
#define WAIT_TRIES 500           /* x 20 ms: ten seconds */

#define JOY(act) APPLE2_TARGET_JOY(act)
#define PDL(n, act) APPLE2_TARGET_PADDLE(n, act)
#define PAD_BTN(name) SDL_GAMEPAD_BUTTON_##name

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static void sleep_ms(int msec)
{
    struct timespec stamp = { msec / 1000, (long)(msec % 1000) * 1000000L };
    nanosleep(&stamp, NULL);
}

static apple2session *g_s;

/* The six held targets of the mode, a bit each: joystick LEFT RIGHT UP DOWN
 * BTN0 BTN1, or paddle 0's CCW CW BUTTON then paddle 1's. */
static unsigned held_mask(void)
{
    unsigned mask = 0;
    if (apple2session_controller_mode(g_s) == APPLE2_MODE_PADDLES) {
        for (int pdl = 0; pdl < 2; pdl++)
            for (int act = 0; act < 3; act++)
                if (apple2session_target_held(g_s, PDL(pdl, act)))
                    mask |= 1u << (pdl * 3 + act);
    } else {
        for (int act = 0; act < APPLE2_JOY_ACTS; act++)
            if (apple2session_target_held(g_s, JOY(act)))
                mask |= 1u << act;
    }
    return mask;
}

static void expect_held(unsigned want, const char *what)
{
    unsigned held = 0;
    for (int tries = 0; tries < WAIT_TRIES; tries++) {
        held = held_mask();
        if (held == want) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (held %02X want %02X)\n", what, held, want);
    failures++;
}

static void expect_axes(float xlo, float xhi, float ylo, float yhi, const char *what)
{
    float xval = 9, yval = 9;
    for (int tries = 0; tries < WAIT_TRIES; tries++) {
        apple2session_axes_get(g_s, &xval, &yval);
        if (xval >= xlo && xval <= xhi && yval >= ylo && yval <= yhi) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (axes %.3f %.3f, want x %.2f..%.2f y %.2f..%.2f)\n", what, xval, yval,
           xlo, xhi, ylo, yhi);
    failures++;
}

static void expect_knob(int paddle, float lo, float hi, const char *what)
{
    float value = 9;
    for (int tries = 0; tries < WAIT_TRIES; tries++) {
        value = apple2session_knob_get(g_s, paddle);
        if (value >= lo && value <= hi) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (knob %.3f, want %.2f..%.2f)\n", what, value, lo, hi);
    failures++;
}

/* A distinct vendor/product per pad: virtual joysticks share one GUID
 * otherwise, and the assignment table would not tell them apart. */
static SDL_JoystickID attach_pad(const char *name, int raw, int product)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    if (raw) {
        desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
        desc.naxes = 4;
        desc.nbuttons = 8;
        desc.nhats = 1;
    } else {
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    }
    desc.vendor_id = 0x1209;
    desc.product_id = (Uint16)product;
    desc.name = name;
    return SDL_AttachVirtualJoystick(&desc);
}

static int pad_slot_now(const char *name)
{
    for (int idx = 0; idx < apple2session_gamepad_count(g_s); idx++) {
        char got[64];
        apple2session_gamepad_name(g_s, idx, got, sizeof got);
        if (strcmp(got, name) == 0)
            return idx;
    }
    return -1;
}

static int wait_for_pad(const char *name)
{
    int idx = pad_slot_now(name);
    for (int tries = 0; tries < WAIT_TRIES && idx < 0; tries++) {
        sleep_ms(20);
        idx = pad_slot_now(name);
    }
    return idx;
}

static void wait_gone(const char *name)
{
    for (int tries = 0; tries < WAIT_TRIES && pad_slot_now(name) >= 0; tries++)
        sleep_ms(20);
}

static int wait_count(int want)
{
    for (int tries = 0; tries < WAIT_TRIES && apple2session_gamepad_count(g_s) != want; tries++)
        sleep_ms(20);
    return apple2session_gamepad_count(g_s) == want;
}

static void pad_name(char *dst, size_t size, int num)
{
    snprintf(dst, size, NAME_FMT, 'A' + num);
}

static int port_of(const char *name)
{
    const int idx = wait_for_pad(name);
    return idx < 0 ? -2 : apple2session_gamepad_effective_port(g_s, idx);
}

/* a port that has settled (the pad thread sees an assignment or a mode change a
 * poll later and lets go of what the pad held) */
static int wait_port(const char *name, int want)
{
    for (int tries = 0; tries < WAIT_TRIES; tries++) {
        if (port_of(name) == want) break;
        sleep_ms(20);
    }
    sleep_ms(120);
    return port_of(name) == want;
}

static int wait_sysaction(unsigned want)
{
    for (int tries = 0; tries < WAIT_TRIES; tries++) {
        const unsigned got = apple2session_sysaction_take(g_s);
        if (got)
            return got == want;
        sleep_ms(20);
    }
    return 0;
}

static apple2session *session_up(const apple2session_paths *paths)
{
    apple2session *sess = apple2session_new(paths);
    if (!sess)
        return NULL;
    if (apple2_gamepad_start(sess) != 0) {
        printf("SKIP: the gamepad layer would not start: %s\n", apple2session_last_error(sess));
        apple2session_free(sess);
        return NULL;
    }
    g_s = sess;
    return sess;
}

static void session_down(void)
{
    apple2_gamepad_stop();
    apple2session_free(g_s);
    g_s = NULL;
}

static int has_entry(const char *guid, const char *name, int port)
{
    char want[200];
    snprintf(want, sizeof want, "%s|%s=%d", guid, name, port);
    return strstr(apple2session_get_str(g_s, "pad_assign", ""), want) != NULL;
}

/* ---- one pad: persistence across sessions ---------------------------------- */

static void persistence(const apple2session_paths *paths)
{
    char name[64], guid[64], text[200], tag[32];
    SDL_JoystickID vid;
    int idx;
    unsigned gen_before;

    {
        /* the earlier run assigned these pads: start from no settings */
        char path[600];
        snprintf(path, sizeof path, "%s/settings.ini", paths->config_dir);
        remove(path);
    }
    pad_name(name, sizeof name, 0);
    vid = attach_pad(name, 0, 0x1100);
    if (!vid) {
        check(0, "persistence: the virtual pad attaches");
        return;
    }
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(vid), guid, sizeof guid);

    if (!session_up(paths)) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(name);
    check(idx >= 0, "persistence: the pad is picked up");
    if (idx < 0) { session_down(); SDL_DetachVirtualJoystick(vid); return; }
    check(apple2session_gamepad_assignment(g_s, idx) == -1, "a pad nobody assigned is automatic");
    check(strstr(apple2session_get_str(g_s, "pad_assign", ""), guid) == NULL, "and has no entry");
    check(apple2session_gamepad_effective_port(g_s, idx) == 0, "the only pad plays the joystick");

    apple2session_gamepad_last_event(g_s, text, sizeof text);
    check(strstr(text, "Connected: ") == text && strstr(text, name) != NULL, "the toast says the pad connected");
    check(strstr(text, "(joystick)") != NULL, "and that it plays the joystick");

    /* paddle 1 only exists in paddles mode; in joystick mode it counts as automatic */
    apple2session_gamepad_assign(g_s, idx, 1);
    check(apple2session_gamepad_assignment(g_s, idx) == 1 && has_entry(guid, name, 1),
          "assigning Paddle 1 is remembered, and writes <guid>|<name>=1 to pad_assign");
    check(apple2session_gamepad_effective_port(g_s, idx) == 0,
          "but in joystick mode the pad still plays the joystick (an assignment past the ports is automatic)");
    apple2session_set_controller_mode(g_s, APPLE2_MODE_PADDLES);
    check(apple2session_gamepad_effective_port(g_s, idx) == 1, "in paddles mode it plays paddle 1");
    {
        char path[600], line[16384];
        int found = 0;
        FILE *file;
        snprintf(path, sizeof path, "%s/settings.ini", paths->config_dir);
        file = fopen(path, "r");
        if (file) {
            while (fgets(line, sizeof line, file))
                if (!strncmp(line, "pad_assign=", 11) && strstr(line, guid))
                    found = 1;
            fclose(file);
        }
        check(found, "the settings file on disk holds it");
    }

    /* restart: a new session on the same settings (the mode is a setting too) */
    session_down();
    if (!session_up(paths)) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(name);
    check(apple2session_controller_mode(g_s) == APPLE2_MODE_PADDLES, "restart: the mode is back");
    check(idx >= 0 && apple2session_gamepad_assignment(g_s, idx) == 1 &&
          apple2session_gamepad_effective_port(g_s, idx) == 1,
          "restart: the assignment is restored on device add");

    /* a stale GUID (another USB port): the name finds the entry */
    snprintf(text, sizeof text, "ffffffffffffffffffffffffffffffff|%s=1", name);
    apple2session_set_str(g_s, "pad_assign", text);
    session_down();
    if (!session_up(paths)) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(name);
    check(idx >= 0 && apple2session_gamepad_assignment(g_s, idx) == 1,
          "a different GUID with the same name gets the assignment too");

    /* Automatic removes the entry */
    apple2session_gamepad_assign(g_s, idx, 1);
    check(has_entry(guid, name, 1), "assigned under the pad's own GUID again");
    apple2session_gamepad_assign(g_s, idx, -1);
    check(apple2session_gamepad_assignment(g_s, idx) == -1 &&
          strstr(apple2session_get_str(g_s, "pad_assign", ""), name) == NULL,
          "Automatic: no explicit port, and the entry is gone");

    /* garbage, and a port outside 0..1, are ignored */
    apple2session_set_str(g_s, "pad_assign", ";;|=;zz|x=9;%%%;00000000000000000000000000000000|apple2test pad A=2");
    session_down();
    if (!session_up(paths)) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(name);
    check(idx >= 0 && apple2session_gamepad_assignment(g_s, idx) == -1,
          "a malformed or out-of-range pad_assign leaves the pad automatic");

    /* a hot replug within a run restores it too, and is announced */
    apple2session_gamepad_assign(g_s, idx, 1);
    gen_before = apple2session_gamepad_generation(g_s);
    SDL_DetachVirtualJoystick(vid);
    wait_gone(name);
    check(pad_slot_now(name) < 0 && apple2session_gamepad_generation(g_s) != gen_before,
          "unplugged: the pad is gone and the generation moved");
    apple2session_gamepad_last_event(g_s, text, sizeof text);
    check(!strcmp(text, "Disconnected: apple2test pad A"), "the toast says the pad disconnected");
    vid = attach_pad(name, 0, 0x1100);
    idx = vid ? wait_for_pad(name) : -1;
    check(idx >= 0 && apple2session_gamepad_assignment(g_s, idx) == 1, "plugged back in: the port comes back");
    apple2session_gamepad_last_event(g_s, text, sizeof text);
    snprintf(tag, sizeof tag, "(paddle %d)", 1);
    check(strstr(text, tag) != NULL, "and the toast names that paddle");
    apple2session_gamepad_assign(g_s, idx, -1);

    /* the gamepad thread stopping and starting again finds the same pad: no
     * new toast, the generation where it was, a held button asserted again in
     * the new machine */
    if (vid && idx >= 0) {
        SDL_Joystick *joy = SDL_OpenJoystick(vid);
        const int port = apple2session_gamepad_effective_port(g_s, idx);
        char before[200];

        gen_before = apple2session_gamepad_generation(g_s);
        apple2session_gamepad_last_event(g_s, before, sizeof before);
        check(joy != NULL && port == 0, "restart: the pad is open and plays paddle 0");
        if (joy && port == 0) {
            sleep_ms(300);       /* a poll after the move to port 0 has let go of what the pad held */
            SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), true);
            expect_held(1u << 2, "restart: South is paddle 0's button");
            apple2_gamepad_stop();
            check(apple2session_gamepad_generation(g_s) == gen_before, "stopped: the generation is where it was");
            apple2_input_clear_all();               /* the new machine holds nothing */
            apple2_gamepad_start(g_s);
            check(wait_for_pad(name) >= 0, "restarted: the pad is found again");
            expect_held(1u << 2, "restarted: the held button is asserted again");
            apple2session_gamepad_last_event(g_s, text, sizeof text);
            check(apple2session_gamepad_generation(g_s) == gen_before && !strcmp(text, before),
                  "restarted: no new generation and no new toast");
            SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), false);
            expect_held(0, "restarted: and the button still lets go");
        }
        if (joy)
            SDL_CloseJoystick(joy);
    }
    session_down();
    if (vid)
        SDL_DetachVirtualJoystick(vid);
}

/* ---- a plain HID adapter ----------------------------------------------------- */

static void raw_adapter(void)
{
    SDL_JoystickID vid = attach_pad(RAW_NAME, 1, 0x1200);
    SDL_Joystick *joy;
    int idx;
    char text[128];

    apple2session_set_controller_mode(g_s, APPLE2_MODE_JOYSTICK);
    if (!vid) {
        printf("SKIP: no raw virtual joystick here: %s\n", SDL_GetError());
        return;
    }
    joy = SDL_OpenJoystick(vid);
    idx = joy ? wait_for_pad(RAW_NAME) : -1;
    check(idx >= 0, "a raw adapter is opened as a joystick, not missed");
    if (idx < 0 || apple2session_gamepad_effective_port(g_s, idx) != 0) {
        check(0, "the raw adapter plays the joystick");
        if (joy) SDL_CloseJoystick(joy);
        SDL_DetachVirtualJoystick(vid);
        return;
    }
    check(!SDL_IsGamepad(vid), "(and SDL has no gamepad mapping for it)");
    apple2session_gamepad_last_event(g_s, text, sizeof text);
    check(strstr(text, "Connected: " RAW_NAME) == text, "the toast names it");
    sleep_ms(300);

    /* button 0 stands in for South until it is bound: Open Apple */
    SDL_SetJoystickVirtualButton(joy, 0, true);
    expect_held(1u << APPLE2_ACT_BTN0, "an unbound raw button 0 is button 0");
    SDL_SetJoystickVirtualButton(joy, 0, false);
    expect_held(0, "released");
    SDL_SetJoystickVirtualButton(joy, 1, true);
    expect_held(1u << APPLE2_ACT_BTN1, "raw button 1 stands in for East: button 1");
    SDL_SetJoystickVirtualButton(joy, 1, false);
    expect_held(0, "released");
    SDL_SetJoystickVirtualButton(joy, 2, true);
    expect_held(1u << APPLE2_ACT_BTN0, "raw button 2 stands in for West, which stands in for South");
    SDL_SetJoystickVirtualButton(joy, 2, false);
    expect_held(0, "released");
    SDL_SetJoystickVirtualButton(joy, 6, true);
    sleep_ms(150);
    expect_held(0, "an unbound raw button above the first four does nothing");
    SDL_SetJoystickVirtualButton(joy, 6, false);
    sleep_ms(150);       /* (SDL folds a release and a press between two polls into nothing) */
    bindings_set_pad(g_s, JOY(APPLE2_ACT_UP), APPLE2_PAD_BTN_RAW_BASE + 6, NULL);
    SDL_SetJoystickVirtualButton(joy, 6, true);
    expect_axes(-0.01f, 0.01f, -1.0f, -0.99f, "raw button 6, bound to up, steers up");
    SDL_SetJoystickVirtualButton(joy, 6, false);
    expect_axes(-0.01f, 0.01f, -0.01f, 0.01f, "released");

    /* hat 0 steers; its positions are also bindable buttons in the band 192.. */
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_LEFT);
    expect_axes(-1.0f, -0.99f, -0.01f, 0.01f, "hat 0 left steers to the stop");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_RIGHTDOWN);
    expect_axes(0.99f, 1.0f, 0.99f, 1.0f, "and a diagonal is two directions");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    expect_axes(-0.01f, 0.01f, -0.01f, 0.01f, "centred: nothing");
    bindings_set_pad(g_s, JOY(APPLE2_ACT_BTN1), APPLE2_PAD_HAT_BASE + 2, NULL);
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_RIGHT);
    expect_held((1u << APPLE2_ACT_RIGHT) | (1u << APPLE2_ACT_BTN1),
                "hat right steers right and, bound in the hat band, is button 1");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    expect_held(0, "centred: both let go");

    /* axes 0 and 1 are its stick, proportionally */
    SDL_SetJoystickVirtualAxis(joy, 0, -16384);
    SDL_SetJoystickVirtualAxis(joy, 1, 32767);
    expect_axes(-0.40f, -0.30f, 0.99f, 1.0f, "axes 0 and 1 are the stick: half left, full down");
    SDL_SetJoystickVirtualAxis(joy, 0, 0);
    SDL_SetJoystickVirtualAxis(joy, 1, 0);
    expect_axes(-0.01f, 0.01f, -0.01f, 0.01f, "centred");

    /* paddles mode: axis 0 (the default knob axis) sets paddle 0 absolutely */
    apple2session_set_controller_mode(g_s, APPLE2_MODE_PADDLES);
    SDL_SetJoystickVirtualAxis(joy, 0, 0);
    sleep_ms(250);                               /* the first sample only records */
    SDL_SetJoystickVirtualAxis(joy, 0, 32767);
    expect_knob(0, 0.99f, 1.0f, "axis 0 sweeps paddle 0's knob to the right");
    SDL_SetJoystickVirtualAxis(joy, 0, -32768);
    expect_knob(0, -1.0f, -0.99f, "and left");
    SDL_SetJoystickVirtualAxis(joy, 0, 0);
    expect_knob(0, -0.01f, 0.01f, "and centres");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_RIGHT);
    expect_held(1u << 1, "a hat's right turns the knob clockwise (paddle 0's CW)");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    expect_held(0, "centred");
    apple2session_set_controller_mode(g_s, APPLE2_MODE_JOYSTICK);

    /* map mode takes a raw button and a hat position by number */
    {
        int btn = -1, got = 0;
        apple2session_gamepad_capture_begin(g_s);
        SDL_SetJoystickVirtualButton(joy, 5, true);
        for (int tries = 0; tries < WAIT_TRIES && !got; tries++) {
            got = apple2session_gamepad_capture_poll(g_s, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == APPLE2_PAD_BTN_RAW_BASE + 5, "map mode captures raw button 5 as 69");
        got = 0;
        SDL_SetJoystickVirtualButton(joy, 5, false);
        apple2session_gamepad_capture_begin(g_s);
        SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_LEFT);
        for (int tries = 0; tries < WAIT_TRIES && !got; tries++) {
            got = apple2session_gamepad_capture_poll(g_s, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == APPLE2_PAD_HAT_BASE + 6, "and hat 0 left as 198");
        SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    }
    expect_held(0, "capturing pressed nothing");

    /* unplugged while held */
    SDL_SetJoystickVirtualButton(joy, 6, true);
    SDL_SetJoystickVirtualAxis(joy, 0, 32767);
    expect_axes(0.99f, 1.0f, -1.0f, -0.99f, "held before the unplug: the stick right, a bound button up");
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(vid);
    wait_gone(RAW_NAME);
    expect_axes(-0.01f, 0.01f, -0.01f, 0.01f, "the raw adapter's holds are released on unplug");
    expect_held(0, "and so is its button");
    bindings_reset_defaults(g_s);
}

/* ---- two pads ---------------------------------------------------------------- */

int main(void)
{
    char cfg[512], data[512], tmpl[512];
    apple2session_paths paths;
    SDL_JoystickID vid[2];
    SDL_Joystick *joy[2];
    char name[2][64];
    char *dir;

    /* held open for the whole run: a session stopping must not take the
     * virtual pads with it (the persistence tests restart sessions) */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        printf("SKIP: SDL gamepad subsystem: %s\n", SDL_GetError());
        return 77;
    }
    test_tmpl(tmpl, sizeof tmpl, "apple2_gm_test");
    dir = mkdtemp(tmpl);
    if (!dir) return 1;
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;

    if (!session_up(&paths))
        return 77;
    sleep_ms(300);
    if (apple2session_gamepad_count(g_s) != 0) {
        printf("SKIP: real pads are attached (%d)\n", apple2session_gamepad_count(g_s));
        session_down();
        return 77;
    }

    /* two pads, attached one after another */
    for (int num = 0; num < 2; num++) {
        pad_name(name[num], sizeof name[num], num);
        vid[num] = attach_pad(name[num], 0, 0x1100 + num);
        if (!vid[num]) {
            printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
            session_down();
            return 77;
        }
        joy[num] = SDL_OpenJoystick(vid[num]);
        check(joy[num] && wait_for_pad(name[num]) >= 0, "a virtual pad is picked up");
    }
    check(wait_count(2), "both are connected");
    check(apple2session_gamepad_generation(g_s) >= 2, "each connecting bumped the generation");

    /* the joystick has one port: the first pad plays it, the second plays nothing */
    check(apple2session_controller_mode(g_s) == APPLE2_MODE_JOYSTICK && apple2session_controller_ports(g_s) == 1,
          "joystick mode: one port");
    check(port_of(name[0]) == 0 && port_of(name[1]) == -1, "the first pad plays the joystick, the second nobody");
    sleep_ms(300);   /* the pad thread takes its baseline of the buttons a poll after opening */

    /* the buttons: South and West are button 0, East and North button 1 */
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
    expect_held(1u << APPLE2_ACT_BTN0, "South is button 0 (Open Apple)");
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(EAST), true);
    sleep_ms(150);
    expect_held(1u << APPLE2_ACT_BTN0, "the second pad plays nothing: its East does nothing");
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(EAST), false);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
    expect_held(0, "released");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), true);
    expect_held(1u << APPLE2_ACT_BTN1, "East is button 1 (Closed Apple)");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), false);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(WEST), true);
    expect_held(1u << APPLE2_ACT_BTN0, "an unbound West stands in for South");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(WEST), false);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(NORTH), true);
    expect_held(1u << APPLE2_ACT_BTN1, "an unbound North stands in for East");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
    expect_held((1u << APPLE2_ACT_BTN0) | (1u << APPLE2_ACT_BTN1), "both buttons at once");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(NORTH), false);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
    expect_held(0, "released");

    /* the D-pad drives the stops, the left stick is proportional, separate sources */
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(DPAD_UP), true);
    expect_axes(-0.01f, 0.01f, -1.0f, -0.99f, "the D-pad up drives y to the stop");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(DPAD_LEFT), true);
    expect_axes(-1.0f, -0.99f, -1.0f, -0.99f, "a diagonal");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(DPAD_DOWN), true);
    expect_axes(-1.0f, -0.99f, -0.01f, 0.01f, "up and down together cancel");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(DPAD_UP), false);
    expect_axes(-1.0f, -0.99f, 0.99f, 1.0f, "and down is back once up lets go");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(DPAD_DOWN), false);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(DPAD_LEFT), false);
    expect_axes(-0.01f, 0.01f, -0.01f, 0.01f, "released");

    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTX, 16384);          /* 0.5 */
    expect_axes(0.35f, 0.40f, -0.01f, 0.01f, "half of the stick is 0.39 once the dead zone is taken out");
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTX, 32767);
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTY, -32768);
    expect_axes(0.99f, 1.0f, -1.0f, -0.99f, "the stick reaches the stops");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(DPAD_LEFT), true);
    expect_axes(0.99f, 1.0f, -1.0f, -0.99f, "a digital hold and an analog one in opposite directions: the strongest wins");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(DPAD_LEFT), false);
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTX, 3000);           /* 0.09: inside the dead zone */
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect_axes(-0.0001f, 0.0001f, -0.0001f, 0.0001f, "a stick inside the dead zone reads exactly centre");
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTX, 0);

    /* the mouse and a pad on one hold: sources do not step on each other */
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
    expect_held(1u << APPLE2_ACT_BTN0, "South is button 0");
    apple2session_press(g_s, JOY(APPLE2_ACT_BTN0), 1);
    apple2session_input_key(g_s, APPLE2SESSION_KEYSYM_KP_0, 1, 0);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
    sleep_ms(150);
    check((held_mask() & (1u << APPLE2_ACT_BTN0)) != 0, "the pad letting go leaves the mouse's and the key's hold");
    apple2session_release_all(g_s);
    check((held_mask() & (1u << APPLE2_ACT_BTN0)) != 0, "release_all lets go of the keyboard's alone");
    apple2session_pointer_release(g_s);
    expect_held(0, "and then the mouse's");

    /* a trigger axis is a bindable button (past half travel: must not hang the pad lock) */
    bindings_set_pad(g_s, JOY(APPLE2_ACT_UP), APPLE2_PAD_BTN_LEFT_TRIGGER, NULL);
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect_axes(-0.01f, 0.01f, -1.0f, -0.99f, "the left trigger, bound to up, steers up");
    check(apple2session_gamepad_effective_port(g_s, 0) == 0, "the pad lock is still free with a trigger held");
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect_axes(-0.01f, 0.01f, -0.01f, 0.01f, "the left trigger released");
    bindings_reset_defaults(g_s);

    /* a pad button on a console action posts it, once, on the press */
    bindings_set_pad(g_s, APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_RESET), APPLE2_PAD_BTN_START, NULL);
    (void)apple2session_sysaction_take(g_s);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(START), true);
    check(wait_sysaction(1u << APPLE2SESSION_SYSACT_RESET), "Start, bound to Reset, posts it");
    sleep_ms(150);
    check(apple2session_sysaction_take(g_s) == 0, "once: holding the button posts nothing more");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(START), false);
    sleep_ms(150);
    check(apple2session_sysaction_take(g_s) == 0 && held_mask() == 0, "and the release posts nothing");
    bindings_reset_defaults(g_s);

    /* map mode: a button is captured and swallowed; a knob's axis is captured by axis mode */
    {
        int btn = -1, got = 0;
        apple2session_gamepad_capture_begin(g_s);
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
        for (int tries = 0; tries < WAIT_TRIES && !got; tries++) {
            got = apple2session_gamepad_capture_poll(g_s, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == APPLE2_PAD_BTN_SOUTH, "map mode captures South");
        check(held_mask() == 0, "and South did not fire while it was captured");
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
        sleep_ms(150);

        apple2session_gamepad_capture_begin_axis(g_s);
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), true);       /* a button is not an axis */
        sleep_ms(150);
        check(!apple2session_gamepad_capture_poll(g_s, &btn), "axis capture ignores a button");
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), false);
        SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_RIGHTY, 30000);
        got = 0;
        for (int tries = 0; tries < WAIT_TRIES && !got; tries++) {
            got = apple2session_gamepad_capture_poll(g_s, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == APPLE2_PAD_AXIS_BASE + 3, "axis capture records the right stick's Y");
        SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_RIGHTY, 0);
        apple2session_gamepad_capture_begin(g_s);
        apple2session_gamepad_capture_cancel(g_s);
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), true);
        sleep_ms(150);
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), false);
        sleep_ms(100);
        check(!apple2session_gamepad_capture_poll(g_s, &btn), "a cancelled capture records nothing");
    }

    /* explicit assignment to the joystick: the other pad plays it, the first nobody */
    apple2session_gamepad_assign(g_s, wait_for_pad(name[1]), 0);
    check(wait_port(name[1], 0) && wait_port(name[0], -1), "pad B assigned to the joystick plays it; pad A plays nobody");
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(SOUTH), true);
    expect_held(1u << APPLE2_ACT_BTN0, "pad B's South is button 0");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), true);
    sleep_ms(150);
    expect_held(1u << APPLE2_ACT_BTN0, "and pad A's East does nothing");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), false);
    /* moving the assignment while held lets go of what the pad held */
    apple2session_gamepad_assign(g_s, wait_for_pad(name[1]), -1);
    expect_held(0, "making pad B automatic lets go of what it held on the joystick");
    check(wait_port(name[1], -1) && wait_port(name[0], 0), "and pad A plays the joystick again");
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(SOUTH), false);
    /* an assignment to Paddle 1 is automatic in joystick mode */
    apple2session_gamepad_assign(g_s, wait_for_pad(name[0]), 1);
    check(wait_port(name[0], 0), "pad A assigned to Paddle 1 still plays the joystick in joystick mode");
    apple2session_gamepad_assign(g_s, wait_for_pad(name[0]), -1);

    /* paddles mode: two ports, one pad each */
    apple2session_set_controller_mode(g_s, APPLE2_MODE_PADDLES);
    check(apple2session_controller_ports(g_s) == 2, "paddles mode: two ports");
    check(wait_port(name[0], 0) && wait_port(name[1], 1), "pad A plays paddle 0 and pad B paddle 1");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
    expect_held(1u << 2, "pad A's South is paddle 0's button");
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(SOUTH), true);
    expect_held((1u << 2) | (1u << 5), "and pad B's South is paddle 1's button");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(SOUTH), false);
    expect_held(0, "released");

    /* the D-pad turns the knob (the pad thread ticks the ramp), and it holds */
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_RIGHT), true);
    expect_knob(1, 0.2f, 1.0f, "the D-pad right turns paddle 1's knob clockwise");
    check(apple2session_knob_get(g_s, 0) == 0.0f, "and paddle 0's knob does not move");
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_RIGHT), false);
    sleep_ms(150);
    {
        const float held_at = apple2session_knob_get(g_s, 1);
        sleep_ms(200);
        check(held_at > 0.1f && fabsf(apple2session_knob_get(g_s, 1) - held_at) < 0.001f,
              "let go: the knob stays where it was left");
    }

    /* the left stick's X sets a knob absolutely: the first sample only records */
    apple2session_knob_set(g_s, 0, 0.0f);
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTX, 0);
    sleep_ms(250);
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTX, 24000);          /* 0.73 */
    expect_knob(0, 0.60f, 0.75f, "pad A's left stick X sets paddle 0's knob (dead zone taken out)");
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTX, -32768);
    expect_knob(0, -1.0f, -0.99f, "to the other stop");
    SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_LEFTX, 0);
    expect_knob(0, -0.01f, 0.01f, "and centres");
    apple2session_knob_set(g_s, 0, 0.5f);                  /* the dial takes it back */
    sleep_ms(150);
    check(fabsf(apple2session_knob_get(g_s, 0) - 0.5f) < 0.001f, "a still axis does not fight the dial");
    check(fabsf(apple2session_knob_get(g_s, 1)) > 0.1f, "and paddle 1's knob is not touched");
    /* a trigger axis sweeps a knob too, 0..1 onto -1..1 */
    bindings_set_pad(g_s, PDL(1, APPLE2_PACT_KNOB), APPLE2_PAD_AXIS_BASE + 5, NULL);
    /* (a virtual trigger is released at the axis minimum: 0 reads as half pressed) */
    SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    sleep_ms(250);
    SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    expect_knob(1, 0.99f, 1.0f, "the right trigger, bound to the knob, sweeps it to the clockwise stop");
    SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    expect_knob(1, -1.0f, -0.99f, "and back to the counter-clockwise stop");
    bindings_reset_defaults(g_s);
    apple2session_knob_set(g_s, 0, 0);
    apple2session_knob_set(g_s, 1, 0);

    /* an explicit assignment: pad A on Paddle 1 puts B on Paddle 0 */
    apple2session_gamepad_assign(g_s, wait_for_pad(name[0]), 1);
    check(wait_port(name[0], 1) && wait_port(name[1], 0), "pad A on Paddle 1: pad B plays Paddle 0");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
    expect_held(1u << 5, "pad A pulls paddle 1's button");
    /* the mode changing under a held pad lets go, and the pad plays on in the new mode */
    apple2session_set_controller_mode(g_s, APPLE2_MODE_JOYSTICK);
    expect_held(0, "switching to the joystick lets go of what the pad held");
    check(wait_port(name[0], 0), "pad A (assigned to Paddle 1, automatic in joystick mode) plays the joystick");
    apple2session_gamepad_assign(g_s, wait_for_pad(name[0]), -1);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
    sleep_ms(200);
    expect_held(0, "and the button still lets go");
    apple2session_set_controller_mode(g_s, APPLE2_MODE_PADDLES);

    /* hot-unplug while held: pad A (paddle 0) lets go of everything; pad B plays on */
    apple2session_gamepad_assign(g_s, wait_for_pad(name[0]), -1);
    apple2session_gamepad_assign(g_s, wait_for_pad(name[1]), 1);   /* pinned: A's going will not move it */
    check(wait_port(name[0], 0) && wait_port(name[1], 1), "pad A plays paddle 0, pad B is pinned to paddle 1");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(DPAD_RIGHT), true);
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(SOUTH), true);
    expect_held((1u << 1) | (1u << 2) | (1u << 5), "held on both paddles before the unplug");
    SDL_CloseJoystick(joy[0]);
    SDL_DetachVirtualJoystick(vid[0]);
    wait_gone(name[0]);
    check(pad_slot_now(name[0]) < 0 && apple2session_gamepad_count(g_s) == 1, "the unplugged pad is gone");
    expect_held(1u << 5, "its button and knob key are released; pad B's button is not disturbed");
    check(wait_port(name[1], 1), "and pad B keeps its paddle");
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(SOUTH), false);
    expect_held(0, "and still lets go");

    SDL_CloseJoystick(joy[1]);
    SDL_DetachVirtualJoystick(vid[1]);
    wait_count(0);
    bindings_reset_defaults(g_s);
    apple2session_set_controller_mode(g_s, APPLE2_MODE_JOYSTICK);

    raw_adapter();

    session_down();

    persistence(&paths);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

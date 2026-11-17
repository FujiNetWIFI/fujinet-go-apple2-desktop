/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as the Apple II's game
 * port, on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices are added and
 * removed as they appear on the system: SDL posts the JOYSTICK_ and
 * GAMEPAD_ add/remove pairs and both are taken, with idempotent open/close,
 * so neither kind of device can be missed. A device SDL has no gamepad
 * mapping for (a plain HID adapter, which includes the USB adapters for real
 * Apple II joysticks) is opened as a raw joystick instead: its buttons and hat
 * positions are bindable in the raw and hat bands, and while a raw button has
 * no binding its first four act as South/East/West/North, so button 0 is
 * still Open Apple.
 *
 * What a pad plays is one port: the joystick (one port) or a paddle (two):
 * the first unassigned pad plays the first port, the next the second, unless
 * pads are assigned to ports explicitly. Every button goes through the
 * bindings table: by default the D-pad steers to the stops (or turns the knob),
 * South is button 0 and East button 1 (an unbound West and North stand in for
 * them); any button may be bound to another target instead. The left analog
 * stick is the joystick, PROPORTIONALLY (the dead zone is taken out so it
 * still reaches the stops); in paddles mode one axis (the left stick's X by
 * default) sets the knob absolutely. A raw joystick's hat 0 steers. The ways
 * of holding an input -- a pad button, the analog stick, the keyboard -- are
 * separate sources in input.c, so letting go of one never releases another.
 * Ported from the Channel F, ADAM and Astrocade siblings' gamepad_sdl.c.
 *
 * An explicit assignment is remembered (settings key "pad_assign", see
 * pad_assign.h): a pad that is unplugged and plugged back in, or is there on
 * the next run, gets it back -- matched by its SDL GUID, else by its name. Two
 * identical pads share one entry; the first to connect gets it. Every
 * connect/disconnect is reported (apple2session_gamepad_last_event) for the
 * frontend's toast. A pad that goes away lets go of everything it held.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bindings.h"
#include "gamepad_sdl.h"
#include "input.h"
#include "pad_assign.h"
#include "session_internal.h"

#define MAX_PADS APPLE2_MAX_PADS
#define MAX_HATS 4
#define TRIGGER_ON 0.5f
#define AXIS_CAPTURE 0.6f

static const char *const k_axis_name[APPLE2_PAD_AXIS_COUNT] = {
    "Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y",
    "Left Trigger", "Right Trigger",
};
static const SDL_GamepadAxis k_axis_sdl[APPLE2_PAD_AXIS_COUNT] = {
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

typedef struct {
    SDL_JoystickID devid;
    int owner;                /* the input.c source number for this pad: stable
                               * while it is connected, whatever its slot */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit port, -1 = automatic */
    int by_name;              /* assign was restored from a name-only match
                               * (the entry's GUID differs): an exact-GUID
                               * pad may take it over */
    int port;                 /* the port it drove on the last poll, or -1 */
    unsigned mode_gen;        /* apple2_input_mode_generation() at the last poll */
    float sent_x, sent_y;     /* the analog stick as last handed to input.c */
    uint8_t sent[APPLE2_ACT_DOWN + 1]; /* the hat's directions it holds (apple2_joy_act) */
    /* What each button's press asserted (target + 1, 0 = nothing), so its
     * release lets go of exactly that even if the binding changed meanwhile,
     * and a pad that goes away drops what it held. */
    int16_t btn_target[256];
    SDL_GUID guid;
    char guidstr[APPLE2_PAD_GUID_MAX];   /* guid as hex, for the pad_assign table */
    char name[APPLE2_PAD_NAME_MAX];
    int trigger_down[2];
    uint32_t axis_over;       /* axes past the capture threshold last poll */
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    struct apple2session *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    atomic_int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    int quiet_scan;           /* thread only: the first scan of a restarted
                               * session re-finds pads without announcing them */
    /* Map-mode capture */
    int cap_active, cap_axis, cap_have, cap_result;
    /* the remembered explicit assignments (the "pad_assign" setting), for
     * pads that are away as well as those here; guarded by lock */
    apple2_pad_table table;
    char event[128];          /* the last hot-plug event, for a toast */
} gamepad_state;

/* One state for the process (the machine is a process singleton). The lock
 * is static so the public getters are safe whether or not the thread runs;
 * the generation and the last event carry across a restart of the thread, so
 * a frontend's baseline never goes stale, and the session remembers that the
 * thread has run (pad_restarted) so a restart's first scan finds the same pads
 * again without announcing them. */
static gamepad_state g = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ---- pure helpers ----------------------------------------------------------- */

unsigned apple2_acts_from_dir(int dir)
{
    unsigned acts = 0;

    if ((dir & APPLE2_DIR_UP) && !(dir & APPLE2_DIR_DOWN)) acts |= 1u << APPLE2_ACT_UP;
    if ((dir & APPLE2_DIR_DOWN) && !(dir & APPLE2_DIR_UP)) acts |= 1u << APPLE2_ACT_DOWN;
    if ((dir & APPLE2_DIR_LEFT) && !(dir & APPLE2_DIR_RIGHT)) acts |= 1u << APPLE2_ACT_LEFT;
    if ((dir & APPLE2_DIR_RIGHT) && !(dir & APPLE2_DIR_LEFT)) acts |= 1u << APPLE2_ACT_RIGHT;
    return acts;
}

int apple2_pad_for_port(const int *assign, int npads, int port)
{
    int idx, next = 0;
    for (idx = 0; idx < npads; idx++)
        if (assign[idx] == port) return idx;
    for (idx = 0; idx < npads; idx++) {
        if (assign[idx] >= 0) continue;   /* explicitly elsewhere */
        if (next == port) return idx;
        next++;
    }
    return -1;
}

float apple2_knob_from_axis(float value, int is_trigger)
{
    float knob = is_trigger ? value * 2.0f - 1.0f
                            : apple2_paddle_from_axis(value, APPLE2_STICK_DEADZONE);
    return knob < -1.0f ? -1.0f : knob > 1.0f ? 1.0f : knob;
}

int apple2_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return APPLE2_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return APPLE2_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return APPLE2_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return APPLE2_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return APPLE2_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return APPLE2_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return APPLE2_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return APPLE2_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return APPLE2_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return APPLE2_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return APPLE2_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return APPLE2_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return APPLE2_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return APPLE2_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return APPLE2_PAD_BTN_DPAD_RIGHT;
    default: return APPLE2_PAD_BTN_NONE;
    }
}

int apple2_sdl_button_from_pad(int button)
{
    switch (button) {
    case APPLE2_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case APPLE2_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case APPLE2_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case APPLE2_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case APPLE2_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case APPLE2_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case APPLE2_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case APPLE2_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case APPLE2_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case APPLE2_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case APPLE2_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case APPLE2_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case APPLE2_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case APPLE2_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case APPLE2_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- names ------------------------------------------------------------------ */

static const char *const k_button_name[APPLE2_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

int apple2session_pad_input_name(int id, char *dst, int dstsz)
{
    static const char *const dirs[APPLE2_PAD_HAT_DIRS] = {
        "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left", "Left", "Up-Left",
    };
    if (!dst || dstsz <= 0) return 0;
    if (id < 0)
        return snprintf(dst, (size_t)dstsz, "%s", "");
    if (APPLE2_PAD_BTN_IS_NAMED(id))
        return snprintf(dst, (size_t)dstsz, "%s", k_button_name[id]);
    if (APPLE2_PAD_IS_AXIS(id))
        return snprintf(dst, (size_t)dstsz, "%s", k_axis_name[id - APPLE2_PAD_AXIS_BASE]);
    if (APPLE2_PAD_BTN_IS_RAW(id))
        return snprintf(dst, (size_t)dstsz, "Button %d", id - APPLE2_PAD_BTN_RAW_BASE);
    if (APPLE2_PAD_BTN_IS_HAT(id))
        return snprintf(dst, (size_t)dstsz, "Hat %d %s",
                        (id - APPLE2_PAD_HAT_BASE) / APPLE2_PAD_HAT_DIRS,
                        dirs[(id - APPLE2_PAD_HAT_BASE) % APPLE2_PAD_HAT_DIRS]);
    return snprintf(dst, (size_t)dstsz, "Button %d", id);
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(SDL_JoystickID devid)
{
    int idx;
    for (idx = 0; idx < g.npads; idx++)
        if (g.pads[idx].devid == devid) return idx;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *name = slot->handle ? SDL_GetGamepadName(slot->handle)
                                    : SDL_GetJoystickName(slot->joy);
    return name ? name : "Gamepad";
}

/* Lets go of everything the pad holds, whichever way it was held. The caller
 * may hold g.lock: input.c's lock is a leaf below it. */
static void slot_release_all(pad_slot *slot)
{
    memset(slot->sent, 0, sizeof slot->sent);
    memset(slot->btn_target, 0, sizeof slot->btn_target);
    slot->sent_x = slot->sent_y = 0.0f;
    apple2_input_pad_clear(slot->owner);
}

static void slot_close(pad_slot *slot)
{
    slot_release_all(slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_port_locked(int idx);

/* Gives the pad in slot idx its remembered port. An entry with the pad's own
 * GUID is the pad's: it takes it from a pad that got it only through the name
 * fallback. A pad known only by name takes the entry if no other pad carries
 * it (two identical pads, or one pad known under two GUIDs): then this one
 * stays automatic. Locked by the caller; only reads g.table, never writes. */
static void restore_assignment(int idx)
{
    pad_slot *slot = &g.pads[idx];
    int ent = apple2_pad_table_find(&g.table, slot->guidstr, slot->name), kix;
    int exact;

    if (ent < 0 || g.table.entry[ent].port >= APPLE2SESSION_PADDLES) return;
    exact = !strcmp(g.table.entry[ent].guid, slot->guidstr);
    for (kix = 0; kix < g.npads; kix++) {
        pad_slot *other = &g.pads[kix];
        if (kix == idx || other->assign < 0 ||
            apple2_pad_table_find(&g.table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name) return;
        other->assign = -1;          /* the entry's owner is here: it wins */
        other->by_name = 0;
    }
    slot->assign = g.table.entry[ent].port;
    slot->by_name = !exact;
}

static void set_event_locked(const char *text)
{
    snprintf(g.event, sizeof g.event, "%s", text ? text : "");
}

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(SDL_JoystickID devid)
{
    pthread_mutex_lock(&g.lock);
    if (g.npads < MAX_PADS && slot_for_id(devid) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *pad = SDL_IsGamepad(devid) ? SDL_OpenGamepad(devid) : NULL;
        SDL_Joystick *joy = pad ? NULL : SDL_OpenJoystick(devid);
        if (pad || joy) {
            pad_slot *slot = &g.pads[g.npads++];
            int hat, used = 0, kix;
            for (kix = 0; kix < g.npads - 1; kix++)
                used |= 1 << g.pads[kix].owner;
            memset(slot, 0, sizeof *slot);
            for (kix = 0; kix < MAX_PADS && (used & (1 << kix)); kix++) {}
            slot->owner = kix;
            slot->devid = devid;
            slot->handle = pad;
            slot->joy = joy;
            slot->assign = -1;
            slot->port = -1;
            slot->mode_gen = apple2_input_mode_generation();
            for (hat = 0; hat < MAX_HATS; hat++) slot->last_hat[hat] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(devid);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(slot->guid, slot->guidstr, sizeof slot->guidstr);
            restore_assignment(g.npads - 1);
            fprintf(stderr, "apple2_gamepad: %s connected (%s%s)\n", slot->name,
                    pad ? "gamepad" : "raw joystick",
                    pad ? "" : "; button 0 = Open Apple, axes 0/1 = stick, hat 0 steers");
            if (!g.quiet_scan) {
                char msg[128];
                int port = effective_port_locked(g.npads - 1);
                if (port >= 0 && apple2_input_mode() == APPLE2_MODE_PADDLES)
                    snprintf(msg, sizeof msg, "Connected: %s (paddle %d)", slot->name, port);
                else if (port >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (joystick)", slot->name);
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                g.generation++;
                set_event_locked(msg);
            }
        }
    }
    pthread_mutex_unlock(&g.lock);
}

static void close_pad(SDL_JoystickID devid)
{
    int idx, kix;
    pthread_mutex_lock(&g.lock);
    idx = slot_for_id(devid);
    if (idx >= 0) {
        pad_slot *slot = &g.pads[idx];
        char msg[128];
        fprintf(stderr, "apple2_gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        slot_close(slot);
        set_event_locked(msg);
        for (kix = idx; kix < g.npads - 1; kix++) g.pads[kix] = g.pads[kix + 1];
        g.npads--;
        g.generation++;
    }
    pthread_mutex_unlock(&g.lock);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The port a slot plays right now in the current mode: 0 for the joystick, 0
 * or 1 for a paddle. In joystick mode an assignment to Paddle 1 counts as
 * automatic (the joystick has one port). Locked by the caller. */
static int effective_port_locked(int idx)
{
    const int mode = apple2_input_mode();
    const int nports = mode == APPLE2_MODE_PADDLES ? APPLE2SESSION_PADDLES : 1;
    int assign[MAX_PADS], kix, port;
    for (kix = 0; kix < MAX_PADS; kix++) assign[kix] = -1;
    for (kix = 0; kix < g.npads; kix++) {
        assign[kix] = g.pads[kix].assign;
        if (assign[kix] >= nports) assign[kix] = -1;
    }
    for (port = 0; port < nports; port++)
        if (apple2_pad_for_port(assign, g.npads, port) == idx) return port;
    return -1;
}

static int is_sysact(int target)
{
    return target >= APPLE2_TARGET_SYSACT_BASE && target < APPLE2_TARGET_PADDLES;
}

/* The target a pad input drives. An unmapped joystick's first four buttons
 * stand in for A B X Y until the user binds them, and an unbound West / North
 * stand in for South / East (the old fixed mapping: South|West is button 0,
 * East|North is button 1), but only for a held input, never a console action. */
static int resolve_target(int mode, int port, int btn)
{
    int target = bindings_target_for_pad(mode, port, btn);
    if (target < 0 && APPLE2_PAD_BTN_IS_RAW(btn) && btn - APPLE2_PAD_BTN_RAW_BASE < 4) {
        btn -= APPLE2_PAD_BTN_RAW_BASE;
        target = bindings_target_for_pad(mode, port, btn);
    }
    if (target < 0 && (btn == APPLE2_PAD_BTN_WEST || btn == APPLE2_PAD_BTN_NORTH)) {
        target = bindings_target_for_pad(mode, port, btn == APPLE2_PAD_BTN_WEST
                                                        ? APPLE2_PAD_BTN_SOUTH
                                                        : APPLE2_PAD_BTN_EAST);
        if (is_sysact(target)) target = -1;
    }
    return target;
}

static void dispatch_button(SDL_JoystickID which, int btn, int down)
{
    int idx, port, owner = 0, target = -1;
    if (btn == APPLE2_PAD_BTN_NONE) return;

    pthread_mutex_lock(&g.lock);
    if (g.cap_active) {
        /* Map mode: every button event is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. (An axis capture records no button.) */
        if (down && !g.cap_have && !g.cap_axis) { g.cap_have = 1; g.cap_result = btn; }
        pthread_mutex_unlock(&g.lock);
        return;
    }
    idx = slot_for_id(which);
    port = idx >= 0 ? effective_port_locked(idx) : -1;
    if (idx >= 0 && port >= 0 && btn >= 0 && btn < 256) {
        pad_slot *slot = &g.pads[idx];
        owner = slot->owner;
        if (down) {
            target = resolve_target(apple2_input_mode(), port, btn);
            if (target >= 0 && !is_sysact(target))
                slot->btn_target[btn] = (int16_t)(target + 1);
        } else if (slot->btn_target[btn]) {
            /* what the press asserted, whatever the binding says now */
            target = slot->btn_target[btn] - 1;
            slot->btn_target[btn] = 0;
        }
    }
    pthread_mutex_unlock(&g.lock);
    if (target < 0) return;

    /* a console action is posted, on the press only, for the UI thread */
    if (is_sysact(target)) {
        if (down)
            apple2session_sysaction_post(g.session,
                (apple2session_sysaction)(target - APPLE2_TARGET_SYSACT_BASE));
        return;
    }
    apple2_input_pad_press(owner, target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;
    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&g.lock);
    idx = slot_for_id(which);
    if (idx < 0 || g.pads[idx].handle) { pthread_mutex_unlock(&g.lock); return; }
    prev_dir = hat_dir_from_mask(g.pads[idx].last_hat[hat]);
    g.pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&g.lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also steer, in poll_one */
    if (prev_dir >= 0)
        dispatch_button(which, APPLE2_PAD_HAT_BASE + hat * APPLE2_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(which, APPLE2_PAD_HAT_BASE + hat * APPLE2_PAD_HAT_DIRS + dir, 1);
}

/* After a session restart the new machine holds nothing, but a pad button
 * may still be physically held: assert it again, as its press would have. */
static void reassert_held(void)
{
    int slot_idx;
    for (slot_idx = 0; slot_idx < MAX_PADS; slot_idx++) {
        int held[64 + 64 + MAX_HATS * APPLE2_PAD_HAT_DIRS];
        int nheld = 0, btn, hat, count;
        SDL_JoystickID devid;
        pad_slot *slot;

        pthread_mutex_lock(&g.lock);
        if (slot_idx >= g.npads) { pthread_mutex_unlock(&g.lock); break; }
        slot = &g.pads[slot_idx];
        devid = slot->devid;
        /* the port it plays from now on, so the first poll sees no move
         * (a move releases whatever the pad holds) */
        slot->port = effective_port_locked(slot_idx);
        slot->mode_gen = apple2_input_mode_generation();
        if (slot->handle) {
            for (btn = 0; btn < SDL_GAMEPAD_BUTTON_COUNT; btn++)
                if (SDL_GetGamepadButton(slot->handle, (SDL_GamepadButton)btn))
                    held[nheld++] = apple2_pad_button_from_sdl((uint8_t)btn);
        } else if (slot->joy) {
            count = SDL_GetNumJoystickButtons(slot->joy);
            for (btn = 0; btn < count && btn < 64; btn++)
                if (SDL_GetJoystickButton(slot->joy, btn))
                    held[nheld++] = APPLE2_PAD_BTN_RAW_BASE + btn;
            count = SDL_GetNumJoystickHats(slot->joy);
            for (hat = 0; hat < count && hat < MAX_HATS; hat++) {
                Uint8 mask = SDL_GetJoystickHat(slot->joy, hat);
                int dir = hat_dir_from_mask(mask);
                slot->last_hat[hat] = mask;
                if (dir >= 0)
                    held[nheld++] = APPLE2_PAD_HAT_BASE + hat * APPLE2_PAD_HAT_DIRS + dir;
            }
        }
        pthread_mutex_unlock(&g.lock);
        for (btn = 0; btn < nheld; btn++)
            dispatch_button(devid, held[btn], 1);
    }
}

/* ---- the stick and the knob --------------------------------------------------- */

static float axis_norm(Sint16 val)
{
    return val < 0 ? (float)val / 32768.0f : (float)val / 32767.0f;
}

/* Pad axis ax (0..5) as -1..1 (a trigger 0..1); a raw joystick without that
 * axis reads 0. */
static float slot_axis(const pad_slot *slot, int ax)
{
    if (slot->handle)
        return axis_norm(SDL_GetGamepadAxis(slot->handle, k_axis_sdl[ax]));
    return ax < SDL_GetNumJoystickAxes(slot->joy)
               ? axis_norm(SDL_GetJoystickAxis(slot->joy, ax)) : 0.0f;
}

/* The raw joystick's hat 0 as directions (a mapped gamepad's D-pad is a
 * button, bound like any other). */
static int slot_hat_dir(const pad_slot *slot)
{
    int dir = 0;
    if (!slot->handle && slot->joy && SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 mask = SDL_GetJoystickHat(slot->joy, 0);
        if (mask & SDL_HAT_UP) dir |= APPLE2_DIR_UP;
        if (mask & SDL_HAT_DOWN) dir |= APPLE2_DIR_DOWN;
        if (mask & SDL_HAT_LEFT) dir |= APPLE2_DIR_LEFT;
        if (mask & SDL_HAT_RIGHT) dir |= APPLE2_DIR_RIGHT;
    }
    return dir;
}

/* A trigger crossing its threshold. Dispatched by poll_pads once it has let
 * go of the lock: dispatch_button takes it too, and it is not recursive. */
typedef struct {
    SDL_JoystickID devid;
    int button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

static void poll_one(pad_slot *slot, int port, int mode, unsigned mode_gen,
                     trigger_edge *edges, int *nedges)
{
    uint8_t want[APPLE2_ACT_DOWN + 1];
    unsigned acts;
    int act, ax;

    memset(want, 0, sizeof want);
    if (port != slot->port || mode_gen != slot->mode_gen) {
        /* moved to another port (or none), or the mode changed under it:
         * release what it held there and start again */
        slot_release_all(slot);
        slot->port = port;
        slot->mode_gen = mode_gen;
        slot->sent_x = slot->sent_y = 0.0f;
        slot->axis_over = 0;
        if (port < 0) return;
    }

    /* a raw joystick's hat 0 steers: to the stops, or the knob's turn keys */
    acts = apple2_acts_from_dir(slot_hat_dir(slot));
    for (act = 0; act <= APPLE2_ACT_DOWN; act++)
        want[act] = (uint8_t)((acts >> act) & 1u);
    for (act = 0; act <= APPLE2_ACT_DOWN; act++) {
        if (want[act] != slot->sent[act]) {
            slot->sent[act] = want[act];
            apple2_input_pad_steer(slot->owner, port, act, want[act]);
        }
    }

    /* the joystick's stick: the left stick, proportionally, the dead zone taken
     * out so it still reaches the stops */
    if (mode == APPLE2_MODE_JOYSTICK) {
        const float xval = apple2_paddle_from_axis(slot_axis(slot, 0), APPLE2_STICK_DEADZONE);
        const float yval = apple2_paddle_from_axis(slot_axis(slot, 1), APPLE2_STICK_DEADZONE);
        if (xval != slot->sent_x || yval != slot->sent_y) {
            slot->sent_x = xval;
            slot->sent_y = yval;
            apple2_input_pad_analog(slot->owner, xval, yval);
        }
    }

    /* Map-mode axis capture: the first axis past 60% */
    if (g.cap_active && g.cap_axis) {
        uint32_t over = 0;
        for (ax = 0; ax < APPLE2_PAD_AXIS_COUNT; ax++)
            if (fabsf(slot_axis(slot, ax)) > AXIS_CAPTURE) over |= 1u << ax;
        for (ax = 0; ax < APPLE2_PAD_AXIS_COUNT && !g.cap_have; ax++)
            if (((over >> ax) & 1) && !((slot->axis_over >> ax) & 1)) {
                g.cap_have = 1;
                g.cap_result = APPLE2_PAD_AXIS_BASE + ax;
            }
        slot->axis_over = over;
    }

    /* a paddle's knob: an absolute axis (a trigger sweeps 0..1, a stick, or
     * any axis of a raw joystick, -1..1) */
    if (mode == APPLE2_MODE_PADDLES) {
        const int kax = bindings_knob_axis(port);
        if (kax >= 0 && !g.cap_active) {
            ax = kax - APPLE2_PAD_AXIS_BASE;
            apple2_input_pad_knob(slot->owner, port,
                apple2_knob_from_axis(slot_axis(slot, ax), ax >= 4 && slot->handle != NULL));
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int trig;
        for (trig = 0; trig < 2; trig++) {
            float level = slot_axis(slot, 4 + trig);
            int down = level >= TRIGGER_ON;
            if (down != slot->trigger_down[trig]) {
                slot->trigger_down[trig] = down;
                if (*nedges < MAX_EDGES) {
                    edges[*nedges].devid = slot->devid;
                    edges[*nedges].button = trig ? APPLE2_PAD_BTN_RIGHT_TRIGGER
                                                 : APPLE2_PAD_BTN_LEFT_TRIGGER;
                    edges[*nedges].down = down;
                    (*nedges)++;
                }
            }
        }
    }
}

static void poll_pads(void)
{
    int idx, nedges = 0;
    trigger_edge edges[MAX_EDGES];
    pthread_mutex_lock(&g.lock);
    {
        const int mode = apple2_input_mode();
        const unsigned mode_gen = apple2_input_mode_generation();
        for (idx = 0; idx < g.npads; idx++)
            poll_one(&g.pads[idx], effective_port_locked(idx), mode, mode_gen,
                     edges, &nedges);
    }
    pthread_mutex_unlock(&g.lock);
    for (idx = 0; idx < nedges; idx++)
        dispatch_button(edges[idx].devid, edges[idx].button, edges[idx].down);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    int count = 0, idx;
    Uint64 last_tick = 0;
    (void)arg;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (idx = 0; idx < count; idx++) open_pad(ids[idx]);
        SDL_free(ids);
    }
    if (g.quiet_scan) {
        g.quiet_scan = 0;
        reassert_held();
    }

    while (!atomic_load(&g.stop_requested)) {
        SDL_Event evt;
        int npads;
        Uint64 tnow;

        pthread_mutex_lock(&g.lock);
        npads = g.npads;
        pthread_mutex_unlock(&g.lock);
        while (SDL_PollEvent(&evt)) {
            switch (evt.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(evt.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(evt.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(evt.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(evt.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(evt.gbutton.which,
                                apple2_pad_button_from_sdl(evt.gbutton.button),
                                evt.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, slot_idx;
                pthread_mutex_lock(&g.lock);
                slot_idx = slot_for_id(evt.jbutton.which);
                gamepad = slot_idx >= 0 && g.pads[slot_idx].handle != NULL;
                pthread_mutex_unlock(&g.lock);
                if (gamepad || evt.jbutton.button >= 64) break;
                dispatch_button(evt.jbutton.which,
                                APPLE2_PAD_BTN_RAW_BASE + evt.jbutton.button,
                                evt.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(evt.jhat.which, evt.jhat.hat, evt.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_pads();
        /* ~60 Hz while a pad is attached or a knob key ramps. With none there
         * are only hotplug events to notice, which can wait a tenth of a
         * second. (Not SDL_WaitEventTimeout: SDL polls inside it at about
         * 1 kHz.) */
        SDL_Delay(npads > 0 || apple2_input_knob_active() ? 16 : 100);
        tnow = SDL_GetTicks();
        if (last_tick) {
            int dt = (int)(tnow - last_tick);
            apple2_input_tick(dt > 100 ? 100 : dt);
        }
        last_tick = tnow;
    }

    pthread_mutex_lock(&g.lock);
    for (idx = 0; idx < g.npads; idx++) slot_close(&g.pads[idx]);
    g.npads = 0;
    pthread_mutex_unlock(&g.lock);
    return NULL;
}

int apple2_gamepad_start(apple2session *sess)
{
    pthread_attr_t attr;
    if (g.running) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* the poll thread is not the focused window: keep reading pads when the
     * app is in the background (the debugger or another window has focus) */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        session_set_error(sess, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }

    pthread_mutex_lock(&g.lock);
    g.session = sess;
    atomic_store(&g.stop_requested, 0);
    g.npads = 0;
    g.cap_active = g.cap_have = g.cap_axis = 0;
    /* a restarted thread's first scan re-finds the same pads: no toast and
     * no generation change */
    g.quiet_scan = sess->pad_restarted;
    /* the remembered assignments, once: garbage or no key is an empty table */
    apple2_pad_table_decode(&g.table, apple2session_get_str(sess, "pad_assign", ""),
                           APPLE2SESSION_PADDLES);
    pthread_mutex_unlock(&g.lock);

    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g.thread, &attr, thread_main, NULL) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(sess, "gamepad thread could not start");
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
        return -1;
    }
    pthread_attr_destroy(&attr);
    g.running = 1;
    return 0;
}

void apple2_gamepad_stop(void)
{
    if (!g.running) return;
    atomic_store(&g.stop_requested, 1);
    pthread_join(g.thread, NULL);
    g.running = 0;
    if (g.session)
        g.session->pad_restarted = 1;
    g.session = NULL;
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
}

/* ---- public API ------------------------------------------------------------- */

int apple2session_gamepad_count(apple2session *sess)
{
    int num;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    num = g.npads;
    pthread_mutex_unlock(&g.lock);
    return num;
}

int apple2session_gamepad_name(apple2session *sess, int idx, char *dst, int dstsz)
{
    int num = 0;
    (void)sess;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g.lock);
    if (idx >= 0 && idx < g.npads)
        num = snprintf(dst, (size_t)dstsz, "%s", g.pads[idx].name);
    pthread_mutex_unlock(&g.lock);
    return num;
}

void apple2session_gamepad_assign(apple2session *sess, int idx, int port)
{
    char text[APPLE2_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;
    pthread_mutex_lock(&g.lock);
    if (idx >= 0 && idx < g.npads) {
        pad_slot *slot = &g.pads[idx];
        if (port >= 0 && port < APPLE2SESSION_PADDLES) {
            slot->assign = port;
            slot->by_name = 0;
            changed = apple2_pad_table_set(&g.table, slot->guidstr, slot->name, port) > 0;
        } else {
            const char *kguid[MAX_PADS], *kname[MAX_PADS];
            int kix, nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (kix = 0; kix < g.npads; kix++)
                if (kix != idx && g.pads[kix].assign >= 0) {
                    kguid[nkeep] = g.pads[kix].guidstr;
                    kname[nkeep++] = g.pads[kix].name;
                }
            changed = apple2_pad_table_remove(&g.table, slot->guidstr, slot->name,
                                             nkeep, kguid, kname);
        }
        if (changed && apple2_pad_table_encode(&g.table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&g.lock);
    /* the settings file is written here, on the caller's thread, with the
     * pad lock let go: the pad thread must never wait on a file write */
    if (changed)
        apple2session_set_str(sess, "pad_assign", text);
}

int apple2session_gamepad_assignment(apple2session *sess, int idx)
{
    int result = -1;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    if (idx >= 0 && idx < g.npads) result = g.pads[idx].assign;
    pthread_mutex_unlock(&g.lock);
    return result;
}

int apple2session_gamepad_effective_port(apple2session *sess, int idx)
{
    int result = -1;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    if (idx >= 0 && idx < g.npads) result = effective_port_locked(idx);
    pthread_mutex_unlock(&g.lock);
    return result;
}

unsigned apple2session_gamepad_generation(apple2session *sess)
{
    unsigned result;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    result = g.generation;
    pthread_mutex_unlock(&g.lock);
    return result;
}

int apple2session_gamepad_last_event(apple2session *sess, char *dst, int dstsz)
{
    int num;
    (void)sess;
    if (!dst || dstsz <= 0) return 0;
    pthread_mutex_lock(&g.lock);
    num = snprintf(dst, (size_t)dstsz, "%s", g.event);
    pthread_mutex_unlock(&g.lock);
    return num;
}

static void capture_begin(int axis)
{
    pthread_mutex_lock(&g.lock);
    g.cap_active = 1; g.cap_axis = axis; g.cap_have = 0; g.cap_result = APPLE2_PAD_BTN_NONE;
    pthread_mutex_unlock(&g.lock);
}

void apple2session_gamepad_capture_begin(apple2session *sess)
{
    (void)sess;
    capture_begin(0);
}

void apple2session_gamepad_capture_begin_axis(apple2session *sess)
{
    (void)sess;
    capture_begin(1);
}

void apple2session_gamepad_capture_cancel(apple2session *sess)
{
    (void)sess;
    pthread_mutex_lock(&g.lock);
    g.cap_active = 0; g.cap_have = 0; g.cap_axis = 0;
    pthread_mutex_unlock(&g.lock);
}

int apple2session_gamepad_capture_poll(apple2session *sess, int *id)
{
    int got = 0;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    if (g.cap_active && g.cap_have) {
        if (id) *id = g.cap_result;
        g.cap_active = 0; g.cap_have = 0; g.cap_axis = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g.lock);
    return got;
}

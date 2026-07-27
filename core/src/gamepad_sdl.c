/*
 * apple2session gamepad backend: SDL3 gamepad subsystem on a dedicated
 * polling thread. Hotplug is automatic (SDL's udev monitor runs inside
 * SDL_UpdateGamepads); the mapping mirrors the Android GameControllerMapper:
 * left stick PROPORTIONAL with d-pad fallback, 0.18 deadzone rescaled to full
 * travel, A/X -> button 0 (Open Apple), B/Y -> button 1 (Closed Apple).
 *
 * Unlike the ADAM's two digital joystick ports, the Apple II game port is a
 * single pair of analog paddles (PDL0/PDL1), so exactly one pad drives the
 * machine: the first connected, unless one is explicitly assigned.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "session_internal.h"

#define MAX_PADS 4
/* Matches the Android GameControllerMapper. Applied by
 * apple2_paddle_from_axis, which rescales the live travel so the paddle can
 * still reach its stops. */
#define DEADZONE 0.18f

typedef struct {
    SDL_Gamepad *pad;
    SDL_JoystickID id;
    int assigned;      /* 1 = explicitly bound to the paddles */
} pad_slot;

typedef struct {
    float x, y;
    int btn0, btn1;
} pad_reading;

typedef struct {
    pthread_t thread;
    volatile int run;
    pthread_mutex_t mtx;
    pad_slot pads[MAX_PADS];
    int count;
    int explicit_idx;  /* -1 = automatic (first connected) */
    pad_reading last;  /* last pushed, to skip redundant updates */
    int have_last;
} gamepad_state;

static void read_pad(SDL_Gamepad *pad, pad_reading *out)
{
    float ax = (float)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    float ay = (float)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;

    out->x = apple2_paddle_from_axis(ax, DEADZONE);
    out->y = apple2_paddle_from_axis(ay, DEADZONE);

    /* D-pad fallback: only when the stick is centred, so a pad with both
     * cannot fight itself. Digital input drives the paddle to its stop. */
    if (out->x == 0.0f) {
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
            out->x = -1.0f;
        else if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
            out->x = 1.0f;
    }
    if (out->y == 0.0f) {
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))
            out->y = -1.0f;
        else if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
            out->y = 1.0f;
    }

    out->btn0 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
                SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
    out->btn1 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ||
                SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
}

static void sync_devices(apple2session *s, gamepad_state *g)
{
    int i, n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);

    pthread_mutex_lock(&g->mtx);
    /* Drop pads that vanished. */
    for (i = 0; i < g->count;) {
        int present = 0, j;
        for (j = 0; j < n; j++)
            if (ids && ids[j] == g->pads[i].id) present = 1;
        if (!present) {
            SDL_CloseGamepad(g->pads[i].pad);
            memmove(&g->pads[i], &g->pads[i + 1],
                    (size_t)(g->count - i - 1) * sizeof(pad_slot));
            g->count--;
        } else {
            i++;
        }
    }
    /* Open pads that appeared. */
    for (i = 0; ids && i < n && g->count < MAX_PADS; i++) {
        int j, known = 0;
        for (j = 0; j < g->count; j++)
            if (g->pads[j].id == ids[i]) known = 1;
        if (!known) {
            SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
            if (pad) {
                g->pads[g->count].pad = pad;
                g->pads[g->count].id = ids[i];
                g->pads[g->count].assigned = 0;
                g->count++;
            }
        }
    }
    pthread_mutex_unlock(&g->mtx);
    SDL_free(ids);
    (void)s;
}

static void *gamepad_thread_main(void *arg)
{
    apple2session *s = arg;
    gamepad_state *g = s->gamepad;
    int tick = 0;

    apple2_thread_setname("apple2-gamepad");
    while (g->run) {
        int i;
        SDL_UpdateGamepads();
        /* Device add/remove is rare; re-enumerate at ~4Hz, poll state at
         * ~125Hz. Flush the (unused) event queue so it can't grow. */
        if (tick++ % 32 == 0)
            sync_devices(s, g);
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

        /* Exactly one pad drives the single pair of paddles: the explicitly
         * assigned one, else the first connected. */
        pthread_mutex_lock(&g->mtx);
        i = g->explicit_idx;
        if (i < 0 || i >= g->count) i = 0;
        if (g->count > 0) {
            pad_reading r;
            int changed;
            read_pad(g->pads[i].pad, &r);
            changed = !g->have_last || r.x != g->last.x || r.y != g->last.y ||
                      r.btn0 != g->last.btn0 || r.btn1 != g->last.btn1;
            if (changed) {
                int b0 = r.btn0, b1 = r.btn1;
                int b0_was = g->have_last ? g->last.btn0 : 0;
                int b1_was = g->have_last ? g->last.btn1 : 0;
                g->last = r;
                g->have_last = 1;
                pthread_mutex_unlock(&g->mtx);
                /* Pushed outside the lock: the session's input path is its
                 * own concern and must not be serialised behind pad polling. */
                apple2session_paddle(s, r.x, r.y);
                if (b0 != b0_was) apple2session_paddle_button(s, 0, b0);
                if (b1 != b1_was) apple2session_paddle_button(s, 1, b1);
                pthread_mutex_lock(&g->mtx);
            }
        }
        pthread_mutex_unlock(&g->mtx);
        SDL_Delay(8);
    }
    return NULL;
}

int gamepad_start(apple2session *s)
{
    gamepad_state *g;

    if (s->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof(*g));
    if (!g) return -1;
    pthread_mutex_init(&g->mtx, NULL);
    g->explicit_idx = -1;
    g->run = 1;
    s->gamepad = g;
    if (pthread_create(&g->thread, NULL, gamepad_thread_main, s) != 0) {
        s->gamepad = NULL;
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    return 0;
}

void gamepad_stop(apple2session *s)
{
    gamepad_state *g = s->gamepad;
    int i;
    if (!g) return;
    g->run = 0;
    pthread_join(g->thread, NULL);
    for (i = 0; i < g->count; i++)
        SDL_CloseGamepad(g->pads[i].pad);
    pthread_mutex_destroy(&g->mtx);
    s->gamepad = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

int apple2session_gamepad_count(apple2session *s)
{
    gamepad_state *g = s->gamepad;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->mtx);
    n = g->count;
    pthread_mutex_unlock(&g->mtx);
    return n;
}

int apple2session_gamepad_name(apple2session *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s->gamepad;
    const char *name = NULL;
    if (!g || dstsz <= 0) return 0;
    pthread_mutex_lock(&g->mtx);
    if (idx >= 0 && idx < g->count)
        name = SDL_GetGamepadName(g->pads[idx].pad);
    snprintf(dst, (size_t)dstsz, "%s", name ? name : "");
    pthread_mutex_unlock(&g->mtx);
    return (int)strlen(dst);
}

/* port is accepted for signature parity with the other targets; the Apple II
 * has a single game port, so any non-negative value binds this pad to it and
 * -1 restores automatic (first-connected) selection. */
void apple2session_gamepad_assign(apple2session *s, int idx, int port)
{
    gamepad_state *g = s->gamepad;
    int i;
    if (!g) return;
    pthread_mutex_lock(&g->mtx);
    if (idx >= 0 && idx < g->count && port >= 0) {
        for (i = 0; i < g->count; i++) g->pads[i].assigned = 0;
        g->pads[idx].assigned = 1;
        g->explicit_idx = idx;
    } else {
        for (i = 0; i < g->count; i++) g->pads[i].assigned = 0;
        g->explicit_idx = -1;
    }
    /* Force the next poll to push, so the newly selected pad takes effect
     * even if it happens to be reading exactly what the old one did. */
    g->have_last = 0;
    pthread_mutex_unlock(&g->mtx);
}

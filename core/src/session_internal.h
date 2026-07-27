/*
 * apple2session internals shared between the session, audio, gamepad, FujiNet
 * and debugger translation units. Nothing here is API; frontends must only
 * include apple2session.h / apple2debug.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2SESSION_INTERNAL_H
#define APPLE2SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "apple2session.h"

#define APPLE2_PATH_MAX 1024

/* Longest core-option label we store (AppleWin's longest is
 * "Enhanced Apple //e"); generous so a new card name cannot truncate. */
#define APPLE2_OPT_MAX 64

#define APPLE2_FB_MAX_PIXELS \
    (APPLE2SESSION_FB_MAX_WIDTH * APPLE2SESSION_FB_MAX_HEIGHT)

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct apple2session {
    /* ---- resolved paths ---- */
    char config_dir[APPLE2_PATH_MAX];
    char data_dir[APPLE2_PATH_MAX];
    char roms_dir[APPLE2_PATH_MAX];    /* user-supplied system ROMs */
    char symbols_dir[APPLE2_PATH_MAX]; /* AppleWin's *.SYM, for the debugger */
    char settings_file[APPLE2_PATH_MAX];
    char fujinet_lib[APPLE2_PATH_MAX];  /* "" = unavailable/disabled */
    char fujinet_src[APPLE2_PATH_MAX];  /* provisioning source override */
    char fujinet_root[APPLE2_PATH_MAX];
    char fujinet_config[APPLE2_PATH_MAX];
    char fujinet_sd[APPLE2_PATH_MAX];
    char fujinet_data[APPLE2_PATH_MAX];
    char webui_url[64];

    /* ---- settings store ---- */
    pthread_mutex_t settings_mtx;
    setting_kv *settings;
    int settings_dirty;

    /* ---- lifecycle ---- */
    pthread_mutex_t lifecycle_mtx;
    pthread_t emu_thread;
    int emu_thread_started;
    volatile int stop_flag;
    volatile int running;
    apple2session_start_opts opts;
    /* opts holds borrowed pointers; these own the strings it points at. */
    char opt_machine[APPLE2_OPT_MAX];
    char opt_slot3[APPLE2_OPT_MAX];
    char opt_slot4[APPLE2_OPT_MAX];
    char opt_slot5[APPLE2_OPT_MAX];
    char opt_slot6[APPLE2_OPT_MAX];
    char opt_slot7[APPLE2_OPT_MAX];
    char opt_video_mode[APPLE2_OPT_MAX];

    /* Reset requested from a UI thread, applied on the emulator thread
     * between frames: 0 none, 1 warm, 2 cold. AppleWin's reset entry points
     * touch core state that retro_run owns. */
    volatile int reset_request;

    /* Handshake: the emulator thread publishes whether the core came up, so
     * apple2session_start can wait for AppleWin's SmartPort listener to be
     * bound before it starts FujiNet. 0 = pending, 1 = up, -1 = failed. */
    pthread_mutex_t start_mtx;
    pthread_cond_t start_cv;
    int core_started;

    /* ---- latest-frame store (emulator thread -> UI thread) ----
     * Sized for the largest geometry the core can produce; frame_w/frame_h
     * say how much of it is live, and change at runtime when a VidHD card is
     * configured. */
    pthread_mutex_t frame_mtx;
    uint32_t *frame; /* APPLE2_FB_MAX_PIXELS XRGB8888, heap: ~1MB */
    int frame_w;
    int frame_h;
    uint64_t frame_serial;

    /* ---- vsync phase-lock (UI tick -> emulator pacing) ---- */
    pthread_mutex_t vs_mtx;
    pthread_cond_t vs_cv; /* CLOCK_MONOTONIC */
    int64_t vs_ns;        /* latest tick time */
    long vs_iv_us;        /* most recent tick interval */
    int64_t vs_seen_ns;   /* last tick the emulator paced on */

    /* ---- backends ---- */
    void *audio;   /* audio_sdl.c state */
    void *gamepad; /* gamepad_sdl.c state */
    volatile int audio_mute;

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    int fujinet_loaded;  /* dlopen'd (kept for process lifetime) */
    int fujinet_running;

    /* ---- debugger ---- */
    apple2debug *debugger;
    volatile int dbg_engaged; /* emulator loop runs the stepped path */

    char last_error[512];
};

/* session.c */
void session_set_error(apple2session *s, const char *fmt, ...);

/* settings.c */
void settings_init(apple2session *s);
void settings_free_all(apple2session *s);

/* paths.c */
int  paths_init(apple2session *s, const apple2session_paths *p);
int  paths_provision_fujinet(apple2session *s); /* fills fujinet_* or "" */
int  mkdir_p(const char *path);
int  copy_file(const char *src, const char *dst);

/* audio_sdl.c */
int  audio_start(apple2session *s);
void audio_stop(apple2session *s);

/* gamepad_sdl.c */
int  gamepad_start(apple2session *s);
void gamepad_stop(apple2session *s);

/* debugger.c -- all emulator-thread side except create/destroy. */
apple2debug *apple2debug_create(apple2session *s);
void apple2debug_destroy(apple2debug *d);
/* One debugger-controlled frame slice; returns 1 when a frame's worth of
 * cycles ran. May block while paused. */
int  apple2debug_session_frame(apple2session *s);
/* Unpark a paused debugger so a stopping emulator thread can exit. */
void apple2debug_resume_for_stop(apple2debug *d);

/* fujinet_runtime.c */
int  fujinet_start(apple2session *s);
void fujinet_stop(apple2session *s);
int  fujinet_copy_log(apple2session *s, char *dst, int max);
void fujinet_mix_audio(apple2session *s, int16_t *buf, int nsamples, int rate);

#endif /* APPLE2SESSION_INTERNAL_H */

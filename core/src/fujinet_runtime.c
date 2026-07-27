/*
 * apple2session FujiNet runtime control: dlopen libfujinet.so and drive the
 * fujinet_desktop_* entry points (the desktop build of fujinet-pc-apple2 plus
 * the in-process entry wrapper, tools/fujinet/support/fujinet_desktop_entry.cpp).
 * Port of the Android fujinet_android.cpp wrapper.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dynlib.h"
#include "session_internal.h"

typedef int (*start_runtime_fn)(const char *root, const char *config,
                                const char *sd, const char *data,
                                int listen_port);
typedef void (*stop_runtime_fn)(void);
typedef const char *(*last_error_fn)(void);
typedef int (*read_audio_fn)(int16_t *out, int max_samples, int rate);
typedef void (*clear_audio_fn)(void);
typedef int (*copy_log_fn)(char *out, int max_bytes);

/* One runtime per process: the library owns background threads (web admin,
 * network listeners) that live inside its mapping, so it is loaded once and
 * NEVER dlclose'd -- unmapping it while any such thread still runs executes
 * freed code and crashes. A stopped runtime is restarted through the same
 * handle. Same pattern (and reasoning) as the Android wrapper. */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static apple2_dynlib g_handle;
static start_runtime_fn g_start;
static stop_runtime_fn g_stop;
static last_error_fn g_last_error;
static read_audio_fn g_read_audio;
static clear_audio_fn g_clear_audio;
static copy_log_fn g_copy_log;

static int load_library_locked(apple2session *s)
{
    char errbuf[256];
    if (g_handle) return 0;

    g_handle = apple2_dynlib_open(s->fujinet_lib);
    if (!g_handle) {
        session_set_error(s, "FujiNet library load failed: %s",
                          apple2_dynlib_error(errbuf, sizeof(errbuf)));
        return -1;
    }
    g_start = (start_runtime_fn)apple2_dynlib_sym(
        g_handle, "fujinet_desktop_start_runtime");
    g_stop = (stop_runtime_fn)apple2_dynlib_sym(
        g_handle, "fujinet_desktop_stop_runtime");
    g_last_error = (last_error_fn)apple2_dynlib_sym(
        g_handle, "fujinet_desktop_last_error_message");
    g_read_audio = (read_audio_fn)apple2_dynlib_sym(
        g_handle, "fujinet_desktop_read_audio");
    g_clear_audio = (clear_audio_fn)apple2_dynlib_sym(
        g_handle, "fujinet_desktop_clear_audio");
    g_copy_log = (copy_log_fn)apple2_dynlib_sym(
        g_handle, "fujinet_desktop_copy_recent_log");

    if (!g_start || !g_stop || !g_last_error) {
        session_set_error(s, "%s is missing the desktop runtime contract",
                          s->fujinet_lib);
        /* Leave the handle mapped (never dlclose); just mark it unusable. */
        g_start = NULL;
        return -1;
    }
    return 0;
}

int fujinet_start(apple2session *s)
{
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    if (paths_provision_fujinet(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "apple2session: FujiNet runtime unavailable; "
                        "continuing without it\n");
        return -1;
    }
    if (load_library_locked(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    if (!g_start(s->fujinet_root, s->fujinet_config, s->fujinet_sd,
                 s->fujinet_data, APPLE2SESSION_SLIP_PORT)) {
        const char *err = g_last_error ? g_last_error() : NULL;
        session_set_error(s, "FujiNet runtime failed to start: %s",
                          err && *err ? err : "(unknown)");
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    s->fujinet_running = 1;
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

void fujinet_stop(apple2session *s)
{
    stop_runtime_fn stop = NULL;
    clear_audio_fn clear = NULL;

    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        stop = g_stop;
        clear = g_clear_audio;
        s->fujinet_running = 0;
    }
    pthread_mutex_unlock(&g_mtx);

    if (stop) stop();
    if (clear) clear();
}

int fujinet_copy_log(apple2session *s, char *dst, int max)
{
    copy_log_fn fn;
    (void)s;
    if (!dst || max <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_mtx);
    fn = g_copy_log;
    pthread_mutex_unlock(&g_mtx);
    return fn ? fn(dst, max) : 0;
}

/* Overlay FujiNet (SAM speech) audio onto the emulator mix, saturating.
 *
 * buf is interleaved stereo (nsamples counts int16 samples, so nsamples/2
 * frames) but the runtime produces MONO -- so each sample it hands back is
 * mixed into both channels of one frame. This is the one place the Apple II
 * target differs from the ADAM's, whose emulator mix is mono throughout. */
void fujinet_mix_audio(apple2session *s, int16_t *buf, int nsamples, int rate)
{
    static int16_t overlay[2048];
    read_audio_fn fn;
    int frames_left;

    if (!s->fujinet_running || nsamples <= 0) return;
    pthread_mutex_lock(&g_mtx);
    fn = g_read_audio;
    pthread_mutex_unlock(&g_mtx);
    if (!fn) return;

    frames_left = nsamples / 2;

    while (frames_left > 0) {
        int chunk = frames_left > (int)(sizeof(overlay) / sizeof(overlay[0]))
                        ? (int)(sizeof(overlay) / sizeof(overlay[0]))
                        : frames_left;
        int produced = fn(overlay, chunk, rate);
        int i;
        if (produced <= 0) return;
        for (i = 0; i < produced; i++) {
            int c;
            for (c = 0; c < 2; c++) {
                int mixed = (int)buf[i * 2 + c] + (int)overlay[i];
                if (mixed > INT16_MAX) mixed = INT16_MAX;
                if (mixed < INT16_MIN) mixed = INT16_MIN;
                buf[i * 2 + c] = (int16_t)mixed;
            }
        }
        if (produced < chunk) return;
        buf += (size_t)produced * 2;
        frames_left -= produced;
    }
}

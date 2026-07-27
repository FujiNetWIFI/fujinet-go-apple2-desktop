/*
 * apple2session -- session lifecycle and the paced emulator loop.
 *
 * Port of the Android host layer (apple2_host.cpp + session_runtime.cpp): the
 * emulator thread free-runs apple2host_core_run_frame (one retro_run, one
 * emulated frame) at the core's declared rate on a wall-clock sleeper,
 * phase-locking to the UI's vsync ticks whenever a steady ~60 Hz stream of
 * them arrives (apple2session_notify_vsync). Frames are published to a
 * latest-frame store the UI thread pulls on its own tick, so a stalled paint
 * can never freeze the 6502 -- and therefore never stall the SmartPort/SLIP
 * traffic FujiNet depends on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apple2_host.h"
#include "compat.h"
#include "session_internal.h"

/* Core options are registered by AppleWin's retroregistry.cpp under this
 * prefix ("applewin_machine", "applewin_slot7", ...). */
#define OPT_PREFIX "applewin_"

/****************************************************************************/
/** Small helpers                                                          **/
/****************************************************************************/

void session_set_error(apple2session *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
    fprintf(stderr, "apple2session: %s\n", s->last_error);
}

static long read_timer_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000L);
}

/* Absolute-deadline sleeping and monotonic condvars live in compat.h so
 * the same code paces correctly on Linux, macOS and Windows. */

static void core_log_sink(int level, const char *msg, void *user)
{
    (void)user;
    if (level >= 2) fprintf(stderr, "apple2core: %s\n", msg);
}

/****************************************************************************/
/** Core option tables                                                     **/
/****************************************************************************/

/* Exactly the labels AppleWin's retroregistry.cpp registers -- the core
 * matches on the string, so a typo here silently falls back to the option's
 * first value. */
static const char *const k_machines[] = {
    "Enhanced Apple //e", "Apple ][ (Original)", "Apple ][+",
    "Apple ][ J-Plus",    "Apple //e",           "Pravets 82",
    "Pravets 8M",         "Pravets 8A",          "Base64A",
    "TK3000 //e",         NULL
};
static const char *const k_slot3[] = { "Empty", "Video HD", NULL };
static const char *const k_slot4[] = { "Empty", "Mockingboard", "Mouse",
                                       "Phasor", NULL };
static const char *const k_slot5[] = { "Empty", "CP/M", "Mockingboard",
                                       "Phasor", "SAM/DAC", "FujiNet", NULL };
static const char *const k_slot7[] = { "Empty", "Hard Disk", "FujiNet", NULL };
/* Order matters only for the menus; the default is chosen by name below. */
static const char *const k_video_modes[] = {
    "Color (Composite Monitor)", "Color (Composite Idealized)", "Color TV",
    "B&W TV", "Monochrome (Amber)", "Monochrome (Green)",
    "Monochrome (White)", "Color (RGB Card/Monitor)", NULL
};

/* The core's own default video mode is "Color (RGB Card/Monitor)", which
 * emulates an RGB card. On a machine with no such card it renders a *blank
 * text screen* as blue horizontal banding instead of black -- the boot screen
 * is unreadable. Every other mode renders correctly, so default to the
 * composite monitor, which is also the most faithful to what an Apple II
 * actually looked like (and gives correct hi-res artifact colours). */
#define APPLE2_DEFAULT_VIDEO_MODE "Color (Composite Monitor)"

static const char *table_at(const char *const *tab, int idx)
{
    int n = 0;
    if (idx < 0) return NULL;
    while (tab[n]) n++;
    return idx < n ? tab[idx] : NULL;
}

const char *apple2session_machine_name(int idx) { return table_at(k_machines, idx); }
const char *apple2session_slot3_name(int idx)   { return table_at(k_slot3, idx); }
const char *apple2session_slot4_name(int idx)   { return table_at(k_slot4, idx); }
const char *apple2session_slot5_name(int idx)   { return table_at(k_slot5, idx); }
const char *apple2session_slot7_name(int idx)   { return table_at(k_slot7, idx); }
const char *apple2session_video_mode_name(int idx)
{
    return table_at(k_video_modes, idx);
}

/* Copy a setting into session-owned storage, falling back to def when the
 * stored value is not one the core would recognise (a hand-edited INI, or a
 * card that a future AppleWin dropped). */
static void adopt_opt(apple2session *s, char *dst, const char *const *tab,
                      const char *value, const char *def)
{
    int i;
    (void)s;
    if (value) {
        for (i = 0; tab[i]; i++) {
            if (strcmp(tab[i], value) == 0) {
                snprintf(dst, APPLE2_OPT_MAX, "%s", value);
                return;
            }
        }
    }
    snprintf(dst, APPLE2_OPT_MAX, "%s", def);
}

/****************************************************************************/
/** Vsync phase-lock (ticks fed by the frontends)                          **/
/****************************************************************************/

void apple2session_notify_vsync(apple2session *s, int64_t frame_time_ns)
{
    if (!s) return;
    pthread_mutex_lock(&s->vs_mtx);
    if (s->vs_ns)
        s->vs_iv_us = (long)((frame_time_ns - s->vs_ns) / 1000);
    s->vs_ns = frame_time_ns;
    pthread_cond_broadcast(&s->vs_cv);
    pthread_mutex_unlock(&s->vs_mtx);
}

/* True while ~60Hz ticks are arriving (window visible on a ~60Hz display). */
static int vsync_fresh(apple2session *s, long now_us)
{
    int fresh;
    pthread_mutex_lock(&s->vs_mtx);
    fresh = s->vs_ns != 0 &&
            (now_us - (long)(s->vs_ns / 1000)) < 100000 && /* fresh (<100ms) */
            s->vs_iv_us >= 15000 && s->vs_iv_us <= 18500;  /* ~55-66Hz */
    pthread_mutex_unlock(&s->vs_mtx);
    return fresh;
}

/* Block until the next UI tick after the one we last paced on and return its
 * time (us). Self-correcting: if a frame's work overran a tick, the next call
 * returns at once. Bails on a short timeout so it can never hang if the ticks
 * stop mid-wait (window hidden). */
static long wait_next_vsync(apple2session *s)
{
    long v;
    pthread_mutex_lock(&s->vs_mtx);
    while (!s->stop_flag && s->vs_ns <= s->vs_seen_ns) {
        /* 40ms safety timeout so this can never hang if the ticks stop. */
        if (apple2_cond_timedwait_ms(&s->vs_cv, &s->vs_mtx, 40) == ETIMEDOUT)
            break;
    }
    s->vs_seen_ns = s->vs_ns;
    v = (long)(s->vs_ns / 1000);
    pthread_mutex_unlock(&s->vs_mtx);
    return v;
}

/****************************************************************************/
/** Frame store                                                            **/
/****************************************************************************/

/* Called by the libretro host from inside retro_run, on the emulator thread.
 * The core only calls the video callback for a real frame (a duplicate is
 * signalled with a NULL buffer, which the host drops), so every call here is
 * a genuine change and the serial only advances on new content. */
static void on_core_frame(const uint32_t *px, int w, int h, void *user)
{
    apple2session *s = user;
    size_t n;

    if (w <= 0 || h <= 0) return;
    if (w > APPLE2SESSION_FB_MAX_WIDTH || h > APPLE2SESSION_FB_MAX_HEIGHT) {
        /* Should be impossible -- the core's largest mode is VidHD's 640x400
         * -- but clamping beats scribbling past the buffer if AppleWin ever
         * grows a bigger one. */
        fprintf(stderr, "apple2session: dropping oversized %dx%d frame "
                        "(max %dx%d)\n",
                w, h, APPLE2SESSION_FB_MAX_WIDTH, APPLE2SESSION_FB_MAX_HEIGHT);
        return;
    }

    n = (size_t)w * (size_t)h;
    pthread_mutex_lock(&s->frame_mtx);
    memcpy(s->frame, px, n * sizeof(uint32_t));
    s->frame_w = w;
    s->frame_h = h;
    s->frame_serial++;
    pthread_mutex_unlock(&s->frame_mtx);
}

int apple2session_copy_frame(apple2session *s, uint32_t *dst,
                             uint64_t *serial_inout, int *w_out, int *h_out)
{
    int copied = 0;
    pthread_mutex_lock(&s->frame_mtx);
    if (s->frame_serial != *serial_inout && s->frame_w > 0) {
        memcpy(dst, s->frame,
               (size_t)s->frame_w * (size_t)s->frame_h * sizeof(uint32_t));
        *serial_inout = s->frame_serial;
        if (w_out) *w_out = s->frame_w;
        if (h_out) *h_out = s->frame_h;
        copied = 1;
    }
    pthread_mutex_unlock(&s->frame_mtx);
    return copied;
}

/****************************************************************************/
/** Emulator thread                                                        **/
/****************************************************************************/

static void *emu_thread_main(void *arg)
{
    apple2session *s = arg;
    long next_us;
    long frame_us;
    const int pace_log = getenv("APPLE2_PACE_LOG") != NULL;

    apple2_thread_setname("apple2-emu");

    /* Core options must be pushed before the core loads: it reads them at
     * load time, and SET_VARIABLES deliberately will not clobber a key that
     * is already present. */
    apple2host_set_variable(OPT_PREFIX "machine", s->opt_machine);
    apple2host_set_variable(OPT_PREFIX "slot3", s->opt_slot3);
    apple2host_set_variable(OPT_PREFIX "slot4", s->opt_slot4);
    apple2host_set_variable(OPT_PREFIX "slot5", s->opt_slot5);
    apple2host_set_variable(OPT_PREFIX "slot7", s->opt_slot7);
    apple2host_set_variable(OPT_PREFIX "video_mode", s->opt_video_mode);

    apple2host_set_log_sink(core_log_sink, s);
    apple2host_set_frame_sink(on_core_frame, s);

    {
        int ok = apple2host_core_start();
        if (!ok) {
            session_set_error(s, "the AppleWin core failed to start");
            apple2host_set_frame_sink(NULL, NULL);
        }
        /* Publish the result either way: apple2session_start is blocked on
         * this, and a failed core must not leave it waiting. By the time
         * core_start returns, AppleWin has inserted the slot cards -- so the
         * SmartPort listener is bound and FujiNet can be started. */
        pthread_mutex_lock(&s->start_mtx);
        s->core_started = ok ? 1 : -1;
        pthread_cond_broadcast(&s->start_cv);
        pthread_mutex_unlock(&s->start_mtx);
        if (!ok) {
            s->running = 0;
            return NULL;
        }
    }

    /* Pace from what the core actually declares rather than a hard-coded
     * constant, so a core change (or a PAL-rate machine) is picked up. */
    {
        double fps = apple2host_frame_rate();
        if (!(fps > 1.0)) fps = 60.0;
        frame_us = (long)(1000000.0 / fps + 0.5);
    }

    next_us = read_timer_us();
    while (!s->stop_flag) {
        /* Resets are requested from UI threads but must be applied here:
         * they reach into core state that retro_run owns. */
        int req = s->reset_request;
        if (req) {
            s->reset_request = 0;
            if (req == 2)
                apple2host_power_cycle();
            else
                apple2host_ctrl_reset();
        }

        apple2host_core_run_frame();

        /* Pacing: one emulated frame per UI vsync tick while ticks arrive,
         * else the wall clock. Once-per-second diagnostics with
         * APPLE2_PACE_LOG=1. */
        {
            long now = read_timer_us();
            int vfresh = vsync_fresh(s, now);
            static long dbg_t0;
            static int dbg_frames, dbg_behind, dbg_vsync;

            if (dbg_t0 == 0) dbg_t0 = now;
            dbg_frames++;
            if (vfresh) dbg_vsync++;

            next_us += frame_us;
            if (now >= next_us) dbg_behind++;

            if (now - dbg_t0 >= 1000000L) {
                if (pace_log)
                    fprintf(stderr,
                            "apple2session pace: %d fps over %ld ms (%d behind, "
                            "%d on vsync)\n",
                            dbg_frames, (now - dbg_t0) / 1000, dbg_behind,
                            dbg_vsync);
                dbg_t0 = now;
                dbg_frames = dbg_behind = dbg_vsync = 0;
            }

            if (vfresh) {
                /* Snap the wall clock so a later fallback resumes from real
                 * time rather than a stale schedule. */
                next_us = wait_next_vsync(s);
            } else if (now < next_us) {
                apple2_sleep_until_us(next_us);
            } else {
                next_us = now; /* behind: don't accumulate debt */
            }
        }
    }

    apple2host_core_stop();
    /* Drop the sink before the session's frame buffer can go away. */
    apple2host_set_frame_sink(NULL, NULL);
    s->running = 0;
    return NULL;
}

/****************************************************************************/
/** Lifecycle                                                              **/
/****************************************************************************/

apple2session *apple2session_new(const apple2session_paths *paths)
{
    apple2session *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    /* ~1MB, so heap rather than inline in the struct. */
    s->frame = calloc(APPLE2_FB_MAX_PIXELS, sizeof(uint32_t));
    if (!s->frame) {
        free(s);
        return NULL;
    }

    if (paths_init(s, paths) != 0) {
        free(s->frame);
        free(s);
        return NULL;
    }

    pthread_mutex_init(&s->lifecycle_mtx, NULL);
    pthread_mutex_init(&s->frame_mtx, NULL);
    pthread_mutex_init(&s->vs_mtx, NULL);
    pthread_mutex_init(&s->start_mtx, NULL);
    apple2_cond_init_monotonic(&s->vs_cv);
    apple2_cond_init_monotonic(&s->start_cv);

    settings_init(s);
    return s;
}

void apple2session_free(apple2session *s)
{
    if (!s) return;
    apple2session_stop(s);
    apple2session_settings_flush(s);
    settings_free_all(s);
    pthread_mutex_destroy(&s->lifecycle_mtx);
    pthread_mutex_destroy(&s->frame_mtx);
    pthread_mutex_destroy(&s->vs_mtx);
    pthread_mutex_destroy(&s->start_mtx);
    pthread_cond_destroy(&s->vs_cv);
    pthread_cond_destroy(&s->start_cv);
    free(s->frame);
    free(s);
}

void apple2session_default_opts(apple2session *s,
                                apple2session_start_opts *opts)
{
    memset(opts, 0, sizeof(*opts));
    /* FujiNet in slot 7 by default: it is the bootable SmartPort slot the
     * //e autostart scans before the Disk][ in slot 6, so the machine comes
     * up in FujiNet's CONFIG. */
    adopt_opt(s, s->opt_machine, k_machines,
              apple2session_get_str(s, "machine", NULL), k_machines[0]);
    adopt_opt(s, s->opt_slot3, k_slot3,
              apple2session_get_str(s, "slot3", NULL), "Empty");
    adopt_opt(s, s->opt_slot4, k_slot4,
              apple2session_get_str(s, "slot4", NULL), "Mockingboard");
    adopt_opt(s, s->opt_slot5, k_slot5,
              apple2session_get_str(s, "slot5", NULL), "Empty");
    adopt_opt(s, s->opt_slot7, k_slot7,
              apple2session_get_str(s, "slot7", NULL), "FujiNet");
    adopt_opt(s, s->opt_video_mode, k_video_modes,
              apple2session_get_str(s, "video_mode", NULL),
              APPLE2_DEFAULT_VIDEO_MODE);

    opts->machine = s->opt_machine;
    opts->slot3 = s->opt_slot3;
    opts->slot4 = s->opt_slot4;
    opts->slot5 = s->opt_slot5;
    opts->slot7 = s->opt_slot7;
    opts->video_mode = s->opt_video_mode;
    opts->enable_fujinet = 1;
    opts->enable_audio = 1;
    opts->enable_gamepad = 1;
}

int apple2session_start(apple2session *s, const apple2session_start_opts *opts)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (s->running) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return 0;
    }

    s->opts = *opts;
    /* Take our own copies: opts may point at caller storage that goes away,
     * and the emulator thread reads these after this call returns. */
    adopt_opt(s, s->opt_machine, k_machines, opts->machine, k_machines[0]);
    adopt_opt(s, s->opt_slot3, k_slot3, opts->slot3, "Empty");
    adopt_opt(s, s->opt_slot4, k_slot4, opts->slot4, "Mockingboard");
    adopt_opt(s, s->opt_slot5, k_slot5, opts->slot5, "Empty");
    adopt_opt(s, s->opt_slot7, k_slot7, opts->slot7, "FujiNet");
    adopt_opt(s, s->opt_video_mode, k_video_modes, opts->video_mode,
              APPLE2_DEFAULT_VIDEO_MODE);
    s->opts.machine = s->opt_machine;
    s->opts.slot3 = s->opt_slot3;
    s->opts.slot4 = s->opt_slot4;
    s->opts.slot5 = s->opt_slot5;
    s->opts.slot7 = s->opt_slot7;
    s->opts.video_mode = s->opt_video_mode;

    s->stop_flag = 0;
    s->reset_request = 0;
    s->frame_serial = 0;
    s->frame_w = 0;
    s->frame_h = 0;
    s->vs_ns = 0;
    s->vs_seen_ns = 0;
    s->vs_iv_us = 0;
    s->core_started = 0;
    s->running = 1;
    if (pthread_create(&s->emu_thread, NULL, emu_thread_main, s) != 0) {
        s->running = 0;
        session_set_error(s, "could not start the emulator thread");
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return -1;
    }
    s->emu_thread_started = 1;

    /* THE EMULATOR MUST BE UP BEFORE FUJINET.
     *
     * This is the opposite of the ADAM target's order, and the difference is
     * not cosmetic. On ADAM the FujiNet BoIP client retries in the background
     * and start_runtime returns immediately, so either order works. Here,
     * FujiNet's APPLE build blocks inside its setup (iwm_slip::setup_spi)
     * until the SmartPort/SLIP connection is made -- so starting it first
     * deadlocks the whole session: start_runtime never returns, the emulator
     * thread is never created, AppleWin never binds the listener, and FujiNet
     * waits for a connection that nothing will ever accept. The Android app
     * starts the emulator first for this same reason.
     *
     * Wait for the core to report itself up (AppleWin inserts the slot cards
     * during core_start, which is what binds the listener), then start
     * FujiNet. The wait is bounded so a wedged core cannot hang the caller. */
    if (opts->enable_fujinet) {
        int up;
        pthread_mutex_lock(&s->start_mtx);
        while (s->core_started == 0) {
            if (apple2_cond_timedwait_ms(&s->start_cv, &s->start_mtx, 15000)
                == ETIMEDOUT)
                break;
        }
        up = s->core_started;
        pthread_mutex_unlock(&s->start_mtx);

        if (up == 1) {
            /* A missing or broken runtime is not fatal: the machine still
             * boots, just with no FujiNet device on the bus. */
            if (fujinet_start(s) == 0) {
                /* The Apple II finishes its boot-slot scan in about a second,
                 * long before FujiNet has finished connecting -- so it finds
                 * no SmartPort device and falls through to whatever is in a
                 * lower slot. fujinet_start only returns once the runtime is
                 * up and its SLIP client has attached, so this is exactly the
                 * moment to restart the machine and let the //e autostart ROM
                 * find the device this time. */
                s->reset_request = 2; /* cold: re-runs the boot scan */
            }
        } else {
            fprintf(stderr, "apple2session: emulator core did not come up; "
                            "starting without FujiNet\n");
        }
    }

    if (opts->enable_audio)
        audio_start(s);
    if (opts->enable_gamepad)
        gamepad_start(s);

    pthread_mutex_unlock(&s->lifecycle_mtx);
    return 0;
}

void apple2session_stop(apple2session *s)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->emu_thread_started) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return;
    }
    s->stop_flag = 1;
    pthread_mutex_lock(&s->start_mtx);
    if (s->core_started == 0) s->core_started = -1;
    pthread_cond_broadcast(&s->start_cv);
    pthread_mutex_unlock(&s->start_mtx);
    pthread_mutex_lock(&s->vs_mtx);
    pthread_cond_broadcast(&s->vs_cv);
    pthread_mutex_unlock(&s->vs_mtx);
    /* Unblock a fill_audio parked waiting for samples the stopped emulator
     * will never produce. */
    apple2host_audio_set_active(0);
    pthread_join(s->emu_thread, NULL);
    s->emu_thread_started = 0;
    audio_stop(s);
    gamepad_stop(s);
    fujinet_stop(s);
    apple2host_audio_set_active(1);
    s->running = 0;
    pthread_mutex_unlock(&s->lifecycle_mtx);
}

int apple2session_is_running(const apple2session *s)
{
    return s && s->running;
}

const char *apple2session_last_error(const apple2session *s)
{
    return s->last_error;
}

/****************************************************************************/
/** Input / audio / misc accessors                                         **/
/****************************************************************************/

void apple2session_key(apple2session *s, int down, uint32_t keysym,
                       uint32_t unicode, int ctrl_down, int shift_down)
{
    unsigned keycode = 0;
    uint32_t ch = 0;
    uint16_t mods = 0;

    if (!s->running) return;
    if (!apple2_key_from_event(keysym, unicode, ctrl_down, shift_down,
                               &keycode, &ch, &mods))
        return;
    apple2host_inject_key(down, keycode, ch, mods);
}

void apple2session_paddle(apple2session *s, float x, float y)
{
    if (!s->running) return;
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f;
    if (y > 1.0f) y = 1.0f;
    apple2host_set_joystick_axis(0, 0, (int16_t)(x * 32767.0f));
    apple2host_set_joystick_axis(0, 1, (int16_t)(y * 32767.0f));
}

void apple2session_paddle_button(apple2session *s, int button, int pressed)
{
    /* RETRO_DEVICE_ID_JOYPAD_A (8) is Apple button 0 / Open Apple;
     * RETRO_DEVICE_ID_JOYPAD_B (0) is Apple button 1 / Closed Apple. */
    if (!s->running) return;
    apple2host_set_joystick_button(0, button == 0 ? 8 : 0, pressed);
}

void apple2session_reset(apple2session *s, int mode)
{
    /* Latched here and applied by the emulator thread between frames. */
    if (s->running) s->reset_request = mode ? 2 : 1;
}

int apple2session_render_audio(apple2session *s, int16_t *out, int nsamples)
{
    if (!s->running || s->audio_mute) {
        memset(out, 0, (size_t)nsamples * sizeof(int16_t));
        return nsamples;
    }
    apple2host_fill_audio(out, nsamples);
    fujinet_mix_audio(s, out, nsamples, 44100);
    return nsamples;
}

const char *apple2session_config_path(const apple2session *s) { return s->config_dir; }
const char *apple2session_data_path(const apple2session *s) { return s->data_dir; }
const char *apple2session_sd_path(const apple2session *s) { return s->fujinet_sd; }

const char *apple2session_fujinet_webui_url(const apple2session *s)
{
    return s->webui_url;
}

int apple2session_fujinet_running(const apple2session *s)
{
    return s->fujinet_running;
}

int apple2session_fujinet_copy_log(apple2session *s, char *dst, int max)
{
    return fujinet_copy_log(s, dst, max);
}

apple2debug *apple2session_debugger(apple2session *s)
{
    /* The debugger engine lands with the 6502 disassembler; until then the
     * frontends' F12 handlers see NULL and leave the menu item insensitive. */
    (void)s;
    return NULL;
}

/****************************************************************************/
/** Media import                                                           **/
/****************************************************************************/

static const char *path_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return "";
    return dot + 1;
}

static int ext_is(const char *ext, const char *want)
{
    for (; *ext && *want; ext++, want++)
        if ((*ext | 0x20) != *want) return 0;
    return *ext == '\0' && *want == '\0';
}

int apple2session_import_media(apple2session *s, const char *src_path,
                               char *dest_out, int dest_sz)
{
    static const char *const k_disk_exts[] = {
        "dsk", "do", "po", "2mg", "nib", "woz", "hdv", NULL
    };
    const char *ext = path_ext(src_path);
    const char *base = strrchr(src_path, '/');
    char dst[APPLE2_PATH_MAX];
    int i, ok = 0;

    base = base ? base + 1 : src_path;
    for (i = 0; k_disk_exts[i]; i++) {
        if (ext_is(ext, k_disk_exts[i])) { ok = 1; break; }
    }
    if (!ok) {
        session_set_error(s, "Unsupported media type \".%s\" (expected a disk "
                          "image: .dsk .do .po .2mg .nib .woz or .hdv)", ext);
        return -1;
    }
    if (!s->fujinet_sd[0]) {
        session_set_error(s, "No FujiNet SD directory (FujiNet runtime "
                          "unavailable)");
        return -1;
    }

    mkdir_p(s->fujinet_sd);
    snprintf(dst, sizeof(dst), "%s/%s", s->fujinet_sd, base);
    if (copy_file(src_path, dst) != 0) {
        session_set_error(s, "Could not copy %s to %s", src_path, dst);
        return -1;
    }
    if (dest_out && dest_sz > 0)
        snprintf(dest_out, (size_t)dest_sz, "%s", dst);
    return 0;
}

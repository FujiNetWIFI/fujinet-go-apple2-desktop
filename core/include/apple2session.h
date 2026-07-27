/*
 * apple2session -- toolkit-agnostic desktop session for FujiNet Go Apple II.
 *
 * Owns the AppleWin libretro core (driven on its own paced thread through
 * core/applewin/apple2_host.h), the SDL audio and gamepad backends, the
 * in-process FujiNet runtime (dlopen'd libfujinet.so, joined to the core over
 * SmartPort-over-SLIP on loopback TCP 1985), the shared settings store, and
 * the media path layout. Frontends (GTK, Qt, AppKit, Win32) drive this API
 * and only do windowing, painting, and event translation.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2SESSION_H
#define APPLE2SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame geometry is NOT fixed: a stock machine renders 560x384, and a VidHD
 * card in slot 3 switches the core to 640x400. Frontends allocate for the
 * maximum and paint whatever apple2session_copy_frame reports. */
#define APPLE2SESSION_FB_MAX_WIDTH  640
#define APPLE2SESSION_FB_MAX_HEIGHT 400

/* SmartPort-over-SLIP loopback port (the emulator listens, FujiNet connects
 * in) and the FujiNet web admin port, both fixed to match the Android app. */
#define APPLE2SESSION_SLIP_PORT  1985
#define APPLE2SESSION_WEBUI_PORT 8000

typedef struct apple2session apple2session;
typedef struct apple2debug apple2debug;

/* All members optional (NULL = default).
 *  config_dir: default $XDG_CONFIG_HOME/fujinet-go-apple2
 *  data_dir:   default $XDG_DATA_HOME/fujinet-go-apple2
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *              $FUJINET_LIB, the executable's directory, the install libdir,
 *              then tools/fujinet/work/out. "" disables.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini +
 *              data/ + SD/ used to provision the user's runtime tree on
 *              first start (a macOS app passes its bundle's
 *              Resources/fujinet here); default searches the install
 *              share dir, then tools/fujinet/work/out. */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} apple2session_paths;

/* Creates the session, provisions the config/data directories, and loads the
 * shared settings store. Does not start emulation. Returns NULL only on
 * out-of-memory / unusable directories. */
apple2session *apple2session_new(const apple2session_paths *paths);
void apple2session_free(apple2session *s);

/* ---- settings (shared INI; one store for all frontends/platforms) ------- */
int         apple2session_get_int(apple2session *s, const char *key, int def);
void        apple2session_set_int(apple2session *s, const char *key, int value);
const char *apple2session_get_str(apple2session *s, const char *key,
                                  const char *def);
void        apple2session_set_str(apple2session *s, const char *key,
                                  const char *value);
void        apple2session_settings_flush(apple2session *s);

/* ---- lifecycle -----------------------------------------------------------
 * machine and the slot cards are libretro core options, read by the core when
 * it loads: changing any of them requires a stop+start. The strings are the
 * exact option labels AppleWin's retroregistry.cpp registers -- see
 * apple2session_machine_name() and friends for the valid sets. */
typedef struct {
    const char *machine;    /* e.g. "Enhanced Apple //e" */
    const char *slot3;      /* "Empty" | "Video HD" */
    const char *slot4;      /* "Empty" | "Mockingboard" | "Mouse" | "Phasor" */
    const char *slot5;      /* "Empty" | "CP/M" | ... | "SAM/DAC" | "FujiNet" */
    const char *slot6;      /* "Disk II" | "Empty" -- see the note below */
    const char *slot7;      /* "Empty" | "Hard Disk" | "FujiNet" */
    /* How the video signal is rendered. NOTE: the core's own default is
     * "Color (RGB Card/Monitor)", which emulates an RGB card -- on a machine
     * that has no such card it renders a blank text screen as blue banding
     * rather than black, so this defaults to the composite monitor instead.
     * See apple2session_video_mode_name(). */
    const char *video_mode;
    int enable_fujinet;     /* start the in-process FujiNet runtime */
    int enable_audio;       /* open the SDL audio device */
    int enable_gamepad;     /* start the SDL gamepad thread */
} apple2session_start_opts;

/* Fills opts from the settings store (defaults: Enhanced Apple //e, FujiNet
 * in slot 7, Mockingboard in slot 4, everything enabled). Convenience so
 * frontends launch with the persisted config. The strings point into the
 * session and stay valid until the next settings change. */
void apple2session_default_opts(apple2session *s,
                                apple2session_start_opts *opts);

/* The option labels the core accepts, for building menus. Returns NULL past
 * the end, so callers can walk idx = 0.. until NULL. */
const char *apple2session_machine_name(int idx);
const char *apple2session_slot3_name(int idx);
const char *apple2session_slot4_name(int idx);
const char *apple2session_slot5_name(int idx);
const char *apple2session_slot6_name(int idx);
const char *apple2session_slot7_name(int idx);
const char *apple2session_video_mode_name(int idx);

/* Starts FujiNet (if enabled) and the emulator thread. Returns 0 on success,
 * -1 with apple2session_last_error() set. Restart (stop+start) applies
 * machine/slot option changes. */
int  apple2session_start(apple2session *s, const apple2session_start_opts *opts);
void apple2session_stop(apple2session *s);
int  apple2session_is_running(const apple2session *s);
const char *apple2session_last_error(const apple2session *s);

/* ---- video ---------------------------------------------------------------
 * The emulator thread stores the latest changed frame; the UI thread pulls it
 * on its own vsync/frame-clock tick. copy_frame copies the latest frame into
 * dst (XRGB8888, tightly packed, at most FB_MAX_WIDTH*FB_MAX_HEIGHT uint32)
 * iff its serial differs from *serial_inout, updates *serial_inout, writes
 * the frame's dimensions through w_out and h_out, and returns 1; returns 0
 * when the frame is unchanged (dst, w_out and h_out untouched). Pass
 * *serial_inout = 0 to force a copy (e.g. first paint after a window map).
 *
 * The dimensions can change at runtime -- a VidHD card in slot 3 switches the
 * core between 560x384 and 640x400 -- so paint what is reported rather than
 * caching it. */
int  apple2session_copy_frame(apple2session *s, uint32_t *dst,
                              uint64_t *serial_inout, int *w_out, int *h_out);

/* Feed the UI's vsync ticks (frame-clock time, CLOCK_MONOTONIC ns). While a
 * steady ~60Hz tick stream arrives, the emulator phase-locks one emulated
 * frame per tick; otherwise it paces on the wall clock at the core's declared
 * rate (60 Hz). */
void apple2session_notify_vsync(apple2session *s, int64_t frame_time_ns);

/* ---- input ---------------------------------------------------------------
 * Keys are delivered to the core in libretro terms. Frontends translate their
 * toolkit's key events to X11/xkb keysyms (GDK keyvals already are; Qt and
 * Win32 map through a small table), then call apple2session_key, so one
 * mapping table serves every frontend. */
void apple2session_key(apple2session *s, int down, uint32_t keysym,
                       uint32_t unicode, int ctrl_down, int shift_down);

/* Analog paddles PDL0/PDL1: x and y in -1.0..1.0, centred at 0. */
void apple2session_paddle(apple2session *s, float x, float y);
/* button 0 = Open Apple, button 1 = Closed Apple. */
void apple2session_paddle_button(apple2session *s, int button, int pressed);

/* mode 0 = warm reset (Ctrl-Reset, drops to BASIC),
 * mode 1 = cold reset / power cycle (Ctrl-OpenApple-Reset, reboots). */
void apple2session_reset(apple2session *s, int mode);

/* Translate a desktop key event to the libretro triple the core expects.
 * keysym is an X11/xkb keysym, unicode the translated character (0 if none),
 * ctrl_down/shift_down the modifier state. Writes the RETROK_* keycode to
 * *keycode_out, the character to *char_out (forced to 0 for Ctrl combos, so
 * the core resolves them from the keycode) and the RETROKMOD_* mask to
 * *mods_out. Returns 1 when the key is forwarded, 0 when it is not.
 *
 * Pure function, shared by every frontend and unit-tested on its own. */
int apple2_key_from_event(uint32_t keysym, uint32_t unicode, int ctrl_down,
                          int shift_down, unsigned *keycode_out,
                          uint32_t *char_out, uint16_t *mods_out);

/* Apply a gamepad stick reading to a paddle axis: applies the deadzone and
 * rescales the remainder to the full -1..1 range so the paddle still reaches
 * its stops. Pure function; shared with the SDL gamepad backend. */
float apple2_paddle_from_axis(float v, float deadzone);

/* ---- gamepads (SDL, hotplug; started by apple2session_start) ------------- */
int  apple2session_gamepad_count(apple2session *s);
/* Name of connected pad idx into dst; returns length or 0. */
int  apple2session_gamepad_name(apple2session *s, int idx, char *dst, int dstsz);
/* Bind pad idx to the paddles; -1 restores automatic assignment (first pad). */
void apple2session_gamepad_assign(apple2session *s, int idx, int port);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend that
 * wants to own the device instead can pull mixed emulator+FujiNet interleaved
 * stereo S16 samples at 44100 Hz. nsamples counts int16 samples, so one
 * stereo frame is two. */
int  apple2session_render_audio(apple2session *s, int16_t *out, int nsamples);

/* ---- FujiNet -------------------------------------------------------------*/
int         apple2session_fujinet_running(const apple2session *s);
const char *apple2session_fujinet_webui_url(const apple2session *s);
/* Copies the most recent FujiNet console output (NUL-terminated) into dst;
 * returns the number of bytes written (excluding the NUL). */
int         apple2session_fujinet_copy_log(apple2session *s, char *dst, int max);

/* Import media for the emulated machine: disk/hard-disk images
 * (.dsk .do .po .2mg .nib .woz .hdv) go to the FujiNet SD directory, to be
 * mounted from the web UI. Returns 0 on success and writes the destination
 * path into dest_out. */
int apple2session_import_media(apple2session *s, const char *src_path,
                               char *dest_out, int dest_sz);

/* Directory paths (valid for the session's lifetime). */
const char *apple2session_config_path(const apple2session *s);
const char *apple2session_data_path(const apple2session *s);
const char *apple2session_sd_path(const apple2session *s);

/* ---- debugger ------------------------------------------------------------
 * Lazily created; engaging it (breakpoints/pause) switches the emulator loop
 * to the instruction-stepped core path. See apple2debug.h. */
apple2debug *apple2session_debugger(apple2session *s);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2SESSION_H */

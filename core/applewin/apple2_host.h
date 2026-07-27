/*
 * apple2_host -- the libretro frontend for the staged AppleWin core.
 *
 * There is no RetroArch here: AppleWin's libretro frontend is built as a
 * STATIC library and linked whole into libapple2session, so the retro_*
 * entry points are called directly rather than dlsym'd out of a core.
 *
 * This is the ONLY translation unit that includes AppleWin headers. It must
 * not include apple2session.h -- the session talks to it through this C API
 * and nothing else, which is what keeps AppleWin's C++ (and its `Apple2*`,
 * `A2_*`, `CT_*` namespace) out of the rest of the tree.
 *
 * Ported from fujinet-go-apple2's app/src/main/cpp/apple2_host.cpp, which
 * does the same job for the Android app; the differences are the logging
 * seam (Android had __android_log_print wired in directly) and nothing else.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_HOST_H
#define APPLE2_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest frame the core can produce: 640x400 with a VidHD card in slot 3
 * (kVideoWidthIIgs/kVideoHeightIIgs), against 560x384 for a stock machine.
 * Callers size their buffers to the max and use the width/height the frame
 * sink reports. */
#define APPLE2_HOST_MAX_WIDTH  640
#define APPLE2_HOST_MAX_HEIGHT 400

/* Native pixel format is XRGB8888 -- the core is told so at
 * RETRO_ENVIRONMENT_SET_PIXEL_FORMAT and no other format is accepted. Rows
 * are tightly packed (the host repacks away the core's pitch), so a frame is
 * exactly width*height uint32_t. */
typedef void (*Apple2FrameSink)(const uint32_t *xrgb8888, int width, int height,
                                void *user);
void apple2host_set_frame_sink(Apple2FrameSink sink, void *user);

/* Diagnostics from the core and from this layer. level: 0 debug, 1 info,
 * 2 warn, 3 error. With no sink installed, warnings and errors go to stderr
 * and the rest is dropped. */
typedef void (*Apple2LogSink)(int level, const char *msg, void *user);
void apple2host_set_log_sink(Apple2LogSink sink, void *user);

/* Core options (libretro "variables"), e.g. key "applewin_slot7" value
 * "FujiNet". MUST be set before apple2host_core_start(): the core registers
 * its own defaults through SET_VARIABLES, which deliberately does not clobber
 * a key already present here, and it reads them at load time. */
void apple2host_set_variable(const char *key, const char *value);

/* Registers the libretro callbacks, runs retro_init / retro_load_game(NULL)
 * and reads the initial geometry. Returns false if the core refused to load.
 * Everything here and every *_run_frame/_stop call must happen on the same
 * thread. */
int  apple2host_core_start(void);
/* One retro_run(): one emulated ~60Hz frame, ending in a frame-sink call. */
void apple2host_core_run_frame(void);
void apple2host_core_stop(void);

/* Current geometry, i.e. what the next frame will most likely be. */
void apple2host_get_geometry(int *width, int *height);
/* The core's declared frame rate (retro_get_system_av_info timing.fps), used
 * to pace the emulator thread. 0 before the core has started. */
double apple2host_frame_rate(void);

/* Warm reset (Ctrl-Reset -- drops to BASIC) and cold reset / power cycle
 * (Ctrl-OpenApple-Reset -- re-runs the autostart ROM and reboots). */
void apple2host_ctrl_reset(void);
void apple2host_power_cycle(void);

/* Pull interleaved stereo int16 @ 44100Hz. Blocks briefly for a full block
 * (bounded, ~1.5 block durations) then silence-pads, so the caller always
 * gets max_samples back and an underrun is a short gap rather than a stall.
 * max_samples counts int16 samples, not frames. */
int  apple2host_fill_audio(int16_t *out, int max_samples);
void apple2host_audio_set_active(int active);
void apple2host_clear_audio(void);

/* Keyboard, in libretro terms: keycode is RETROK_*, character the translated
 * Unicode value (0 for none, and 0 for Ctrl combos so the core resolves them
 * from the keycode), mods a RETROKMOD_* bitmask. See core/src/input_map.c. */
void apple2host_inject_key(int down, unsigned keycode, uint32_t character,
                           uint16_t mods);

/* Port 0 is an analog controller so the stick drives the Apple II paddles
 * (PDL0/PDL1) proportionally; buttons are read as JOYPAD A/B either way.
 * id 8 (RETRO_DEVICE_ID_JOYPAD_A) is Apple button 0 / Open Apple,
 * id 0 (RETRO_DEVICE_ID_JOYPAD_B) is Apple button 1 / Closed Apple.
 * axis 0 = X, 1 = Y, full scale -32768..32767. */
void apple2host_set_joystick_button(int port, int id, int pressed);
void apple2host_set_joystick_axis(int port, int axis, int16_t value);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_HOST_H */

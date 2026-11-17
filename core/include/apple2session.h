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
 * in), fixed to match the Android app. The FujiNet web admin port is fixed
 * too, but deliberately NOT 8000 (the Android app's port): 8000 is a common
 * default that collides with other local dev servers, so the desktop targets
 * use this one instead. */
#define APPLE2SESSION_SLIP_PORT  1985
#define APPLE2SESSION_WEBUI_PORT 64001

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
    /* Half-scanline video style. The core's own hidden default is ON; this
     * just makes it a settable option instead of a permanent default. */
    int scanlines;
    int enable_fujinet;     /* start the in-process FujiNet runtime */
    int external_fujinet;   /* 1: don't start it; listen for another FujiNet
                             * instance on slip_listen instead */
    const char *slip_listen; /* "host:port" or "port" the SmartPort listener
                              * binds (external only); NULL/"" = 127.0.0.1:1985 */
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

/* Ask to be told when a new frame is ready, for a frontend with no frame
 * clock of its own: cb(ctx) runs on the emulator thread after a changed frame
 * is stored. Keep it quick (post an event and return) and do not call into
 * the session from it. Passing NULL stops the calls, and returns only after
 * any call in flight has finished, so ctx may be freed right after. */
typedef void (*apple2session_frame_cb)(void *ctx);
void apple2session_set_frame_callback(apple2session *s, apple2session_frame_cb cb,
                                      void *ctx);
/* Frames the machine has run since it started. Unlike copy_frame's serial it
 * counts every frame, including the ones identical to their predecessor
 * (which are not published): use it to ask "is the machine running", not "is
 * the picture changing". */
uint64_t apple2session_frame_count(apple2session *s);

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

/* ---- controller mode and model ------------------------------------------
 * The Apple II has ONE game port: two analog inputs (PDL0, PDL1) and two
 * buttons (PB0, PB1; the Open and Closed Apple keys are wired to the same
 * two). What is plugged into it is a choice:
 *   JOYSTICK  an Apple joystick: PDL0 is its X, PDL1 its Y, and it springs back
 *             to the centre when let go. One "port" for gamepad assignment.
 *   PADDLES   two Apple Hand Controllers: PDL0 is paddle 0's knob, PDL1
 *             paddle 1's. A knob is a pot, so it HOLDS where it was left.
 *             Two "ports" (Paddle 0, Paddle 1) for gamepad assignment.
 * The mode and the model (which joystick the window draws, display only) are
 * settings ("controller_mode", "controller_model"). Changing the mode lets go
 * of everything and centres the machine's inputs. */
enum { APPLE2_MODE_JOYSTICK = 0, APPLE2_MODE_PADDLES = 1 };
enum { APPLE2_MODEL_IIE = 0, APPLE2_MODEL_IIC = 1 };
#define APPLE2SESSION_PADDLES 2
int  apple2session_controller_mode(apple2session *s);
void apple2session_set_controller_mode(apple2session *s, int mode);
int  apple2session_controller_model(apple2session *s);
void apple2session_set_controller_model(apple2session *s, int model);
/* How many ports gamepads can be assigned to now: 1 (joystick) or 2 (paddles). */
int  apple2session_controller_ports(apple2session *s);

/* ---- system actions (fire/post/take latch) ------------------------------
 * The console's two buttons. RESET is the reset key (warm); POWER is the power
 * switch (cold: the machine starts again and boots the card in slot 7 into
 * CONFIG, so there is no separate "reset to CONFIG"). _fire may restart
 * FujiNet, so it must not be called from an SDL callback thread: _post
 * latches a bitmask a UI timer drains with _take + _fire. */
typedef enum {
    APPLE2SESSION_SYSACT_RESET = 0,
    APPLE2SESSION_SYSACT_POWER,
    APPLE2SESSION_SYSACT_DEBUG_STOP,   /* break into the debugger at the next instruction */
    APPLE2SESSION_SYSACT_COUNT
} apple2session_sysaction;

void apple2session_sysaction_fire(apple2session *s, apple2session_sysaction a);
void apple2session_sysaction_post(apple2session *s, apple2session_sysaction a);
/* Returns a bitmask of posted actions and clears the latch. */
unsigned apple2session_sysaction_take(apple2session *s);
/* take + fire everything posted. UI thread only. */
void apple2session_sysaction_drain(apple2session *s);

/* ---- binding targets ----------------------------------------------------
 * Everything a key, a pad button or a pad axis can drive: the joystick's six
 * (X-, X+, Y-, Y+, button 0, button 1), the three system actions, then four
 * for each paddle (turn counter-clockwise, turn clockwise, the button, and the
 * knob itself, which a pad AXIS drives). The joystick targets act in joystick
 * mode, the paddle targets in paddles mode; a key or pad input bound to the
 * other mode's target is not consumed. */
typedef enum {
    APPLE2_ACT_LEFT = 0, APPLE2_ACT_RIGHT,     /* X-, X+ (PDL0) */
    APPLE2_ACT_UP, APPLE2_ACT_DOWN,            /* Y-, Y+ (PDL1; down is +) */
    APPLE2_ACT_BTN0, APPLE2_ACT_BTN1,          /* PB0 (Open Apple), PB1 (Closed Apple) */
    APPLE2_JOY_ACTS
} apple2_joy_act;
typedef enum {
    APPLE2_PACT_CCW = 0,      /* ramp the knob counter-clockwise (-) while held */
    APPLE2_PACT_CW,           /* ramp it clockwise (+) while held */
    APPLE2_PACT_BUTTON,       /* the paddle's button */
    APPLE2_PACT_KNOB,         /* the knob itself: a pad axis only */
    APPLE2_PADDLE_ACTS
} apple2_paddle_act;
#define APPLE2_TARGET_JOY(act)        (act)
#define APPLE2_TARGET_SYSACT_BASE     APPLE2_JOY_ACTS
#define APPLE2_TARGET_SYSACT(sa)      (APPLE2_TARGET_SYSACT_BASE + (sa))
#define APPLE2_TARGET_PADDLES         (APPLE2_TARGET_SYSACT_BASE + APPLE2SESSION_SYSACT_COUNT)
#define APPLE2_TARGET_PADDLE(n, act)  (APPLE2_TARGET_PADDLES + (n) * APPLE2_PADDLE_ACTS + (act))
#define APPLE2_TARGET_COUNT \
    (APPLE2_TARGET_PADDLES + APPLE2SESSION_PADDLES * APPLE2_PADDLE_ACTS)

/* ---- driving the machine --------------------------------------------------
 * Each input source (the keyboard, the Controllers window's mouse, the Open
 * and Closed Apple keys, every gamepad) keeps its own holds; the machine sees
 * their union, so one source letting go can never cancel another's. An analog
 * source (the mouse on the stick, a pad's stick) and the digital ones
 * (keys, buttons, a hat) are merged by strongest deflection: a digital hold
 * drives its axis to the stop, opposite digital holds cancel. */

/* The Controllers window's mouse pressing or releasing a held target (a
 * joystick button, a paddle's button or its turn keys): the POINTER source.
 * System actions are not pressed this way: use apple2session_sysaction_fire. */
void apple2session_press(apple2session *s, int target, int down);
/* The mouse on the joystick's stick: x and y in -1..1 (+y toward the player),
 * the proportional position the pointer holds. Sets the POINTER source's
 * position until the next call or _pointer_release. */
void apple2session_pointer_set(apple2session *s, float x, float y);
/* Let go of everything the pointer holds, and centre its stick (mouse up,
 * focus lost, window hidden). The keyboard's and the pads' holds are kept. */
void apple2session_pointer_release(apple2session *s);
/* 1 while any source holds the target (the window paints it lit): includes the
 * Alt keys for button 0 and 1 (Open and Closed Apple), which reach the machine
 * through apple2session_key. */
int  apple2session_target_held(apple2session *s, int target);
/* What the machine's PDL0 and PDL1 are being driven with now, -1..1 (the
 * joystick's X and Y, or paddle 0's and paddle 1's knobs). */
void apple2session_axes_get(apple2session *s, float *pdl0, float *pdl1);
/* Release everything the keyboard holds (focus lost). The pointer's and the
 * pads' holds are left alone. */
void apple2session_release_all(apple2session *s);

/* A paddle knob (paddles mode) is an absolute position, -1 (fully
 * counter-clockwise) to 1, 0 at the centre, that HOLDS where it was left. Whoever
 * moved it last wins: a pad axis, the window's dial, the wheel, or the turn
 * keys, which ramp it. */
void  apple2session_knob_set(apple2session *s, int paddle, float value);
float apple2session_knob_get(apple2session *s, int paddle);
void  apple2session_knob_nudge(apple2session *s, int paddle, float delta);
/* The dial's geometry, shared so the four toolkits cannot drift: the knob value
 * for a point (dx, dy) from the dial's centre, y down. The dial sweeps 270
 * degrees, -1 at the lower left, +1 at the lower right, 0 straight up. Returns
 * 1 and sets *value, or 0 inside the dead centre (radius * 0.2). */
int    apple2session_knob_from_point(double dx, double dy, double radius,
                                     float *value);
/* The inverse: degrees clockwise from straight up, -135..135. */
double apple2session_knob_angle(float value);

/* A key event, as a keysym (below), for the controller bindings. 1 = the key is
 * bound (in the current mode) and was consumed. Call it FIRST for every press
 * and release, and call apple2session_key only when it returns 0. A key bound
 * to a system action POSTS it: call apple2session_sysaction_drain afterwards.
 * With ctrl_down a press is never consumed, but a release always clears what
 * its press set. Alt, Meta and Super (the Open and Closed Apple keys), F9, F11
 * and F12 are never bound. */
int  apple2session_input_key(apple2session *s, uint32_t keysym, int down,
                             int ctrl_down);

/* Keysyms are X11's, and name the PHYSICAL key (the unshifted US-layout
 * symbol), so Shift or NumLock never changes what a key is. */
enum {
    APPLE2SESSION_KEYSYM_NONE = 0,
    APPLE2SESSION_KEYSYM_UP = 0xff52, APPLE2SESSION_KEYSYM_DOWN = 0xff54,
    APPLE2SESSION_KEYSYM_LEFT = 0xff51, APPLE2SESSION_KEYSYM_RIGHT = 0xff53,
    APPLE2SESSION_KEYSYM_ESCAPE = 0xff1b, APPLE2SESSION_KEYSYM_RETURN = 0xff0d,
    APPLE2SESSION_KEYSYM_BACKSPACE = 0xff08, APPLE2SESSION_KEYSYM_TAB = 0xff09,
    APPLE2SESSION_KEYSYM_SPACE = 0x20,
    APPLE2SESSION_KEYSYM_F1 = 0xffbe, APPLE2SESSION_KEYSYM_F2, APPLE2SESSION_KEYSYM_F3,
    APPLE2SESSION_KEYSYM_F4, APPLE2SESSION_KEYSYM_F5, APPLE2SESSION_KEYSYM_F6,
    APPLE2SESSION_KEYSYM_F7, APPLE2SESSION_KEYSYM_F8, APPLE2SESSION_KEYSYM_F9,
    APPLE2SESSION_KEYSYM_F10, APPLE2SESSION_KEYSYM_F11, APPLE2SESSION_KEYSYM_F12,
    APPLE2SESSION_KEYSYM_LSHIFT = 0xffe1, APPLE2SESSION_KEYSYM_RSHIFT = 0xffe2,
    APPLE2SESSION_KEYSYM_LCTRL = 0xffe3, APPLE2SESSION_KEYSYM_RCTRL = 0xffe4,
    APPLE2SESSION_KEYSYM_LALT = 0xffe9, APPLE2SESSION_KEYSYM_RALT = 0xffea,
    APPLE2SESSION_KEYSYM_LMETA = 0xffe7, APPLE2SESSION_KEYSYM_RMETA = 0xffe8,
    APPLE2SESSION_KEYSYM_LSUPER = 0xffeb, APPLE2SESSION_KEYSYM_RSUPER = 0xffec,
    APPLE2SESSION_KEYSYM_KP_0 = 0xffb0, APPLE2SESSION_KEYSYM_KP_1,
    APPLE2SESSION_KEYSYM_KP_2, APPLE2SESSION_KEYSYM_KP_3, APPLE2SESSION_KEYSYM_KP_4,
    APPLE2SESSION_KEYSYM_KP_5, APPLE2SESSION_KEYSYM_KP_6, APPLE2SESSION_KEYSYM_KP_7,
    APPLE2SESSION_KEYSYM_KP_8, APPLE2SESSION_KEYSYM_KP_9,
    APPLE2SESSION_KEYSYM_KP_ENTER = 0xff8d, APPLE2SESSION_KEYSYM_KP_MULTIPLY = 0xffaa,
    APPLE2SESSION_KEYSYM_KP_DIVIDE = 0xffaf, APPLE2SESSION_KEYSYM_KP_PERIOD = 0xffae
};
/* A key with no X11 keysym of its own is still bindable: 0x10000 + HID usage. */
#define APPLE2SESSION_KEYSYM_HID_BASE 0x10000u
#define APPLE2SESSION_HID_USAGE_MAX   0xE7u
/* Native key codes -> keysym, 0 when unknown: Windows scan code (set 1, with
 * the E0 flag = lParam bit 24), Linux evdev code (GTK/Qt keycode minus 8) and
 * macOS virtual key code. */
uint32_t apple2session_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t apple2session_keysym_from_evdev(unsigned code);
uint32_t apple2session_keysym_from_macos_keycode(unsigned keycode);
/* "Space", "Keypad 5", "F1", "A"; returns the length. */
int apple2session_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- gamepads (SDL; started by apple2session_start) ----------------------
 * USB and Bluetooth pads, mapped or not (a plain HID adapter, which includes
 * the USB adapters for real Apple II joysticks, is opened as a raw joystick),
 * drive the game port. A pad plays the first port nobody is assigned to, in
 * connection order, unless it is assigned explicitly: the joystick has one
 * port, the paddles two (Paddle 0, Paddle 1; see apple2session_controller_ports).
 * An explicit assignment is remembered (setting "pad_assign": a pad's GUID,
 * else its name) and applies again when the pad is plugged back in or the app
 * restarted. In joystick mode an assignment to Paddle 1 is ignored (the pad
 * counts as automatic). Every button goes through the bindings; the left stick
 * is the joystick, proportionally, and (paddles mode) the knob follows one
 * axis absolutely. A pad that goes away lets go of everything it held.
 *
 * A pad INPUT id is a button, in one of three bands, or (for a paddle's KNOB
 * only) an axis: the named buttons 0..APPLE2_PAD_BTN_COUNT-1, the raw buttons
 * of an unmapped joystick (64..127: button index + 64), a raw joystick's hat
 * positions (192..255: hat * 8 + direction), or APPLE2_PAD_AXIS_BASE + an
 * SDL_GamepadAxis index (LEFTX LEFTY RIGHTX RIGHTY LTRIGGER RTRIGGER). -1 =
 * none. While a raw joystick's button 0-3 is unbound it stands in for A/B/X/Y,
 * and an unbound West / North stand in for South / East, so button 0 is Open
 * Apple on any pad. Pad `idx` below is its slot in the connection order. */
typedef enum {
    APPLE2_PAD_BTN_NONE = -1,
    APPLE2_PAD_BTN_SOUTH = 0, APPLE2_PAD_BTN_EAST, APPLE2_PAD_BTN_WEST,
    APPLE2_PAD_BTN_NORTH, APPLE2_PAD_BTN_BACK, APPLE2_PAD_BTN_GUIDE,
    APPLE2_PAD_BTN_START, APPLE2_PAD_BTN_LEFT_STICK, APPLE2_PAD_BTN_RIGHT_STICK,
    APPLE2_PAD_BTN_LEFT_SHOULDER, APPLE2_PAD_BTN_RIGHT_SHOULDER,
    APPLE2_PAD_BTN_DPAD_UP, APPLE2_PAD_BTN_DPAD_DOWN, APPLE2_PAD_BTN_DPAD_LEFT,
    APPLE2_PAD_BTN_DPAD_RIGHT,
    APPLE2_PAD_BTN_LEFT_TRIGGER, APPLE2_PAD_BTN_RIGHT_TRIGGER,
    APPLE2_PAD_BTN_COUNT,
    APPLE2_PAD_BTN_RAW_BASE = 64, APPLE2_PAD_BTN_RAW_LAST = 127,
    APPLE2_PAD_HAT_BASE = 192, APPLE2_PAD_HAT_LAST = 255
} apple2_pad_button;
#define APPLE2_PAD_HAT_DIRS 8
#define APPLE2_PAD_BTN_IS_NAMED(btn) ((btn) >= 0 && (btn) < APPLE2_PAD_BTN_COUNT)
#define APPLE2_PAD_BTN_IS_RAW(btn)   ((btn) >= APPLE2_PAD_BTN_RAW_BASE && (btn) <= APPLE2_PAD_BTN_RAW_LAST)
#define APPLE2_PAD_BTN_IS_HAT(btn)   ((btn) >= APPLE2_PAD_HAT_BASE && (btn) <= APPLE2_PAD_HAT_LAST)
#define APPLE2_PAD_AXIS_BASE   128
#define APPLE2_PAD_AXIS_COUNT  6
#define APPLE2_PAD_IS_AXIS(id) ((id) >= APPLE2_PAD_AXIS_BASE && (id) < APPLE2_PAD_AXIS_BASE + APPLE2_PAD_AXIS_COUNT)

/* "A", "D-pad Up", "Button 3", "Hat 0 Up-Right", "Right Stick X"; returns the
 * length. */
int  apple2session_pad_input_name(int id, char *dst, int dstsz);
int  apple2session_gamepad_count(apple2session *s);
/* Name of connected pad idx into dst; returns its length, or 0. */
int  apple2session_gamepad_name(apple2session *s, int idx, char *dst, int dstsz);
/* port: 0 (the joystick, or Paddle 0), 1 (Paddle 1), -1 for automatic.
 * Remembered. */
void apple2session_gamepad_assign(apple2session *s, int idx, int port);
/* The explicit port (0..1), or -1 for automatic. */
int  apple2session_gamepad_assignment(apple2session *s, int idx);
/* The port the pad drives right now in the current mode (0..1), or -1 for none. */
int  apple2session_gamepad_effective_port(apple2session *s, int idx);
/* Bumped on every pad added or removed (and carried across a restart of the
 * session), so a frontend can refresh its lists and toasts cheaply. */
unsigned apple2session_gamepad_generation(apple2session *s);
/* The last hot-plug event as text ("Connected: 8BitDo SN30 Pro (joystick)",
 * "Disconnected: ..."), for a toast; returns the length, 0 if none yet. */
int  apple2session_gamepad_last_event(apple2session *s, char *dst, int dstsz);

/* ---- pad-input capture, for the Controllers window's Map mode -------------
 * A frontend arming a remap to a pad input calls _begin, then polls _poll on
 * a UI timer (pad events land on the session's own poll thread); _poll
 * returns 1 with *id set once a button is pressed, then disarms. _cancel
 * disarms without capturing. While armed every pad button event is swallowed,
 * so the press that maps a button does not also do what it used to. For a
 * paddle's KNOB target use _begin_axis: it records an axis deflected past 60%
 * instead. */
void apple2session_gamepad_capture_begin(apple2session *s);
void apple2session_gamepad_capture_begin_axis(apple2session *s);
int  apple2session_gamepad_capture_poll(apple2session *s, int *id);
void apple2session_gamepad_capture_cancel(apple2session *s);

/* ---- the on-screen controllers (controls.c) ----------------------------------
 * One layout for the four toolkits, and its hit test. Three views, each a list
 * of controls in PAINT order, in layout units (the joystick's own proportions:
 * about 34 px per unit in the reference photo); a toolkit adds its margin and
 * scale. No colours here (DESIGN-apple2.md has them: a Beige and a Platinum
 * scheme, apple2session_controller_model). Shapes the layout cannot describe by
 * a rectangle (the raised block and its pyramid well, the paddle wedge) are
 * PART entries the toolkit paints by their apple2_part id.
 *
 *  JOYSTICK  the Apple Joystick IIe and IIc: the base, a raised block with a
 *            pyramid well, a stick that tilts in it, and two square buttons
 *            (the left one is button 0 = Open Apple, the top one button 1 =
 *            Closed Apple).
 *  PADDLES   two Apple Hand Controllers: a ribbed wedge each, a knob you turn
 *            and a button at the front (paddle 0 = PDL0 + button 0).
 *  CONSOLE   the two buttons of the console strip (Reset, Power Cycle). */
#define APPLE2SESSION_ACCENT_RGB 0xF44336u

enum { APPLE2_VIEW_JOYSTICK = 0, APPLE2_VIEW_PADDLES, APPLE2_VIEW_CONSOLE, APPLE2_VIEW_COUNT };

typedef enum {
    APPLE2_CTL_PART = 0,    /* a drawn shape (see apple2_part); never pressed */
    APPLE2_CTL_STICK,       /* the joystick's stick: drag in its well (apple2session_joystick_from_point) */
    APPLE2_CTL_BUTTON,      /* a fire button (target = the joystick's or a paddle's button) */
    APPLE2_CTL_KNOB,        /* a paddle's dial: drag (apple2session_knob_from_point), wheel nudges */
    APPLE2_CTL_SYSBUTTON,   /* a console button (target = the system action) */
    APPLE2_CTL_CAPTION      /* text: "Paddle 0" */
} apple2_control_kind;

typedef enum {
    APPLE2_PART_NONE = 0,
    APPLE2_PART_JOY_BASE,   /* the case: a rounded rectangle with a seam near the bottom */
    APPLE2_PART_JOY_BLOCK,  /* the raised block the well is in (lighter top edge, a shadow on its left) */
    APPLE2_PART_JOY_WELL,   /* the pyramid's outer square: four sloping walls (top and bottom darker) */
    APPLE2_PART_JOY_FLOOR,  /* the well's floor, where the stick's socket is */
    APPLE2_PART_JOY_LOGO,   /* the outlined rounded square with the small Apple */
    APPLE2_PART_PADDLE_BODY /* the ribbed wedge: polygon (0,0.6) (5.2,0) (6,0.9) (6,2.4) (5,3) (0,2.6) */
} apple2_part;

typedef struct {
    int kind;               /* apple2_control_kind */
    int part;               /* apple2_part for kind PART, else APPLE2_PART_NONE */
    const char *label;      /* "Button 0", "Reset", "Paddle 1", "Stick" */
    int port;               /* a paddle's number (0 or 1) for its controls, else -1 */
    int target;             /* APPLE2_TARGET_*, or -1 (parts, the stick, the knob, captions) */
    float xpos, ypos, width, height;   /* bounding box, layout units, from the top left */
    float cx, cy;           /* STICK: the socket's centre; KNOB: the dial's centre */
    float capx, capy;       /* STICK: where the cap rests (deflection 0, 0) */
    float reach;            /* STICK: how far the cap travels at full deflection; KNOB: its radius */
} apple2_control;

const apple2_control *apple2session_controls_layout(int view, int *count,
                                                    float *width, float *height);
/* The topmost pressable control of a view under a point (layout units), or -1.
 * Exact shapes: the dial is a circle, the stick's well a square. */
int  apple2session_controls_hit(int view, float xpos, float ypos);
/* The stick's deflection for a pointer at (xpos, ypos) in joystick-view units:
 * the cap follows the pointer from where it rests, one `reach` per full
 * deflection, each axis clamped to -1..1 (+y toward the player). Returns 1 if
 * the point is inside the well. Feed the result to apple2session_pointer_set. */
int  apple2session_joystick_from_point(float xpos, float ypos, float *xval, float *yval);

/* ---- the on-screen keyboard (keyboard.c) ---------------------------------------
 * The Apple IIe's keyboard as printed on the Platinum //e (DESIGN-apple2.md):
 * five rows of a 15-unit main block, with the small Reset cap above Esc. A cap
 * is what a click on it types: the same events apple2session_key takes, press
 * and release (the keyboard is edge-based). SHIFT and CONTROL latch (a second
 * press lets go) and are spent by the next ordinary key; CAPS LOCK toggles and
 * is ON at power-up as on the real //e: with it on a letter cap types the
 * capital (a setting, "caps_lock"; it does not touch the host keyboard). The
 * Open Apple and Option (Closed Apple) caps are HELD with the mouse and drive
 * buttons 0 and 1 of whichever controller is plugged in. The Reset cap posts
 * the console's Reset. Keys are units: the main block is 15 caps wide plus the
 * gaps. */
typedef enum {
    APPLE2_KEYCAP_CHAR = 0,   /* a printing key: ch, and shifted_ch with Shift */
    APPLE2_KEYCAP_SPACE,      /* the space bar */
    APPLE2_KEYCAP_SPECIAL,    /* Esc, Tab, Delete, Return, the arrows: a keysym */
    APPLE2_KEYCAP_MODIFIER,   /* Shift, Control: latch until the next key (keysym = APPLE2_MOD_*) */
    APPLE2_KEYCAP_LOCK,       /* Caps Lock: toggles */
    APPLE2_KEYCAP_APPLE,      /* Open Apple (keysym 0) / Option, Closed Apple (keysym 1): held */
    APPLE2_KEYCAP_RESET       /* the Reset cap: posts the console's Reset */
} apple2_keycap_kind;
enum { APPLE2_MOD_SHIFT = 1, APPLE2_MOD_CONTROL = 2 };

typedef struct {
    const char *legend;       /* printed base legend, UTF-8 ("Q", "1", "Delete", "\xE2\x86\x90") */
    const char *shifted;      /* the legend printed above it ("!"), or NULL */
    float xpos, ypos, width, height;   /* layout units, from the top left */
    int kind;                 /* apple2_keycap_kind */
    uint32_t ch;              /* CHAR: the unshifted character (lower case for a letter) */
    uint32_t shifted_ch;      /* CHAR: the character with Shift */
    uint32_t keysym;          /* SPECIAL: the X11 keysym; MODIFIER: APPLE2_MOD_*; APPLE: the button */
} apple2_keycap;

const apple2_keycap *apple2session_keyboard_layout(int *count, float *width, float *height);
/* A press (down 1) or release (down 0) of cap `index`. A key cap sends its
 * event on both; a latch toggles on the press; the Apple caps hold a button
 * (the mouse-source hold, so apple2session_pointer_release lets go too). */
void apple2session_keyboard_press(apple2session *s, int index, int down);
/* Bit 0: the cap is down, or flashed in the last 120 ms (the Apple caps: or the
 * button is held by any source). Bit 1: the cap is a latch that is on (Shift,
 * Control, Caps Lock). */
int  apple2session_keyboard_held(apple2session *s, int index);
/* Let go of every cap that is down (sends their key-ups) and every latch but
 * Caps Lock (window hidden, focus lost, mode changed). */
void apple2session_keyboard_release(apple2session *s);
/* Caps Lock: on at power-up. Remembered. */
int  apple2session_caps_lock(apple2session *s);
void apple2session_set_caps_lock(apple2session *s, int on);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend that
 * wants to own the device instead can pull mixed emulator+FujiNet interleaved
 * stereo S16 samples at 44100 Hz. nsamples counts int16 samples, so one
 * stereo frame is two. */
int  apple2session_render_audio(apple2session *s, int16_t *out, int nsamples);

/* ---- FujiNet -------------------------------------------------------------*/
/* Parses the "Use external FujiNet" address field: "host:port" (host an IPv4
 * literal; "localhost" is 127.0.0.1) or a bare "port". Empty text means the
 * default, 127.0.0.1:1985. Returns 0 and fills host/port, or -1 when the text
 * is not an address:port (nothing is written then). */
int         apple2session_parse_hostport(const char *text, char *host,
                                         int hostsz, int *port);
int         apple2session_fujinet_running(const apple2session *s);
/* 1 when the session was started to listen for an external FujiNet. */
int         apple2session_fujinet_external(const apple2session *s);
/* "host:port" the SmartPort listener is bound to (the running session's, or
 * the default 127.0.0.1:1985 before the first start). */
const char *apple2session_slip_endpoint(const apple2session *s);
/* FujiNet has registered its SmartPort devices with the card (the SLIP link
 * is up). */
int         apple2session_fujinet_link(const apple2session *s);
/* The slot the FujiNet SmartPort card is in (7 or 5), or 0 for none. */
int         apple2session_fujinet_slot(const apple2session *s);
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

/* Import an Apple II system ROM. The file is copied into the session's ROM
 * directory under its own name -- AppleWin looks ROMs up by exact filename
 * (Apple2e_Enhanced.rom, DISK2.rom, ...), so the name must be preserved.
 * Takes effect on the next start. Returns 0 on success and writes the
 * destination into dest_out.
 *
 * Only builds configured with WITH_APPLE_ROMS=OFF read this directory; a
 * build with the ROMs compiled in ignores it. See COMPLIANCE.md. */
int apple2session_import_rom(apple2session *s, const char *src_path,
                             char *dest_out, int dest_sz);

/* Directory paths (valid for the session's lifetime). */
const char *apple2session_roms_path(const apple2session *s);
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

/*
 * apple2_host -- the libretro frontend for the staged AppleWin core.
 * See apple2_host.h for the contract.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "apple2_host.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "libretro.h"

// AppleWin's two reset entry points (defined in source/Utilities.cpp,
// statically linked from the core). Forward-declared with C++ linkage rather
// than via Utilities.h to avoid pulling AppleWin's full header set in here.
//   CtrlReset()         : warm reset  (Ctrl-Reset)
//   ResetMachineState() : power-cycle (Ctrl-OpenApple-Reset)
void CtrlReset();
void ResetMachineState();

namespace {

// --- logging ----------------------------------------------------------------
Apple2LogSink g_log_sink = nullptr;
void* g_log_user = nullptr;

void emit_log(int level, const char* msg) {
    if (g_log_sink) {
        g_log_sink(level, msg, g_log_user);
    } else if (level >= 2) {
        std::fprintf(stderr, "apple2core: %s\n", msg);
    }
}

void logf(int level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit_log(level, buf);
}

#define LOGI(...) logf(1, __VA_ARGS__)
#define LOGW(...) logf(2, __VA_ARGS__)
#define LOGE(...) logf(3, __VA_ARGS__)

// --- frame sink -------------------------------------------------------------
Apple2FrameSink g_frame_sink = nullptr;
void* g_frame_user = nullptr;
std::vector<uint32_t> g_repack;  // tight XRGB8888 scratch (emulator thread only)

// --- geometry ---------------------------------------------------------------
int g_width = 560;
int g_height = 384;
double g_fps = 0.0;

// --- audio ring (interleaved stereo int16 @ 44100) --------------------------
// The libretro core pushes samples once per ~60Hz frame (producer); the audio
// backend pulls fixed full blocks (consumer). The consumer blocks on
// g_audio_cv until a whole block is ready, so it always writes full,
// real-time-paced blocks -- no partial/choppy writes -- and degrades to a
// brief silence pad on underrun instead of stuttering.
std::mutex g_audio_mutex;
std::condition_variable g_audio_cv;
std::vector<int16_t> g_audio;            // FIFO
bool g_audio_active = true;
// ~250ms cap: enough elastic buffer to ride out frame-pacing jitter without
// unbounded latency growth.
constexpr size_t kAudioCapSamples = (44100 / 4) * 2;

// --- core options (libretro variables) --------------------------------------
std::mutex g_var_mutex;
std::map<std::string, std::string> g_vars;
bool g_vars_dirty = false;

// --- input ------------------------------------------------------------------
constexpr int kMaxPorts = 2;
constexpr int kJoypadIds = 16;
int16_t g_buttons[kMaxPorts][kJoypadIds] = {};
int16_t g_axes[kMaxPorts][4] = {};       // [analog index*2 + x/y]

// --- keyboard ---------------------------------------------------------------
retro_keyboard_event_t g_keyboard_cb = nullptr;

void RETRO_CALLCONV host_log(enum retro_log_level level, const char* fmt, ...) {
    int mapped = 1;
    switch (level) {
        case RETRO_LOG_DEBUG: mapped = 0; break;
        case RETRO_LOG_WARN:  mapped = 2; break;
        case RETRO_LOG_ERROR: mapped = 3; break;
        default:              mapped = 1; break;
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    // The core's messages usually carry their own newline; the sink adds
    // framing, so strip it here.
    size_t n = std::strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    emit_log(mapped, buf);
}

// Parse a SET_VARIABLES value string ("Description; Default|Other|...") and
// return the default option (the first token after "; ").
std::string default_from_spec(const char* spec) {
    if (!spec) return {};
    const char* semi = std::strstr(spec, "; ");
    const char* opts = semi ? semi + 2 : spec;
    const char* bar = std::strchr(opts, '|');
    return bar ? std::string(opts, bar - opts) : std::string(opts);
}

// --- libretro callbacks -----------------------------------------------------
void RETRO_CALLCONV host_video_refresh(const void* data, unsigned width,
                                       unsigned height, size_t pitch) {
    if (!data || !g_frame_sink || width == 0 || height == 0) return;
    const auto* src = static_cast<const uint8_t*>(data);
    const size_t row_px = width;
    if (g_repack.size() != row_px * height) g_repack.resize(row_px * height);
    for (unsigned y = 0; y < height; ++y) {
        std::memcpy(g_repack.data() + static_cast<size_t>(y) * row_px,
                    src + static_cast<size_t>(y) * pitch,
                    row_px * sizeof(uint32_t));
    }
    g_frame_sink(g_repack.data(), static_cast<int>(width), static_cast<int>(height),
                 g_frame_user);
}

void push_audio(const int16_t* data, size_t count) {
    {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_audio.insert(g_audio.end(), data, data + count);
        if (g_audio.size() > kAudioCapSamples) {
            // Drop oldest in whole stereo frames to bound latency if the
            // consumer falls behind (keeps L/R alignment, avoids a
            // channel-swap glitch).
            size_t drop = g_audio.size() - kAudioCapSamples;
            drop &= ~static_cast<size_t>(1);
            g_audio.erase(g_audio.begin(), g_audio.begin() + drop);
        }
    }
    g_audio_cv.notify_one();
}

size_t RETRO_CALLCONV host_audio_batch(const int16_t* data, size_t frames) {
    if (data && frames) push_audio(data, frames * 2);
    return frames;
}

void RETRO_CALLCONV host_audio_sample(int16_t left, int16_t right) {
    const int16_t pair[2] = {left, right};
    push_audio(pair, 2);
}

void RETRO_CALLCONV host_input_poll(void) {}

int16_t RETRO_CALLCONV host_input_state(unsigned port, unsigned device,
                                        unsigned index, unsigned id) {
    if (port >= kMaxPorts) return 0;
    switch (device) {
        case RETRO_DEVICE_JOYPAD:
            if (id < kJoypadIds) return g_buttons[port][id];
            return 0;
        case RETRO_DEVICE_ANALOG: {
            const int slot = (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT ? 2 : 0)
                           + (id == RETRO_DEVICE_ID_ANALOG_Y ? 1 : 0);
            if (slot < 4) return g_axes[port][slot];
            return 0;
        }
        default:
            return 0;
    }
}

bool RETRO_CALLCONV host_environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const auto fmt = *static_cast<const enum retro_pixel_format*>(data);
            return fmt == RETRO_PIXEL_FORMAT_XRGB8888;
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            static_cast<retro_log_callback*>(data)->log = host_log;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: {
            g_keyboard_cb = static_cast<const retro_keyboard_callback*>(data)->callback;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true;
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return false;  // core reads individual buttons
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *static_cast<unsigned*>(data) = 0;  // use the SET_VARIABLES path
            return true;
        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
            return false;  // core falls back to the basic interface
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            std::lock_guard<std::mutex> lock(g_var_mutex);
            for (auto* v = static_cast<const retro_variable*>(data); v && v->key; ++v) {
                // Don't clobber an override already set by the session.
                if (g_vars.find(v->key) == g_vars.end()) {
                    g_vars[v->key] = default_from_spec(v->value);
                }
            }
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* v = static_cast<retro_variable*>(data);
            if (!v || !v->key) return false;
            std::lock_guard<std::mutex> lock(g_var_mutex);
            auto it = g_vars.find(v->key);
            if (it == g_vars.end() || it->second.empty()) {
                v->value = nullptr;
                return false;
            }
            v->value = it->second.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            std::lock_guard<std::mutex> lock(g_var_mutex);
            *static_cast<bool*>(data) = g_vars_dirty;
            g_vars_dirty = false;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const auto* m = static_cast<const retro_message*>(data);
            if (m && m->msg) LOGI("core message: %s", m->msg);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const auto* gi = static_cast<const retro_game_geometry*>(data);
            if (gi && gi->base_width && gi->base_height) {
                g_width = static_cast<int>(gi->base_width);
                g_height = static_cast<int>(gi->base_height);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
            const auto* av = static_cast<const retro_system_av_info*>(data);
            if (av && av->geometry.base_width && av->geometry.base_height) {
                g_width = static_cast<int>(av->geometry.base_width);
                g_height = static_cast<int>(av->geometry.base_height);
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

// --- C API ------------------------------------------------------------------
extern "C" {

void apple2host_set_frame_sink(Apple2FrameSink sink, void* user) {
    g_frame_sink = sink;
    g_frame_user = user;
}

void apple2host_set_log_sink(Apple2LogSink sink, void* user) {
    g_log_sink = sink;
    g_log_user = user;
}

void apple2host_set_variable(const char* key, const char* value) {
    if (!key || !value) return;
    std::lock_guard<std::mutex> lock(g_var_mutex);
    g_vars[key] = value;
    g_vars_dirty = true;
}

int apple2host_core_start(void) {
    retro_set_environment(host_environment);
    retro_set_video_refresh(host_video_refresh);
    retro_set_audio_sample(host_audio_sample);
    retro_set_audio_sample_batch(host_audio_batch);
    retro_set_input_poll(host_input_poll);
    retro_set_input_state(host_input_state);

    retro_init();
    // Port 0 as an analog controller so a stick drives the Apple II paddles
    // proportionally (Analog::getAxis reads ANALOG_LEFT X/Y). Buttons are
    // still read as JOYPAD A/B regardless of the port device.
    retro_set_controller_port_device(0, RETRO_DEVICE_ANALOG);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

    if (!retro_load_game(nullptr)) {
        LOGE("retro_load_game(no content) failed");
        return 0;
    }

    retro_system_av_info av;
    std::memset(&av, 0, sizeof(av));
    retro_get_system_av_info(&av);
    if (av.geometry.base_width && av.geometry.base_height) {
        g_width = static_cast<int>(av.geometry.base_width);
        g_height = static_cast<int>(av.geometry.base_height);
    }
    g_fps = av.timing.fps;
    LOGI("core started: %dx%d @ %.2ffps, %.0fHz audio",
         g_width, g_height, av.timing.fps, av.timing.sample_rate);
    return 1;
}

void apple2host_core_run_frame(void) { retro_run(); }

void apple2host_core_stop(void) {
    retro_unload_game();
    retro_deinit();
    // Drop the core's keyboard callback: it points into core state that
    // retro_deinit has just torn down, so a key injected before the next
    // core_start re-registers it would jump through a stale pointer. Cleared
    // here, apple2host_inject_key's null-guard makes such a key a safe no-op.
    g_keyboard_cb = nullptr;
    // Release any held buttons/axes so a restarted session doesn't inherit a
    // stuck input from the previous one.
    std::memset(g_buttons, 0, sizeof(g_buttons));
    std::memset(g_axes, 0, sizeof(g_axes));
    apple2host_clear_audio();
}

void apple2host_ctrl_reset(void) { CtrlReset(); }
void apple2host_power_cycle(void) { ResetMachineState(); }

void apple2host_get_geometry(int* width, int* height) {
    if (width) *width = g_width;
    if (height) *height = g_height;
}

double apple2host_frame_rate(void) { return g_fps; }

int apple2host_fill_audio(int16_t* out, int maxSamples) {
    if (!out || maxSamples <= 0) return 0;
    const size_t want = static_cast<size_t>(maxSamples);
    std::unique_lock<std::mutex> lock(g_audio_mutex);
    // Wait for a whole block so the consumer always writes a full, paced
    // buffer. Bounded wait (~1.5 block-durations at 44100Hz stereo) so a
    // producer stall degrades to a brief silence pad rather than blocking the
    // audio thread.
    const auto budget = std::chrono::microseconds(
        (want * 1000000ULL) / (44100ULL * 2ULL) + 8000ULL);
    g_audio_cv.wait_for(lock, budget, [&] {
        return !g_audio_active || g_audio.size() >= want;
    });
    const size_t have = std::min(g_audio.size(), want);
    if (have > 0) {
        std::memcpy(out, g_audio.data(), have * sizeof(int16_t));
        g_audio.erase(g_audio.begin(), g_audio.begin() + have);
    }
    if (have < want) {
        // Silence-pad the remainder so the caller still gets a full block.
        std::memset(out + have, 0, (want - have) * sizeof(int16_t));
    }
    return static_cast<int>(want);
}

void apple2host_audio_set_active(int active) {
    {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_audio_active = active != 0;
    }
    g_audio_cv.notify_all();  // wake a blocked fill on shutdown
}

void apple2host_clear_audio(void) {
    {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_audio.clear();
    }
    g_audio_cv.notify_all();
}

void apple2host_inject_key(int down, unsigned keycode, uint32_t character,
                           uint16_t mods) {
    if (g_keyboard_cb) g_keyboard_cb(down != 0, keycode, character, mods);
}

void apple2host_set_joystick_button(int port, int id, int pressed) {
    if (port >= 0 && port < kMaxPorts && id >= 0 && id < kJoypadIds) {
        g_buttons[port][id] = pressed ? 1 : 0;
    }
}

void apple2host_set_joystick_axis(int port, int axis, int16_t value) {
    if (port >= 0 && port < kMaxPorts && axis >= 0 && axis < 4) {
        g_axes[port][axis] = value;
    }
}

}  // extern "C"

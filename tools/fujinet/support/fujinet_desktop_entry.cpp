/*
 * In-process desktop entry for the FujiNet Apple II runtime (libfujinet.so).
 *
 * Adapted from the Android app's fujinet_android_entry.cpp: runs FujiNet's
 * main_setup() + fn_service_loop() on a background thread, captures the
 * runtime's stdout/stderr into a ring buffer + fujinet-console.log (the
 * host app shows both), and queues SAM speech audio for the host mixer.
 * Differences from Android: console lines are echoed to the saved host
 * stderr instead of logcat, the per-block log suppression toggle is the
 * FUJINET_QUIET_BLOCKS environment variable, and the web admin binds
 * 127.0.0.1 by default (FUJINET_WEBUI_BIND overrides).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fnSystem.h"

extern void main_setup(int argc, char* argv[]);
extern void fn_service_loop(void* param);
extern void main_shutdown_handler();

/* Export control. ELF and Mach-O builds hand the linker an explicit symbol
 * list (see build-fujinet-desktop.sh); PE has no equivalent, so the entry
 * points mark themselves -- which also stops MinGW from auto-exporting every
 * symbol in the library. */
#if defined(_WIN32)
#define FUJINET_ENTRY extern "C" __declspec(dllexport)
#else
#define FUJINET_ENTRY extern "C"
#endif

namespace {
void set_thread_name(const char* name) {
#if defined(_WIN32)
    /* winpthreads has no portable setname; the debugger names threads by
     * their entry point instead. */
    (void)name;
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

/* POSIX pipe() over the MSVCRT flavour, which wants a buffer size and a
 * text/binary mode. Binary: the log pump moves bytes, not lines. */
int fn_make_pipe(int fds[2]) {
#if defined(_WIN32)
    return _pipe(fds, 1 << 16, _O_BINARY);
#else
    return pipe(fds);
#endif
}

std::mutex g_mutex;
std::condition_variable g_setup_condition;
/* Signalled when the runtime thread clears g_running on its way out, so
 * fujinet_desktop_stop_runtime() can wait for that with a timeout instead of
 * joining unconditionally -- see the comment there. */
std::condition_variable g_stop_condition;
std::thread g_runtime_thread;
std::string g_last_error;
bool g_running = false;
bool g_setup_complete = false;
bool g_setup_failed = false;
std::mutex g_audio_mutex;
std::condition_variable g_audio_condition;
std::deque<std::vector<int16_t>> g_audio_utterances;
size_t g_audio_queued_samples = 0;
size_t g_audio_current_sample_index = 0;
double g_audio_resample_cursor = 0.0;
std::mutex g_log_mutex;
std::thread g_log_thread;
std::string g_log_tail;
std::string g_log_file_path;
size_t g_log_file_size = 0;
int g_log_read_fd = -1;
int g_log_write_fd = -1;
int g_saved_stdout_fd = -1;
int g_saved_stderr_fd = -1;

constexpr int kSamSampleRate = 22050;
constexpr size_t kMaxQueuedSamSamples = static_cast<size_t>(kSamSampleRate * 60);
constexpr size_t kMaxLogTailBytes = 1024 * 1024;
constexpr size_t kMaxLogFileBytes = 1024 * 1024;

/* Write a host-visible diagnostic line. While capture is active the real
 * stderr is redirected into our own pipe, so use the saved fd. */
void host_log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    const int fd = g_saved_stderr_fd >= 0 ? g_saved_stderr_fd : STDERR_FILENO;
    ssize_t ignored = write(fd, line, strlen(line));
    (void)ignored;
}

void set_last_error_locked(const std::string& message) {
    g_last_error = message;
    host_log("fujinet: %s", g_last_error.c_str());
}

void publish_setup_result_locked(bool complete, bool failed) {
    g_setup_complete = complete;
    g_setup_failed = failed;
    g_setup_condition.notify_all();
}

void clear_audio_locked() {
    g_audio_utterances.clear();
    g_audio_queued_samples = 0;
    g_audio_current_sample_index = 0;
    g_audio_resample_cursor = 0.0;
    g_audio_condition.notify_all();
}

void advance_sam_samples_locked(size_t samples_to_advance) {
    while (samples_to_advance > 0 && !g_audio_utterances.empty()) {
        std::vector<int16_t>& current = g_audio_utterances.front();
        if (g_audio_current_sample_index >= current.size()) {
            g_audio_utterances.pop_front();
            g_audio_current_sample_index = 0;
            continue;
        }

        const size_t available = current.size() - g_audio_current_sample_index;
        const size_t step = std::min(samples_to_advance, available);
        g_audio_current_sample_index += step;
        g_audio_queued_samples -= step;
        samples_to_advance -= step;

        if (g_audio_current_sample_index >= current.size()) {
            g_audio_utterances.pop_front();
            g_audio_current_sample_index = 0;
        }
    }

    if (g_audio_utterances.empty()) {
        g_audio_queued_samples = 0;
        g_audio_current_sample_index = 0;
        g_audio_resample_cursor = 0.0;
    }

    g_audio_condition.notify_all();
}

void append_log_chunk(const char* data, size_t size);

std::string sanitize_log_chunk(const char* data, size_t size) {
    if (data == nullptr || size == 0) {
        return {};
    }

    std::string sanitized;
    sanitized.reserve(size);

    char escaped[5];
    for (size_t index = 0; index < size; ++index) {
        const unsigned char byte = static_cast<unsigned char>(data[index]);
        switch (byte) {
            case '\n':
            case '\r':
            case '\t':
                sanitized.push_back(static_cast<char>(byte));
                break;
            default:
                if (byte >= 0x20 && byte <= 0x7e) {
                    sanitized.push_back(static_cast<char>(byte));
                } else {
                    std::snprintf(escaped, sizeof(escaped), "\\x%02X", byte);
                    sanitized.append(escaped);
                }
                break;
        }
    }

    return sanitized;
}

void trim_log_tail_locked() {
    if (g_log_tail.size() > kMaxLogTailBytes) {
        g_log_tail.erase(0, g_log_tail.size() - kMaxLogTailBytes);
    }
}

void rewrite_log_file_locked() {
    if (g_log_file_path.empty()) {
        return;
    }
    FILE* file = fopen(g_log_file_path.c_str(), "wb");
    if (file == nullptr) {
        return;
    }
    if (!g_log_tail.empty()) {
        fwrite(g_log_tail.data(), 1, g_log_tail.size(), file);
    }
    fclose(file);
    g_log_file_size = g_log_tail.size();
}

void append_log_chunk_locked(const char* data, size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    g_log_tail.append(data, size);
    trim_log_tail_locked();

    if (!g_log_file_path.empty()) {
        FILE* file = fopen(g_log_file_path.c_str(), "ab");
        if (file != nullptr) {
            fwrite(data, 1, size, file);
            fclose(file);
            g_log_file_size += size;
        }
        if (g_log_file_size > kMaxLogFileBytes) {
            rewrite_log_file_locked();
        }
    }
}

// One of these is logged per ~1KB block read on the DDP/disk path, which floods
// the console during a config-disk or TNFS boot and buries the actual failure.
// They carry no signal beyond "a block was read"; mounts, read completions, Fuji
// commands, errors and resets are not matched and so are always kept.
bool is_per_block_log_line(const std::string& line) {
    return line.rfind("BLOCK: ", 0) == 0 || line.rfind("DDP READ", 0) == 0;
}

// Opt-in firehose suppression for a clean repro log, toggled without a
// rebuild: FUJINET_QUIET_BLOCKS=1. Default off. Read once.
bool suppress_per_block_logs() {
    static const bool suppress = []() {
        const char* value = getenv("FUJINET_QUIET_BLOCKS");
        if (value == nullptr) {
            return false;
        }
        return value[0] == '1' || value[0] == 't' || value[0] == 'T';
    }();
    return suppress;
}

void forward_console_line(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line.empty()) {
        return;
    }
    if (suppress_per_block_logs() && is_per_block_log_line(line)) {
        return;
    }
    host_log("[fujinet] %s", line.c_str());
}

void stop_log_capture() {
    fflush(stdout);
    fflush(stderr);

    int saved_stdout_fd = -1;
    int saved_stderr_fd = -1;
    int log_read_fd = -1;
    int log_write_fd = -1;
    std::thread log_thread;

    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        saved_stdout_fd = g_saved_stdout_fd;
        saved_stderr_fd = g_saved_stderr_fd;
        log_read_fd = g_log_read_fd;
        log_write_fd = g_log_write_fd;
        g_saved_stdout_fd = -1;
        g_saved_stderr_fd = -1;
        g_log_read_fd = -1;
        g_log_write_fd = -1;
        log_thread = std::move(g_log_thread);
    }

    if (saved_stdout_fd >= 0) {
        dup2(saved_stdout_fd, STDOUT_FILENO);
        close(saved_stdout_fd);
    }
    if (saved_stderr_fd >= 0) {
        dup2(saved_stderr_fd, STDERR_FILENO);
        close(saved_stderr_fd);
    }
    if (log_write_fd >= 0) {
        close(log_write_fd);
    }

    if (log_thread.joinable()) {
        log_thread.join();
    }
    if (log_read_fd >= 0) {
        close(log_read_fd);
    }

    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_file_path.clear();
    g_log_file_size = 0;
}

void append_log_chunk(const char* data, size_t size) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    append_log_chunk_locked(data, size);
}

void start_log_capture(const std::string& runtime_root) {
    stop_log_capture();

    int log_pipe[2];
    if (fn_make_pipe(log_pipe) != 0) {
        host_log("fujinet: unable to create log pipe");
        return;
    }

    const int saved_stdout_fd = dup(STDOUT_FILENO);
    const int saved_stderr_fd = dup(STDERR_FILENO);
    if (saved_stdout_fd < 0 || saved_stderr_fd < 0) {
        if (saved_stdout_fd >= 0) close(saved_stdout_fd);
        if (saved_stderr_fd >= 0) close(saved_stderr_fd);
        close(log_pipe[0]);
        close(log_pipe[1]);
        host_log("fujinet: unable to duplicate stdio handles");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_log_read_fd = log_pipe[0];
        g_log_write_fd = log_pipe[1];
        g_saved_stdout_fd = saved_stdout_fd;
        g_saved_stderr_fd = saved_stderr_fd;
        g_log_tail.clear();
        g_log_file_size = 0;
        g_log_file_path = runtime_root + "/fujinet-console.log";
        rewrite_log_file_locked();
    }

    dup2(log_pipe[1], STDOUT_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    std::thread log_thread([read_fd = log_pipe[0]]() {
        set_thread_name("fujinet-log");
        char buffer[1024];
        std::string pending_line;

        while (true) {
            const ssize_t bytes_read = read(read_fd, buffer, sizeof(buffer));
            if (bytes_read <= 0) {
                break;
            }

            const std::string sanitized = sanitize_log_chunk(
                    buffer,
                    static_cast<size_t>(bytes_read)
            );
            if (sanitized.empty()) {
                continue;
            }

            append_log_chunk(sanitized.data(), sanitized.size());
            pending_line.append(sanitized);
            size_t newline_index = 0;
            while ((newline_index = pending_line.find('\n')) != std::string::npos) {
                forward_console_line(pending_line.substr(0, newline_index));
                pending_line.erase(0, newline_index + 1);
            }
        }

        if (!pending_line.empty()) {
            forward_console_line(pending_line);
        }
    });

    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_thread = std::move(log_thread);
}
}  // namespace

FUJINET_ENTRY void fujinet_desktop_submit_sam_audio(
        const uint8_t* audio,
        int sampleCount,
        int sampleRate
) {
    if (audio == nullptr || sampleCount <= 0) {
        return;
    }
    if (sampleRate != kSamSampleRate) {
        host_log("fujinet: SAM sample rate %d differs from expected %d; using raw stream",
                 sampleRate, kSamSampleRate);
    }

    std::unique_lock<std::mutex> lock(g_audio_mutex);
    std::vector<int16_t> utterance;
    utterance.reserve(static_cast<size_t>(sampleCount));
    for (int index = 0; index < sampleCount; ++index) {
        utterance.push_back(static_cast<int16_t>(
                (static_cast<int>(audio[index]) - 128) << 8
        ));
    }

    const size_t utterance_samples = utterance.size();
    if (utterance_samples == 0) {
        return;
    }

    if (utterance_samples > kMaxQueuedSamSamples) {
        host_log("fujinet: dropping oversized SAM utterance");
        return;
    }

    while (g_audio_queued_samples > 0) {
        g_audio_condition.wait(lock);
    }

    g_audio_queued_samples += utterance_samples;
    g_audio_utterances.push_back(std::move(utterance));
    g_audio_condition.notify_all();
}

/* Alias kept for firmware branches that call the Android submit hook. */
FUJINET_ENTRY void fujinet_android_submit_sam_audio(
        const uint8_t* audio, int sampleCount, int sampleRate) {
    fujinet_desktop_submit_sam_audio(audio, sampleCount, sampleRate);
}

FUJINET_ENTRY int fujinet_desktop_read_audio(
        int16_t* output,
        int maxSamples,
        int outputSampleRate
) {
    if (output == nullptr || maxSamples <= 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_audio_mutex);
    if (g_audio_utterances.empty()) {
        return 0;
    }

    const int resolvedOutputRate = outputSampleRate > 0 ? outputSampleRate : kSamSampleRate;
    const double sourcePerOutput = static_cast<double>(kSamSampleRate) / resolvedOutputRate;

    int produced = 0;
    while (produced < maxSamples && !g_audio_utterances.empty()) {
        if (g_audio_current_sample_index >= g_audio_utterances.front().size()) {
            g_audio_utterances.pop_front();
            g_audio_current_sample_index = 0;
            if (g_audio_utterances.empty()) {
                g_audio_queued_samples = 0;
                g_audio_resample_cursor = 0.0;
            }
            continue;
        }

        const std::vector<int16_t>& current = g_audio_utterances.front();
        output[produced++] = current[g_audio_current_sample_index];
        g_audio_resample_cursor += sourcePerOutput;
        size_t samples_to_advance = 0;
        while (g_audio_resample_cursor >= 1.0) {
            ++samples_to_advance;
            g_audio_resample_cursor -= 1.0;
        }
        if (samples_to_advance > 0) {
            advance_sam_samples_locked(samples_to_advance);
        }
    }

    if (g_audio_utterances.empty()) {
        g_audio_resample_cursor = 0.0;
    }
    return produced;
}

FUJINET_ENTRY void fujinet_desktop_clear_audio() {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    clear_audio_locked();
}

FUJINET_ENTRY int fujinet_desktop_copy_recent_log(char* output, int maxBytes) {
    if (output == nullptr || maxBytes <= 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_tail.empty()) {
        output[0] = '\0';
        return 0;
    }

    const size_t copyable = std::min(
        g_log_tail.size(),
        static_cast<size_t>(maxBytes - 1)
    );
    const size_t offset = g_log_tail.size() - copyable;
    memcpy(output, g_log_tail.data() + offset, copyable);
    output[copyable] = '\0';
    return static_cast<int>(copyable);
}

FUJINET_ENTRY bool fujinet_desktop_start_runtime(
        const char* runtimeRootPath,
        const char* configPath,
        const char* sdPath,
        const char* dataPath,
        int listenPort
) {
    (void)dataPath;
    (void)listenPort;

    std::unique_lock<std::mutex> lock(g_mutex);
    g_last_error.clear();

    if (g_running) {
        return true;
    }
    if (runtimeRootPath == nullptr || configPath == nullptr || sdPath == nullptr) {
        set_last_error_locked("FujiNet runtime arguments were missing");
        return false;
    }

    fnSystem.clear_shutdown_request();
    optind = 1;
    {
        std::lock_guard<std::mutex> audioLock(g_audio_mutex);
        clear_audio_locked();
    }

    const std::string runtimeRoot(runtimeRootPath);
    const std::string config(configPath);
    const std::string sd(sdPath);
    const char* webui_env = getenv("FUJINET_WEBUI_BIND");
    const std::string webui_bind(webui_env && *webui_env ? webui_env
                                                         : "127.0.0.1:64001");
    g_setup_complete = false;
    g_setup_failed = false;

    try {
        g_runtime_thread = std::thread([runtimeRoot, config, sd, webui_bind]() {
            set_thread_name("fujinet-svc");
            start_log_capture(runtimeRoot);
            if (chdir(runtimeRoot.c_str()) != 0) {
                std::lock_guard<std::mutex> lock(g_mutex);
                set_last_error_locked("FujiNet failed to change into runtime root");
                publish_setup_result_locked(false, true);
                stop_log_capture();
                return;
            }

            std::vector<std::string> argsStorage = {
                "fujinet",
                "-c",
                config,
                "-s",
                sd,
                // Web admin UI on the port the host app's webview points at.
                "-u",
                webui_bind,
            };
            std::vector<char*> argv;
            argv.reserve(argsStorage.size());
            for (std::string& arg : argsStorage) {
                argv.push_back(arg.data());
            }

            try {
                main_setup(static_cast<int>(argv.size()), argv.data());
                std::lock_guard<std::mutex> lock(g_mutex);
                g_running = true;
                publish_setup_result_locked(true, false);
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(g_mutex);
                set_last_error_locked(
                    std::string("FujiNet main_setup failed: ") + error.what()
                );
                g_running = false;
                publish_setup_result_locked(false, true);
                stop_log_capture();
                return;
            } catch (...) {
                std::lock_guard<std::mutex> lock(g_mutex);
                set_last_error_locked("FujiNet main_setup failed with an unknown error");
                g_running = false;
                publish_setup_result_locked(false, true);
                stop_log_capture();
                return;
            }

            fn_service_loop(nullptr);
            main_shutdown_handler();
            fnSystem.clear_shutdown_request();
            stop_log_capture();

            std::lock_guard<std::mutex> lock(g_mutex);
            g_running = false;
            g_stop_condition.notify_all();
        });
    } catch (const std::exception& error) {
        set_last_error_locked(
            std::string("FujiNet runtime thread could not start: ") + error.what()
        );
        g_setup_failed = true;
        return false;
    }

    g_setup_condition.wait(lock, []() {
        return g_setup_complete || g_setup_failed;
    });

    if (g_setup_complete) {
        return true;
    }

    std::thread failedThread = std::move(g_runtime_thread);
    lock.unlock();
    if (failedThread.joinable()) {
        failedThread.join();
    }

    return false;
}

FUJINET_ENTRY void fujinet_desktop_stop_runtime() {
    std::thread runtimeThread;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_running && !g_runtime_thread.joinable()) {
            return;
        }
        fnSystem.request_for_shutdown();
        runtimeThread = std::move(g_runtime_thread);

        /* fn_service_loop's own loop and the SmartPort-over-SLIP device check
         * the shutdown flag on a short poll, but iwmNetwork::shutdown() and
         * iwmCPM::shutdown() are no-ops (upstream TODOs) -- a device with a
         * blocking socket call in flight when shutdown is requested can miss
         * the flag for as long as that call takes to unblock, which for a
         * stalled remote host can be minutes. Wait for the thread with a
         * timeout instead of joining unconditionally, so a wedged FujiNet
         * runtime cannot hang the whole host process on exit: better a
         * detached thread than a window the user has to kill -9. */
        if (runtimeThread.joinable()) {
            const bool stopped = g_stop_condition.wait_for(
                lock, std::chrono::seconds(5), [] { return !g_running; });
            if (!stopped) {
                fprintf(stderr,
                        "fujinet_desktop_stop_runtime: runtime thread did not "
                        "stop within 5s (a device is likely blocked on I/O); "
                        "detaching it instead of hanging the process\n");
                runtimeThread.detach();
                /* Leave g_running set: the detached thread is still alive and
                 * touching FujiNet's global state, so a subsequent start must
                 * not spawn a second thread on top of it. */
                return;
            }
        }
    }

    if (runtimeThread.joinable()) {
        runtimeThread.join();
    }

    stop_log_capture();

    std::lock_guard<std::mutex> lock(g_mutex);
    fnSystem.clear_shutdown_request();
    g_running = false;
    {
        std::lock_guard<std::mutex> audioLock(g_audio_mutex);
        clear_audio_locked();
    }
}

FUJINET_ENTRY const char* fujinet_desktop_last_error_message() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_last_error.empty()) {
        return nullptr;
    }
    return g_last_error.c_str();
}

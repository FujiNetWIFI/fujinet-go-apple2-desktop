/*
 * Small portability seams for the session core across Linux, macOS, and
 * Windows (MinGW-w64 + winpthreads). The Linux-isms behind these are
 * clock_nanosleep(TIMER_ABSTIME), pthread_condattr_setclock, and
 * pthread_setname_np's signature -- each absent or different elsewhere.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2SESSION_COMPAT_H
#define APPLE2SESSION_COMPAT_H

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

/* setenv() is POSIX; MSVCRT spells it _putenv_s. Same shim the FujiNet
 * firmware already carries in lib/clock/Clock.cpp. */
static inline int apple2_setenv(const char *name, const char *value)
{
#if defined(_WIN32)
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

static inline void apple2_thread_setname(const char *name)
{
#if defined(_WIN32)
    (void)name; /* winpthreads names are not surfaced to tools; skip */
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

/* Sleep (don't spin) until an absolute CLOCK_MONOTONIC time in
 * microseconds. */
static inline void apple2_sleep_until_us(long target_us)
{
#if defined(__linux__)
    struct timespec ts;
    ts.tv_sec = target_us / 1000000L;
    ts.tv_nsec = (target_us % 1000000L) * 1000L;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) ==
           EINTR)
        ;
#elif defined(_WIN32)
    /* nanosleep is Sleep() under winpthreads, which rounds up to the 15.6 ms
     * scheduler tick: frames came out in bursts though the average rate was
     * right. A high-resolution waitable timer (Windows 10 1803+) wakes within
     * a fraction of a millisecond; older systems get a 1 ms tick
     * (timeBeginPeriod) and an ordinary waitable timer. The emulator thread is
     * the only caller. */
    static HANDLE timer;
    static int tried;
    struct timespec now;
    long remain;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (!timer) {
            timeBeginPeriod(1);
            timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    remain = target_us - (long)(now.tv_sec * 1000000L + now.tv_nsec / 1000L);
    if (remain <= 0)
        return;
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)remain * 10; /* relative, 100 ns units */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    {
        struct timespec rel;
        rel.tv_sec = remain / 1000000L;
        rel.tv_nsec = (remain % 1000000L) * 1000L;
        nanosleep(&rel, NULL);
    }
#else
    /* macOS has no absolute-deadline sleep: nap the remaining
     * delta and re-check, so a short nap can't overshoot the target. */
    for (;;) {
        struct timespec now, rel;
        long remain;
        clock_gettime(CLOCK_MONOTONIC, &now);
        remain = target_us -
                 (long)(now.tv_sec * 1000000L + now.tv_nsec / 1000L);
        if (remain <= 0)
            return;
        rel.tv_sec = remain / 1000000L;
        rel.tv_nsec = (remain % 1000000L) * 1000L;
        if (nanosleep(&rel, NULL) == 0)
            return;
    }
#endif
}

/* A condvar whose timed waits are stable: CLOCK_MONOTONIC-attributed on
 * Linux; default clock elsewhere (the only timed use is a short safety
 * bail, so wall-clock skew there is harmless). */
static inline void apple2_cond_init_monotonic(pthread_cond_t *cv)
{
#if defined(__linux__)
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &a);
    pthread_condattr_destroy(&a);
#else
    pthread_cond_init(cv, NULL);
#endif
}

/* Wait on cv (initialized by apple2_cond_init_monotonic) for at most ms
 * milliseconds. Returns 0 or ETIMEDOUT. */
static inline int apple2_cond_timedwait_ms(pthread_cond_t *cv,
                                         pthread_mutex_t *mtx, int ms)
{
#if defined(__APPLE__)
    struct timespec rel;
    rel.tv_sec = ms / 1000;
    rel.tv_nsec = (long)(ms % 1000) * 1000000L;
    return pthread_cond_timedwait_relative_np(cv, mtx, &rel);
#else
    /* Linux: CLOCK_MONOTONIC (matches the cond attr). Windows/MinGW: the
     * default cond clock is CLOCK_REALTIME. */
    struct timespec ts;
#if defined(__linux__)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cv, mtx, &ts);
#endif
}

#endif /* APPLE2SESSION_COMPAT_H */

/*
 * apple_realtime.h -- ask the macOS scheduler for a real-time slot for the
 * calling thread, the way a game's main loop does.
 *
 * THREAD_TIME_CONSTRAINT_POLICY: one frame period, a few milliseconds of work,
 * preemptible. A busy desktop or a power-saving core then does not make a
 * frame late. QOS_CLASS_USER_INTERACTIVE is the fallback if the kernel
 * refuses. Call it once, on the emulation thread. A no-op elsewhere.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>

static inline void apple_realtime_begin(double frame_ns)
{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);

    thread_time_constraint_policy_data_t p;
    p.period = (uint32_t)(frame_ns * tb.denom / tb.numer);
    p.computation = (uint32_t)(5000000.0 * tb.denom / tb.numer);   /* ~5 ms of work */
    p.constraint = p.period;
    p.preemptible = 1;
    mach_port_t self = mach_thread_self();
    thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), self);
}
#else
static inline void apple_realtime_begin(double frame_ns) { (void)frame_ns; }
#endif

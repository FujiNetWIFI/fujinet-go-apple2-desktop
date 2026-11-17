/*
 * audioring_s16_test.c -- the rate-controlled int16 ring holds its fill against
 * a clock that does not match the producer's, with no hard trims and no
 * glitches. Built as C and as C++.
 *
 * The producer makes 735 stereo frames every 1/59.92 s (44.1 kHz); the
 * consumer pulls 512-frame chunks on a device clock 0.08% FAST or SLOW against
 * it. A CONSTANT signal goes in: once the ring has primed, every sample coming
 * out must be that constant (a dropped or spliced block would show as a
 * different value, an underrun as a zero). Ten simulated minutes each way.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>

#include AUDIORING_HEADER

#define RATE   44100
#define TARGET (RATE / 30)
#define EMERG  (RATE / 4)
#define CHUNK  512
#define LEVEL  1234

static int run(double dev_ratio, const char *name)
{
    static audioring r;
    int16_t in[2 * 1024], out[2 * CHUNK];
    const double frame_s = 1.0 / 59.92;
    const int per_frame = (int)(RATE * frame_s + 0.5);
    const double dev_hz = RATE * dev_ratio;
    double t_prod = 0, t_cons = 0;
    uint32_t minfill = ~0u, maxfill = 0;
    long underrun_calls = 0, pulled = 0, bad = 0;
    int i;

    audioring_init(&r);
    for (i = 0; i < 2 * per_frame; i++) in[i] = LEVEL;
    for (long step = 0; step < 60L * 10 * 120; step++) {
        if (t_prod <= t_cons) {
            audioring_push(&r, in, per_frame, TARGET);
            t_prod += frame_s;
        } else {
            int n = audioring_pull(&r, out, CHUNK, TARGET, EMERG);
            if (n < CHUNK) underrun_calls++;
            /* the first output frame interpolates from the resampler's initial
             * "previous sample" of 0: skip stream start */
            for (i = 0; i < 2 * n; i++)
                if (pulled + i / 2 >= 8 && out[i] != LEVEL) bad++;
            pulled += n;
            t_cons += CHUNK / dev_hz;
            if (t_cons > 2.0) {
                uint32_t f = audioring_fill(&r);
                if (f < minfill) minfill = f;
                if (f > maxfill) maxfill = f;
            }
        }
    }
    printf("%s: fill %u..%u (target %d), overruns %u, trims %u, short pulls %ld, "
           "wrong samples %ld, pulled %ld\n", name, minfill, maxfill, TARGET,
           (unsigned)AR_LOAD_ACQ(r.overruns), (unsigned)AR_LOAD_ACQ(r.trims),
           underrun_calls, bad, pulled);
    if (AR_LOAD_ACQ(r.trims) || AR_LOAD_ACQ(r.overruns)) return 1;
    if (maxfill > TARGET * 3) return 1;
    if (underrun_calls > 4) return 1;     /* start-up priming only */
    if (bad) return 1;
    return 0;
}

int main(void)
{
    int bad = 0;
    bad |= run(1.0008, "device 0.08% fast");
    bad |= run(0.9992, "device 0.08% slow");
    bad |= run(1.0, "device exact");
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad;
}

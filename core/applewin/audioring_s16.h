/*
 * audioring_s16.h -- the rate-controlled sample ring between the machine thread
 * (producer) and the audio device callback (consumer): interleaved stereo
 * int16. Usable from C and C++.
 *
 * The machine makes one frame of samples per frame of emulated time; the
 * device drains on its own clock. The two never agree exactly (a vsync-locked
 * 60.00 Hz frame clock against the machine's 59.92 Hz), so a plain FIFO either
 * grows until it is cut with a click or runs dry. Here the producer runs a
 * linear resampler that steps up to 0.5% faster or slower to hold the fill
 * near the target, so the drift is absorbed as an inaudible pitch change of a
 * few cents instead.
 *
 * Same design as audioring.h (stereo float, the sms-style desktops) and the
 * coleco / atari800 / atari7800 / MS-DOS rings.
 *
 * Single producer, single consumer, acquire/release indices, no locks.
 * Header-only so a test links nothing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AUDIORING_S16_H
#define AUDIORING_S16_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#include <atomic>
#define AR_ATOMIC(T)        std::atomic<T>
#define AR_LOAD_ACQ(x)      (x).load(std::memory_order_acquire)
#define AR_LOAD_RLX(x)      (x).load(std::memory_order_relaxed)
#define AR_STORE_REL(x, v)  (x).store((v), std::memory_order_release)
#define AR_STORE(x, v)      (x).store((v))
#define AR_XCHG(x, v)       (x).exchange((v))
#define AR_ADD(x, v)        (x).fetch_add((v))
#else
#include <stdatomic.h>
#define AR_ATOMIC(T)        _Atomic T
#define AR_LOAD_ACQ(x)      atomic_load_explicit(&(x), memory_order_acquire)
#define AR_LOAD_RLX(x)      atomic_load_explicit(&(x), memory_order_relaxed)
#define AR_STORE_REL(x, v)  atomic_store_explicit(&(x), (v), memory_order_release)
#define AR_STORE(x, v)      atomic_store(&(x), (v))
#define AR_XCHG(x, v)       atomic_exchange(&(x), (v))
#define AR_ADD(x, v)        atomic_fetch_add(&(x), (v))
#endif

#define AR_FRAMES    16384u             /* stereo frames; power of two */
#define AR_MASK      (AR_FRAMES - 1u)
#define AR_MAX_STEP  0.005              /* the resampler's largest nudge */

typedef struct {
    int16_t buf[AR_FRAMES * 2];
    AR_ATOMIC(uint32_t) rd;             /* consumer's */
    AR_ATOMIC(uint32_t) wr;             /* producer's */
    AR_ATOMIC(int) flush;               /* producer -> consumer: drop it all */
    /* producer-only */
    double pos;
    int16_t last[2];
    /* consumer-only */
    int primed;
    /* counters, for the test and for curiosity */
    AR_ATOMIC(uint32_t) overruns;       /* frames the producer had to drop */
    AR_ATOMIC(uint32_t) trims;          /* emergency trims by the consumer */
} audioring;

/* Not thread-safe: only before both sides run. */
static inline void audioring_init(audioring *r)
{
    memset(r->buf, 0, sizeof r->buf);
    AR_STORE(r->rd, 0u);
    AR_STORE(r->wr, 0u);
    AR_STORE(r->flush, 0);
    r->pos = 0.0;
    r->last[0] = r->last[1] = 0;
    r->primed = 0;
    AR_STORE(r->overruns, 0u);
    AR_STORE(r->trims, 0u);
}

/* Asks the consumer to empty the ring (reset, unpause). Safe from any thread:
 * only the consumer ever moves `rd`. */
static inline void audioring_flush(audioring *r)
{
    AR_STORE(r->flush, 1);
}

/* Producer start-up: forget the resampler's phase. Touches producer-only
 * state, so it is safe while a consumer is already pulling. */
static inline void audioring_producer_reset(audioring *r)
{
    r->pos = 0.0;
    r->last[0] = r->last[1] = 0;
    audioring_flush(r);
}

static inline uint32_t audioring_fill(audioring *r)
{
    uint32_t w = AR_LOAD_ACQ(r->wr);
    uint32_t rd = AR_LOAD_ACQ(r->rd);
    return (w - rd) & AR_MASK;
}

static inline int16_t audioring_round(double v)
{
    return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/* Producer: push `frames` stereo frames, resampled toward `target` fill. */
static inline void audioring_push(audioring *r, const int16_t *in, int frames,
                                  uint32_t target)
{
    uint32_t w = AR_LOAD_RLX(r->wr);
    uint32_t rd = AR_LOAD_ACQ(r->rd);
    double fill = (double)((w - rd) & AR_MASK);
    double err = (fill - (double)target) / (double)target;
    double step = err * AR_MAX_STEP;
    double pos = r->pos;
    uint32_t dropped = 0;

    if (frames <= 0) return;
    if (step > AR_MAX_STEP) step = AR_MAX_STEP;
    if (step < -AR_MAX_STEP) step = -AR_MAX_STEP;
    /* A fuller ring plays its input FASTER (a bigger step consumes input
     * quicker, so fewer output frames per input frame); an emptier one
     * stretches it. */
    step += 1.0;

    while (pos < (double)frames) {
        int i = (int)pos;
        double frac = pos - (double)i;
        const int16_t *a = i == 0 ? r->last : in + 2 * (i - 1);
        const int16_t *b = in + 2 * i;
        uint32_t next = (w + 1) & AR_MASK;
        if (next != rd) {
            r->buf[2 * w]     = audioring_round(a[0] + (b[0] - a[0]) * frac);
            r->buf[2 * w + 1] = audioring_round(a[1] + (b[1] - a[1]) * frac);
            w = next;
        } else {
            dropped++;
        }
        pos += step;
    }
    r->pos = pos - (double)frames;
    r->last[0] = in[2 * (frames - 1)];
    r->last[1] = in[2 * (frames - 1) + 1];
    AR_STORE_REL(r->wr, w);
    if (dropped) AR_ADD(r->overruns, dropped);
}

/* Consumer: copy up to `n` stereo frames and return how many were real. The
 * caller zero-fills the rest.
 *
 * Priming: no output until `target` frames have accumulated, so one frame's
 * burst does not dribble out as several short requests; primed again only
 * once the ring runs completely dry.
 *
 * `emergency` is far beyond anything rate control produces (a stall on the
 * producer's side, a long pause): the oldest audio is dropped down to the
 * target rather than played late. */
static inline int audioring_pull(audioring *r, int16_t *out, int n,
                                 uint32_t target, uint32_t emergency)
{
    uint32_t rd = AR_LOAD_RLX(r->rd);
    uint32_t w = AR_LOAD_ACQ(r->wr);
    int i = 0;

    if (AR_XCHG(r->flush, 0)) {
        rd = w;
        r->primed = 0;
    }
    if (((w - rd) & AR_MASK) > emergency) {
        rd = (w - target) & AR_MASK;
        AR_ADD(r->trims, 1u);
    }
    if (!r->primed) {
        if (((w - rd) & AR_MASK) < target) {
            AR_STORE_REL(r->rd, rd);
            return 0;
        }
        r->primed = 1;
    }
    for (; i < n && rd != w; i++) {
        out[2 * i]     = r->buf[2 * rd];
        out[2 * i + 1] = r->buf[2 * rd + 1];
        rd = (rd + 1) & AR_MASK;
    }
    if (rd == w) r->primed = 0;
    AR_STORE_REL(r->rd, rd);
    return i;
}

#endif

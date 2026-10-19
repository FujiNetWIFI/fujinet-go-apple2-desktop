/*
 * sptrace -- the debugger's FujiNet trace: SmartPort-over-SLIP requests and
 * replies, decoded into apple2_fuji_packet lines and kept in a ring.
 *
 * No emulator in here: it takes the packets apple2_host's trace seam hands
 * the session, so a unit test can feed it bytes of its own. Every call locks;
 * feeds come from the emulator thread and the SmartPort Listener's thread,
 * reads from a UI thread.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_SPTRACE_H
#define APPLE2_SPTRACE_H

#include <stddef.h>
#include <stdint.h>

#include "apple2debug.h"

#define SPTRACE_MAX 512

typedef struct sptrace sptrace;

sptrace *sptrace_new(void);
void sptrace_free(sptrace *t);

/* kind 0 is a request, 1 its reply, 2 no reply in time (buf NULL); seq is
 * the request's sequence number, buf the whole SLIP-decoded packet. */
void sptrace_feed(sptrace *t, int kind, uint8_t seq, const uint8_t *buf,
                  size_t len);

/* The newest max packets, oldest first; returns the count. */
int  sptrace_copy(sptrace *t, apple2_fuji_packet *out, int max);
/* Empties the log and its counters; the unit names are kept. */
void sptrace_clear(sptrace *t);

/* Request/reply/timeout counts since the last clear, then the units named by
 * their DIB replies, as text lines; returns the length. */
int  sptrace_describe(sptrace *t, char *dst, int dstsz);

#endif /* APPLE2_SPTRACE_H */

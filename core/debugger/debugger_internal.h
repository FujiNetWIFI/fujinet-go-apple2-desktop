/*
 * debugger_internal -- the engine's side of the console (prompt.c) and of the
 * session (session.c). Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_DEBUGGER_INTERNAL_H
#define APPLE2_DEBUGGER_INTERNAL_H

#include "apple2debug.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the console ---- */
/* Step n instructions (stopped only). */
void apple2debug_step_count(apple2debug *d, long n);
/* Wait up to ms for the machine to be stopped with a generation other than
 * gen (read before issuing the command). 1 if it stopped. */
int apple2debug_wait_stop(apple2debug *d, unsigned gen, int ms);
/* A command ran: bump the generation so windows refresh. */
void apple2debug_touch(apple2debug *d);
/* Register names the evaluator knows, for completion. */
extern const char *const apple2debug_reg_names[];
/* Label names starting with word (case-insensitive), one per line into dst,
 * at most maxn. Returns the count. */
int apple2debug_label_completions(apple2debug *d, const char *word, char *dst,
                                  int dstsz, int maxn);
/* The video soft switches as text (the `video` command). */
int apple2debug_video_state(apple2debug *d, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_DEBUGGER_INTERNAL_H */

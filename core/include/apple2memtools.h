/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the four debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range lo-hi / lo..hi) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_MEMTOOLS_H
#define APPLE2_MEMTOOLS_H

#include <stdint.h>

#include "apple2debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "lo..hi", "lo-hi" (plain numbers or labels on both sides), or one
 * expression (an address, label, label+3, [ptr]). A bare token of 1-4 hex
 * digits ("C000") is hex unless it names a label; "$C000" and expressions
 * work as in the console. Returns 1 and fills lo and hi (inclusive,
 * lo <= hi), 0 if the text means nothing. */
int apple2sym_parse_range(apple2debug *d, const char *text, uint16_t *lo,
                         uint16_t *hi);

/* ---- watches ---- */
#define APPLE2WATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  apple2watch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  apple2watch_add(const char *text);
void apple2watch_remove(int index);
void apple2watch_clear(void);
const char *apple2watch_text(int index);
unsigned apple2watch_generation(void);
/* "count  $0300  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  apple2watch_format(apple2debug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  apple2watch_range(apple2debug *d, int index, uint16_t *lo, uint16_t *hi);

/* ---- memory search ---- */
enum { APPLE2SEARCH_HEX = 0, APPLE2SEARCH_TEXT };
#define APPLE2SEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, n, mask (0xFF) and flags for
 * apple2debug_search. Returns 1, or 0 for empty / malformed text. */
int apple2search_parse(int kind, const char *text, uint8_t *pat, int *n,
                      uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $0300 (count)  if value != old" */
int apple2bp_describe(apple2debug *d, const apple2debug_breakpoint *b, char *dst,
                     int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_MEMTOOLS_H */

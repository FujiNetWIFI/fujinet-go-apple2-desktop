/*
 * expr -- the debugger's expression evaluator: breakpoint conditions,
 * print, set, and every address argument the console takes.
 *
 *   numbers     $C000  0xC000  C000h  49152  %1010   (a name wins over a
 *               hex literal like "ADh")
 *   names       whatever the context resolves: registers, flags, labels,
 *               line/frame/cycles, and value/addr in a condition
 *   memory      [expr] byte, {expr} word (little-endian), side-effect free
 *   operators   C's, with C's precedence: unary - ! ~, * / %, + -, << >>,
 *               < <= > >=, == !=, &, ^, |, &&, ||
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_EXPR_H
#define APPLE2_EXPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* A name to a value; return 1 if known. */
    int (*name)(void *user, const char *name, long *out);
    /* A byte of memory, without side effects. */
    uint8_t (*read8)(void *user, uint16_t addr);
    /* Like read8 for a bank (apple2debug_bank) named by a prefix inside [] and
     * {}: [aux:$0400]. May be NULL (the prefix is then read as the CPU view). */
    uint8_t (*read8b)(void *user, int bank, uint16_t addr);
    void *user;
} expr_ctx;

/* Evaluate; 0 and *out on success, -1 with a message otherwise. With a NULL
 * context, names evaluate to 0 and memory reads to 0 -- a syntax check. */
int expr_eval(const char *text, const expr_ctx *ctx, long *out, char *err,
              int errsz);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_EXPR_H */

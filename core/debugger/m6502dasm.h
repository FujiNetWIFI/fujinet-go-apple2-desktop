/*
 * 6502 / 65C02 disassembler.
 *
 * Written fresh rather than reused from AppleWin. Reuse would be
 * licence-clean (GPL-2.0-or-later into GPL-3.0), but AppleWin's
 * Debugger_Disassembler.cpp is coupled to its own console, DisasmData_t and
 * formatting flags; a table-driven decoder producing the shape the engine and
 * all four frontends already consume is less work than untangling it. No code
 * from AppleWin's debugger is present. See COMPLIANCE.md.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_M6502DASM_H
#define APPLE2_M6502DASM_H

#include <stdint.h>

#include "apple2debug.h"

/* Decode one instruction from `bytes` (at least 3 readable bytes; short reads
 * are safe because the length is determined by the opcode alone) placed at
 * `addr`. Fills every field of `out` except `symbol`, which the engine
 * resolves. Returns the instruction length in bytes (1..3). */
int apple2dasm_decode(uint16_t addr, const uint8_t *bytes,
                      apple2dasm_line *out);

/* Instruction length for an opcode, without decoding it -- used to walk
 * backwards/forwards through memory. Always 1..3. */
int apple2dasm_length(uint8_t opcode);

#endif /* APPLE2_M6502DASM_H */

/*
 * apple2debug -- the debugger engine contract.
 *
 * One engine shared by every frontend; the frontends are thin native windows
 * over this. Modelled on the ADAM target's adamdebug.h so the two stay
 * diffable, with the 6502 register model and Apple II video views in place of
 * the Z80/TMS9928A ones.
 *
 * THREADING. Control calls (pause, step, breakpoints, register and memory
 * access) are safe to make from a UI thread: they are marshalled onto the
 * emulator thread, which is the only thread allowed to touch core state.
 * The stop callback fires ON THE EMULATOR THREAD and must be marshalled back
 * (g_idle_add / QMetaObject::invokeMethod) before touching any widget.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2DEBUG_H
#define APPLE2DEBUG_H

#include <stdint.h>

#include "apple2session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct apple2debug apple2debug;

typedef enum {
    APPLE2DBG_STOP_PAUSE = 0,
    APPLE2DBG_STOP_BREAKPOINT,
    APPLE2DBG_STOP_STEP,
    APPLE2DBG_STOP_RUNTO,
    /* The NMOS 6502 jams on an illegal opcode; the 65C02 does not. Surfaced
     * separately because it means the program is lost, not paused. */
    APPLE2DBG_STOP_JAMMED,
} apple2debug_stop_reason;

/* ---- disassembly --------------------------------------------------------- */

#define APPLE2DASM_JUMP     0x01
#define APPLE2DASM_CALL     0x02
#define APPLE2DASM_RET      0x04
#define APPLE2DASM_COND     0x08
#define APPLE2DASM_RELATIVE 0x10
#define APPLE2DASM_HALT     0x20  /* BRK / STP / an illegal opcode */
#define APPLE2DASM_ILLEGAL  0x40  /* not a documented 6502/65C02 opcode */

typedef struct {
    uint16_t addr;
    uint8_t  len;         /* 1..3 */
    uint8_t  bytes[3];
    char     text[32];    /* "LDA $C0F0,X" */
    uint16_t target;      /* branch/jump/call destination, else 0 */
    uint8_t  flags;       /* APPLE2DASM_* */
    const char *symbol;   /* label at addr, or NULL */
} apple2dasm_line;

/* ---- CPU state ----------------------------------------------------------- */

typedef struct {
    uint8_t  a, x, y, ps;
    uint16_t pc, sp;
    uint8_t  jammed;
} apple2debug_regs;

/* Processor status bits, for a frontend that wants to render them. */
#define APPLE2_PS_CARRY     0x01
#define APPLE2_PS_ZERO      0x02
#define APPLE2_PS_INTERRUPT 0x04
#define APPLE2_PS_DECIMAL   0x08
#define APPLE2_PS_BREAK     0x10
#define APPLE2_PS_RESERVED  0x20
#define APPLE2_PS_OVERFLOW  0x40
#define APPLE2_PS_SIGN      0x80

/* ---- lifecycle ----------------------------------------------------------- */

/* The engine is created lazily by apple2session_debugger(). */

/* ---- run control --------------------------------------------------------- */
void apple2debug_pause(apple2debug *d);
void apple2debug_resume(apple2debug *d);
int  apple2debug_is_paused(const apple2debug *d);
void apple2debug_step_into(apple2debug *d);
void apple2debug_step_over(apple2debug *d);
void apple2debug_step_out(apple2debug *d);
void apple2debug_run_to(apple2debug *d, uint16_t addr);

/* Fired on the EMULATOR thread when execution stops. Marshal before touching
 * a widget. */
typedef void (*apple2debug_stop_cb)(apple2debug_stop_reason reason,
                                    uint16_t pc, void *user);
void apple2debug_set_stop_callback(apple2debug *d, apple2debug_stop_cb cb,
                                   void *user);

/* ---- breakpoints --------------------------------------------------------- */
void apple2debug_bp_toggle(apple2debug *d, uint16_t addr);
void apple2debug_bp_set(apple2debug *d, uint16_t addr);
void apple2debug_bp_clear(apple2debug *d, uint16_t addr);
void apple2debug_bp_clear_all(apple2debug *d);
int  apple2debug_bp_is_set(const apple2debug *d, uint16_t addr);
/* Copies up to max set breakpoints into out, ascending; returns the count. */
int  apple2debug_bp_list(const apple2debug *d, uint16_t *out, int max);

/* ---- state --------------------------------------------------------------- */
void apple2debug_get_regs(apple2debug *d, apple2debug_regs *out);
void apple2debug_set_regs(apple2debug *d, const apple2debug_regs *in);
void apple2debug_read_mem(apple2debug *d, uint16_t addr, uint8_t *out,
                          int len);
void apple2debug_write_mem(apple2debug *d, uint16_t addr, const uint8_t *in,
                           int len);

/* Disassemble `count` instructions starting at addr into out[]; returns how
 * many were produced and, through next_addr, the address just past the last. */
int apple2debug_disassemble(apple2debug *d, uint16_t addr, int count,
                            apple2dasm_line *out, uint16_t *next_addr);

/* ---- symbols -------------------------------------------------------------
 * AppleWin ships APPLE2E.SYM / A2_BASIC.SYM / A2_DOS33.SYM2, which the
 * staging step already copies into the tree -- so unlike the ADAM target
 * there is no symbol-extraction script to run. Loading is best effort. */
int apple2debug_symbols_load(apple2debug *d, const char *path);
/* Exact label at addr, or NULL. */
const char *apple2debug_symbol_at(apple2debug *d, uint16_t addr);
/* Resolve a label to an address; returns 1 on success. */
int apple2debug_symbol_find(apple2debug *d, const char *name, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2DEBUG_H */

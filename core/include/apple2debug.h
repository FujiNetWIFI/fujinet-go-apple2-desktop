/*
 * apple2debug -- the debugger contract: a 6502/65C02 debugger engine over the
 * AppleWin core (core/debugger/) behind a C API the four native debugger
 * windows share. Same shape as the astrocade, SMS and Intellivision siblings'
 * debug headers (astrodebug.h) so the windows carry over: the engine is this
 * repository's own, built on the instruction-stepped path the session loop
 * takes while the engine is engaged, and on a data-access hook patched into
 * AppleWin's CPU loop (tools/applewin/patch-staged-tree.py) for the
 * read/write watchpoints.
 *
 * The engine only engages while a window holds it attached:
 * apple2debug_attach() when the window opens -- which also stops the machine --
 * and apple2debug_detach() when it closes, which disengages and lets the
 * machine run at full speed. Breakpoints are kept for the next attach. The
 * FujiNet trace is the exception: it records from session start.
 *
 * Stopping parks the emulation thread between instructions; "stopped" means
 * it is parked. Everything that changes the machine (registers, step, run)
 * needs it stopped; reads are always allowed. A window learns of stops,
 * breakpoint hits and edits by polling apple2debug_generation() (every 100 ms
 * is plenty); the optional stop callback fires on the emulator thread.
 *
 * Restarting the session while stopped does not hang: the parked thread is
 * released first. The engine, its breakpoints and its attach state survive the
 * restart; an attached engine stops the new machine at its first instruction.
 *
 * Memory is the CPU's live view (bank switching, language card, aux RAM,
 * ROM). Reads never run the soft-switch handlers, so peeking at $C000-$CFFF
 * cannot toggle hardware.
 *
 * Numbers in the console and in conditions: $C000, 0xC000, C000h (a name
 * wins over a hex literal), 49152 (decimal), %1010.
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

/* ---- attach / stop / go ---------------------------------------------------- */
/* Engage the engine and stop at the next instruction. The emulation thread
 * notices within a frame; poll apple2debug_is_stopped() or the generation.
 * Idempotent. Call it when the debugger window opens. */
void apple2debug_attach(apple2debug *d);
/* Disengage and let the machine run (the engine stays engaged while it has
 * nothing to do but nothing is installed in the CPU loop either). Breakpoints
 * are kept. Call it when the debugger window closes. */
void apple2debug_detach(apple2debug *d);
int  apple2debug_is_attached(apple2debug *d);

/* 1 once the emulation thread is parked. */
int  apple2debug_is_stopped(apple2debug *d);
void apple2debug_stop(apple2debug *d);       /* break at the next instruction */
void apple2debug_resume(apple2debug *d);     /* "run" */
/* Why the machine last stopped ("stopped", "breakpoint: exec $FA62",
 * "write $0300 = $03", "read $C030", "step", "step over", "step out",
 * "run to", "scanline", "frame", "jammed") and the PC it stopped at;
 * "running" and -1 while it runs. Returns the length. */
int  apple2debug_stop_reason(apple2debug *d, char *dst, int dstsz, int *address);
/* Bumped whenever the machine stopped or resumed, a command ran, a register,
 * memory or label was edited, or the breakpoint list changed. */
unsigned apple2debug_generation(apple2debug *d);
/* Bumped by every stop; frontends recentre their views on the PC when it
 * changes. */
uint64_t apple2debug_stop_serial(apple2debug *d);

/* Optional push for tests: fires on the EMULATOR thread, with no engine lock
 * held, each time the machine parks. Windows should poll the generation. */
typedef enum {
    APPLE2DBG_STOP_PAUSE = 0,   /* stop request, or attach */
    APPLE2DBG_STOP_BREAKPOINT,  /* an execute breakpoint (before it runs) */
    APPLE2DBG_STOP_WATCH,       /* a read/write breakpoint (after the
                                   accessing instruction) */
    APPLE2DBG_STOP_STEP,        /* step, over, out, scanline or frame done */
    APPLE2DBG_STOP_RUNTO,       /* run-to address reached */
    /* The NMOS 6502 jams on an illegal opcode; the 65C02 does not. Surfaced
     * separately because it means the program is lost, not paused. */
    APPLE2DBG_STOP_JAMMED
} apple2debug_stop_kind;
void apple2debug_set_stop_callback(apple2debug *d,
                                   void (*cb)(void *ud, int kind, uint16_t pc),
                                   void *ud);

/* ---- the console ------------------------------------------------------------ */
/* Run one command line. "help" lists them: step [n], over, out, run, stop,
 * runto <addr>, scanline [n], frame [n], break <addr>[-<end>] [cond],
 * bpr/bpw <addr>[-<end>] [cond], delete <id|addr>, enable/disable <id>, clear,
 * breaks, print <expr>, regs, set <reg> <expr>, mem <addr> [len], poke <addr>
 * <val...>, video, io (the soft switches and slots), fujinet, label <addr>
 * <name>, labels [file], disasm [addr] [n], trace [on|off|clear|n], watch
 * <expr>, unwatch <n>, find <hex...>, save <kind> <file>. Expressions: numbers,
 * registers (a x y p sp pc), flags (cf zf if df bf vf nf), [addr] byte and
 * {addr} word reads, labels, line frame cycles, and in a breakpoint condition
 * `value`, `addr` and `old` for the access; C operators with C precedence.
 * The stepping commands wait up to a quarter second for the machine to stop
 * again and then say where it is; run/go return at once.
 * Output text into dst; returns its length. */
int  apple2debug_command(apple2debug *d, const char *command, char *dst,
                         int dstsz);
/* Completions for the end of a line, one per line. Returns the count. */
int  apple2debug_completions(apple2debug *d, const char *prefix, char *dst,
                             int dstsz);
/* Evaluate an expression (no `value`/`addr`/`old`): 0 and *value on success,
 * -1 with the reason in err. */
int  apple2debug_eval(apple2debug *d, const char *text, long *value, char *err,
                      int errsz);

/* ---- stepping shortcuts (the toolbar) -------------------------------------- */
/* All need the machine stopped and return at once; the machine runs and stops
 * again, bumping the generation. The first instruction after a stop always
 * runs, whatever sits at the PC. Breakpoints and watchpoints stop any of them
 * early, with their own reason. */
void apple2debug_step(apple2debug *d);        /* F7: one instruction */
void apple2debug_step_over(apple2debug *d);   /* F8: over a JSR/BRK; else one step */
void apple2debug_step_out(apple2debug *d);    /* Shift+F8: until the stack unwinds
                                                 past the current routine (RTS/RTI) */
/* n scanlines (the beam line wraps 261 -> 0 on NTSC, 311 on PAL) / n frames:
 * "frame" lands on the first line of a frame. */
void apple2debug_scanline(apple2debug *d, int n);
void apple2debug_frame(apple2debug *d, int n);
/* Run until PC == addr. */
void apple2debug_run_to(apple2debug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int pc, sp;                 /* sp: the stack pointer as $0100-$01FF */
    int a, x, y, p;             /* p: the status byte */
    int nf, vf, bf, df, if_, zf, cf;   /* the flags in P (bit 5 is always 1) */
    int jammed;
    int is_65c02;               /* Enhanced //e (65C02) vs NMOS 6502 */
    uint64_t cycles;            /* cycles since power-on */
    int line;                   /* video scanline of the frame in progress */
    int horz;                   /* video clock within the line, 0-64 */
    uint32_t frame;             /* frames completed since power-on */
} apple2debug_cpu;
void apple2debug_cpu_get(apple2debug *d, apple2debug_cpu *out);
typedef enum {
    APPLE2_REG_PC = 0, APPLE2_REG_SP, APPLE2_REG_A, APPLE2_REG_X, APPLE2_REG_Y,
    APPLE2_REG_P,
    APPLE2_FLAG_N, APPLE2_FLAG_V, APPLE2_FLAG_B, APPLE2_FLAG_D, APPLE2_FLAG_I,
    APPLE2_FLAG_Z, APPLE2_FLAG_C,
    APPLE2_REG_COUNT
} apple2debug_reg;
/* Needs the machine stopped. Setting PC redirects the next instruction. */
void apple2debug_cpu_set(apple2debug *d, int reg, int value);

/* ---- memory ------------------------------------------------------------------- */
/* CPU-bus reads through the live bank map, without side effects. Returns n
 * (0 with the session not running). */
int  apple2debug_read(apple2debug *d, uint16_t addr, uint8_t *dst, int n);
/* A debugger edit through the same map: RAM takes it, ROM ignores it. */
void apple2debug_write(apple2debug *d, uint16_t addr, uint8_t value);

typedef struct {
    uint16_t lo, hi;
    int readable, writable;
    char name[48];          /* "Main RAM", "Aux RAM", "Language Card bank 2", ... */
} apple2debug_region;
/* Ordered, non-overlapping, covering $0000-$FFFF as the CPU sees it now
 * (RAMRD/RAMWRT/ALTZP/80STORE, LC read/write, slot ROM vs internal ROM).
 * Returns the count. */
int  apple2debug_memory_map(apple2debug *d, apple2debug_region *out, int max);

/* The machine, the soft switches and the slots. */
typedef struct {
    int slot;               /* 0-7; 8 is the //e's auxiliary slot */
    char card[48];          /* "Disk II", "FujiNet SmartPort", "Mockingboard", "" */
    int fujinet;            /* the FujiNet SmartPort card */
} apple2debug_slot;
/* Returns the count (9 on a //e, 8 otherwise; empty slots included). */
int  apple2debug_slots(apple2debug *d, apple2debug_slot *out, int max);
/* The model, the soft switches (TEXT MIXED PAGE2 HIRES 80COL 80STORE ALTCHAR
 * RAMRD RAMWRT ALTZP LC bank/read/write INTCXROM SLOTC3ROM) and the slot table
 * as monospace text lines (the Slots & I/O tab; ~2K is plenty). Returns the
 * length. */
int  apple2debug_io_state(apple2debug *d, char *dst, int dstsz);

/* ---- disassembly -------------------------------------------------------------- */
#define APPLE2DASM_JUMP     0x01
#define APPLE2DASM_CALL     0x02
#define APPLE2DASM_RET      0x04
#define APPLE2DASM_COND     0x08
#define APPLE2DASM_RELATIVE 0x10
#define APPLE2DASM_HALT     0x20  /* BRK / STP / an illegal opcode */
#define APPLE2DASM_ILLEGAL  0x40  /* not a documented 6502/65C02 opcode */

typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;      /* an execute breakpoint here, enabled or not */
    int breakpoint_enabled;
    int length;              /* 1..3 */
    char bytes[16];          /* "20 62 FA" */
    char label[48];          /* the label at this address, or "" */
    char disasm[48];         /* "JSR $FA62" */
    char comment[64];        /* a jump/call target's label, a soft switch's name */
    uint16_t target;         /* branch/jump/call destination when flags say so */
    uint8_t flags;           /* APPLE2DASM_* */
} apple2debug_line;
/* Up to `max` lines starting at `addr`, through the current memory map.
 * Returns the count; *pc_line is the index of the PC's line in `out`, -1 when
 * not among them (may be NULL). */
int  apple2debug_disassemble(apple2debug *d, uint16_t addr,
                             apple2debug_line *out, int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after `addr` --
 * for scrolling and "Follow PC". Backwards is a best guess. Clicks in the view
 * should map through the lines' `address`, never through a row number. */
int  apple2debug_row_address(apple2debug *d, uint16_t addr, int rows);

/* ---- video views ---------------------------------------------------------
 * Decode a page of Apple II video memory into pixels for a frontend to show
 * -- the Apple II equivalent of the ADAM target's VDP visualizers, and the
 * same contract: a fixed output size, rendered into the caller's buffer.
 *
 * These read through the debugger's memory accessor, so they show what the
 * CPU sees RIGHT NOW at a given page. That is deliberately not the same
 * thing as what the video hardware is scanning out (soft switches decide
 * that, and the session's own frame is the authority) -- it is a memory
 * inspector, which is what makes it useful for finding the page a program is
 * drawing into before it flips to it. */
#define APPLE2VIEW_WIDTH  280
#define APPLE2VIEW_HEIGHT 192

typedef enum {
    APPLE2VIEW_TEXT_1 = 0,
    APPLE2VIEW_TEXT_2,
    APPLE2VIEW_LORES_1,
    APPLE2VIEW_LORES_2,
    APPLE2VIEW_HIRES_1,
    APPLE2VIEW_HIRES_2,
    APPLE2VIEW_COUNT
} apple2debug_view;

/* Menu label for a view, or NULL past the end -- so a frontend can walk
 * idx = 0.. until NULL, as it does for the machine options. */
const char *apple2debug_view_name(int idx);

/* The base address a view decodes, for a frontend that wants to show it. */
uint16_t apple2debug_view_base(apple2debug_view view);

/* Renders view into dst, which must hold APPLE2VIEW_WIDTH *
 * APPLE2VIEW_HEIGHT uint32 (XRGB8888, tightly packed -- the same pixel
 * format apple2session_copy_frame produces, so a frontend paints it exactly
 * the way it paints the machine's own screen). Returns 0 on success, -1 for
 * an unknown view. */
int apple2debug_render_view(apple2debug *d, apple2debug_view view,
                            uint32_t *dst);

/* ---- the video inspector ---------------------------------------------------
 * What is under a point of the picture render_view drew, for a hover/click
 * inspector: the memory byte that holds it, its value now, and what it means
 * (a text cell's character and attribute, a lo-res block's colour nibble, a
 * hi-res pixel's bit, its byte, and the palette bit). x and y are picture
 * coordinates (0..APPLE2VIEW_WIDTH-1, 0..APPLE2VIEW_HEIGHT-1), not window
 * ones. Reads live memory without side effects; fine while the machine
 * runs (the value is then whatever it was a moment ago). */
typedef struct {
    int      valid;            /* 0: the point is outside the picture */
    uint16_t addr;             /* the byte that holds it */
    uint8_t  value;            /* that byte now */
    int      rx, ry, rw, rh;   /* the cell (text, lo-res) or the byte's seven
                                  pixels (hi-res), in picture coordinates, for
                                  a highlight */
    char     text[240];        /* monospace lines describing it */
} apple2debug_pixel;
/* Returns 0 and fills out, or -1 for an unknown view or a point outside. */
int apple2debug_view_inspect(apple2debug *d, apple2debug_view view, int x,
                             int y, apple2debug_pixel *out);

/* ---- FujiNet: the SmartPort packets on the SLIP link, both ways -----------
 * Every request the SmartPort card sends FujiNet and every reply, recorded
 * from session start whether or not a debugger window is open: it is most
 * useful about what happened before anyone looked. The newest 512 are kept,
 * and consecutive identical exchanges (all but the sequence number) fold into
 * one with a repeat count. Safe to call from any thread. */
typedef struct {
    uint64_t serial;        /* rises with every change, including a fold */
    int      to_host;       /* 1: FujiNet -> Apple II (a reply), 0: a request */
    int      device;        /* the unit, -1 if unknown */
    int      command;       /* request: SmartPort command; reply: its status */
    int      length;        /* bytes past the header, -1 if short */
    int      nbytes;
    uint8_t  data[64];      /* the packet's first bytes */
    char     text[96];      /* "-> #2A STATUS u1 THE_FUJI E8 GET ADAPTERCONFIG" */
    uint32_t repeat;        /* identical exchanges folded into this one */
} apple2_fuji_packet;
/* The newest max packets, oldest first; returns the count. */
int  apple2debug_fujinet_trace(apple2debug *d, apple2_fuji_packet *out, int max);
void apple2debug_fujinet_trace_clear(apple2debug *d);
/* The SmartPort card, FujiNet's link and the units it has named, as text
 * lines; returns the length. */
int  apple2debug_fujinet_state(apple2debug *d, char *dst, int dstsz);

/* ---- symbols and labels -------------------------------------------------------
 * AppleWin's APPLE2E.SYM / A2_BASIC.SYM / A2_DOS33.SYM2 ("HHHH NAME" per line)
 * load at session creation, with a built-in table of the soft switches and
 * monitor entry points. Also read: ca65 .lbl/.sym ("al 00C000 .name"),
 * WLA-DX-style "BB:AAAA name" and plain "name = $AAAA". */
int  apple2debug_load_symbols(apple2debug *d, const char *path, char *msg,
                              int msgsz);
/* Address of a label, or -1. Label at an address into dst (exact match;
 * empty if none). A label set here goes into table "user". */
int  apple2debug_label_address(apple2debug *d, const char *label);
int  apple2debug_address_label(apple2debug *d, uint16_t addr, char *dst,
                               int dstsz);
/* Label addr (NULL or "" removes the user label there). Returns 0. */
int  apple2debug_set_label(apple2debug *d, uint16_t addr, const char *label);
/* Lower-level: load a file into a named table (best effort; returns labels
 * added); the nearest label at or below addr with its offset (NULL if none);
 * a name's address (1 found). */
int  apple2debug_symbols_load(apple2debug *d, const char *path,
                              const char *table);
const char *apple2debug_symbol_at(apple2debug *d, uint16_t addr,
                                  uint16_t *offset);
int  apple2debug_symbol_find(apple2debug *d, const char *name, uint16_t *addr);

/* ---- breakpoints -------------------------------------------------------------- */
typedef enum {
    APPLE2DEBUG_BP_EXEC = 1, APPLE2DEBUG_BP_READ = 2, APPLE2DEBUG_BP_WRITE = 4
} apple2debug_bp_type;
typedef struct {
    int id;                  /* never reused */
    int type;                /* apple2debug_bp_type bits */
    uint16_t start, end;     /* an address range (soft switches included) */
    int enabled;
    unsigned hits;           /* times it fired (its condition true) */
    char condition[128];     /* an expression, "" for none */
} apple2debug_breakpoint;
/* READ and WRITE see data accesses only: opcode and operand fetches, stack
 * pushes/pulls and indirect-pointer fetches are not watched. READ, WRITE stop
 * after the accessing instruction; EXEC stops before the instruction runs.
 * `old` in a condition is the byte a write replaced. A condition that fails to
 * evaluate at run time stops rather than hides. */
/* Execute breakpoint at addr: added if absent, removed if present. Returns 1
 * when one is now set. */
int  apple2debug_breakpoint_toggle(apple2debug *d, uint16_t addr);
/* 1 when an execute breakpoint is at addr (enabled or not). */
int  apple2debug_breakpoint_check(apple2debug *d, uint16_t addr);
/* Returns the new id, or -1 with the reason in err. */
int  apple2debug_breakpoint_add(apple2debug *d, int type, uint16_t start,
                                uint16_t end, const char *condition, char *err,
                                int errsz);
void apple2debug_breakpoint_remove(apple2debug *d, int id);
void apple2debug_breakpoint_enable(apple2debug *d, int id, int enabled);
/* Copies up to max, in creation order. Returns the count. */
int  apple2debug_breakpoint_list(apple2debug *d, apple2debug_breakpoint *out,
                                 int max);
void apple2debug_breakpoint_clear(apple2debug *d);
void apple2debug_breakpoint_clear_hits(apple2debug *d);

/* ---- the instruction trace ring ---------------------------------------------- */
/* Records only while attached with tracing on. */
typedef struct {
    uint16_t pc, sp;
    uint8_t a, x, y, p;
    uint8_t len, bytes[3];
    uint64_t cycles;
    int16_t line;
} apple2debug_trace;
void apple2debug_trace_enable(apple2debug *d, int on);
int  apple2debug_trace_enabled(apple2debug *d);
/* Newest first; returns entries copied. */
int  apple2debug_trace_read(apple2debug *d, apple2debug_trace *out, int max);
void apple2debug_trace_clear(apple2debug *d);
/* Entries held (the ring keeps the newest 65536; allocated on first enable). */
int  apple2debug_trace_count(apple2debug *d);
/* "FA62  JSR $FB1E  A=.. X=.. Y=.. P=.. SP=..". Returns the length. */
int  apple2debug_trace_format(const apple2debug_trace *t, char *dst, int dstsz);
enum { APPLE2DEBUG_TRACE_ALL = 0, APPLE2DEBUG_TRACE_BRANCHES,
       APPLE2DEBUG_TRACE_RANGE };
/* 1 if the entry passes the filter (BRANCHES: jumps, branches, calls,
 * returns; RANGE: pc in lo..hi). */
int  apple2debug_trace_match(const apple2debug_trace *t, int filter,
                             uint16_t lo, uint16_t hi);

/* ---- memory search --------------------------------------------------------- */
#define APPLE2DEBUG_SEARCH_NOCASE 1
/* Stopped only (-1 while running). Finds up to max start addresses in from..to
 * where (mem & mask) == (pat & mask) for all n bytes. Returns the count. */
int  apple2debug_search(apple2debug *d, const uint8_t *pat, int n, uint8_t mask,
                        int flags, uint16_t from, uint16_t to, uint16_t *out,
                        int max);
/* The next match after `from` (before it if backward), wrapping once.
 * Returns 1 and *found, 0 for none, -1 while running. */
int  apple2debug_search_next(apple2debug *d, const uint8_t *pat, int n,
                             uint8_t mask, int flags, uint16_t from,
                             int backward, uint16_t *found);

/* ---- memory banks (read/write a bank without flipping soft switches) ------- */
/* Implemented through AppleWin's own pointers (main, aux, language card, ROM),
 * never the soft-switch handlers; works whether or not the flat memory cache
 * is valid. NOT covered: RamWorks / Saturn extra banks. */
typedef enum {
    APPLE2_BANK_CURRENT = 0,  /* as the CPU sees it now (today's behaviour) */
    APPLE2_BANK_MAIN,         /* main RAM $0000-$BFFF (the language card has its own banks) */
    APPLE2_BANK_AUX,          /* aux RAM $0000-$BFFF (//e with an extended 80-col / RamWorks card) */
    APPLE2_BANK_LC1,          /* language-card RAM bank 1 at $D000-$DFFF */
    APPLE2_BANK_LC2,          /* language-card RAM bank 2 at $D000-$DFFF */
    APPLE2_BANK_LCHI,         /* language-card RAM $E000-$FFFF */
    APPLE2_BANK_ROM,          /* system ROM $D000-$FFFF (internal ROM $C100-$CFFF on a //e too,
                                 see apple2debug_bank_range) */
    /* Added later: the aux side's language card RAM (a //e with aux RAM). */
    APPLE2_BANK_AUXLC1,       /* aux LC RAM bank 1 at $D000-$DFFF (console "alc1:") */
    APPLE2_BANK_AUXLC2,       /* aux LC RAM bank 2 at $D000-$DFFF ("alc2:") */
    APPLE2_BANK_AUXLCHI,      /* aux LC RAM $E000-$FFFF ("alchi:") */
    APPLE2_BANK_COUNT
} apple2debug_bank;
/* "As the CPU sees it", "Main RAM", "Aux RAM", "LC RAM bank 1", ... NULL past the end. */
const char *apple2debug_bank_name(int bank);
/* The console prefix ("aux", "main", "lc1", "lc2", "lchi", "rom"; "" for CURRENT). */
const char *apple2debug_bank_prefix(int bank);
/* The bank a console prefix names at the start of text ("aux:$400" -> APPLE2_BANK_AUX,
 * *consumed = 4, i.e. including the colon); -1 when there is none. Case-insensitive. */
int  apple2debug_bank_parse(const char *text, int *consumed);
/* The addresses the bank covers (CURRENT/ROM-less: 0000-FFFF; MAIN/AUX 0000-BFFF;
 * LC1/LC2 D000-DFFF; LCHI E000-FFFF; ROM D000-FFFF, C100-FFFF on a //e).
 * Returns apple2debug_bank_valid. */
int  apple2debug_bank_range(apple2debug *d, int bank, uint16_t *lo, uint16_t *hi);
/* 0 when absent (no aux RAM on a ][+, no LC on a ][), 1 present. CURRENT is always 1. */
int  apple2debug_bank_valid(apple2debug *d, int bank);
/* Returns n read; 0 if the bank is absent or the session is not running. Bytes
 * outside what the bank covers (LC1/LC2: outside $D000-$DFFF; LCHI: outside
 * $E000-$FFFF; ROM: outside $D000-$FFFF and the $C100-$CFFF internal ROM on a //e)
 * read as 0xFF; n is still returned if at least one byte is covered, else 0. */
int  apple2debug_read_bank(apple2debug *d, int bank, uint16_t addr, uint8_t *dst, int n);
/* RAM banks only; ROM and uncovered addresses are ignored. */
void apple2debug_write_bank(apple2debug *d, int bank, uint16_t addr, uint8_t value);
/* Like apple2debug_search for one bank; returns the count (-1 while running, 0 absent).
 * Addresses are the bank's own addresses. */
int  apple2debug_search_bank(apple2debug *d, int bank, const uint8_t *pat, int n,
                             uint8_t mask, int flags, uint16_t from, uint16_t to,
                             uint16_t *out, int max);
/* Like apple2debug_disassemble but over a bank (the Disassembly tab stays CPU-view).
 * pc_line is -1 for any bank but CURRENT. */
int  apple2debug_disassemble_bank(apple2debug *d, int bank, uint16_t addr,
                                  apple2debug_line *out, int max, int *pc_line);

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (the
 * disassembly of $0000-$FFFF as mapped now), "mem" (the 64K the CPU sees),
 * "regs" (CPU registers and soft switches as text). Message into msg; returns
 * 0 or -1. */
int  apple2debug_save(apple2debug *d, const char *kind, const char *path,
                      char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2DEBUG_H */

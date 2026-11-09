/*
 * debugger.c -- the apple2debug engine (core/include/apple2debug.h).
 *
 * The session loop (core/src/session.c) hands each frame to
 * apple2debug_session_frame() while the engine is engaged (a debugger window
 * holds it attached), instead of running the frame through retro_run(). That
 * function steps AppleWin's CPU one instruction at a time on the EMULATOR
 * thread, does the per-frame work retro_run would have done (the screen, the
 * sound, the cards: apple2host_step_*), and between instructions decides
 * whether to stop. Stopping PARKS the emulator thread inside that function on
 * a condition variable until a window resumes it, so "stopped" means parked:
 *   - at each instruction boundary it records the trace, then checks, in
 *     order, a stop request, a watchpoint hit by the previous instruction, an
 *     enabled execute breakpoint at the PC, and what the current action
 *     (step, over, out, run-to, scanline, frame) is waiting for. The first
 *     instruction after a stop always runs, whatever sits at the PC;
 *   - READ and WRITE breakpoints use a data-access hook patched into
 *     AppleWin's running CPU loops (tools/applewin/patch-staged-tree.py,
 *     patch 18). It is installed only while an enabled read/write breakpoint
 *     exists and the engine is attached. The hook only RECORDS candidates
 *     (address, value, old byte) on the emulator thread; after the
 *     instruction completes the engine evaluates the range and condition and,
 *     on a hit, stops at the next boundary -- after the accessing
 *     instruction, as on the sibling engines. Immediate-operand fetches are
 *     ignored; opcode and operand fetches, stack pushes/pulls and
 *     indirect-pointer fetches never reach the hook (AppleWin reads them
 *     through other macros), so they are not watched.
 * Detached, nothing is installed and the session runs retro_run() at full
 * speed.
 *
 * Threading. UI threads call the public API under d->mtx; the emulator loop
 * takes the same mutex at each boundary and never holds it while the CPU
 * runs. `live` is true while the machine exists (apple2debug_core_started()
 * .. apple2debug_core_gone(), called by the session on the emulator thread),
 * so no UI call touches memory the core has freed. The engine, its
 * breakpoints and its attach state outlive any number of machines; an
 * attached engine stops each new machine at its first instruction.
 *
 * The FujiNet trace stands apart: the session's sptrace records from session
 * start, attached or not.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apple2_host.h"
#include "compat.h"
#include "debugger_internal.h"
#include "expr.h"
#include "m6502dasm.h"
#include "session_internal.h"
#include "sptrace.h"

#define MAX_BP     128
#define TRACE_N    65536
#define MAX_CAND   32
#define BATCH_CYC  1023     /* the cards catch up every ~1ms, as AppleWin does */

enum { ACT_RUN = 0, ACT_STEP, ACT_OVER, ACT_OUT, ACT_RUNTO, ACT_SCANLINE,
       ACT_FRAME };

typedef struct {
    int pc, sp, a, x, y, p, jammed;
    uint64_t cycles;
    int line, horz;
    uint32_t frame;
} regs_t;

typedef struct {
    uint16_t addr;
    uint8_t value, old;
    int write;
} cand_t;

struct apple2debug {
    apple2session *s;

    pthread_mutex_t mtx;
    pthread_cond_t cv;      /* parks the emulation thread */
    pthread_cond_t evcv;    /* the machine parked */

    int live;               /* the machine exists */
    int releasing;          /* the session is stopping the machine */

    int attached;
    int stopped;            /* the emulation thread is parked */
    int stop_req;           /* park at the next boundary */
    int executed;           /* an instruction ran since the last boundary check */
    int act;                /* what the machine does once released */
    long count;             /* instructions / lines / frames still to go */
    uint16_t target;
    int sp_guard;
    int last_line;
    int last_op;            /* the opcode that just ran */

    char reason[128];
    int reason_addr;
    unsigned generation;
    uint64_t stop_serial;

    void (*stop_cb)(void *ud, int kind, uint16_t pc);
    void *stop_ud;

    /* The breakpoint table, and what the hook looks up. */
    apple2debug_breakpoint bp[MAX_BP];
    int nbp, next_id, nwatch;
    uint8_t map_exec[65536];    /* nonzero: an enabled execute bp covers it */
    uint8_t map_mem[65536];     /* APPLE2DEBUG_BP_READ / _WRITE bits */

    /* Hook-private (emulator thread). */
    volatile int cur_imm;       /* the running instruction is immediate */
    int ncand;
    cand_t cand[MAX_CAND];
    int pending;                /* a watchpoint fired during the instruction */
    char pend_reason[128];

    /* The expression context (mtx held while evaluating). */
    regs_t rs;
    long acc_value, acc_addr, acc_old;
    int in_access;

    pthread_mutex_t trace_mtx;
    int trace_on;
    apple2debug_trace *ring;    /* TRACE_N entries, allocated on first enable */
    int ring_head, ring_count;

    sym_tables syms;
};

static apple2debug *g_hook_d;   /* the engine the access hook feeds */

/* ---- small helpers ------------------------------------------------------------ */

static void bump_locked(apple2debug *d)
{
    d->generation++;
}

void apple2debug_touch(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

static void set_err(char *err, int errsz, const char *msg)
{
    if (err && errsz > 0)
        snprintf(err, (size_t)errsz, "%s", msg);
}

static uint8_t peek(apple2debug *d, uint16_t addr)
{
    uint8_t b = 0xFF;
    if (d->live)
        apple2host_read_mem(addr, &b, 1);
    return b;
}

static void get_regs(apple2debug *d, regs_t *r)
{
    apple2host_regs h;
    unsigned line = 0, horz = 0;
    unsigned cpf;

    memset(r, 0, sizeof *r);
    if (!d->live)
        return;
    apple2host_get_regs(&h);
    apple2host_get_beam(&line, &horz);
    cpf = apple2host_cycles_per_frame();
    r->pc = h.pc;
    r->sp = 0x100 | (h.sp & 0xFF);
    r->a = h.a; r->x = h.x; r->y = h.y;
    r->p = h.ps | 0x20;
    r->jammed = h.jammed;
    r->cycles = apple2host_cycles();
    r->line = (int)line;
    r->horz = (int)horz;
    r->frame = cpf ? (uint32_t)(r->cycles / cpf) : 0;
}

/* ---- expressions ------------------------------------------------------------------ */

const char *const apple2debug_reg_names[] = {
    "a", "x", "y", "p", "sp", "pc", "cf", "zf", "if", "df", "bf", "vf", "nf",
    "line", "frame", "cycles", "value", "old", "addr", NULL,
};

static uint8_t expr_read(void *user, uint16_t addr)
{
    return peek(user, addr);
}

static int bank_read_locked(apple2debug *d, int bank, uint16_t addr,
                            uint8_t *dst, int n);

static uint8_t expr_read_bank(void *user, int bank, uint16_t addr)
{
    uint8_t b = 0xFF;
    bank_read_locked(user, bank, addr, &b, 1);
    return b;
}

static int expr_name(void *user, const char *name, long *out)
{
    apple2debug *d = user;
    const regs_t *r = &d->rs;
    char n[16];
    size_t i;
    uint16_t addr;

    for (i = 0; name[i] && i + 1 < sizeof n; i++)
        n[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    n[i] = '\0';
    if (name[i]) n[0] = '\0'; /* too long to be a register */

    if (!strcmp(n, "value") || !strcmp(n, "addr") || !strcmp(n, "old")) {
        if (!d->in_access)
            return 0;
        *out = n[0] == 'v' ? d->acc_value : n[0] == 'o' ? d->acc_old
                                                         : d->acc_addr;
        return 1;
    }
#define REG(s, v) if (!strcmp(n, s)) { *out = (v); return 1; }
    REG("a", r->a) REG("x", r->x) REG("y", r->y) REG("p", r->p)
    REG("sp", r->sp) REG("pc", r->pc)
    REG("cf", r->p & 1) REG("zf", (r->p >> 1) & 1) REG("if", (r->p >> 2) & 1)
    REG("df", (r->p >> 3) & 1) REG("bf", (r->p >> 4) & 1)
    REG("vf", (r->p >> 6) & 1) REG("nf", (r->p >> 7) & 1)
    REG("line", r->line) REG("frame", (long)r->frame)
    REG("cycles", (long)r->cycles)
#undef REG
    if (symtabs_find(&d->syms, name, &addr)) {
        *out = addr;
        return 1;
    }
    return 0;
}

static int eval_locked(apple2debug *d, const char *text, long *out, char *err,
                       int errsz)
{
    expr_ctx ctx;

    ctx.name = expr_name;
    ctx.read8 = expr_read;
    ctx.read8b = expr_read_bank;
    ctx.user = d;
    get_regs(d, &d->rs);
    return expr_eval(text, &ctx, out, err, errsz);
}

int apple2debug_eval(apple2debug *d, const char *text, long *value, char *err,
                     int errsz)
{
    int rc;
    if (!d) return -1;
    pthread_mutex_lock(&d->mtx);
    d->in_access = 0;
    rc = eval_locked(d, text, value, err, errsz);
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

static int cond_true_locked(apple2debug *d, const apple2debug_breakpoint *b)
{
    long v = 0;
    char err[64];
    if (!b->condition[0])
        return 1;
    if (eval_locked(d, b->condition, &v, err, sizeof err) != 0)
        return 1; /* a condition that no longer evaluates stops, not hides */
    return v != 0;
}

/* ---- the access hook (emulator thread, inside the CPU loop) ----------------------- */

static void access_hook(uint16_t addr, uint8_t value, int write)
{
    apple2debug *d = g_hook_d;
    cand_t *c;

    if (!d)
        return;
    if (!write && d->cur_imm)
        return; /* the operand of an immediate instruction is not a data read */
    if (!(d->map_mem[addr] & (write ? APPLE2DEBUG_BP_WRITE : APPLE2DEBUG_BP_READ)))
        return;
    if (d->ncand >= MAX_CAND)
        return;
    c = &d->cand[d->ncand++];
    c->addr = addr;
    c->value = value;
    c->write = write;
    c->old = value;
    if (write)
        apple2host_read_mem(addr, &c->old, 1); /* the hook runs before the store */
}

/* mtx held: the hook is installed exactly while it can matter. */
static void sync_hooks_locked(apple2debug *d)
{
    const int want = d->live && !d->releasing && d->attached && d->nwatch > 0;

    if (want) {
        g_hook_d = d;
        apple2host_set_access_hook(access_hook);
    } else {
        apple2host_set_access_hook(NULL);
        if (g_hook_d == d)
            g_hook_d = NULL;
    }
}

static void rebuild_locked(apple2debug *d)
{
    int i;
    unsigned a;

    memset(d->map_exec, 0, sizeof d->map_exec);
    memset(d->map_mem, 0, sizeof d->map_mem);
    d->nwatch = 0;
    for (i = 0; i < d->nbp; i++) {
        const apple2debug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        for (a = b->start; a <= b->end; a++) {
            if (b->type & APPLE2DEBUG_BP_EXEC)
                d->map_exec[a] = 1;
            if (b->type & (APPLE2DEBUG_BP_READ | APPLE2DEBUG_BP_WRITE))
                d->map_mem[a] |= (uint8_t)(b->type & (APPLE2DEBUG_BP_READ |
                                                      APPLE2DEBUG_BP_WRITE));
        }
        if (b->type & (APPLE2DEBUG_BP_READ | APPLE2DEBUG_BP_WRITE))
            d->nwatch++;
    }
    if (!d->nwatch)
        d->pending = 0;
    sync_hooks_locked(d);
}

/* mtx held, after an instruction: turn recorded candidates into a pending
 * stop when a breakpoint's range and condition accept one. */
static void eval_candidates_locked(apple2debug *d)
{
    int k, i;

    for (k = 0; k < d->ncand && !d->pending; k++) {
        const cand_t *c = &d->cand[k];
        const int type = c->write ? APPLE2DEBUG_BP_WRITE : APPLE2DEBUG_BP_READ;

        d->acc_value = c->value;
        d->acc_addr = c->addr;
        d->acc_old = c->old;
        d->in_access = 1;
        for (i = 0; i < d->nbp; i++) {
            apple2debug_breakpoint *b = &d->bp[i];
            if (b->enabled && (b->type & type) && c->addr >= b->start &&
                c->addr <= b->end && cond_true_locked(d, b)) {
                b->hits++;
                if (c->write)
                    snprintf(d->pend_reason, sizeof d->pend_reason,
                             "write $%04X = $%02X", c->addr, c->value);
                else
                    snprintf(d->pend_reason, sizeof d->pend_reason,
                             "read $%04X", c->addr);
                d->pending = 1;
                break;
            }
        }
        d->in_access = 0;
    }
    d->ncand = 0;
}

/* ---- the trace ring --------------------------------------------------------------- */

static void trace_record(apple2debug *d, const regs_t *r, const apple2dasm_line *ins)
{
    apple2debug_trace *t;

    if (!d->ring)
        return;
    pthread_mutex_lock(&d->trace_mtx);
    t = &d->ring[d->ring_head];
    t->pc = (uint16_t)r->pc;
    t->sp = (uint16_t)r->sp;
    t->a = (uint8_t)r->a;
    t->x = (uint8_t)r->x;
    t->y = (uint8_t)r->y;
    t->p = (uint8_t)r->p;
    t->len = ins->len;
    memcpy(t->bytes, ins->bytes, 3);
    t->cycles = r->cycles;
    t->line = (int16_t)r->line;
    d->ring_head = (d->ring_head + 1) % TRACE_N;
    if (d->ring_count < TRACE_N)
        d->ring_count++;
    pthread_mutex_unlock(&d->trace_mtx);
}

/* ---- decoding -------------------------------------------------------------------- */

static int decode_at(apple2debug *d, uint16_t pc, apple2dasm_line *ins)
{
    uint8_t code[3] = { 0xFF, 0xFF, 0xFF };
    if (d->live)
        apple2host_read_mem(pc, code, 3);
    return apple2dasm_decode(pc, code, ins);
}

/* ---- parking ---------------------------------------------------------------------- */

/* mtx held; drops it around the picture and the callback. Returns with it
 * held, once released (or the session is stopping / the engine detached). */
static void park_locked(apple2debug *d, const char *reason, int kind,
                        uint16_t pc, unsigned *batch)
{
    void (*cb)(void *, int, uint16_t);
    void *ud;

    snprintf(d->reason, sizeof d->reason, "%s", reason);
    d->reason_addr = pc;
    d->stopped = 1;
    d->stop_req = 0;
    d->pending = 0;
    d->act = ACT_RUN;
    d->stop_serial++;
    bump_locked(d);
    pthread_cond_broadcast(&d->evcv);
    cb = d->stop_cb;
    ud = d->stop_ud;
    pthread_mutex_unlock(&d->mtx);
    /* the cards and the picture as far as the machine has run */
    apple2host_step_work(*batch);
    *batch = 0;
    apple2host_present_screen();
    if (cb)
        cb(ud, kind, pc);
    pthread_mutex_lock(&d->mtx);
    while (d->stopped && d->attached && !d->releasing && d->live &&
           !d->s->stop_flag)
        apple2_cond_timedwait_ms(&d->cv, &d->mtx, 100);
}

/* mtx held, at an instruction boundary. Returns 1 and fills why/kind when the
 * machine should stop here. `executed` says an instruction ran since the last
 * boundary (so the action's progress and the watchpoints count). */
static int boundary_locked(apple2debug *d, const regs_t *r, int executed,
                           char *why, size_t whysz, int *kind)
{
    const uint16_t pc = (uint16_t)r->pc;

    if (executed && r->jammed) {
        snprintf(why, whysz, "jammed");
        *kind = APPLE2DBG_STOP_JAMMED;
        return 1;
    }
    if (d->stop_req) {
        snprintf(why, whysz, "stopped");
        *kind = APPLE2DBG_STOP_PAUSE;
        return 1;
    }
    if (!executed)
        return 0;                       /* the first instruction always runs */
    if (d->pending) {
        snprintf(why, whysz, "%s", d->pend_reason);
        *kind = APPLE2DBG_STOP_WATCH;
        return 1;
    }
    if (d->map_exec[pc]) {
        int i, hit = 0;
        d->acc_addr = pc;
        d->acc_value = peek(d, pc);
        d->acc_old = d->acc_value;
        d->in_access = 1;
        for (i = 0; i < d->nbp; i++) {
            apple2debug_breakpoint *b = &d->bp[i];
            if (b->enabled && (b->type & APPLE2DEBUG_BP_EXEC) && pc >= b->start &&
                pc <= b->end && cond_true_locked(d, b)) {
                b->hits++;
                hit = 1;
            }
        }
        d->in_access = 0;
        if (hit) {
            snprintf(why, whysz, "breakpoint: exec $%04X", pc);
            *kind = APPLE2DBG_STOP_BREAKPOINT;
            return 1;
        }
    }
    switch (d->act) {
    case ACT_STEP:
        if (d->count <= 0) {
            snprintf(why, whysz, "step");
            *kind = APPLE2DBG_STOP_STEP;
            return 1;
        }
        break;
    case ACT_RUNTO:
        if (pc == d->target) {
            snprintf(why, whysz, "run to");
            *kind = APPLE2DBG_STOP_RUNTO;
            return 1;
        }
        break;
    case ACT_OVER:
        /* back at the instruction after the call, with the stack at (or
         * above, modulo the wrap) the level it was at; recursion and an
         * interrupt on the way leave it deeper */
        if (pc == d->target && ((r->sp - d->sp_guard) & 0xFF) < 0x80) {
            snprintf(why, whysz, "step over");
            *kind = APPLE2DBG_STOP_STEP;
            return 1;
        }
        break;
    case ACT_OUT: {
        const int delta = (r->sp - d->sp_guard) & 0xFF;
        if ((d->last_op == 0x60 || d->last_op == 0x40) && delta >= 1 &&
            delta < 0x80) {
            snprintf(why, whysz, "step out");
            *kind = APPLE2DBG_STOP_STEP;
            return 1;
        }
        break;
    }
    case ACT_SCANLINE:
        if (r->line != d->last_line) {
            d->last_line = r->line;
            if (--d->count <= 0) {
                snprintf(why, whysz, "scanline");
                *kind = APPLE2DBG_STOP_STEP;
                return 1;
            }
        }
        break;
    case ACT_FRAME:
        /* the beam wrapped: the first instruction of a new frame */
        if (r->line < d->last_line) {
            d->last_line = r->line;
            if (--d->count <= 0) {
                snprintf(why, whysz, "frame");
                *kind = APPLE2DBG_STOP_STEP;
                return 1;
            }
        }
        d->last_line = r->line;
        break;
    default:
        break;
    }
    return 0;
}

/* One frame's worth of debugger-controlled execution. Emulator thread, called
 * from session.c's loop while dbg_engaged. Returns 1 when a frame's worth of
 * cycles ran (the session paces and goes on), 0 if the session is stopping. */
int apple2debug_session_frame(apple2session *s)
{
    apple2debug *d = s->debugger;
    const unsigned budget = apple2host_cycles_per_frame();
    unsigned spent = 0, batch = 0;
    int result = 1;

    if (!d) {
        apple2host_core_run_frame();
        return 1;
    }
    apple2host_step_begin();

    for (;;) {
        regs_t r;
        apple2dasm_line ins;
        char why[128];
        int kind = 0, stop;
        unsigned cycles;

        pthread_mutex_lock(&d->mtx);
        if (s->stop_flag || d->releasing || !d->attached || !d->live) {
            pthread_mutex_unlock(&d->mtx);
            result = s->stop_flag ? 0 : 1;
            break;
        }
        if (d->executed) {
            eval_candidates_locked(d);
            if (d->act == ACT_STEP)
                d->count--;
        }
        get_regs(d, &r);
        /* After a release nothing ran yet, so the checks are skipped and the
         * first instruction runs whatever sits at the PC. */
        stop = boundary_locked(d, &r, d->executed, why, sizeof why, &kind);
        if (stop) {
            d->executed = 0;
            park_locked(d, why, kind, (uint16_t)r.pc, &batch);
            pthread_mutex_unlock(&d->mtx);
            continue;
        }
        pthread_mutex_unlock(&d->mtx);

        decode_at(d, (uint16_t)r.pc, &ins);
        if (d->trace_on)
            trace_record(d, &r, &ins);
        d->cur_imm = ins.imm;
        d->ncand = 0;
        d->last_op = ins.bytes[0];
        cycles = apple2host_step_instruction();
        d->executed = 1;
        if (!cycles) cycles = 1;
        batch += cycles;
        spent += cycles;
        if (batch >= BATCH_CYC) {
            apple2host_step_work(batch);
            batch = 0;
        }
        if (spent >= budget)
            break;
    }
    apple2host_step_work(batch);
    apple2host_step_end();
    return result;
}

/* ---- construction and the session's side (session_internal.h) ----------------------- */

apple2debug *apple2debug_create(apple2session *s)
{
    apple2debug *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->s = s;
    pthread_mutex_init(&d->mtx, NULL);
    pthread_mutex_init(&d->trace_mtx, NULL);
    apple2_cond_init_monotonic(&d->cv);
    apple2_cond_init_monotonic(&d->evcv);
    d->next_id = 1;
    d->reason_addr = -1;
    snprintf(d->reason, sizeof d->reason, "running");
    symtabs_load_text(&d->syms, apple2debug_builtin_sym, "builtin");
    /* The machine may already be up (the debugger is created lazily). */
    d->live = s->running && s->core_started == 1;
    return d;
}

void apple2debug_destroy(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->live = 0;
    sync_hooks_locked(d);
    pthread_mutex_unlock(&d->mtx);
    pthread_mutex_destroy(&d->mtx);
    pthread_mutex_destroy(&d->trace_mtx);
    pthread_cond_destroy(&d->cv);
    pthread_cond_destroy(&d->evcv);
    symtabs_free(&d->syms);
    free(d->ring);
    free(d);
}

/* The machine came up (emulator thread, before its first frame). An attached
 * engine stops it at its first instruction. */
void apple2debug_core_started(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->live = 1;
    d->releasing = 0;
    d->stopped = 0;
    d->pending = 0;
    d->ncand = 0;
    d->executed = 0;
    d->act = ACT_RUN;
    d->stop_req = d->attached;
    d->s->dbg_engaged = d->attached;
    snprintf(d->reason, sizeof d->reason, "running");
    d->reason_addr = -1;
    pthread_mutex_lock(&d->trace_mtx);
    d->ring_head = d->ring_count = 0;      /* a new machine, a new history */
    pthread_mutex_unlock(&d->trace_mtx);
    bump_locked(d);
    sync_hooks_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

/* The session is stopping the machine: release a parked emulator thread so it
 * can exit (called before the thread is joined). */
void apple2debug_resume_for_stop(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->releasing = 1;
    d->stopped = 0;
    d->stop_req = 0;
    d->pending = 0;
    d->act = ACT_RUN;
    sync_hooks_locked(d);
    snprintf(d->reason, sizeof d->reason, "running");
    d->reason_addr = -1;
    bump_locked(d);
    pthread_cond_broadcast(&d->cv);
    pthread_cond_broadcast(&d->evcv);
    pthread_mutex_unlock(&d->mtx);
}

/* The emulator thread is about to tear the core down: nothing may touch it
 * after this returns. */
void apple2debug_core_gone(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->live = 0;
    d->stopped = 0;
    d->stop_req = 0;
    d->pending = 0;
    d->act = ACT_RUN;
    sync_hooks_locked(d);
    snprintf(d->reason, sizeof d->reason, "running");
    d->reason_addr = -1;
    bump_locked(d);
    pthread_cond_broadcast(&d->cv);
    pthread_cond_broadcast(&d->evcv);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- attach / stop / go ------------------------------------------------------------- */

void apple2debug_attach(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->attached) {
        d->attached = 1;
        d->stop_req = d->live;
        d->executed = 0;
        d->s->dbg_engaged = 1;
        sync_hooks_locked(d);
    }
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_detach(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->attached = 0;
    d->stop_req = 0;
    d->stopped = 0;
    d->pending = 0;
    d->executed = 0;
    d->act = ACT_RUN;
    d->s->dbg_engaged = 0;
    sync_hooks_locked(d);       /* nothing is installed in the CPU loop now */
    snprintf(d->reason, sizeof d->reason, "running");
    d->reason_addr = -1;
    pthread_cond_broadcast(&d->cv);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int apple2debug_is_attached(apple2debug *d)
{
    int a;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    a = d->attached;
    pthread_mutex_unlock(&d->mtx);
    return a;
}

int apple2debug_is_stopped(apple2debug *d)
{
    int s;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    s = d->stopped;
    pthread_mutex_unlock(&d->mtx);
    return s;
}

void apple2debug_stop(apple2debug *d)
{
    if (!d) return;
    if (!apple2debug_is_attached(d)) {
        apple2debug_attach(d);
        return;
    }
    pthread_mutex_lock(&d->mtx);
    if (!d->stopped && d->live)
        d->stop_req = 1;
    pthread_mutex_unlock(&d->mtx);
}

/* Leave the parked state with an action (mtx held, stopped). The first
 * instruction after it runs unchecked: executed = 0. */
static void release_locked(apple2debug *d, int act)
{
    d->act = act;
    d->stopped = 0;
    d->stop_req = 0;
    d->executed = 0;
    d->ncand = 0;
    d->pending = 0;
    bump_locked(d);
    pthread_cond_broadcast(&d->cv);
}

void apple2debug_resume(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped)
        release_locked(d, ACT_RUN);
    else if (d->stop_req) /* asked to stop, not parked yet: never mind */
        d->stop_req = 0;
    pthread_mutex_unlock(&d->mtx);
}

int apple2debug_stop_reason(apple2debug *d, char *dst, int dstsz, int *address)
{
    int n;
    if (!d || !dst || dstsz <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    n = snprintf(dst, (size_t)dstsz, "%s", d->stopped ? d->reason : "running");
    if (address)
        *address = d->stopped ? d->reason_addr : -1;
    pthread_mutex_unlock(&d->mtx);
    return n < dstsz ? n : dstsz - 1;
}

unsigned apple2debug_generation(apple2debug *d)
{
    unsigned g;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    g = d->generation;
    pthread_mutex_unlock(&d->mtx);
    return g;
}

uint64_t apple2debug_stop_serial(apple2debug *d)
{
    uint64_t v;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    v = d->stop_serial;
    pthread_mutex_unlock(&d->mtx);
    return v;
}

int apple2debug_wait_stop(apple2debug *d, unsigned gen, int ms)
{
    int waited = 0, ok = 0;
    pthread_mutex_lock(&d->mtx);
    while (waited < ms) {
        if (d->stopped && d->generation != gen) {
            ok = 1;
            break;
        }
        apple2_cond_timedwait_ms(&d->evcv, &d->mtx, 5);
        waited += 5;
    }
    pthread_mutex_unlock(&d->mtx);
    return ok;
}

void apple2debug_set_stop_callback(apple2debug *d,
                                   void (*cb)(void *, int, uint16_t), void *ud)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->stop_cb = cb;
    d->stop_ud = ud;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- stepping ------------------------------------------------------------------------ */

void apple2debug_step_count(apple2debug *d, long n)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped && d->attached) {
        d->count = n > 0 ? n : 1;
        release_locked(d, ACT_STEP);
    }
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_step(apple2debug *d)
{
    apple2debug_step_count(d, 1);
}

void apple2debug_step_over(apple2debug *d)
{
    apple2dasm_line ins;
    regs_t r;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->live || !d->stopped || !d->attached) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    get_regs(d, &r);
    decode_at(d, (uint16_t)r.pc, &ins);
    if (ins.flags & APPLE2DASM_CALL) {
        d->target = (uint16_t)(r.pc + ins.len);
        d->sp_guard = r.sp;
        release_locked(d, ACT_OVER);
    } else if (ins.bytes[0] == 0x00) {
        /* BRK pushes PC+2 and RTI comes back there */
        d->target = (uint16_t)(r.pc + 2);
        d->sp_guard = r.sp;
        release_locked(d, ACT_OVER);
    } else {
        d->count = 1;
        release_locked(d, ACT_STEP);
    }
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_step_out(apple2debug *d)
{
    regs_t r;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->live && d->stopped && d->attached) {
        /* until an RTS/RTI leaves the stack above where it is now (modulo the
         * page wrap); a routine that juggles its own return address is not
         * recognised, and the machine then runs on rather than stopping
         * wrong */
        get_regs(d, &r);
        d->sp_guard = r.sp;
        release_locked(d, ACT_OUT);
    }
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_scanline(apple2debug *d, int n)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->live && d->stopped && d->attached) {
        regs_t r;
        get_regs(d, &r);
        d->last_line = r.line;
        d->count = n > 0 ? n : 1;
        release_locked(d, ACT_SCANLINE);
    }
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_frame(apple2debug *d, int n)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->live && d->stopped && d->attached) {
        regs_t r;
        get_regs(d, &r);
        d->last_line = r.line;
        d->count = n > 0 ? n : 1;
        release_locked(d, ACT_FRAME);
    }
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_run_to(apple2debug *d, uint16_t addr)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped && d->attached) {
        d->target = addr;
        release_locked(d, ACT_RUNTO);
    }
    pthread_mutex_unlock(&d->mtx);
}

/* ---- CPU ------------------------------------------------------------------------------ */

void apple2debug_cpu_get(apple2debug *d, apple2debug_cpu *o)
{
    regs_t r;

    memset(o, 0, sizeof *o);
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    get_regs(d, &r);
    o->is_65c02 = d->live ? apple2host_is_65c02() : 0;
    pthread_mutex_unlock(&d->mtx);
    o->pc = r.pc;
    o->sp = r.sp ? r.sp : 0x1FF;
    o->a = r.a; o->x = r.x; o->y = r.y; o->p = r.p;
    o->nf = (r.p >> 7) & 1;
    o->vf = (r.p >> 6) & 1;
    o->bf = (r.p >> 4) & 1;
    o->df = (r.p >> 3) & 1;
    o->if_ = (r.p >> 2) & 1;
    o->zf = (r.p >> 1) & 1;
    o->cf = r.p & 1;
    o->jammed = r.jammed;
    o->cycles = r.cycles;
    o->line = r.line;
    o->horz = r.horz;
    o->frame = r.frame;
}

static void set_bit(uint8_t *p, int bit, int on)
{
    if (on)
        *p = (uint8_t)(*p | (1u << bit));
    else
        *p = (uint8_t)(*p & ~(1u << bit));
}

void apple2debug_cpu_set(apple2debug *d, int reg, int v)
{
    apple2host_regs h;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->live || !d->stopped) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    apple2host_get_regs(&h);
    switch (reg) {
    case APPLE2_REG_PC:
        h.pc = (uint16_t)v;
        h.jammed = 0;               /* redirecting a jammed CPU un-jams it */
        d->reason_addr = v & 0xFFFF;
        break;
    case APPLE2_REG_SP: h.sp = (uint16_t)(0x100 | (v & 0xFF)); break;
    case APPLE2_REG_A:  h.a = (uint8_t)v; break;
    case APPLE2_REG_X:  h.x = (uint8_t)v; break;
    case APPLE2_REG_Y:  h.y = (uint8_t)v; break;
    case APPLE2_REG_P:  h.ps = (uint8_t)(v | 0x20); break;
    case APPLE2_FLAG_N: set_bit(&h.ps, 7, v); break;
    case APPLE2_FLAG_V: set_bit(&h.ps, 6, v); break;
    case APPLE2_FLAG_B: set_bit(&h.ps, 4, v); break;
    case APPLE2_FLAG_D: set_bit(&h.ps, 3, v); break;
    case APPLE2_FLAG_I: set_bit(&h.ps, 2, v); break;
    case APPLE2_FLAG_Z: set_bit(&h.ps, 1, v); break;
    case APPLE2_FLAG_C: set_bit(&h.ps, 0, v); break;
    default:
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    apple2host_set_regs(&h);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- memory ----------------------------------------------------------------------------- */

int apple2debug_read(apple2debug *d, uint16_t addr, uint8_t *dst, int n)
{
    int i;

    if (!d || !dst || n <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    if (!d->live) {
        pthread_mutex_unlock(&d->mtx);
        memset(dst, 0, (size_t)n);
        return 0;
    }
    for (i = 0; i < n; i++)
        apple2host_read_mem((uint16_t)(addr + i), &dst[i], 1);
    pthread_mutex_unlock(&d->mtx);
    return n;
}

void apple2debug_write(apple2debug *d, uint16_t addr, uint8_t value)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->live) {
        apple2host_write_mem(addr, &value, 1);
        bump_locked(d);
    }
    pthread_mutex_unlock(&d->mtx);
}

/* One entry per 256-byte page of what the CPU sees now. */
typedef struct {
    char name[48];
    int r, w;
} page_t;

static void page_set(page_t *pg, int first, int last, const char *name, int r,
                     int w)
{
    int i;
    for (i = first; i <= last; i++) {
        snprintf(pg[i].name, sizeof pg[i].name, "%s", name);
        pg[i].r = r;
        pg[i].w = w;
    }
}

static void build_pages(page_t *pg, uint32_t mm, int has_aux)
{
    const int altzp = (mm & APPLE2HOST_MF_ALTZP) != 0;
    const int rd = (mm & APPLE2HOST_MF_RAMRD) != 0;
    const int wr = (mm & APPLE2HOST_MF_RAMWRT) != 0;
    const int store = (mm & APPLE2HOST_MF_80STORE) != 0;
    const int p2 = (mm & APPLE2HOST_MF_PAGE2) != 0;
    const int hires = (mm & APPLE2HOST_MF_HIRES) != 0;
    char nm[48];
    int a;

    memset(pg, 0, 256 * sizeof *pg);
    /* RAM: read and write banks are chosen separately by RAMRD / RAMWRT */
    for (a = 0; a < 256; a++) {
        int auxr = rd, auxw = wr;
        if (a < 2)                       /* zero page and stack */
            auxr = auxw = altzp;
        else if (store && ((a >= 0x04 && a <= 0x07) ||
                           (hires && a >= 0x20 && a <= 0x3F)))
            auxr = auxw = p2;            /* 80STORE: PAGE2 picks the bank */
        if (a >= 0xC0)
            continue;
        if (!has_aux)
            auxr = auxw = 0;
        if (auxr == auxw)
            snprintf(nm, sizeof nm, "%s RAM", auxr ? "Aux" : "Main");
        else
            snprintf(nm, sizeof nm, "%s RAM (writes to %s)", auxr ? "Aux" : "Main",
                     auxw ? "Aux" : "Main");
        page_set(pg, a, a, nm, 1, 1);
    }
    page_set(pg, 0xC0, 0xC0, "I/O soft switches", 1, 1);
    if (!has_aux || !(mm & APPLE2HOST_MF_INTCXROM)) {
        page_set(pg, 0xC1, 0xC2, "Slot 1-2 ROM", 1, 0);
        if (has_aux && !(mm & APPLE2HOST_MF_SLOTC3ROM))
            page_set(pg, 0xC3, 0xC3, "Internal ROM (80-column firmware)", 1, 0);
        else
            page_set(pg, 0xC3, 0xC3, "Slot 3 ROM", 1, 0);
        page_set(pg, 0xC4, 0xC7, "Slot 4-7 ROM", 1, 0);
        page_set(pg, 0xC8, 0xCF, "Slot expansion ROM", 1, 0);
    } else {
        page_set(pg, 0xC1, 0xCF, "Internal ROM", 1, 0);
    }
    /* $D000-$FFFF: the language card over the ROM */
    {
        const int high = (mm & APPLE2HOST_MF_HIGHRAM) != 0;
        const int wram = (mm & APPLE2HOST_MF_WRITERAM) != 0;
        const char *aux = (has_aux && altzp) ? "Aux " : "";
        const char *rom = wram ? "ROM (writes to Language Card RAM)" : "ROM";

        if (high) {
            snprintf(nm, sizeof nm, "%sLanguage Card RAM bank %d", aux,
                     (mm & APPLE2HOST_MF_BANK2) ? 2 : 1);
            page_set(pg, 0xD0, 0xDF, nm, 1, wram);
            snprintf(nm, sizeof nm, "%sLanguage Card RAM", aux);
            page_set(pg, 0xE0, 0xFF, nm, 1, wram);
        } else {
            page_set(pg, 0xD0, 0xFF, rom, 1, wram);
        }
    }
}

int apple2debug_memory_map(apple2debug *d, apple2debug_region *out, int max)
{
    page_t pg[256];
    int n = 0, a = 0;

    if (!d || !out || max <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    if (!d->live) {
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    build_pages(pg, apple2host_mem_mode(), apple2host_has_aux());
    pthread_mutex_unlock(&d->mtx);
    while (a < 256 && n < max) {
        int b = a;
        while (b + 1 < 256 && !strcmp(pg[b + 1].name, pg[a].name) &&
               pg[b + 1].r == pg[a].r && pg[b + 1].w == pg[a].w)
            b++;
        out[n].lo = (uint16_t)(a << 8);
        out[n].hi = (uint16_t)((b << 8) | 0xFF);
        out[n].readable = pg[a].r;
        out[n].writable = pg[a].w;
        snprintf(out[n].name, sizeof out[n].name, "%s", pg[a].name);
        n++;
        a = b + 1;
    }
    return n;
}

int apple2debug_slots(apple2debug *d, apple2debug_slot *out, int max)
{
    int n = 0, i, total;

    if (!d || !out || max <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    if (!d->live) {
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    total = apple2host_has_aux() ? 9 : 8;
    for (i = 0; i < total && n < max; i++) {
        memset(&out[n], 0, sizeof out[n]);
        out[n].slot = i;
        out[n].fujinet = apple2host_slot_card(i, out[n].card, sizeof out[n].card);
        n++;
    }
    pthread_mutex_unlock(&d->mtx);
    return n;
}

static int video_text(char *dst, int dstsz, uint32_t vm, int alt)
{
    return snprintf(dst, (size_t)dstsz,
                    "TEXT %s  MIXED %s  PAGE2 %s  HIRES %s  80COL %s  DHIRES %s\n"
                    "80STORE %s  ALTCHAR %s\n",
                    (vm & APPLE2HOST_VF_TEXT) ? "on" : "off",
                    (vm & APPLE2HOST_VF_MIXED) ? "on" : "off",
                    (vm & APPLE2HOST_VF_PAGE2) ? "on" : "off",
                    (vm & APPLE2HOST_VF_HIRES) ? "on" : "off",
                    (vm & APPLE2HOST_VF_80COL) ? "on" : "off",
                    (vm & APPLE2HOST_VF_DHIRES) ? "on" : "off",
                    (vm & APPLE2HOST_VF_80STORE) ? "on" : "off",
                    alt ? "on" : "off");
}

int apple2debug_video_state(apple2debug *d, char *dst, int dstsz)
{
    int live;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    live = d->live;
    if (live)
        video_text(dst, dstsz, apple2host_video_mode(), apple2host_altcharset());
    pthread_mutex_unlock(&d->mtx);
    if (!live)
        snprintf(dst, (size_t)dstsz, "The machine is not running.\n");
    return (int)strlen(dst);
}

int apple2debug_io_state(apple2debug *d, char *dst, int dstsz)
{
    int n = 0, i, total;
    uint32_t mm, vm;
    int alt;
    char cards[9][48];
    int fuji[9];
    char model[48];
    int c65;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    if (!d->live) {
        pthread_mutex_unlock(&d->mtx);
        return snprintf(dst, (size_t)dstsz, "The machine is not running.\n");
    }
    mm = apple2host_mem_mode();
    vm = apple2host_video_mode();
    alt = apple2host_altcharset();
    c65 = apple2host_is_65c02();
    snprintf(model, sizeof model, "%s", apple2host_model_name());
    total = apple2host_has_aux() ? 9 : 8;
    for (i = 0; i < total; i++)
        fuji[i] = apple2host_slot_card(i, cards[i], sizeof cards[i]);
    pthread_mutex_unlock(&d->mtx);

#define PUT(...) do { if (n < dstsz - 1) { \
        int w_ = snprintf(dst + n, (size_t)(dstsz - n), __VA_ARGS__); \
        if (w_ > 0) n += w_ < dstsz - n ? w_ : dstsz - n - 1; } } while (0)

    PUT("Machine   %s (%s)\n\n", model, c65 ? "65C02" : "6502");
    PUT("Soft switches\n");
    PUT("  TEXT    %-3s  MIXED   %-3s  PAGE2   %-3s  HIRES   %-3s\n",
        (vm & APPLE2HOST_VF_TEXT) ? "on" : "off",
        (vm & APPLE2HOST_VF_MIXED) ? "on" : "off",
        (mm & APPLE2HOST_MF_PAGE2) ? "on" : "off",
        (mm & APPLE2HOST_MF_HIRES) ? "on" : "off");
    PUT("  80COL   %-3s  80STORE %-3s  ALTCHAR %-3s\n",
        (vm & APPLE2HOST_VF_80COL) ? "on" : "off",
        (mm & APPLE2HOST_MF_80STORE) ? "on" : "off", alt ? "on" : "off");
    PUT("  RAMRD   %-3s  RAMWRT  %-3s  ALTZP   %-3s\n",
        (mm & APPLE2HOST_MF_RAMRD) ? "on" : "off",
        (mm & APPLE2HOST_MF_RAMWRT) ? "on" : "off",
        (mm & APPLE2HOST_MF_ALTZP) ? "on" : "off");
    PUT("  LC      bank %d, read %s, write %s\n",
        (mm & APPLE2HOST_MF_BANK2) ? 2 : 1,
        (mm & APPLE2HOST_MF_HIGHRAM) ? "RAM" : "ROM",
        (mm & APPLE2HOST_MF_WRITERAM) ? "RAM" : "protected");
    PUT("  INTCXROM %-3s  SLOTC3ROM %-3s\n",
        (mm & APPLE2HOST_MF_INTCXROM) ? "on" : "off",
        (mm & APPLE2HOST_MF_SLOTC3ROM) ? "on" : "off");
    PUT("\nSlots\n");
    for (i = 0; i < total; i++) {
        if (i == 8)
            PUT("  Aux   %s%s\n", cards[i][0] ? cards[i] : "(empty)",
                fuji[i] ? "  <- FujiNet" : "");
        else
            PUT("  %d     %s%s\n", i, cards[i][0] ? cards[i] : "(empty)",
                fuji[i] ? "  <- FujiNet" : "");
    }
#undef PUT
    return n;
}

/* ---- disassembly ------------------------------------------------------------------------ */

static void label_for_locked(apple2debug *d, uint16_t addr, char *dst, int dstsz,
                             int with_offset)
{
    const char *l = symtabs_exact(&d->syms, addr);
    dst[0] = '\0';
    if (l) {
        snprintf(dst, (size_t)dstsz, "%s", l);
    } else if (with_offset) {
        uint16_t off = 0;
        l = symtabs_at(&d->syms, addr, &off);
        if (l && off > 0 && off <= 16)
            snprintf(dst, (size_t)dstsz, "%s+$%X", l, off);
    }
}

static int bank_read_locked(apple2debug *d, int bank, uint16_t addr,
                            uint8_t *dst, int n);

static void fill_line_bank_locked(apple2debug *d, int bank, uint16_t addr,
                                  apple2debug_line *l, uint16_t pc)
{
    apple2dasm_line ins;
    int i, o = 0;

    memset(l, 0, sizeof *l);
    if (bank == APPLE2_BANK_CURRENT) {
        decode_at(d, addr, &ins);
    } else {
        uint8_t code[3] = { 0xFF, 0xFF, 0xFF };
        bank_read_locked(d, bank, addr, code, 3);
        apple2dasm_decode(addr, code, &ins);
    }
    l->address = addr;
    l->length = ins.len;
    l->is_pc = bank == APPLE2_BANK_CURRENT && addr == pc;
    l->target = ins.target;
    l->flags = ins.flags;
    for (i = 0; bank == APPLE2_BANK_CURRENT && i < d->nbp; i++) {
        const apple2debug_breakpoint *b = &d->bp[i];
        if ((b->type & APPLE2DEBUG_BP_EXEC) && addr >= b->start &&
            addr <= b->end) {
            l->has_breakpoint = 1;
            if (b->enabled)
                l->breakpoint_enabled = 1;
        }
    }
    for (i = 0; i < ins.len && o < (int)sizeof l->bytes - 3; i++)
        o += snprintf(l->bytes + o, sizeof l->bytes - (size_t)o, "%s%02X",
                      i ? " " : "", ins.bytes[i]);
    label_for_locked(d, addr, l->label, sizeof l->label, 0);
    snprintf(l->disasm, sizeof l->disasm, "%s", ins.text);

    if (ins.flags & (APPLE2DASM_JUMP | APPLE2DASM_CALL)) {
        label_for_locked(d, ins.target, l->comment, sizeof l->comment, 1);
    } else {
        /* an absolute operand that names a place: a soft switch, a monitor
         * variable, a routine's address */
        const char *p = strchr(ins.text, '$');
        if (p && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) &&
            isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
            unsigned v;
            if (sscanf(p + 1, "%4x", &v) == 1)
                label_for_locked(d, (uint16_t)v, l->comment,
                                 sizeof l->comment, 0);
        }
    }
}

static void fill_line_locked(apple2debug *d, uint16_t addr, apple2debug_line *l,
                             uint16_t pc)
{
    fill_line_bank_locked(d, APPLE2_BANK_CURRENT, addr, l, pc);
}

int apple2debug_disassemble(apple2debug *d, uint16_t addr, apple2debug_line *out,
                            int max, int *pc_line)
{
    int n = 0;
    regs_t r;

    if (pc_line) *pc_line = -1;
    if (!d || !out) return 0;
    pthread_mutex_lock(&d->mtx);
    if (!d->live) {
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    get_regs(d, &r);
    while (n < max) {
        fill_line_locked(d, addr, &out[n], (uint16_t)r.pc);
        if (out[n].is_pc && pc_line && *pc_line < 0)
            *pc_line = n;
        addr = (uint16_t)(addr + out[n].length);
        n++;
    }
    pthread_mutex_unlock(&d->mtx);
    return n;
}

static int row_address_locked(apple2debug *d, uint16_t addr, int rows)
{
    apple2dasm_line ins;
    int back;

    if (!d->live || rows == 0)
        return addr;
    if (rows > 0) {
        int i;
        for (i = 0; i < rows; i++)
            addr = (uint16_t)(addr + decode_at(d, addr, &ins));
        return addr;
    }
    /* Backwards: the longest run of instructions that lands exactly on addr,
     * from as far back as the rows could take. */
    rows = -rows;
    for (back = rows * 3; back >= 1; back--) {
        uint16_t starts[256];
        int count = 0;
        uint16_t a = (uint16_t)(addr - back);
        while (count < 256) {
            int dist = (uint16_t)(addr - a);
            if (dist == 0 || dist > back)
                break;
            starts[count++] = a;
            a = (uint16_t)(a + decode_at(d, a, &ins));
        }
        if (a == addr && count >= rows)
            return starts[count - rows];
    }
    return (uint16_t)(addr - rows);
}

int apple2debug_row_address(apple2debug *d, uint16_t addr, int rows)
{
    int a;
    if (!d) return addr;
    pthread_mutex_lock(&d->mtx);
    a = row_address_locked(d, addr, rows);
    pthread_mutex_unlock(&d->mtx);
    return a;
}

/* ---- symbols and labels ---------------------------------------------------------- */

int apple2debug_symbols_load(apple2debug *d, const char *path, const char *table)
{
    int rc;
    pthread_mutex_lock(&d->mtx);
    rc = symtabs_load_file(&d->syms, path, table);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

const char *apple2debug_symbol_at(apple2debug *d, uint16_t addr, uint16_t *offset)
{
    return symtabs_at(&d->syms, addr, offset);
}

int apple2debug_symbol_find(apple2debug *d, const char *name, uint16_t *addr)
{
    return symtabs_find(&d->syms, name, addr);
}

int apple2debug_load_symbols(apple2debug *d, const char *path, char *msg,
                             int msgsz)
{
    int n;

    if (!d) return -1;
    if (msg && msgsz > 0) msg[0] = '\0';
    if (!path || !*path) {
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Choose a symbol file.");
        return -1;
    }
    pthread_mutex_lock(&d->mtx);
    n = symtabs_load_file(&d->syms, path, "file");
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    if (msg && msgsz > 0) {
        if (n < 0)
            snprintf(msg, (size_t)msgsz, "Cannot read %s", path);
        else
            snprintf(msg, (size_t)msgsz, "%d label%s from %s", n,
                     n == 1 ? "" : "s", path);
    }
    return n;
}

int apple2debug_label_address(apple2debug *d, const char *label)
{
    uint16_t a;
    int rc;
    if (!d || !label) return -1;
    pthread_mutex_lock(&d->mtx);
    rc = symtabs_find(&d->syms, label, &a) ? (int)a : -1;
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

int apple2debug_address_label(apple2debug *d, uint16_t addr, char *dst, int dstsz)
{
    if (!d || !dst || dstsz <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    label_for_locked(d, addr, dst, dstsz, 0);
    pthread_mutex_unlock(&d->mtx);
    return (int)strlen(dst);
}

int apple2debug_set_label(apple2debug *d, uint16_t addr, const char *label)
{
    if (!d) return -1;
    pthread_mutex_lock(&d->mtx);
    symtabs_set_user(&d->syms, addr, label);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return 0;
}

int apple2debug_label_completions(apple2debug *d, const char *word, char *dst,
                                 int dstsz, int maxn)
{
    size_t wl = strlen(word);
    const char *name;
    int i, count = 0, n = 0;

    if (!wl || dstsz <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&d->mtx);
    for (i = 0; (name = symtabs_name_at(&d->syms, i)) != NULL && count < maxn;
         i++) {
        size_t k;
        for (k = 0; k < wl && name[k]; k++) {
            char a = name[k], b = word[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (k == wl) {
            int w = snprintf(dst + n, (size_t)(dstsz - n), "%s\n", name);
            if (w < 0 || w >= dstsz - n) break;
            n += w;
            count++;
        }
    }
    pthread_mutex_unlock(&d->mtx);
    return count;
}

/* ---- breakpoints ----------------------------------------------------------------- */

int apple2debug_breakpoint_add(apple2debug *d, int type, uint16_t start,
                              uint16_t end, const char *cond, char *err,
                              int errsz)
{
    apple2debug_breakpoint *b;
    int id;

    if (err && errsz > 0) err[0] = '\0';
    if (!d) return -1;
    if (!(type & 7) || (type & ~7)) {
        set_err(err, errsz, "no such breakpoint type");
        return -1;
    }
    if (end < start) {
        uint16_t t = start;
        start = end;
        end = t;
    }
    pthread_mutex_lock(&d->mtx);
    if (cond && *cond) {
        long v;
        char why[96];
        d->in_access = 1;
        d->acc_value = d->acc_addr = 0;
        if (eval_locked(d, cond, &v, why, sizeof why) != 0 &&
            strcmp(why, "division by zero") != 0) {
            d->in_access = 0;
            pthread_mutex_unlock(&d->mtx);
            set_err(err, errsz, why);
            return -1;
        }
        d->in_access = 0;
    }
    if (d->nbp >= MAX_BP) {
        pthread_mutex_unlock(&d->mtx);
        set_err(err, errsz, "too many breakpoints");
        return -1;
    }
    b = &d->bp[d->nbp++];
    memset(b, 0, sizeof *b);
    b->id = id = d->next_id++;
    b->type = type;
    b->start = start;
    b->end = end;
    b->enabled = 1;
    snprintf(b->condition, sizeof b->condition, "%s", cond ? cond : "");
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return id;
}

void apple2debug_breakpoint_remove(apple2debug *d, int id)
{
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++) {
        if (d->bp[i].id == id) {
            memmove(&d->bp[i], &d->bp[i + 1],
                    (size_t)(d->nbp - i - 1) * sizeof d->bp[0]);
            d->nbp--;
            break;
        }
    }
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_breakpoint_enable(apple2debug *d, int id, int enabled)
{
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if (d->bp[i].id == id)
            d->bp[i].enabled = enabled != 0;
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int apple2debug_breakpoint_list(apple2debug *d, apple2debug_breakpoint *out, int max)
{
    int n;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    n = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bp, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&d->mtx);
    return n;
}

void apple2debug_breakpoint_clear_hits(apple2debug *d)
{
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        d->bp[i].hits = 0;
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_breakpoint_clear(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->nbp = 0;
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int apple2debug_breakpoint_check(apple2debug *d, uint16_t addr)
{
    int i, found = 0;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if ((d->bp[i].type & APPLE2DEBUG_BP_EXEC) && addr >= d->bp[i].start &&
            addr <= d->bp[i].end)
            found = 1;
    pthread_mutex_unlock(&d->mtx);
    return found;
}

int apple2debug_breakpoint_toggle(apple2debug *d, uint16_t addr)
{
    int i, found = -1;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if (d->bp[i].type == APPLE2DEBUG_BP_EXEC && d->bp[i].start == addr &&
            d->bp[i].end == addr) {
            found = d->bp[i].id;
            break;
        }
    pthread_mutex_unlock(&d->mtx);
    if (found >= 0) {
        apple2debug_breakpoint_remove(d, found);
        return 0;
    }
    return apple2debug_breakpoint_add(d, APPLE2DEBUG_BP_EXEC, addr, addr, NULL,
                                     NULL, 0) >= 0;
}

/* ---- trace ------------------------------------------------------------------------ */

void apple2debug_trace_enable(apple2debug *d, int on)
{
    if (!d) return;
    if (on) {
        pthread_mutex_lock(&d->trace_mtx);
        if (!d->ring)
            d->ring = calloc(TRACE_N, sizeof *d->ring);
        if (!d->ring)
            on = 0;
        pthread_mutex_unlock(&d->trace_mtx);
    }
    pthread_mutex_lock(&d->mtx);
    d->trace_on = on != 0;
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int apple2debug_trace_enabled(apple2debug *d)
{
    return d && d->trace_on;
}

int apple2debug_trace_read(apple2debug *d, apple2debug_trace *out, int max)
{
    int n, i;
    if (!d || !out) return 0;
    pthread_mutex_lock(&d->trace_mtx);
    n = !d->ring ? 0 : d->ring_count < max ? d->ring_count : max;
    for (i = 0; i < n; i++)
        out[i] = d->ring[(d->ring_head - 1 - i + TRACE_N) % TRACE_N];
    pthread_mutex_unlock(&d->trace_mtx);
    return n;
}

int apple2debug_trace_count(apple2debug *d)
{
    int n;
    if (!d) return 0;
    pthread_mutex_lock(&d->trace_mtx);
    n = d->ring_count;
    pthread_mutex_unlock(&d->trace_mtx);
    return n;
}

void apple2debug_trace_clear(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->trace_mtx);
    d->ring_head = d->ring_count = 0;
    pthread_mutex_unlock(&d->trace_mtx);
    apple2debug_touch(d);
}

int apple2debug_trace_format(const apple2debug_trace *t, char *dst, int dstsz)
{
    apple2dasm_line ins;
    uint8_t code[3];

    if (!t || !dst || dstsz <= 0) return 0;
    memcpy(code, t->bytes, 3);
    apple2dasm_decode(t->pc, code, &ins);
    return snprintf(dst, (size_t)dstsz,
                    "%04X  %-14s A=%02X X=%02X Y=%02X P=%02X SP=%02X",
                    t->pc, ins.text, t->a, t->x, t->y, t->p, t->sp & 0xFF);
}

int apple2debug_trace_match(const apple2debug_trace *t, int filter, uint16_t lo,
                            uint16_t hi)
{
    apple2dasm_line ins;
    uint8_t code[3];

    if (!t) return 0;
    if (filter == APPLE2DEBUG_TRACE_RANGE) {
        if (lo > hi) { uint16_t x = lo; lo = hi; hi = x; }
        return t->pc >= lo && t->pc <= hi;
    }
    if (filter == APPLE2DEBUG_TRACE_BRANCHES) {
        memcpy(code, t->bytes, 3);
        apple2dasm_decode(t->pc, code, &ins);
        return (ins.flags & (APPLE2DASM_JUMP | APPLE2DASM_CALL |
                             APPLE2DASM_RET)) != 0;
    }
    return 1;
}

/* ---- memory banks ---------------------------------------------------------------- */

static const struct { const char *name, *prefix; } bank_info[APPLE2_BANK_COUNT] = {
    { "As the CPU sees it", "" },
    { "Main RAM", "main" },
    { "Aux RAM", "aux" },
    { "Language card RAM bank 1 ($D000)", "lc1" },
    { "Language card RAM bank 2 ($D000)", "lc2" },
    { "Language card RAM ($E000)", "lchi" },
    { "System ROM", "rom" },
    { "Aux language card RAM bank 1 ($D000)", "alc1" },
    { "Aux language card RAM bank 2 ($D000)", "alc2" },
    { "Aux language card RAM ($E000)", "alchi" },
};

const char *apple2debug_bank_name(int bank)
{
    return bank >= 0 && bank < APPLE2_BANK_COUNT ? bank_info[bank].name : NULL;
}

const char *apple2debug_bank_prefix(int bank)
{
    return bank >= 0 && bank < APPLE2_BANK_COUNT ? bank_info[bank].prefix : NULL;
}

int apple2debug_bank_parse(const char *text, int *consumed)
{
    int b;
    size_t n;

    if (!text) return -1;
    while (*text == ' ' || *text == '\t') text++;
    for (b = APPLE2_BANK_MAIN; b < APPLE2_BANK_COUNT; b++) {
        size_t k;
        n = strlen(bank_info[b].prefix);
        for (k = 0; k < n && tolower((unsigned char)text[k]) == bank_info[b].prefix[k]; k++) ;
        if (k == n && text[n] == ':') {
            if (consumed) *consumed = (int)n + 1;
            return b;
        }
    }
    return -1;
}

/* The host store and offset behind (bank, addr); 0 when the bank does not
 * cover addr. */
static int bank_map(int bank, uint16_t addr, int *store, unsigned *off)
{
    switch (bank) {
    case APPLE2_BANK_MAIN: case APPLE2_BANK_AUX:
        if (addr >= 0xC000) return 0;
        *store = bank == APPLE2_BANK_MAIN ? APPLE2HOST_BANKSTORE_MAIN
                                          : APPLE2HOST_BANKSTORE_AUX;
        *off = addr;
        return 1;
    case APPLE2_BANK_LC1: case APPLE2_BANK_LC2: case APPLE2_BANK_LCHI:
    case APPLE2_BANK_AUXLC1: case APPLE2_BANK_AUXLC2: case APPLE2_BANK_AUXLCHI: {
        int hi = bank == APPLE2_BANK_LCHI || bank == APPLE2_BANK_AUXLCHI;
        int two = bank == APPLE2_BANK_LC2 || bank == APPLE2_BANK_AUXLC2;
        if (hi ? addr < 0xE000 : (addr < 0xD000 || addr >= 0xE000)) return 0;
        *store = bank >= APPLE2_BANK_AUXLC1 ? APPLE2HOST_BANKSTORE_LCAUX
                                            : APPLE2HOST_BANKSTORE_LCMAIN;
        *off = hi ? 0x2000u + (addr - 0xE000u)
                  : (two ? 0x1000u : 0u) + (addr - 0xD000u);
        return 1;
    }
    case APPLE2_BANK_ROM:
        if (addr < (apple2host_has_aux() ? 0xC100 : 0xD000)) return 0;
        *store = APPLE2HOST_BANKSTORE_ROM;
        *off = addr - 0xC000u;
        return 1;
    default:
        return 0;
    }
}

static int bank_present_locked(apple2debug *d, int bank)
{
    if (bank == APPLE2_BANK_CURRENT) return 1;
    if (!d->live) return 0;
    switch (bank) {
    case APPLE2_BANK_MAIN: return apple2host_bank_present(APPLE2HOST_BANKSTORE_MAIN);
    case APPLE2_BANK_AUX: return apple2host_bank_present(APPLE2HOST_BANKSTORE_AUX);
    case APPLE2_BANK_LC1: case APPLE2_BANK_LC2: case APPLE2_BANK_LCHI:
        return apple2host_bank_present(APPLE2HOST_BANKSTORE_LCMAIN);
    case APPLE2_BANK_AUXLC1: case APPLE2_BANK_AUXLC2: case APPLE2_BANK_AUXLCHI:
        return apple2host_bank_present(APPLE2HOST_BANKSTORE_LCAUX);
    case APPLE2_BANK_ROM: return apple2host_bank_present(APPLE2HOST_BANKSTORE_ROM);
    default: return 0;
    }
}

static int bank_read_locked(apple2debug *d, int bank, uint16_t addr,
                            uint8_t *dst, int n)
{
    int i, covered = 0;

    if (bank == APPLE2_BANK_CURRENT) {
        for (i = 0; i < n; i++) dst[i] = peek(d, (uint16_t)(addr + i));
        return n;
    }
    memset(dst, 0xFF, (size_t)n);
    if (!bank_present_locked(d, bank)) return 0;
    for (i = 0; i < n; i++) {
        int store;
        unsigned off;
        if (bank_map(bank, (uint16_t)(addr + i), &store, &off) &&
            apple2host_bank_read(store, off, &dst[i], 1) == 1)
            covered++;
    }
    return covered ? n : 0;
}

int apple2debug_bank_valid(apple2debug *d, int bank)
{
    int v;
    if (!d || bank < 0 || bank >= APPLE2_BANK_COUNT) return 0;
    pthread_mutex_lock(&d->mtx);
    v = bank_present_locked(d, bank);
    pthread_mutex_unlock(&d->mtx);
    return v;
}

int apple2debug_bank_range(apple2debug *d, int bank, uint16_t *lo, uint16_t *hi)
{
    uint16_t l = 0, h = 0xFFFF;
    if (!d || bank < 0 || bank >= APPLE2_BANK_COUNT) return 0;
    switch (bank) {
    case APPLE2_BANK_MAIN: case APPLE2_BANK_AUX: h = 0xBFFF; break;
    case APPLE2_BANK_LC1: case APPLE2_BANK_LC2:
    case APPLE2_BANK_AUXLC1: case APPLE2_BANK_AUXLC2: l = 0xD000; h = 0xDFFF; break;
    case APPLE2_BANK_LCHI: case APPLE2_BANK_AUXLCHI: l = 0xE000; break;
    case APPLE2_BANK_ROM: l = apple2host_has_aux() ? 0xC100 : 0xD000; break;
    default: break;
    }
    if (lo) *lo = l;
    if (hi) *hi = h;
    return apple2debug_bank_valid(d, bank);
}

int apple2debug_read_bank(apple2debug *d, int bank, uint16_t addr, uint8_t *dst, int n)
{
    int r;
    if (!d || !dst || n <= 0 || bank < 0 || bank >= APPLE2_BANK_COUNT) return 0;
    pthread_mutex_lock(&d->mtx);
    if (!d->live) {
        memset(dst, 0, (size_t)n);
        r = 0;
    } else {
        r = bank_read_locked(d, bank, addr, dst, n);
    }
    pthread_mutex_unlock(&d->mtx);
    return r;
}

void apple2debug_write_bank(apple2debug *d, int bank, uint16_t addr, uint8_t value)
{
    int store;
    unsigned off;

    if (!d || bank < 0 || bank >= APPLE2_BANK_COUNT) return;
    if (bank == APPLE2_BANK_CURRENT) {
        apple2debug_write(d, addr, value);
        return;
    }
    if (bank == APPLE2_BANK_ROM) return;
    pthread_mutex_lock(&d->mtx);
    if (d->live && bank_present_locked(d, bank) &&
        bank_map(bank, addr, &store, &off)) {
        apple2host_bank_write(store, off, &value, 1);
        bump_locked(d);
    }
    pthread_mutex_unlock(&d->mtx);
}

int apple2debug_search_bank(apple2debug *d, int bank, const uint8_t *pat, int n,
                            uint8_t mask, int flags, uint16_t from, uint16_t to,
                            uint16_t *out, int max)
{
    uint16_t lo, hi;
    uint8_t *buf;
    unsigned a, first, last;
    int found = 0, k;

    if (bank == APPLE2_BANK_CURRENT)
        return apple2debug_search(d, pat, n, mask, flags, from, to, out, max);
    if (!d || !pat || n <= 0 || !out || max <= 0 || bank < 0 ||
        bank >= APPLE2_BANK_COUNT) return -1;
    pthread_mutex_lock(&d->mtx);
    if (!d->live || !d->stopped) {
        pthread_mutex_unlock(&d->mtx);
        return -1;
    }
    if (!bank_present_locked(d, bank)) {
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    pthread_mutex_unlock(&d->mtx);
    apple2debug_bank_range(d, bank, &lo, &hi);
    if (from > to) { uint16_t x = from; from = to; to = x; }
    first = from < lo ? lo : from;
    last = to > hi ? hi : to;
    if (first > last) return 0;
    buf = malloc(0x10000);
    if (!buf) return -1;
    pthread_mutex_lock(&d->mtx);
    bank_read_locked(d, bank, (uint16_t)first, buf, (int)(last - first + 1));
    pthread_mutex_unlock(&d->mtx);
    for (a = first; a <= last && a + (unsigned)n - 1 <= last && found < max; a++) {
        for (k = 0; k < n; k++) {
            uint8_t b = buf[a - first + (unsigned)k];
            uint8_t p = pat[k];
            if (flags & APPLE2DEBUG_SEARCH_NOCASE) {
                if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 32);
                if (p >= 'a' && p <= 'z') p = (uint8_t)(p - 32);
            }
            if ((b & mask) != (p & mask)) break;
        }
        if (k == n) out[found++] = (uint16_t)a;
    }
    free(buf);
    return found;
}

int apple2debug_disassemble_bank(apple2debug *d, int bank, uint16_t addr,
                                 apple2debug_line *out, int max, int *pc_line)
{
    int n = 0;
    regs_t r;

    if (bank == APPLE2_BANK_CURRENT)
        return apple2debug_disassemble(d, addr, out, max, pc_line);
    if (pc_line) *pc_line = -1;
    if (!d || !out || bank < 0 || bank >= APPLE2_BANK_COUNT) return 0;
    pthread_mutex_lock(&d->mtx);
    if (!d->live || !bank_present_locked(d, bank)) {
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    get_regs(d, &r);
    while (n < max) {
        fill_line_bank_locked(d, bank, addr, &out[n], (uint16_t)r.pc);
        addr = (uint16_t)(addr + out[n].length);
        n++;
    }
    pthread_mutex_unlock(&d->mtx);
    return n;
}

/* ---- memory search ------------------------------------------------------------ */

int apple2debug_search(apple2debug *d, const uint8_t *pat, int n, uint8_t mask,
                      int flags, uint16_t from, uint16_t to, uint16_t *out,
                      int max)
{
    unsigned a, last;
    int found = 0, k;

    if (!d || !pat || n <= 0 || !out || max <= 0) return -1;
    pthread_mutex_lock(&d->mtx);
    if (!d->live || !d->stopped) {
        pthread_mutex_unlock(&d->mtx);
        return -1;
    }
    if (from > to) { uint16_t x = from; from = to; to = x; }
    last = to;
    for (a = from; a <= last && a + (unsigned)n - 1 <= 0xFFFF && found < max;
         a++) {
        for (k = 0; k < n; k++) {
            uint8_t b = peek(d, (uint16_t)(a + (unsigned)k));
            uint8_t p = pat[k];
            if (flags & APPLE2DEBUG_SEARCH_NOCASE) {
                if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 32);
                if (p >= 'a' && p <= 'z') p = (uint8_t)(p - 32);
            }
            if ((b & mask) != (p & mask))
                break;
        }
        if (k == n)
            out[found++] = (uint16_t)a;
    }
    pthread_mutex_unlock(&d->mtx);
    return found;
}

int apple2debug_search_next(apple2debug *d, const uint8_t *pat, int n,
                           uint8_t mask, int flags, uint16_t from,
                           int backward, uint16_t *found)
{
    uint16_t hit[1];
    int r;

    if (!found) return -1;
    if (!backward) {
        unsigned start = (unsigned)from + 1;
        if (start <= 0xFFFF) {
            r = apple2debug_search(d, pat, n, mask, flags, (uint16_t)start,
                                  0xFFFF, hit, 1);
            if (r < 0) return -1;
            if (r > 0) { *found = hit[0]; return 1; }
        }
        r = apple2debug_search(d, pat, n, mask, flags, 0, from, hit, 1);
        if (r <= 0) return r;
        *found = hit[0];
        return 1;
    } else {
        uint16_t last = 0;
        int have = 0;
        uint16_t buf[64];
        unsigned lo = 0;
        /* the last match strictly before `from`, else the last overall */
        for (int pass = 0; pass < 2 && !have; pass++) {
            unsigned a = pass == 0 ? 0 : (unsigned)from;
            unsigned end = pass == 0 ? (from ? (unsigned)from - 1 : 0)
                                     : 0xFFFF;
            if (pass == 0 && from == 0) continue;
            lo = a;
            while (lo <= end) {
                r = apple2debug_search(d, pat, n, mask, flags, (uint16_t)lo,
                                      (uint16_t)end, buf, 64);
                if (r < 0) return -1;
                if (r == 0) break;
                last = buf[r - 1];
                have = 1;
                if (r < 64) break;
                lo = (unsigned)buf[r - 1] + 1;
            }
        }
        if (!have) return 0;
        *found = last;
        return 1;
    }
}

/* ---- files ------------------------------------------------------------------------ */

static int write_bytes(const char *path, const uint8_t *data, size_t n,
                       char *msg, int msgsz)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n) {
        if (f) fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
        return -1;
    }
    fclose(f);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Saved %zu bytes to %s", n, path);
    return 0;
}

static void not_running(char *msg, int msgsz)
{
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "The machine is not running.");
}

int apple2debug_save(apple2debug *d, const char *kind, const char *path,
                     char *msg, int msgsz)
{
    FILE *f;
    int live;

    if (!d || !kind || !path) {
        not_running(msg, msgsz);
        return -1;
    }
    if (!strcmp(kind, "mem")) {
        uint8_t *buf = malloc(0x10000);
        int rc;

        if (!buf) return -1;
        pthread_mutex_lock(&d->mtx);
        live = d->live;
        if (live) {
            uint32_t a;
            for (a = 0; a < 0x10000; a++)
                buf[a] = peek(d, (uint16_t)a);
        }
        pthread_mutex_unlock(&d->mtx);
        if (!live) {
            free(buf);
            not_running(msg, msgsz);
            return -1;
        }
        rc = write_bytes(path, buf, 0x10000, msg, msgsz);
        free(buf);
        return rc;
    }
    if (!strcmp(kind, "dis")) {
        uint32_t a = 0;

        pthread_mutex_lock(&d->mtx);
        live = d->live;
        f = live ? fopen(path, "w") : NULL;
        if (f) {
            regs_t r;
            get_regs(d, &r);
            while (a < 0x10000) {
                apple2debug_line l;
                fill_line_locked(d, (uint16_t)a, &l, (uint16_t)r.pc);
                if (l.label[0])
                    fprintf(f, "%s:\n", l.label);
                fprintf(f, "  %04X  %-9s %-18s%s%s\n", l.address, l.bytes,
                        l.disasm, l.comment[0] ? "; " : "", l.comment);
                a += (uint32_t)l.length;
            }
            fclose(f);
        }
        pthread_mutex_unlock(&d->mtx);
        if (!live) {
            not_running(msg, msgsz);
            return -1;
        }
        if (!f) {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
            return -1;
        }
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (!strcmp(kind, "regs")) {
        apple2debug_cpu cp;
        char text[2048];

        apple2debug_cpu_get(d, &cp);
        pthread_mutex_lock(&d->mtx);
        live = d->live;
        pthread_mutex_unlock(&d->mtx);
        if (!live) {
            not_running(msg, msgsz);
            return -1;
        }
        f = fopen(path, "w");
        if (!f) {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
            return -1;
        }
        fprintf(f, "PC=%04X SP=%04X A=%02X X=%02X Y=%02X P=%02X (%s)\n",
                cp.pc, cp.sp, cp.a, cp.x, cp.y, cp.p,
                cp.is_65c02 ? "65C02" : "6502");
        fprintf(f, "cycles=%llu frame=%u line=%d horz=%d%s\n",
                (unsigned long long)cp.cycles, cp.frame, cp.line, cp.horz,
                cp.jammed ? " JAMMED" : "");
        apple2debug_io_state(d, text, sizeof text);
        fprintf(f, "\n%s", text);
        fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Unknown save kind %s (dis, mem, regs)",
                 kind);
    return -1;
}

/* ---- FujiNet ----------------------------------------------------------------
 * The trace belongs to the session (it runs from session start); these read
 * it. The rest of the state is the card's slot and the Listener's count. */

int apple2debug_fujinet_trace(apple2debug *d, apple2_fuji_packet *out, int max)
{
    return d ? sptrace_copy(d->s->sptrace, out, max) : 0;
}

void apple2debug_fujinet_trace_clear(apple2debug *d)
{
    if (d) sptrace_clear(d->s->sptrace);
}

int apple2debug_fujinet_state(apple2debug *d, char *dst, int dstsz)
{
    apple2session *s;
    int slot, len;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!d) return 0;
    s = d->s;
    slot = apple2session_fujinet_slot(s);
    if (slot)
        len = snprintf(dst, (size_t)dstsz,
                       "Card       slot %d\n"
                       "Listener   %s (SLIP)\n",
                       slot, apple2session_slip_endpoint(s));
    else
        len = snprintf(dst, (size_t)dstsz, "Card       none\n");
    if (len < 0 || len >= dstsz) return (int)strlen(dst);
    len += snprintf(dst + len, (size_t)(dstsz - len),
                    "FujiNet    %s\n"
                    "Devices    %d registered\n"
                    "Link       %s\n\n",
                    apple2session_fujinet_external(s) ? "external"
                    : apple2session_fujinet_running(s) ? "running"
                                                       : "not running",
                    s->running ? apple2host_fujinet_devices() : 0,
                    apple2session_fujinet_link(s) ? "connected"
                                                  : "not connected");
    if (len < 0 || len >= dstsz) return (int)strlen(dst);
    return len + sptrace_describe(s->sptrace, dst + len, dstsz - len);
}

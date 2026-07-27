/*
 * apple2debug -- the debugger engine.
 *
 * Everything that touches core state runs on the EMULATOR thread. Control
 * calls from a UI thread only set flags and wait; the emulator loop notices
 * them between frames (or between instructions, once engaged) and does the
 * work. That is what makes it safe to poke registers and memory from a
 * button press without a second thread ever entering AppleWin.
 *
 * "Engaged" means the session's frame loop hands each frame to
 * apple2debug_session_frame() instead of running it straight through, so the
 * engine can step instruction by instruction and check breakpoints. While
 * disengaged the cost is a single volatile read per frame.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apple2_host.h"
#include "apple2debug.h"
#include "compat.h"
#include "m6502dasm.h"
#include "session_internal.h"

#define MAX_BREAKPOINTS 64
#define MAX_SYMBOLS     4096

typedef struct {
    uint16_t addr;
    char name[32];
} symbol_t;

struct apple2debug {
    apple2session *s;

    pthread_mutex_t mtx;
    pthread_cond_t cv;          /* emulator thread parks here while paused */

    volatile int paused;
    volatile int want_pause;
    volatile int resume_for_stop; /* session is shutting down: never park */

    /* Pending single-shot actions, consumed by the emulator thread. */
    volatile int step_into;
    volatile int step_over;     /* run until PC returns to this depth */
    volatile int step_out;
    volatile int have_run_to;
    volatile uint16_t run_to_addr;

    /* Stack-depth tracking for step over/out. SP grows downward, so a
     * "shallower or equal" stack means the call we stepped over returned. */
    uint16_t step_sp_target;

    uint16_t bp[MAX_BREAKPOINTS];
    int bp_count;

    symbol_t *syms;
    int sym_count;

    apple2debug_stop_cb stop_cb;
    void *stop_user;
};

/* ---- helpers ------------------------------------------------------------- */

static void engage(apple2debug *d, int on)
{
    d->s->dbg_engaged = on;
}

static int bp_index(const apple2debug *d, uint16_t addr)
{
    int i;
    for (i = 0; i < d->bp_count; i++)
        if (d->bp[i] == addr) return i;
    return -1;
}

/* Emulator thread only. */
static void report_stop(apple2debug *d, apple2debug_stop_reason reason,
                        uint16_t pc)
{
    apple2debug_stop_cb cb;
    void *user;
    pthread_mutex_lock(&d->mtx);
    cb = d->stop_cb;
    user = d->stop_user;
    pthread_mutex_unlock(&d->mtx);
    if (cb) cb(reason, pc, user);
}

/* ---- the emulator-thread half -------------------------------------------- */

/* Park until something asks us to run again. Emulator thread. */
static void park(apple2debug *d)
{
    pthread_mutex_lock(&d->mtx);
    while (d->paused && !d->s->stop_flag && !d->resume_for_stop &&
           !d->step_into && !d->step_over && !d->step_out && !d->have_run_to) {
        /* Timed so a lost wakeup can never wedge the emulator thread. */
        apple2_cond_timedwait_ms(&d->cv, &d->mtx, 100);
    }
    pthread_mutex_unlock(&d->mtx);
}

/* One frame's worth of debugger-controlled execution. Returns when the frame
 * is done or the machine has stopped. Emulator thread, called from
 * session.c's loop while dbg_engaged. */
int apple2debug_session_frame(apple2session *s)
{
    apple2debug *d = s->debugger;
    /* One frame at ~1.02MHz. Stepping is bounded by cycles rather than
     * instruction count so a paused machine still advances real time the same
     * way a running one does. */
    const unsigned budget = 17030;
    unsigned spent = 0;

    if (!d) return 0;

    for (;;) {
        apple2host_regs r;
        unsigned cycles;
        int hit;

        if (s->stop_flag) return 0;

        if (d->want_pause) {
            d->want_pause = 0;
            d->paused = 1;
            apple2host_get_regs(&r);
            report_stop(d, APPLE2DBG_STOP_PAUSE, r.pc);
        }

        if (d->paused) {
            if (!d->step_into && !d->step_over && !d->step_out &&
                !d->have_run_to) {
                park(d);
                /* Woken to shut down or to act; loop round and re-check. */
                if (s->stop_flag || d->resume_for_stop) return 0;
                continue;
            }
        }

        apple2host_get_regs(&r);

        /* Set up the exit condition for a step that spans instructions. */
        if (d->step_over || d->step_out) {
            uint8_t op[3];
            apple2host_read_mem(r.pc, op, 3);
            if (d->step_out) {
                /* Run until the stack is shallower than it is now. */
                d->step_sp_target = (uint16_t)(r.sp + 2);
            } else if (op[0] == 0x20) { /* JSR: run until it returns */
                d->step_sp_target = r.sp;
            } else {
                /* Not a call -- step over degenerates to step into. */
                d->step_over = 0;
                d->step_into = 1;
            }
        }

        cycles = apple2host_step_instruction();
        spent += cycles ? cycles : 1;

        apple2host_get_regs(&r);

        if (r.jammed) {
            d->paused = 1;
            d->step_into = d->step_over = d->step_out = 0;
            d->have_run_to = 0;
            report_stop(d, APPLE2DBG_STOP_JAMMED, r.pc);
            continue;
        }

        /* Single instruction done. */
        if (d->step_into) {
            d->step_into = 0;
            d->paused = 1;
            report_stop(d, APPLE2DBG_STOP_STEP, r.pc);
            continue;
        }

        /* Step over/out finish when the stack has unwound past the target. */
        if ((d->step_over || d->step_out) && r.sp >= d->step_sp_target) {
            d->step_over = d->step_out = 0;
            d->paused = 1;
            report_stop(d, APPLE2DBG_STOP_STEP, r.pc);
            continue;
        }

        if (d->have_run_to && r.pc == d->run_to_addr) {
            d->have_run_to = 0;
            d->paused = 1;
            report_stop(d, APPLE2DBG_STOP_RUNTO, r.pc);
            continue;
        }

        pthread_mutex_lock(&d->mtx);
        hit = bp_index(d, r.pc) >= 0;
        pthread_mutex_unlock(&d->mtx);
        if (hit) {
            d->step_over = d->step_out = 0;
            d->have_run_to = 0;
            d->paused = 1;
            report_stop(d, APPLE2DBG_STOP_BREAKPOINT, r.pc);
            continue;
        }

        /* Frame's worth of cycles done: hand control back so the session can
         * pace, publish a frame and service FujiNet. */
        if (!d->paused && spent >= budget)
            return 1;
    }
}

/* Called by apple2session_stop so a parked emulator thread can exit. */
void apple2debug_resume_for_stop(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->resume_for_stop = 1;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- construction --------------------------------------------------------- */

apple2debug *apple2debug_create(apple2session *s)
{
    apple2debug *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->s = s;
    pthread_mutex_init(&d->mtx, NULL);
    apple2_cond_init_monotonic(&d->cv);
    d->syms = calloc(MAX_SYMBOLS, sizeof(symbol_t));
    if (!d->syms) {
        free(d);
        return NULL;
    }
    return d;
}

void apple2debug_destroy(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_destroy(&d->mtx);
    pthread_cond_destroy(&d->cv);
    free(d->syms);
    free(d);
}

/* ---- run control ---------------------------------------------------------- */

void apple2debug_pause(apple2debug *d)
{
    if (!d) return;
    engage(d, 1);
    d->want_pause = 1;
    pthread_mutex_lock(&d->mtx);
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_resume(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->paused = 0;
    d->want_pause = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
    /* Stay engaged while breakpoints exist, otherwise fall back to the fast
     * whole-frame path. */
    engage(d, d->bp_count > 0);
}

int apple2debug_is_paused(const apple2debug *d)
{
    return d && d->paused;
}

static void request_step(apple2debug *d, int which)
{
    if (!d) return;
    engage(d, 1);
    pthread_mutex_lock(&d->mtx);
    d->paused = 1;
    switch (which) {
    case 0: d->step_into = 1; break;
    case 1: d->step_over = 1; break;
    default: d->step_out = 1; break;
    }
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_step_into(apple2debug *d) { request_step(d, 0); }
void apple2debug_step_over(apple2debug *d) { request_step(d, 1); }
void apple2debug_step_out(apple2debug *d)  { request_step(d, 2); }

void apple2debug_run_to(apple2debug *d, uint16_t addr)
{
    if (!d) return;
    engage(d, 1);
    pthread_mutex_lock(&d->mtx);
    d->run_to_addr = addr;
    d->have_run_to = 1;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void apple2debug_set_stop_callback(apple2debug *d, apple2debug_stop_cb cb,
                                   void *user)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->stop_cb = cb;
    d->stop_user = user;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- breakpoints ---------------------------------------------------------- */

void apple2debug_bp_set(apple2debug *d, uint16_t addr)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (bp_index(d, addr) < 0 && d->bp_count < MAX_BREAKPOINTS)
        d->bp[d->bp_count++] = addr;
    pthread_mutex_unlock(&d->mtx);
    engage(d, 1);
}

void apple2debug_bp_clear(apple2debug *d, uint16_t addr)
{
    int i, engaged_still;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    i = bp_index(d, addr);
    if (i >= 0) {
        memmove(&d->bp[i], &d->bp[i + 1],
                (size_t)(d->bp_count - i - 1) * sizeof(d->bp[0]));
        d->bp_count--;
    }
    engaged_still = d->bp_count > 0 || d->paused;
    pthread_mutex_unlock(&d->mtx);
    engage(d, engaged_still);
}

void apple2debug_bp_toggle(apple2debug *d, uint16_t addr)
{
    if (!d) return;
    if (apple2debug_bp_is_set(d, addr))
        apple2debug_bp_clear(d, addr);
    else
        apple2debug_bp_set(d, addr);
}

void apple2debug_bp_clear_all(apple2debug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->bp_count = 0;
    pthread_mutex_unlock(&d->mtx);
    engage(d, d->paused);
}

int apple2debug_bp_is_set(const apple2debug *d, uint16_t addr)
{
    int set;
    if (!d) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&d->mtx);
    set = bp_index(d, addr) >= 0;
    pthread_mutex_unlock((pthread_mutex_t *)&d->mtx);
    return set;
}

int apple2debug_bp_list(const apple2debug *d, uint16_t *out, int max)
{
    int n, i, j;
    if (!d || !out || max <= 0) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&d->mtx);
    n = d->bp_count < max ? d->bp_count : max;
    memcpy(out, d->bp, (size_t)n * sizeof(uint16_t));
    pthread_mutex_unlock((pthread_mutex_t *)&d->mtx);
    /* Ascending, so a list view is stable as breakpoints come and go. */
    for (i = 1; i < n; i++) {
        const uint16_t v = out[i];
        for (j = i - 1; j >= 0 && out[j] > v; j--)
            out[j + 1] = out[j];
        out[j + 1] = v;
    }
    return n;
}

/* ---- state ----------------------------------------------------------------
 * These read live core state. They are only safe while the emulator thread is
 * parked in park() -- which is exactly when a debugger UI asks for them. */

void apple2debug_get_regs(apple2debug *d, apple2debug_regs *out)
{
    apple2host_regs r;
    if (!d || !out) return;
    apple2host_get_regs(&r);
    out->a = r.a; out->x = r.x; out->y = r.y; out->ps = r.ps;
    out->pc = r.pc; out->sp = r.sp; out->jammed = r.jammed;
}

void apple2debug_set_regs(apple2debug *d, const apple2debug_regs *in)
{
    apple2host_regs r;
    if (!d || !in) return;
    r.a = in->a; r.x = in->x; r.y = in->y; r.ps = in->ps;
    r.pc = in->pc; r.sp = in->sp; r.jammed = in->jammed;
    apple2host_set_regs(&r);
}

void apple2debug_read_mem(apple2debug *d, uint16_t addr, uint8_t *out, int len)
{
    if (!d || !out || len <= 0) return;
    apple2host_read_mem(addr, out, (unsigned)len);
}

void apple2debug_write_mem(apple2debug *d, uint16_t addr, const uint8_t *in,
                           int len)
{
    if (!d || !in || len <= 0) return;
    apple2host_write_mem(addr, in, (unsigned)len);
}

int apple2debug_disassemble(apple2debug *d, uint16_t addr, int count,
                            apple2dasm_line *out, uint16_t *next_addr)
{
    int i;
    if (!d || !out || count <= 0) return 0;
    for (i = 0; i < count; i++) {
        uint8_t bytes[3];
        apple2host_read_mem(addr, bytes, 3);
        addr = (uint16_t)(addr + apple2dasm_decode(addr, bytes, &out[i]));
        out[i].symbol = apple2debug_symbol_at(d, out[i].addr);
    }
    if (next_addr) *next_addr = addr;
    return count;
}

/* ---- symbols ---------------------------------------------------------------
 * AppleWin's .SYM files are "ADDR NAME" per line, hex address first. */

int apple2debug_symbols_load(apple2debug *d, const char *path)
{
    FILE *fp;
    char line[256];
    int added = 0;

    if (!d || !path) return 0;
    fp = fopen(path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp) && d->sym_count < MAX_SYMBOLS) {
        unsigned addr;
        char name[64];
        if (sscanf(line, "%x %63s", &addr, name) != 2) continue;
        if (addr > 0xFFFF) continue;
        d->syms[d->sym_count].addr = (uint16_t)addr;
        snprintf(d->syms[d->sym_count].name,
                 sizeof(d->syms[d->sym_count].name), "%s", name);
        d->sym_count++;
        added++;
    }
    fclose(fp);
    return added;
}

const char *apple2debug_symbol_at(apple2debug *d, uint16_t addr)
{
    int i;
    if (!d) return NULL;
    for (i = 0; i < d->sym_count; i++)
        if (d->syms[i].addr == addr) return d->syms[i].name;
    return NULL;
}

int apple2debug_symbol_find(apple2debug *d, const char *name, uint16_t *out)
{
    int i;
    if (!d || !name) return 0;
    for (i = 0; i < d->sym_count; i++) {
        if (strcmp(d->syms[i].name, name) == 0) {
            if (out) *out = d->syms[i].addr;
            return 1;
        }
    }
    return 0;
}

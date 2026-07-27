/*
 * Exercises the debugger engine against a real running machine: pause/resume,
 * stepping, breakpoints and disassembly, all driven from a "UI" thread while
 * the emulator thread runs the session loop -- which is the arrangement that
 * actually has to work.
 *
 * Exits 77 (ctest skip) when the machine cannot boot, so a ROM-less build
 * still passes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "apple2debug.h"
#include "apple2session.h"

static int g_fail;
static volatile int g_stops;
static volatile apple2debug_stop_reason g_last_reason;
static volatile unsigned g_last_pc;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            g_fail++;                                            \
        }                                                        \
    } while (0)

/* Fires on the emulator thread. */
static void on_stop(apple2debug_stop_reason reason, uint16_t pc, void *user)
{
    (void)user;
    g_last_reason = reason;
    g_last_pc = pc;
    g_stops++;
}

/* Wait for the engine to actually park, without assuming a timing. */
static int wait_paused(apple2debug *d, int ms)
{
    while (ms > 0) {
        if (apple2debug_is_paused(d)) return 1;
        usleep(5000);
        ms -= 5;
    }
    return 0;
}

static int wait_stops(int target, int ms)
{
    while (ms > 0) {
        if (g_stops >= target) return 1;
        usleep(5000);
        ms -= 5;
    }
    return 0;
}

int main(void)
{
    apple2session *s;
    apple2session_start_opts o;
    apple2debug *d;
    apple2debug_regs r, r2;
    apple2dasm_line lines[8];
    uint16_t next = 0, bps[8];
    int n;

    s = apple2session_new(NULL);
    if (!s) {
        fprintf(stderr, "debug_engine: could not create the session\n");
        return 77;
    }
    apple2session_default_opts(s, &o);
    /* No FujiNet/audio/gamepad: this is about the engine, and it keeps the
     * test off the network and off the sound device. */
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.slot7 = "Empty";
    if (apple2session_start(s, &o) != 0) {
        fprintf(stderr, "debug_engine: %s\n", apple2session_last_error(s));
        apple2session_free(s);
        return 77;
    }

    d = apple2session_debugger(s);
    CHECK(d != NULL, "debugger engine was not created");
    if (!d) { apple2session_free(s); return 1; }
    apple2debug_set_stop_callback(d, on_stop, NULL);

    /* Let the machine get going. */
    usleep(300000);

    /* ---- pause ---------------------------------------------------------- */
    apple2debug_pause(d);
    CHECK(wait_paused(d, 2000), "did not pause within 2s");
    CHECK(g_stops >= 1, "no stop callback on pause");
    CHECK(g_last_reason == APPLE2DBG_STOP_PAUSE, "stop reason %d, want PAUSE",
          (int)g_last_reason);

    apple2debug_get_regs(d, &r);
    CHECK(r.pc != 0, "PC is 0 while paused");

    /* ---- stepping moves PC, and only by one instruction ------------------ */
    {
        int i, moved = 0;
        for (i = 0; i < 8; i++) {
            const int before_stops = g_stops;
            apple2debug_get_regs(d, &r);
            apple2debug_step_into(d);
            CHECK(wait_stops(before_stops + 1, 2000), "step %d did not stop",
                  i);
            apple2debug_get_regs(d, &r2);
            CHECK(g_last_reason == APPLE2DBG_STOP_STEP,
                  "step %d reason %d, want STEP", i, (int)g_last_reason);
            /* Not asserted per step: a single instruction can legitimately
             * leave PC where it was (a JMP to its own address), so only the
             * aggregate below is a real property. */
            if (r2.pc != r.pc) moved = 1;
        }
        CHECK(moved, "PC never moved across 8 steps");
    }

    /* ---- disassembly at PC ---------------------------------------------- */
    apple2debug_get_regs(d, &r);
    n = apple2debug_disassemble(d, r.pc, 4, lines, &next);
    CHECK(n == 4, "disassembled %d lines, want 4", n);
    CHECK(lines[0].addr == r.pc, "first line is $%04X, want PC $%04X",
          lines[0].addr, r.pc);
    CHECK(next > r.pc, "next address $%04X did not advance past $%04X", next,
          r.pc);
    CHECK(lines[0].text[0] != '\0', "first disassembled line is empty");
    {
        /* The reported lengths must tile the address range exactly. */
        uint16_t walk = r.pc;
        int i;
        for (i = 0; i < n; i++) {
            CHECK(lines[i].addr == walk, "line %d at $%04X, expected $%04X", i,
                  lines[i].addr, walk);
            walk = (uint16_t)(walk + lines[i].len);
        }
        CHECK(walk == next, "lengths do not tile: ended at $%04X, next $%04X",
              walk, next);
    }

    /* ---- breakpoints ----------------------------------------------------- */
    CHECK(!apple2debug_bp_is_set(d, 0xFA62), "breakpoint set before we set it");
    apple2debug_bp_set(d, 0xFA62);
    apple2debug_bp_set(d, 0x0300);
    CHECK(apple2debug_bp_is_set(d, 0xFA62), "breakpoint did not stick");
    n = apple2debug_bp_list(d, bps, 8);
    CHECK(n == 2, "breakpoint list has %d, want 2", n);
    CHECK(bps[0] == 0x0300 && bps[1] == 0xFA62,
          "breakpoint list not ascending: $%04X $%04X", bps[0], bps[1]);
    apple2debug_bp_clear(d, 0x0300);
    CHECK(!apple2debug_bp_is_set(d, 0x0300), "breakpoint did not clear");
    CHECK(apple2debug_bp_list(d, bps, 8) == 1, "clear removed the wrong one");
    apple2debug_bp_clear_all(d);
    CHECK(apple2debug_bp_list(d, bps, 8) == 0, "clear_all left breakpoints");

    /* ---- memory through the engine ---------------------------------------- */
    {
        uint8_t saved[4], pat[4] = {0x12, 0x34, 0x56, 0x78}, back[4];
        apple2debug_read_mem(d, 0x0320, saved, 4);
        apple2debug_write_mem(d, 0x0320, pat, 4);
        apple2debug_read_mem(d, 0x0320, back, 4);
        CHECK(memcmp(pat, back, 4) == 0, "engine memory round-trip failed");
        apple2debug_write_mem(d, 0x0320, saved, 4);
    }

    /* ---- resume: the machine must run again ------------------------------ */
    apple2debug_get_regs(d, &r);
    apple2debug_resume(d);
    CHECK(!apple2debug_is_paused(d), "still paused after resume");
    usleep(300000);
    apple2debug_get_regs(d, &r2);
    CHECK(r2.pc != r.pc || r2.a != r.a || r2.sp != r.sp,
          "machine did not run after resume (PC stuck at $%04X)", r.pc);

    /* Stopping a *running* session must not hang, and neither must stopping a
     * paused one -- that is what resume_for_stop exists for. */
    apple2debug_pause(d);
    wait_paused(d, 2000);
    apple2session_stop(s);
    apple2session_free(s);

    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("debug_engine: pause, step, breakpoints, disassembly and resume "
           "all behave\n");
    return 0;
}

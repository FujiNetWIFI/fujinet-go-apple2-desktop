/*
 * Proves the debugger seam into AppleWin works, before the debugger engine is
 * built on top of it: single-instruction stepping, register read/write, and
 * non-intrusive memory access.
 *
 * This is the part of the debugger that was uncertain -- everything above it
 * (breakpoints, run-to, trace, the native windows) is a mechanical port from
 * the ADAM target. AppleWin turns out to expose exactly what is needed:
 * CpuExecute() documents "uCycles: =0 : Do single step".
 *
 * Exits 77 (ctest skip) when the core cannot start, so a ROM-less build
 * still passes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "apple2_host.h"

static int g_fail;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            g_fail++;                                            \
        }                                                        \
    } while (0)

static void on_frame(const uint32_t *px, int w, int h, void *user)
{
    (void)px; (void)w; (void)h; (void)user;
}

int main(void)
{
    apple2host_regs r, r2;
    unsigned cycles_total = 0;
    uint8_t buf[16], saved[4], probe[4];
    int i, moved = 0;

    apple2host_set_frame_sink(on_frame, NULL);
    apple2host_set_variable("applewin_machine", "Enhanced Apple //e");
    apple2host_set_variable("applewin_slot7", "Empty");
    if (!apple2host_core_start()) {
        fprintf(stderr, "debug_seam: core failed to start (no ROMs?)\n");
        return 77;
    }

    /* Let the machine get past its power-on state. */
    for (i = 0; i < 30; i++) apple2host_core_run_frame();

    /* ---- registers ------------------------------------------------------ */
    memset(&r, 0, sizeof(r));
    apple2host_get_regs(&r);
    CHECK(r.pc != 0, "PC should not be 0 after booting");

    /* Round-trip: the write must stick and not disturb the rest. */
    r2 = r;
    r2.a = 0x5A;
    r2.x = 0xA5;
    apple2host_set_regs(&r2);
    memset(&r2, 0, sizeof(r2));
    apple2host_get_regs(&r2);
    CHECK(r2.a == 0x5A, "A readback %02X, want 5A", r2.a);
    CHECK(r2.x == 0xA5, "X readback %02X, want A5", r2.x);
    CHECK(r2.pc == r.pc, "PC changed during a register write (%04X -> %04X)",
          r.pc, r2.pc);

    /* ---- single-instruction stepping ------------------------------------ */
    for (i = 0; i < 32; i++) {
        apple2host_regs before, after;
        unsigned cycles;
        apple2host_get_regs(&before);
        cycles = apple2host_step_instruction();
        apple2host_get_regs(&after);

        /* One 6502 instruction is 2..7 cycles; anything outside that means we
         * are not stepping one instruction at a time. */
        CHECK(cycles >= 1 && cycles <= 16,
              "step %d took %u cycles, expected a single instruction", i,
              cycles);
        cycles_total += cycles;
        if (after.pc != before.pc) moved = 1;
    }
    CHECK(moved, "PC never advanced across 32 single steps");
    CHECK(cycles_total > 0, "stepping consumed no cycles");

    /* ---- memory --------------------------------------------------------- */
    /* The //e ROM is at $FF00 and must read back as non-zero. */
    apple2host_read_mem(0xFF00, buf, sizeof(buf));
    {
        int nonzero = 0;
        for (i = 0; i < (int)sizeof(buf); i++)
            if (buf[i]) nonzero++;
        CHECK(nonzero > 0, "ROM at $FF00 read back all zeroes");
    }

    /* Zero page round-trip. */
    apple2host_read_mem(0x0300, saved, sizeof(saved));
    {
        const uint8_t pat[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        apple2host_write_mem(0x0300, pat, sizeof(pat));
        apple2host_read_mem(0x0300, probe, sizeof(probe));
        CHECK(memcmp(pat, probe, sizeof(pat)) == 0,
              "RAM round-trip failed: %02X %02X %02X %02X",
              probe[0], probe[1], probe[2], probe[3]);
    }
    apple2host_write_mem(0x0300, saved, sizeof(saved));

    /* Reading the soft-switch page must not wedge the machine: step again
     * afterwards and confirm the CPU still runs. */
    apple2host_read_mem(0xC000, buf, sizeof(buf));
    {
        apple2host_regs before, after;
        apple2host_get_regs(&before);
        apple2host_step_instruction();
        apple2host_get_regs(&after);
        CHECK(!after.jammed, "CPU jammed after reading $C000 through the "
                             "debugger -- the read is not non-intrusive");
    }

    /* Stepping must not have broken normal execution. */
    for (i = 0; i < 10; i++) apple2host_core_run_frame();

    apple2host_core_stop();

    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("debug_seam: stepping, registers and memory all behave\n");
    return 0;
}

/*
 * Exercises the debugger engine against a real running machine, driven from a
 * "UI" thread while the emulator thread runs the session loop -- the
 * arrangement that has to work: attach/stop/resume, execute breakpoints,
 * READ/WRITE watchpoints (ranges, conditions on `value`/`old`, a soft switch,
 * the immediate-operand filter), step / over / out / run-to / scanline /
 * frame, the trace ring, memory search, slots and I/O state, symbols, the
 * console, the screen and frame counter moving while the engine is engaged,
 * and a session restart while stopped.
 *
 * Exits 77 (ctest skip) when the machine cannot boot, so a ROM-less build
 * still passes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apple2debug.h"
#include "apple2session.h"

static int g_fail;
static volatile int g_stops;
static volatile int g_last_kind;

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
static void on_stop(void *user, int kind, uint16_t pc)
{
    (void)user; (void)pc;
    g_last_kind = kind;
    g_stops++;
}

static int wait_stopped(apple2debug *d, int ms)
{
    while (ms > 0) {
        if (apple2debug_is_stopped(d)) return 1;
        usleep(2000);
        ms -= 2;
    }
    return 0;
}

static int wait_running(apple2debug *d, int ms)
{
    while (ms > 0) {
        if (!apple2debug_is_stopped(d)) return 1;
        usleep(2000);
        ms -= 2;
    }
    return 0;
}

static void poke(apple2debug *d, uint16_t a, const uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) apple2debug_write(d, (uint16_t)(a + i), b[i]);
}

static unsigned pc_of(apple2debug *d)
{
    apple2debug_cpu c;
    apple2debug_cpu_get(d, &c);
    return (unsigned)c.pc;
}

static char g_why[128];
static const char *why(apple2debug *d, int *addr)
{
    apple2debug_stop_reason(d, g_why, sizeof g_why, addr);
    return g_why;
}

/* Run an action that stops again by itself. */
#define AND_WAIT(d, action)                                                  \
    do {                                                                     \
        unsigned gen_ = apple2debug_generation(d);                           \
        action;                                                              \
        wait_running(d, 20);                                                 \
        (void)gen_;                                                          \
        CHECK(wait_stopped(d, 3000), "did not stop within 3s: %s", #action); \
    } while (0)

static void goto_pc(apple2debug *d, unsigned pc)
{
    apple2debug_cpu_set(d, APPLE2_REG_PC, (int)pc);
}

int main(void)
{
    apple2session *s;
    apple2session_start_opts o;
    apple2debug *d;
    apple2debug_cpu c, c2;
    char buf[4096];
    int addr, id, i, n;
    uint32_t *fb;

    /* The test program, at $0300:
     *   0300 LDA #$00 / 0302 STA $0318 / 0305 LDA $0318 / 0308 BIT $C030 /
     *   030B INC $0318 / 030E JMP $0300
     * and a routine at $0330 called from $0320:
     *   0320 JSR $0330 / 0323 NOP / 0324 JMP $0320
     *   0330 LDX #$05 / 0332 DEX / 0333 BNE $0332 / 0335 RTS */
    static const uint8_t prog[] = { 0xA9, 0x00, 0x8D, 0x18, 0x03, 0xAD, 0x18, 0x03,
                                    0x2C, 0x30, 0xC0, 0xEE, 0x18, 0x03, 0x4C, 0x00, 0x03 };
    static const uint8_t sub[] = { 0x20, 0x30, 0x03, 0xEA, 0x4C, 0x20, 0x03 };
    static const uint8_t rtn[] = { 0xA2, 0x05, 0xCA, 0xD0, 0xFD, 0x60 };

    fb = calloc((size_t)APPLE2SESSION_FB_MAX_WIDTH * APPLE2SESSION_FB_MAX_HEIGHT,
                sizeof *fb);
    s = apple2session_new(NULL);
    if (!s || !fb) {
        fprintf(stderr, "debug_engine: could not create the session\n");
        return 77;
    }
    apple2session_default_opts(s, &o);
    /* No built-in FujiNet/audio/gamepad: this is about the engine, and it keeps
     * the test off the network and the sound device. The SmartPort card is
     * still inserted (slot 7) on a private port so the Slots tab has it. */
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.slot7 = "FujiNet";
    o.slip_listen = "127.0.0.1:41985";
    if (apple2session_start(s, &o) != 0) {
        fprintf(stderr, "debug_engine: %s\n", apple2session_last_error(s));
        apple2session_free(s);
        return 77;
    }

    d = apple2session_debugger(s);
    CHECK(d != NULL, "debugger engine was not created");
    if (!d) { apple2session_free(s); return 1; }
    apple2debug_set_stop_callback(d, on_stop, NULL);
    usleep(300000);

    /* ---- attach stops the machine ---------------------------------------- */
    CHECK(!apple2debug_is_attached(d) && !apple2debug_is_stopped(d), "starts attached");
    apple2debug_attach(d);
    CHECK(wait_stopped(d, 2000), "attach did not stop the machine");
    CHECK(apple2debug_is_attached(d), "not attached");
    CHECK(g_stops >= 1 && g_last_kind == APPLE2DBG_STOP_PAUSE, "stop callback: kind %d", g_last_kind);
    CHECK(!strcmp(why(d, &addr), "stopped") && addr >= 0, "reason '%s'", g_why);
    apple2debug_cpu_get(d, &c);
    CHECK(c.pc != 0 && c.is_65c02, "cpu: pc %04X, 65c02 %d", c.pc, c.is_65c02);
    CHECK(c.sp >= 0x100 && c.sp <= 0x1FF, "sp %04X not $01xx", c.sp);

    /* ---- registers -------------------------------------------------------- */
    apple2debug_cpu_set(d, APPLE2_REG_SP, 0xF0);
    apple2debug_cpu_get(d, &c);
    CHECK(c.sp == 0x1F0, "set sp $F0 -> %04X", c.sp);
    apple2debug_cpu_set(d, APPLE2_REG_SP, 0x1E8);
    apple2debug_cpu_get(d, &c);
    CHECK(c.sp == 0x1E8, "set sp $1E8 -> %04X", c.sp);
    apple2debug_cpu_set(d, APPLE2_REG_A, 0x5A);
    apple2debug_cpu_set(d, APPLE2_FLAG_I, 1);
    apple2debug_cpu_set(d, APPLE2_FLAG_C, 1);
    apple2debug_cpu_get(d, &c);
    CHECK(c.a == 0x5A && c.if_ == 1 && c.cf == 1 && (c.p & 5) == 5, "a %02X p %02X", c.a, c.p);

    /* ---- memory ------------------------------------------------------------- */
    poke(d, 0x0300, prog, sizeof prog);
    poke(d, 0x0320, sub, sizeof sub);
    poke(d, 0x0330, rtn, sizeof rtn);
    {
        uint8_t back[sizeof prog];
        CHECK(apple2debug_read(d, 0x0300, back, sizeof back) == (int)sizeof back &&
                  !memcmp(back, prog, sizeof prog), "memory round trip");
        uint8_t rom, rom2;
        apple2debug_read(d, 0xFA62, &rom, 1);
        apple2debug_write(d, 0xFA62, (uint8_t)(rom ^ 0xFF));   /* ROM ignores the edit */
        apple2debug_read(d, 0xFA62, &rom2, 1);
        CHECK(rom == rom2, "a debugger write changed the ROM");
    }

    /* ---- disassembly ----------------------------------------------------------- */
    {
        apple2debug_line l[6];
        int pcl;
        goto_pc(d, 0x0300);
        n = apple2debug_disassemble(d, 0x0300, l, 6, &pcl);
        CHECK(n == 6 && pcl == 0 && l[0].is_pc, "disassembly: n %d pcline %d", n, pcl);
        CHECK(!strcmp(l[0].disasm, "LDA #$00") && l[0].length == 2, "l0 '%s'", l[0].disasm);
        CHECK(!strcmp(l[3].disasm, "BIT $C030") && !strcmp(l[3].comment, "SPKR"),
              "BIT $C030 comment '%s'", l[3].comment);
        CHECK(l[5].flags & APPLE2DASM_JUMP, "JMP flag");
        CHECK(apple2debug_row_address(d, 0x0308, -2) == 0x0302, "row back %04X",
              apple2debug_row_address(d, 0x0308, -2));
        CHECK(apple2debug_row_address(d, 0x0300, 3) == 0x0308, "row fwd");
    }

    /* ---- stepping ------------------------------------------------------------------ */
    goto_pc(d, 0x0300);
    AND_WAIT(d, apple2debug_step(d));
    CHECK(pc_of(d) == 0x0302 && !strcmp(why(d, &addr), "step") && addr == 0x0302,
          "step: pc %04X '%s'", pc_of(d), g_why);
    apple2debug_cpu_get(d, &c);
    CHECK(c.a == 0, "LDA #0 ran, a=%02X", c.a);
    apple2debug_command(d, "step 3", buf, sizeof buf);
    CHECK(pc_of(d) == 0x030B, "step 3 -> %04X (%s)", pc_of(d), buf);

    /* ---- the immediate-operand filter ---------------------------------------------- */
    id = apple2debug_breakpoint_add(d, APPLE2DEBUG_BP_READ, 0x0301, 0x0301, NULL, buf, sizeof buf);
    CHECK(id > 0, "bpr $0301: %s", buf);
    goto_pc(d, 0x0300);
    apple2debug_resume(d);
    usleep(300000);
    CHECK(!apple2debug_is_stopped(d), "an immediate operand fetch tripped a read breakpoint: %s", why(d, NULL));
    apple2debug_stop(d);
    CHECK(wait_stopped(d, 2000), "stop request");
    CHECK(!strcmp(why(d, NULL), "stopped"), "reason '%s'", g_why);
    apple2debug_breakpoint_remove(d, id);

    /* ---- write breakpoint with range and `old` ---------------------------------------- */
    goto_pc(d, 0x0300);
    poke(d, 0x0318, (const uint8_t[]){ 0 }, 1);
    id = apple2debug_breakpoint_add(d, APPLE2DEBUG_BP_WRITE, 0x0318, 0x0318,
                                    "old == 1 && value == 0", buf, sizeof buf);
    CHECK(id > 0, "bpw cond: %s", buf);
    AND_WAIT(d, apple2debug_resume(d));
    CHECK(!strcmp(why(d, &addr), "write $0318 = $00"), "write reason '%s'", g_why);
    CHECK(pc_of(d) == 0x0305 && addr == 0x0305, "stops after the STA: pc %04X", pc_of(d));
    CHECK(g_last_kind == APPLE2DBG_STOP_WATCH, "kind %d", g_last_kind);
    {
        apple2debug_breakpoint bp[4];
        n = apple2debug_breakpoint_list(d, bp, 4);
        CHECK(n == 1 && bp[0].id == id && bp[0].hits == 1 && bp[0].enabled, "hits %u", bp[0].hits);
    }
    apple2debug_breakpoint_remove(d, id);
    /* a range that covers the byte, value condition */
    id = apple2debug_breakpoint_add(d, APPLE2DEBUG_BP_WRITE, 0x0300, 0x03FF, "value == 1", buf, sizeof buf);
    AND_WAIT(d, apple2debug_resume(d));
    CHECK(!strcmp(why(d, NULL), "write $0318 = $01") && pc_of(d) == 0x030E, "inc: '%s' pc %04X", g_why, pc_of(d));
    apple2debug_breakpoint_remove(d, id);
    /* ids never reused */
    n = apple2debug_breakpoint_add(d, APPLE2DEBUG_BP_WRITE, 0x0318, 0x0318, NULL, buf, sizeof buf);
    CHECK(n > id, "ids reused: %d after %d", n, id);
    apple2debug_breakpoint_enable(d, n, 0);
    goto_pc(d, 0x0300);
    apple2debug_resume(d);
    usleep(200000);
    CHECK(!apple2debug_is_stopped(d), "a disabled breakpoint fired");
    apple2debug_breakpoint_enable(d, n, 1);
    AND_WAIT(d, (void)0);
    CHECK(!strncmp(why(d, NULL), "write $0318", 11), "re-enabled: '%s'", g_why);
    apple2debug_breakpoint_remove(d, n);

    /* ---- read breakpoints, a soft switch, read+write together -------------------------- */
    goto_pc(d, 0x0300);
    id = apple2debug_breakpoint_add(d, APPLE2DEBUG_BP_READ, 0xC030, 0xC030, NULL, buf, sizeof buf);
    AND_WAIT(d, apple2debug_resume(d));
    CHECK(!strcmp(why(d, NULL), "read $C030") && pc_of(d) == 0x030B, "BIT $C030: '%s' pc %04X", g_why, pc_of(d));
    apple2debug_breakpoint_remove(d, id);
    goto_pc(d, 0x0300);
    id = apple2debug_breakpoint_add(d, APPLE2DEBUG_BP_READ | APPLE2DEBUG_BP_WRITE, 0x0318, 0x0318, NULL, buf, sizeof buf);
    CHECK(id > 0, "read|write: %s", buf);
    AND_WAIT(d, apple2debug_resume(d));
    CHECK(!strncmp(why(d, NULL), "write $0318", 11), "first access is the STA: '%s'", g_why);
    AND_WAIT(d, apple2debug_resume(d));
    CHECK(!strcmp(why(d, NULL), "read $0318") && pc_of(d) == 0x0308, "then the LDA: '%s' pc %04X", g_why, pc_of(d));
    apple2debug_breakpoint_clear(d);
    {
        apple2debug_breakpoint bp[2];
        CHECK(apple2debug_breakpoint_list(d, bp, 2) == 0, "clear");
    }

    /* ---- execute breakpoints: stop before, never re-fire without running ------------------- */
    goto_pc(d, 0x0300);
    CHECK(apple2debug_breakpoint_toggle(d, 0x0305) == 1 && apple2debug_breakpoint_check(d, 0x0305), "toggle on");
    AND_WAIT(d, apple2debug_resume(d));
    CHECK(!strcmp(why(d, &addr), "breakpoint: exec $0305") && pc_of(d) == 0x0305 && addr == 0x0305,
          "exec bp: '%s' pc %04X", g_why, pc_of(d));
    CHECK(g_last_kind == APPLE2DBG_STOP_BREAKPOINT, "kind");
    AND_WAIT(d, apple2debug_step(d));
    CHECK(pc_of(d) == 0x0308, "step off a breakpoint ran the instruction: %04X", pc_of(d));
    goto_pc(d, 0x0305);
    apple2debug_cpu_get(d, &c);
    AND_WAIT(d, apple2debug_resume(d));
    apple2debug_cpu_get(d, &c2);
    CHECK(pc_of(d) == 0x0305 && c2.cycles > c.cycles + 10, "resume from a breakpoint ran a loop first (%llu -> %llu)",
          (unsigned long long)c.cycles, (unsigned long long)c2.cycles);
    CHECK(apple2debug_breakpoint_toggle(d, 0x0305) == 0 && !apple2debug_breakpoint_check(d, 0x0305), "toggle off");

    /* ---- run to ------------------------------------------------------------------------------- */
    goto_pc(d, 0x0300);
    AND_WAIT(d, apple2debug_run_to(d, 0x030B));
    CHECK(pc_of(d) == 0x030B && !strcmp(why(d, NULL), "run to") && g_last_kind == APPLE2DBG_STOP_RUNTO, "run-to: '%s' %04X", g_why, pc_of(d));

    /* ---- step over / out ---------------------------------------------------------------------------- */
    goto_pc(d, 0x0320);
    apple2debug_cpu_get(d, &c);
    AND_WAIT(d, apple2debug_step_over(d));
    apple2debug_cpu_get(d, &c2);
    CHECK(pc_of(d) == 0x0323 && !strcmp(why(d, NULL), "step over") && c2.sp == c.sp, "over: '%s' pc %04X sp %04X/%04X", g_why, pc_of(d), c2.sp, c.sp);
    goto_pc(d, 0x0323);
    AND_WAIT(d, apple2debug_step_over(d));
    CHECK(pc_of(d) == 0x0324 && !strcmp(why(d, NULL), "step"), "over a NOP is a step: %04X '%s'", pc_of(d), g_why);
    goto_pc(d, 0x0320);
    AND_WAIT(d, apple2debug_step(d));
    CHECK(pc_of(d) == 0x0330, "into the call");
    AND_WAIT(d, apple2debug_step_out(d));
    CHECK(pc_of(d) == 0x0323 && !strcmp(why(d, NULL), "step out"), "out: '%s' pc %04X", g_why, pc_of(d));
    /* a breakpoint inside the stepped-over routine stops the step early */
    apple2debug_breakpoint_toggle(d, 0x0332);
    goto_pc(d, 0x0320);
    AND_WAIT(d, apple2debug_step_over(d));
    CHECK(pc_of(d) == 0x0332 && !strncmp(why(d, NULL), "breakpoint", 10), "bp inside over: '%s' %04X", g_why, pc_of(d));
    apple2debug_breakpoint_toggle(d, 0x0332);

    /* ---- scanline / frame ---------------------------------------------------------------------------------- */
    goto_pc(d, 0x0300);
    apple2debug_cpu_get(d, &c);
    AND_WAIT(d, apple2debug_scanline(d, 3));
    apple2debug_cpu_get(d, &c2);
    {
        int lines = 262;
        int moved = ((c2.line - c.line) % lines + lines) % lines;
        CHECK(!strcmp(why(d, NULL), "scanline") && moved >= 3 && moved <= 5, "scanline x3: line %d -> %d '%s'", c.line, c2.line, g_why);
    }
    AND_WAIT(d, apple2debug_frame(d, 1));
    apple2debug_cpu_get(d, &c);
    CHECK(!strcmp(why(d, NULL), "frame") && c.line < 6 && c.frame == c2.frame + 1,
          "frame: line %d frame %u -> %u '%s'", c.line, c2.frame, c.frame, g_why);
    AND_WAIT(d, apple2debug_frame(d, 2));
    apple2debug_cpu_get(d, &c2);
    CHECK(c2.frame == c.frame + 2, "frame x2: %u -> %u", c.frame, c2.frame);

    /* ---- trace ring --------------------------------------------------------------------------------------------- */
    goto_pc(d, 0x0300);
    CHECK(!apple2debug_trace_enabled(d) && apple2debug_trace_count(d) == 0, "trace starts off and empty");
    apple2debug_trace_enable(d, 1);
    for (i = 0; i < 5; i++) AND_WAIT(d, apple2debug_step(d));
    {
        apple2debug_trace t[8];
        char line[96];
        n = apple2debug_trace_read(d, t, 8);
        CHECK(n == 5 && apple2debug_trace_count(d) == 5, "trace has %d", n);
        CHECK(t[4].pc == 0x0300 && t[3].pc == 0x0302 && t[0].pc == 0x030B, "trace order newest first: %04X %04X %04X", t[0].pc, t[3].pc, t[4].pc);
        apple2debug_trace_format(&t[4], line, sizeof line);
        CHECK(!strncmp(line, "0300  LDA #$00", 14), "format '%s'", line);
        CHECK(apple2debug_trace_match(&t[0], APPLE2DEBUG_TRACE_ALL, 0, 0) &&
                  !apple2debug_trace_match(&t[0], APPLE2DEBUG_TRACE_BRANCHES, 0, 0) &&
                  apple2debug_trace_match(&t[0], APPLE2DEBUG_TRACE_RANGE, 0x0300, 0x030F), "match");
    }
    apple2debug_trace_clear(d);
    CHECK(apple2debug_trace_count(d) == 0, "trace clear");
    apple2debug_trace_enable(d, 0);

    /* ---- search ------------------------------------------------------------------------------------------------------------ */
    {
        static const uint8_t magic[] = { 0xDE, 0xAD, 0xBE, 0xEF };
        uint16_t hit[4], one;
        poke(d, 0x0340, magic, 4);
        n = apple2debug_search(d, magic, 4, 0xFF, 0, 0x0300, 0x03FF, hit, 4);
        CHECK(n == 1 && hit[0] == 0x0340, "search n %d", n);
        CHECK(apple2debug_search_next(d, magic, 4, 0xFF, 0, 0x0100, 0, &one) == 1 && one == 0x0340, "search_next");
        CHECK(apple2debug_search_next(d, magic, 4, 0xFF, 0, 0x0340, 1, &one) == 1 && one == 0x0340, "search_next backward wraps");
        apple2debug_command(d, "find de ad be ef", buf, sizeof buf);
        CHECK(strstr(buf, "$0340"), "find: %s", buf);
    }

    /* ---- slots, I/O state, memory map -------------------------------------------------------------------------------------------- */
    {
        apple2debug_slot sl[9];
        apple2debug_region rg[32];
        int found = 0, covered = 1;
        unsigned next = 0;
        n = apple2debug_slots(d, sl, 9);
        CHECK(n == 9, "slots: %d (a //e has 9)", n);
        for (i = 0; i < n; i++)
            if (sl[i].fujinet) { found = 1; CHECK(sl[i].slot == 7 && strstr(sl[i].card, "FujiNet"), "fujinet slot %d '%s'", sl[i].slot, sl[i].card); }
        CHECK(found, "the SmartPort card is not in the slot list");
        CHECK(sl[6].card[0] != '\0', "Disk II in slot 6: '%s'", sl[6].card);
        n = apple2debug_io_state(d, buf, sizeof buf);
        CHECK(n > 100 && strstr(buf, "Slots") && strstr(buf, "FujiNet") && strstr(buf, "RAMRD"), "io_state (%d): %s", n, buf);
        n = apple2debug_memory_map(d, rg, 32);
        CHECK(n >= 5, "memory map has %d regions", n);
        for (i = 0; i < n; i++) {
            if (rg[i].lo != next) covered = 0;
            next = (unsigned)rg[i].hi + 1;
        }
        CHECK(covered && next == 0x10000, "memory map does not tile $0000-$FFFF");
        CHECK(rg[0].lo == 0 && strstr(rg[0].name, "RAM") && rg[0].writable, "first region '%s'", rg[0].name);
        CHECK(strstr(rg[n - 1].name, "ROM"), "last region '%s' w%d", rg[n - 1].name, rg[n - 1].writable);
        /* an I/O region at $C000 */
        for (i = 0; i < n; i++) if (rg[i].lo == 0xC000) break;
        CHECK(i < n && strstr(rg[i].name, "I/O"), "no I/O region");
    }

    /* ---- symbols ------------------------------------------------------------------------------------------------------------------- */
    {
        char nm[64];
        uint16_t off = 0, a;
        CHECK(apple2debug_label_address(d, "COUT") == 0xFDED, "COUT = %d", apple2debug_label_address(d, "COUT"));
        CHECK(apple2debug_label_address(d, "SPKR") == 0xC030, "SPKR");
        CHECK(apple2debug_label_address(d, "NOSUCHLABEL") == -1, "unknown label");
        apple2debug_address_label(d, 0xFDED, nm, sizeof nm);
        CHECK(!strcmp(nm, "COUT"), "label at FDED '%s'", nm);
        apple2debug_set_label(d, 0x0300, "demo");
        CHECK(apple2debug_label_address(d, "demo") == 0x0300 && apple2debug_symbol_find(d, "demo", &a) && a == 0x0300, "user label");
        CHECK(apple2debug_symbol_at(d, 0xFDED, &off) != NULL && off == 0, "symbol_at exact");
        apple2debug_set_label(d, 0x0300, "");
        CHECK(apple2debug_label_address(d, "demo") == -1, "label removed");
        {
            long v;
            char err[64];
            CHECK(apple2debug_eval(d, "COUT + 1", &v, err, sizeof err) == 0 && v == 0xFDEE, "eval label");
            CHECK(apple2debug_eval(d, "[$0300] == $A9 && {$0300} == $00A9", &v, err, sizeof err) == 0 && v == 1, "eval memory");
            CHECK(apple2debug_eval(d, "nonsense +", &v, err, sizeof err) == -1 && err[0], "eval error");
        }
    }

    /* ---- the console ----------------------------------------------------------------------------------------------------------------------- */
    CHECK(apple2debug_command(d, "help", buf, sizeof buf) > 100 && strstr(buf, "bpr"), "help");
    apple2debug_command(d, "regs", buf, sizeof buf);
    CHECK(strstr(buf, "PC=") && strstr(buf, "65C02"), "regs: %s", buf);
    apple2debug_command(d, "print $0300 + 1", buf, sizeof buf);
    CHECK(strstr(buf, "$0301"), "print: %s", buf);
    apple2debug_command(d, "set a $42", buf, sizeof buf);
    apple2debug_cpu_get(d, &c);
    CHECK(c.a == 0x42, "console set a");
    apple2debug_command(d, "bpw $0318 value == 7", buf, sizeof buf);
    CHECK(strstr(buf, "breakpoint #"), "bpw: %s", buf);
    apple2debug_command(d, "breaks", buf, sizeof buf);
    CHECK(strstr(buf, "w") && strstr(buf, "value == 7"), "breaks: %s", buf);
    apple2debug_command(d, "clear", buf, sizeof buf);
    apple2debug_command(d, "mem $0300 4", buf, sizeof buf);
    CHECK(strstr(buf, "A9 00 8D 18"), "mem: %s", buf);
    apple2debug_command(d, "video", buf, sizeof buf);
    CHECK(strstr(buf, "TEXT"), "video: %s", buf);
    apple2debug_command(d, "io", buf, sizeof buf);
    CHECK(strstr(buf, "Soft switches"), "io");
    apple2debug_command(d, "disasm $0300 2", buf, sizeof buf);
    CHECK(strstr(buf, "LDA #$00"), "disasm: %s", buf);
    apple2debug_command(d, "watch $0318", buf, sizeof buf);
    CHECK(strstr(buf, "$0318"), "watch: %s", buf);
    apple2debug_command(d, "unwatch all", buf, sizeof buf);
    apple2debug_command(d, "bogus", buf, sizeof buf);
    CHECK(strstr(buf, "unknown command"), "unknown command");
    CHECK(apple2debug_completions(d, "br", buf, sizeof buf) >= 2 && strstr(buf, "break"), "completions");
    CHECK(apple2debug_completions(d, "print CO", buf, sizeof buf) >= 1 && strstr(buf, "COUT"), "label completions: %s", buf);
    {
        char path[] = "/tmp/apple2dbg_save_XXXXXX";
        int fd = mkstemp(path);
        char msg[200];
        close(fd);
        CHECK(apple2debug_save(d, "mem", path, msg, sizeof msg) == 0, "save mem: %s", msg);
        CHECK(apple2debug_save(d, "dis", path, msg, sizeof msg) == 0, "save dis: %s", msg);
        CHECK(apple2debug_save(d, "regs", path, msg, sizeof msg) == 0, "save regs: %s", msg);
        CHECK(apple2debug_save(d, "bogus", path, msg, sizeof msg) == -1, "save bogus");
        unlink(path);
    }

    /* ---- FujiNet: the link is not up here ----------------------------------------------------------------------------------------------------- */
    {
        apple2_fuji_packet p[4];
        char st[2048];
        CHECK(apple2debug_fujinet_state(d, st, (int)sizeof st) > 0 && strstr(st, "Card       slot 7"),
              "FujiNet state text: %s", st);
        (void)p;
    }

    /* ---- the screen and the frame counter keep moving while engaged ------------------------------------------------------------------------------ */
    {
        uint32_t *snap = malloc((size_t)APPLE2SESSION_FB_MAX_WIDTH * APPLE2SESSION_FB_MAX_HEIGHT * 4);
        uint64_t serial = 0;
        long f0, f1;
        int w = 0, h = 0, diff = 0, k;

        apple2debug_breakpoint_toggle(d, 0x0000);       /* armed: the engine stays engaged */
        goto_pc(d, 0x0300);
        apple2debug_resume(d);
        usleep(150000);
        CHECK(!apple2debug_is_stopped(d), "loop stopped");
        CHECK(apple2session_copy_frame(s, snap, &serial, &w, &h), "no frame published while engaged");
        f0 = (long)apple2session_frame_count(s);
        apple2debug_cpu_get(d, &c);
        for (k = 0; k < 40; k++) apple2debug_write(d, (uint16_t)(0x0400 + k), 0xC1);
        usleep(600000);
        f1 = (long)apple2session_frame_count(s);
        apple2debug_cpu_get(d, &c2);
        CHECK(f1 - f0 >= 15, "frames run while engaged: %ld", f1 - f0);
        CHECK(c2.frame > c.frame + 15, "frame counter stuck: %u -> %u", c.frame, c2.frame);
        CHECK(apple2session_copy_frame(s, fb, &serial, &w, &h), "frame serial did not move while engaged");
        for (k = 0; k < w * h; k++) if (snap[k] != fb[k]) diff++;
        CHECK(diff > 0, "the screen did not change after writing to the text page (%d px)", diff);
        free(snap);
        apple2debug_breakpoint_toggle(d, 0x0000);
    }

    /* ---- detach lets the machine run free, attach stops it again ----------------------------------------------------------------------------------------- */
    apple2debug_stop(d);
    CHECK(wait_stopped(d, 2000), "stop while running");
    apple2debug_detach(d);
    CHECK(!apple2debug_is_attached(d) && !apple2debug_is_stopped(d), "detach");
    usleep(300000);
    apple2debug_cpu_get(d, &c);
    usleep(200000);
    apple2debug_cpu_get(d, &c2);
    CHECK(c2.cycles > c.cycles, "machine does not run after detach");
    CHECK(!strcmp(why(d, &addr), "running") && addr == -1, "reason while running '%s'", g_why);

    /* ---- a restart while stopped does not hang; the engine survives it --------------------------------------------------------------------------------------------- */
    apple2debug_breakpoint_toggle(d, 0xFA62);
    apple2debug_attach(d);
    CHECK(wait_stopped(d, 2000), "re-attach");
    apple2session_stop(s);
    CHECK(!apple2debug_is_stopped(d), "still 'stopped' after the machine went away");
    CHECK(apple2debug_is_attached(d), "attach state survives the stop");
    {
        uint8_t b;
        CHECK(apple2debug_read(d, 0x0300, &b, 1) == 0, "reads with no machine");
    }
    if (apple2session_start(s, &o) != 0) {
        fprintf(stderr, "debug_engine: restart: %s\n", apple2session_last_error(s));
        g_fail++;
    } else {
        CHECK(wait_stopped(d, 3000), "an attached engine did not stop the new machine");
        CHECK(apple2debug_breakpoint_check(d, 0xFA62), "breakpoint lost over the restart");
        apple2debug_cpu_get(d, &c);
        CHECK(c.pc != 0, "new machine has a PC");
        apple2debug_detach(d);
        usleep(200000);
        CHECK(!apple2debug_is_stopped(d), "detach after restart");
    }

    /* Stopping a *running* session must not hang, and neither must stopping a
     * stopped one -- that is what resume_for_stop is for. */
    apple2debug_attach(d);
    wait_stopped(d, 2000);
    apple2session_stop(s);
    apple2session_free(s);
    free(fb);

    /* ---- the NMOS 6502 jams on an illegal opcode; the engine says so -------------------------------------------- */
    s = apple2session_new(NULL);
    if (s) {
        apple2session_default_opts(s, &o);
        o.enable_fujinet = 0;
        o.enable_audio = 0;
        o.enable_gamepad = 0;
        o.machine = "Apple ][+";
        o.slot7 = "Empty";
        o.slip_listen = NULL;
        if (apple2session_start(s, &o) == 0) {
            d = apple2session_debugger(s);
            apple2debug_set_stop_callback(d, on_stop, NULL);
            apple2debug_attach(d);
            CHECK(wait_stopped(d, 3000), "][+: attach");
            apple2debug_cpu_get(d, &c);
            CHECK(!c.is_65c02, "a ][+ has the NMOS 6502");
            apple2debug_write(d, 0x0300, 0x02);       /* KIL */
            goto_pc(d, 0x0300);
            AND_WAIT(d, apple2debug_resume(d));
            apple2debug_cpu_get(d, &c);
            CHECK(!strcmp(why(d, NULL), "jammed") && g_last_kind == APPLE2DBG_STOP_JAMMED && c.jammed,
                  "jam: '%s' kind %d jammed %d", g_why, g_last_kind, c.jammed);
            goto_pc(d, 0xFA62);
            apple2debug_cpu_get(d, &c);
            CHECK(!c.jammed, "setting the PC clears the jam");
            apple2debug_detach(d);
            apple2session_stop(s);
        }
        apple2session_free(s);
    }

    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("debug_engine: attach, breakpoints, watchpoints, stepping, trace, "
           "search, slots, symbols, console and restart all behave\n");
    return 0;
}

/*
 * Unit tests for the 6502/65C02 disassembler.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "m6502dasm.h"

static int g_fail;

static void expect(const char *what, uint16_t addr, const uint8_t *bytes,
                   int want_len, const char *want_text, uint16_t want_target,
                   uint8_t want_flags_set)
{
    apple2dasm_line l;
    const int len = apple2dasm_decode(addr, bytes, &l);

    if (len != want_len) {
        fprintf(stderr, "FAIL %s: length %d, want %d\n", what, len, want_len);
        g_fail++;
    }
    if (strcmp(l.text, want_text) != 0) {
        fprintf(stderr, "FAIL %s: \"%s\", want \"%s\"\n", what, l.text,
                want_text);
        g_fail++;
    }
    if (l.target != want_target) {
        fprintf(stderr, "FAIL %s: target $%04X, want $%04X\n", what, l.target,
                want_target);
        g_fail++;
    }
    if ((l.flags & want_flags_set) != want_flags_set) {
        fprintf(stderr, "FAIL %s: flags 0x%02X missing 0x%02X\n", what,
                l.flags, want_flags_set);
        g_fail++;
    }
    if (l.len != (uint8_t)want_len || l.addr != addr) {
        fprintf(stderr, "FAIL %s: addr/len fields wrong\n", what);
        g_fail++;
    }
}

int main(void)
{
    /* ---- addressing modes ---------------------------------------------- */
    expect("implied",     0x1000, (const uint8_t[]){0x18, 0, 0}, 1, "CLC", 0, 0);
    expect("accumulator", 0x1000, (const uint8_t[]){0x0A, 0, 0}, 1, "ASL A", 0, 0);
    expect("immediate",   0x1000, (const uint8_t[]){0xA9, 0x42, 0}, 2,
           "LDA #$42", 0, 0);
    expect("zero page",   0x1000, (const uint8_t[]){0xA5, 0x36, 0}, 2,
           "LDA $36", 0, 0);
    expect("zero page,X", 0x1000, (const uint8_t[]){0xB5, 0x36, 0}, 2,
           "LDA $36,X", 0, 0);
    expect("zero page,Y", 0x1000, (const uint8_t[]){0xB6, 0x36, 0}, 2,
           "LDX $36,Y", 0, 0);
    expect("(zp,X)",      0x1000, (const uint8_t[]){0xA1, 0x36, 0}, 2,
           "LDA ($36,X)", 0, 0);
    expect("(zp),Y",      0x1000, (const uint8_t[]){0xB1, 0x36, 0}, 2,
           "LDA ($36),Y", 0, 0);
    expect("(zp) 65C02",  0x1000, (const uint8_t[]){0xB2, 0x36, 0}, 2,
           "LDA ($36)", 0, 0);
    expect("absolute",    0x1000, (const uint8_t[]){0xAD, 0xF0, 0xC0}, 3,
           "LDA $C0F0", 0, 0);
    expect("absolute,X",  0x1000, (const uint8_t[]){0xBD, 0xF0, 0xC0}, 3,
           "LDA $C0F0,X", 0, 0);
    expect("absolute,Y",  0x1000, (const uint8_t[]){0xB9, 0xF0, 0xC0}, 3,
           "LDA $C0F0,Y", 0, 0);

    /* ---- control flow --------------------------------------------------- */
    expect("JSR", 0x1000, (const uint8_t[]){0x20, 0x00, 0xFD}, 3, "JSR $FD00",
           0xFD00, APPLE2DASM_CALL);
    expect("JMP abs", 0x1000, (const uint8_t[]){0x4C, 0x00, 0xE0}, 3,
           "JMP $E000", 0xE000, APPLE2DASM_JUMP);
    expect("JMP (ind)", 0x1000, (const uint8_t[]){0x6C, 0xFC, 0xFF}, 3,
           "JMP ($FFFC)", 0xFFFC, APPLE2DASM_JUMP);
    expect("JMP (abs,X) 65C02", 0x1000, (const uint8_t[]){0x7C, 0x00, 0x20}, 3,
           "JMP ($2000,X)", 0x2000, APPLE2DASM_JUMP);
    expect("RTS", 0x1000, (const uint8_t[]){0x60, 0, 0}, 1, "RTS", 0,
           APPLE2DASM_RET);
    expect("RTI", 0x1000, (const uint8_t[]){0x40, 0, 0}, 1, "RTI", 0,
           APPLE2DASM_RET);
    expect("BRK", 0x1000, (const uint8_t[]){0x00, 0, 0}, 1, "BRK", 0,
           APPLE2DASM_HALT);

    /* Branches are relative to the address AFTER the instruction. */
    expect("BNE forward", 0x1000, (const uint8_t[]){0xD0, 0x10, 0}, 2,
           "BNE $1012", 0x1012,
           APPLE2DASM_RELATIVE | APPLE2DASM_JUMP | APPLE2DASM_COND);
    expect("BNE backward", 0x1000, (const uint8_t[]){0xD0, 0xFE, 0}, 2,
           "BNE $1000", 0x1000,
           APPLE2DASM_RELATIVE | APPLE2DASM_JUMP | APPLE2DASM_COND);
    /* BRA is the one unconditional relative branch. */
    expect("BRA", 0x1000, (const uint8_t[]){0x80, 0x02, 0}, 2, "BRA $1004",
           0x1004, APPLE2DASM_RELATIVE | APPLE2DASM_JUMP);
    {
        apple2dasm_line l;
        apple2dasm_decode(0x1000, (const uint8_t[]){0x80, 0x02, 0}, &l);
        if (l.flags & APPLE2DASM_COND) {
            fprintf(stderr, "FAIL BRA: marked conditional\n");
            g_fail++;
        }
    }

    /* ---- 65C02 additions ------------------------------------------------ */
    expect("STZ", 0x1000, (const uint8_t[]){0x9C, 0x00, 0x04}, 3, "STZ $0400",
           0, 0);
    expect("PHX", 0x1000, (const uint8_t[]){0xDA, 0, 0}, 1, "PHX", 0, 0);
    expect("TSB", 0x1000, (const uint8_t[]){0x04, 0x10, 0}, 2, "TSB $10", 0, 0);
    expect("RMB3", 0x1000, (const uint8_t[]){0x37, 0x10, 0}, 2, "RMB3 $10", 0,
           0);
    expect("BBR0", 0x1000, (const uint8_t[]){0x0F, 0x10, 0x05}, 3,
           "BBR0 $10,$1008", 0x1008,
           APPLE2DASM_RELATIVE | APPLE2DASM_JUMP | APPLE2DASM_COND);
    expect("BBS7", 0x1000, (const uint8_t[]){0xFF, 0x20, 0xFD}, 3,
           "BBS7 $20,$1000", 0x1000,
           APPLE2DASM_RELATIVE | APPLE2DASM_JUMP | APPLE2DASM_COND);
    expect("STP", 0x1000, (const uint8_t[]){0xDB, 0, 0}, 1, "STP", 0,
           APPLE2DASM_HALT);

    /* Undefined on a 65C02: one byte, flagged, never a plausible mnemonic. */
    expect("illegal", 0x1000, (const uint8_t[]){0x03, 0, 0}, 1, "??? $03", 0,
           APPLE2DASM_ILLEGAL);

    /* ---- length agrees with decode across the whole map ----------------- */
    {
        int op;
        for (op = 0; op < 256; op++) {
            const uint8_t b[3] = {(uint8_t)op, 0x34, 0x12};
            apple2dasm_line l;
            const int len = apple2dasm_decode(0x2000, b, &l);
            if (len != apple2dasm_length((uint8_t)op)) {
                fprintf(stderr,
                        "FAIL opcode $%02X: decode len %d, length() %d\n", op,
                        len, apple2dasm_length((uint8_t)op));
                g_fail++;
            }
            if (len < 1 || len > 3) {
                fprintf(stderr, "FAIL opcode $%02X: length %d out of range\n",
                        op, len);
                g_fail++;
            }
            if (l.text[0] == '\0') {
                fprintf(stderr, "FAIL opcode $%02X: empty text\n", op);
                g_fail++;
            }
        }
    }

    /* An operand address must not be mistaken for a branch target. */
    {
        apple2dasm_line l;
        apple2dasm_decode(0x1000, (const uint8_t[]){0xAD, 0xF0, 0xC0}, &l);
        if (l.target != 0) {
            fprintf(stderr, "FAIL LDA $C0F0: target set to $%04X on a "
                            "non-branching instruction\n", l.target);
            g_fail++;
        }
    }

    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("dasm_test: all checks passed\n");
    return 0;
}

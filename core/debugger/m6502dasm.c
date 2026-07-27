/*
 * 6502 / 65C02 disassembler. See m6502dasm.h for why this is written fresh.
 *
 * The table is the full 65C02 (Rockwell/WDC) opcode matrix, which is what an
 * Enhanced Apple //e has. Opcodes the 65C02 does not define are decoded as
 * one-byte "???" rather than as NMOS undocumented instructions: on a 65C02
 * those slots are genuinely NOPs of various lengths, and showing a plausible
 * NMOS mnemonic for them would be actively misleading in a debugger.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m6502dasm.h"

#include <stdio.h>
#include <string.h>

/* Addressing modes. */
enum {
    IMP,   /* implied              CLC        */
    ACC,   /* accumulator          ASL        */
    IMM,   /* immediate            LDA #$nn   */
    ZP,    /* zero page            LDA $nn    */
    ZPX,   /* zero page,X          LDA $nn,X  */
    ZPY,   /* zero page,Y          LDX $nn,Y  */
    IZX,   /* (zero page,X)        LDA ($nn,X)*/
    IZY,   /* (zero page),Y        LDA ($nn),Y*/
    IZP,   /* (zero page)   65C02  LDA ($nn)  */
    ABS,   /* absolute             LDA $nnnn  */
    ABX,   /* absolute,X           LDA $nnnn,X*/
    ABY,   /* absolute,Y           LDA $nnnn,Y*/
    IND,   /* (absolute)           JMP ($nnnn)*/
    IAX,   /* (absolute,X)  65C02  JMP ($nnnn,X) */
    REL,   /* relative             BNE $nnnn  */
    ZPREL, /* zp,relative   65C02  BBR0 $nn,$nnnn */
    ILL,   /* not a 65C02 opcode              */
};

static const uint8_t k_len[] = {
    [IMP] = 1, [ACC] = 1, [IMM] = 2, [ZP] = 2,  [ZPX] = 2, [ZPY] = 2,
    [IZX] = 2, [IZY] = 2, [IZP] = 2, [ABS] = 3, [ABX] = 3, [ABY] = 3,
    [IND] = 3, [IAX] = 3, [REL] = 2, [ZPREL] = 3, [ILL] = 1,
};

typedef struct {
    const char *mn;
    uint8_t mode;
} op_t;

#define X { "???", ILL }

/* Transcribed row by row from the 65C02 opcode matrix. */
static const op_t k_ops[256] = {
/* 00 */ {"BRK",IMP},{"ORA",IZX},X,X,{"TSB",ZP},{"ORA",ZP},{"ASL",ZP},{"RMB0",ZP},
         {"PHP",IMP},{"ORA",IMM},{"ASL",ACC},X,{"TSB",ABS},{"ORA",ABS},{"ASL",ABS},{"BBR0",ZPREL},
/* 10 */ {"BPL",REL},{"ORA",IZY},{"ORA",IZP},X,{"TRB",ZP},{"ORA",ZPX},{"ASL",ZPX},{"RMB1",ZP},
         {"CLC",IMP},{"ORA",ABY},{"INC",ACC},X,{"TRB",ABS},{"ORA",ABX},{"ASL",ABX},{"BBR1",ZPREL},
/* 20 */ {"JSR",ABS},{"AND",IZX},X,X,{"BIT",ZP},{"AND",ZP},{"ROL",ZP},{"RMB2",ZP},
         {"PLP",IMP},{"AND",IMM},{"ROL",ACC},X,{"BIT",ABS},{"AND",ABS},{"ROL",ABS},{"BBR2",ZPREL},
/* 30 */ {"BMI",REL},{"AND",IZY},{"AND",IZP},X,{"BIT",ZPX},{"AND",ZPX},{"ROL",ZPX},{"RMB3",ZP},
         {"SEC",IMP},{"AND",ABY},{"DEC",ACC},X,{"BIT",ABX},{"AND",ABX},{"ROL",ABX},{"BBR3",ZPREL},
/* 40 */ {"RTI",IMP},{"EOR",IZX},X,X,X,{"EOR",ZP},{"LSR",ZP},{"RMB4",ZP},
         {"PHA",IMP},{"EOR",IMM},{"LSR",ACC},X,{"JMP",ABS},{"EOR",ABS},{"LSR",ABS},{"BBR4",ZPREL},
/* 50 */ {"BVC",REL},{"EOR",IZY},{"EOR",IZP},X,X,{"EOR",ZPX},{"LSR",ZPX},{"RMB5",ZP},
         {"CLI",IMP},{"EOR",ABY},{"PHY",IMP},X,X,{"EOR",ABX},{"LSR",ABX},{"BBR5",ZPREL},
/* 60 */ {"RTS",IMP},{"ADC",IZX},X,X,{"STZ",ZP},{"ADC",ZP},{"ROR",ZP},{"RMB6",ZP},
         {"PLA",IMP},{"ADC",IMM},{"ROR",ACC},X,{"JMP",IND},{"ADC",ABS},{"ROR",ABS},{"BBR6",ZPREL},
/* 70 */ {"BVS",REL},{"ADC",IZY},{"ADC",IZP},X,{"STZ",ZPX},{"ADC",ZPX},{"ROR",ZPX},{"RMB7",ZP},
         {"SEI",IMP},{"ADC",ABY},{"PLY",IMP},X,{"JMP",IAX},{"ADC",ABX},{"ROR",ABX},{"BBR7",ZPREL},
/* 80 */ {"BRA",REL},{"STA",IZX},X,X,{"STY",ZP},{"STA",ZP},{"STX",ZP},{"SMB0",ZP},
         {"DEY",IMP},{"BIT",IMM},{"TXA",IMP},X,{"STY",ABS},{"STA",ABS},{"STX",ABS},{"BBS0",ZPREL},
/* 90 */ {"BCC",REL},{"STA",IZY},{"STA",IZP},X,{"STY",ZPX},{"STA",ZPX},{"STX",ZPY},{"SMB1",ZP},
         {"TYA",IMP},{"STA",ABY},{"TXS",IMP},X,{"STZ",ABS},{"STA",ABX},{"STZ",ABX},{"BBS1",ZPREL},
/* A0 */ {"LDY",IMM},{"LDA",IZX},{"LDX",IMM},X,{"LDY",ZP},{"LDA",ZP},{"LDX",ZP},{"SMB2",ZP},
         {"TAY",IMP},{"LDA",IMM},{"TAX",IMP},X,{"LDY",ABS},{"LDA",ABS},{"LDX",ABS},{"BBS2",ZPREL},
/* B0 */ {"BCS",REL},{"LDA",IZY},{"LDA",IZP},X,{"LDY",ZPX},{"LDA",ZPX},{"LDX",ZPY},{"SMB3",ZP},
         {"CLV",IMP},{"LDA",ABY},{"TSX",IMP},X,{"LDY",ABX},{"LDA",ABX},{"LDX",ABY},{"BBS3",ZPREL},
/* C0 */ {"CPY",IMM},{"CMP",IZX},X,X,{"CPY",ZP},{"CMP",ZP},{"DEC",ZP},{"SMB4",ZP},
         {"INY",IMP},{"CMP",IMM},{"DEX",IMP},{"WAI",IMP},{"CPY",ABS},{"CMP",ABS},{"DEC",ABS},{"BBS4",ZPREL},
/* D0 */ {"BNE",REL},{"CMP",IZY},{"CMP",IZP},X,X,{"CMP",ZPX},{"DEC",ZPX},{"SMB5",ZP},
         {"CLD",IMP},{"CMP",ABY},{"PHX",IMP},{"STP",IMP},X,{"CMP",ABX},{"DEC",ABX},{"BBS5",ZPREL},
/* E0 */ {"CPX",IMM},{"SBC",IZX},X,X,{"CPX",ZP},{"SBC",ZP},{"INC",ZP},{"SMB6",ZP},
         {"INX",IMP},{"SBC",IMM},{"NOP",IMP},X,{"CPX",ABS},{"SBC",ABS},{"INC",ABS},{"BBS6",ZPREL},
/* F0 */ {"BEQ",REL},{"SBC",IZY},{"SBC",IZP},X,X,{"SBC",ZPX},{"INC",ZPX},{"SMB7",ZP},
         {"SED",IMP},{"SBC",ABY},{"PLX",IMP},X,X,{"SBC",ABX},{"INC",ABX},{"BBS7",ZPREL},
};

#undef X

int apple2dasm_length(uint8_t opcode)
{
    return k_len[k_ops[opcode].mode];
}

static uint8_t flags_for(uint8_t opcode, const op_t *op)
{
    uint8_t f = 0;

    switch (op->mode) {
    case ILL:
        return APPLE2DASM_ILLEGAL | APPLE2DASM_HALT;
    case REL:
        f |= APPLE2DASM_RELATIVE | APPLE2DASM_JUMP;
        /* BRA is the only unconditional relative branch. */
        if (opcode != 0x80) f |= APPLE2DASM_COND;
        return f;
    case ZPREL:
        /* BBRn/BBSn: branch on a bit, so conditional and relative. */
        return APPLE2DASM_RELATIVE | APPLE2DASM_JUMP | APPLE2DASM_COND;
    default:
        break;
    }

    switch (opcode) {
    case 0x20: return APPLE2DASM_CALL;                    /* JSR */
    case 0x4C: case 0x6C: case 0x7C: return APPLE2DASM_JUMP; /* JMP */
    case 0x40: case 0x60: return APPLE2DASM_RET;          /* RTI, RTS */
    case 0x00: return APPLE2DASM_HALT;                    /* BRK */
    case 0xDB: return APPLE2DASM_HALT;                    /* STP */
    default:   return 0;
    }
}

int apple2dasm_decode(uint16_t addr, const uint8_t *bytes,
                      apple2dasm_line *out)
{
    const uint8_t opcode = bytes[0];
    const op_t *op = &k_ops[opcode];
    const int len = k_len[op->mode];
    const uint8_t lo = bytes[1];
    const uint8_t hi = bytes[2];
    const uint16_t abs16 = (uint16_t)(lo | (hi << 8));
    int i;

    memset(out, 0, sizeof(*out));
    out->addr = addr;
    out->len = (uint8_t)len;
    for (i = 0; i < len; i++)
        out->bytes[i] = bytes[i];
    out->flags = flags_for(opcode, op);

    switch (op->mode) {
    case ILL:
        snprintf(out->text, sizeof(out->text), "??? $%02X", opcode);
        break;
    case IMP:
        snprintf(out->text, sizeof(out->text), "%s", op->mn);
        break;
    case ACC:
        snprintf(out->text, sizeof(out->text), "%s A", op->mn);
        break;
    case IMM:
        snprintf(out->text, sizeof(out->text), "%s #$%02X", op->mn, lo);
        break;
    case ZP:
        snprintf(out->text, sizeof(out->text), "%s $%02X", op->mn, lo);
        break;
    case ZPX:
        snprintf(out->text, sizeof(out->text), "%s $%02X,X", op->mn, lo);
        break;
    case ZPY:
        snprintf(out->text, sizeof(out->text), "%s $%02X,Y", op->mn, lo);
        break;
    case IZX:
        snprintf(out->text, sizeof(out->text), "%s ($%02X,X)", op->mn, lo);
        break;
    case IZY:
        snprintf(out->text, sizeof(out->text), "%s ($%02X),Y", op->mn, lo);
        break;
    case IZP:
        snprintf(out->text, sizeof(out->text), "%s ($%02X)", op->mn, lo);
        break;
    case ABS:
        snprintf(out->text, sizeof(out->text), "%s $%04X", op->mn, abs16);
        out->target = abs16;
        break;
    case ABX:
        snprintf(out->text, sizeof(out->text), "%s $%04X,X", op->mn, abs16);
        break;
    case ABY:
        snprintf(out->text, sizeof(out->text), "%s $%04X,Y", op->mn, abs16);
        break;
    case IND:
        snprintf(out->text, sizeof(out->text), "%s ($%04X)", op->mn, abs16);
        out->target = abs16;
        break;
    case IAX:
        snprintf(out->text, sizeof(out->text), "%s ($%04X,X)", op->mn, abs16);
        out->target = abs16;
        break;
    case REL: {
        /* The branch is relative to the address AFTER the instruction. */
        const uint16_t dest = (uint16_t)(addr + 2 + (int8_t)lo);
        snprintf(out->text, sizeof(out->text), "%s $%04X", op->mn, dest);
        out->target = dest;
        break;
    }
    case ZPREL: {
        /* BBRn/BBSn $zp,$dest -- three bytes: opcode, zp, displacement. */
        const uint16_t dest = (uint16_t)(addr + 3 + (int8_t)hi);
        snprintf(out->text, sizeof(out->text), "%s $%02X,$%04X", op->mn, lo,
                 dest);
        out->target = dest;
        break;
    }
    default:
        snprintf(out->text, sizeof(out->text), "%s", op->mn);
        break;
    }

    /* ABS on a non-branching instruction is an operand address, not a control
     * transfer -- only keep target where it means "goes here". */
    if (!(out->flags & (APPLE2DASM_JUMP | APPLE2DASM_CALL)))
        out->target = 0;

    return len;
}

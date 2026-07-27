/*
 * Apple II video page decoders for the debugger: text, lo-res and hi-res,
 * pages 1 and 2, rendered to XRGB8888. The Apple II counterpart to the ADAM
 * target's vdpview.c (TMS9928A).
 *
 * All three modes are pure memory-layout arithmetic, and all three share the
 * Apple II's famously non-linear addressing -- the row a byte belongs to is
 * not its offset divided by the row length. Woz interleaved the rows so the
 * video counter could be derived from a handful of gates instead of a real
 * address adder, and the whole machine is cheaper because of it; the cost is
 * that every piece of software that touches the screen carries a lookup
 * table, this file included.
 *
 * These decode a PAGE, not the live display: which page (and which mode) the
 * hardware is actually scanning out depends on the soft switches, and the
 * session's own frame is the authority on that. Decoding a page directly is
 * what makes this useful -- it shows the buffer a program is drawing into
 * before it flips to it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "apple2debug.h"

#include "font5x7.inc"

#define W APPLE2VIEW_WIDTH   /* 280 */
#define H APPLE2VIEW_HEIGHT  /* 192 */

#define TEXT_COLS 40
#define TEXT_ROWS 24
#define CELL_W 7             /* 40 * 7 == 280 */
#define CELL_H 8             /* 24 * 8 == 192 */

/* ---- palettes -------------------------------------------------------------
 * The 16 lo-res colours, as the Apple IIgs RGB values -- the closest thing to
 * a canonical set for what were originally NTSC artifacts with no RGB
 * definition at all. */
static const uint32_t kLoresPalette[16] = {
    0x000000, /*  0 black       */
    0xDD0033, /*  1 deep red    */
    0x000099, /*  2 dark blue   */
    0xDD22DD, /*  3 purple      */
    0x007722, /*  4 dark green  */
    0x555555, /*  5 grey 1      */
    0x2222FF, /*  6 medium blue */
    0x66AAFF, /*  7 light blue  */
    0x885500, /*  8 brown       */
    0xFF6600, /*  9 orange      */
    0xAAAAAA, /* 10 grey 2      */
    0xFF9988, /* 11 pink        */
    0x00DD00, /* 12 green       */
    0xFFFF00, /* 13 yellow      */
    0x55FF99, /* 14 aqua        */
    0xFFFFFF, /* 15 white       */
};

/* Hi-res is six colours: black and white, plus two pairs the high bit of
 * each byte selects between. */
#define HR_BLACK  0x000000u
#define HR_WHITE  0xFFFFFFu
#define HR_GREEN  0x14F53Cu
#define HR_VIOLET 0xFF44FDu
#define HR_ORANGE 0xFF6A3Cu
#define HR_BLUE   0x14CFFDu

#define TEXT_FG 0xFFFFFFu
#define TEXT_BG 0x000000u

/* ---- address arithmetic ---------------------------------------------------
 * Text and lo-res share one layout: 24 rows in three interleaved bands of
 * eight. Hi-res adds a third level -- eight scanlines interleaved within each
 * of those rows -- which is why a hi-res page is 8K for 8K of pixels but a
 * text page is 1K for 960 characters: both leave holes. */

static uint16_t text_row_addr(uint16_t base, int row)
{
    return (uint16_t)(base + (row & 7) * 0x80 + (row >> 3) * 0x28);
}

static uint16_t hires_row_addr(uint16_t base, int y)
{
    return (uint16_t)(base + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 +
                      (y >> 6) * 0x28);
}

/* ---- text ----------------------------------------------------------------*/

/* Which glyph a screen byte shows, and whether it shows inverse.
 *   $00-$3F  inverse
 *   $40-$7F  flashing -- drawn steady here; a blinking debugger view is a
 *            worse debugger view
 *   $80-$FF  normal
 * The character generator holds only $20-$5F, so control codes fold up into
 * the @A-Z[\]^_ range and lowercase folds to uppercase, which is exactly what
 * an unenhanced II or II+ puts on the screen. */
static int glyph_index(uint8_t b, int *inverse)
{
    uint8_t c = (uint8_t)(b & 0x7F);

    *inverse = (b < 0x40);
    if (c < 0x20)
        c = (uint8_t)(c + 0x40);
    if (c >= 0x60)
        c = (uint8_t)(c - 0x20);
    return c - 0x20; /* 0..63 */
}

static void render_text(apple2debug *d, uint16_t base, uint32_t *dst)
{
    uint8_t line[TEXT_COLS];
    int row, col, gy, gx;

    for (row = 0; row < TEXT_ROWS; row++) {
        apple2debug_read_mem(d, text_row_addr(base, row), line, TEXT_COLS);
        for (col = 0; col < TEXT_COLS; col++) {
            int inverse = 0;
            const int idx = glyph_index(line[col], &inverse);
            const uint32_t fg = inverse ? TEXT_BG : TEXT_FG;
            const uint32_t bg = inverse ? TEXT_FG : TEXT_BG;

            for (gy = 0; gy < CELL_H; gy++) {
                /* The glyph is 5x7 in a 7x8 cell: one blank column each side
                 * and a blank scanline underneath, which is the spacing the
                 * character ROM itself provides. */
                const uint8_t bits = gy < 7 ? kFont5x7[idx][gy] : 0;
                uint32_t *out = dst + (size_t)(row * CELL_H + gy) * W +
                                col * CELL_W;
                for (gx = 0; gx < CELL_W; gx++) {
                    const int on = (gx >= 1 && gx <= 5) &&
                                   (bits & (1 << (gx - 1)));
                    out[gx] = on ? fg : bg;
                }
            }
        }
    }
}

/* ---- lo-res ---------------------------------------------------------------
 * Same memory as the text page, read as colour instead: each byte is two
 * stacked blocks, the low nibble on top. 40 x 48 blocks, so each is 7 wide
 * and 4 tall. */

static void render_lores(apple2debug *d, uint16_t base, uint32_t *dst)
{
    uint8_t line[TEXT_COLS];
    int row, col, half, py, px;

    for (row = 0; row < TEXT_ROWS; row++) {
        apple2debug_read_mem(d, text_row_addr(base, row), line, TEXT_COLS);
        for (col = 0; col < TEXT_COLS; col++) {
            for (half = 0; half < 2; half++) {
                const uint32_t rgb =
                    kLoresPalette[half ? (line[col] >> 4) & 0x0F
                                       : line[col] & 0x0F];
                const int y0 = row * CELL_H + half * 4;
                for (py = 0; py < 4; py++) {
                    uint32_t *out =
                        dst + (size_t)(y0 + py) * W + col * CELL_W;
                    for (px = 0; px < CELL_W; px++)
                        out[px] = rgb;
                }
            }
        }
    }
}

/* ---- hi-res ---------------------------------------------------------------
 * 40 bytes per scanline, seven pixels each in bits 0-6; bit 7 shifts the
 * colour pair. Two adjacent lit pixels read as white, and a lone one takes
 * its colour from whether its column is even or odd -- these were never
 * really colours, just what an NTSC set made of a dot at a given phase. */

static void render_hires(apple2debug *d, uint16_t base, uint32_t *dst)
{
    uint8_t line[TEXT_COLS];
    uint8_t on[W];      /* is this pixel lit */
    uint8_t shift[W];   /* the high bit of the byte it came from */
    int y, col, i, x;

    for (y = 0; y < H; y++) {
        apple2debug_read_mem(d, hires_row_addr(base, y), line, TEXT_COLS);

        for (col = 0; col < TEXT_COLS; col++) {
            const uint8_t b = line[col];
            for (i = 0; i < 7; i++) {
                on[col * 7 + i] = (uint8_t)((b >> i) & 1);
                shift[col * 7 + i] = (uint8_t)((b & 0x80) != 0);
            }
        }

        for (x = 0; x < W; x++) {
            uint32_t *out = dst + (size_t)y * W + x;
            if (!on[x]) {
                *out = HR_BLACK;
            } else if ((x > 0 && on[x - 1]) || (x + 1 < W && on[x + 1])) {
                *out = HR_WHITE;
            } else if (x & 1) {
                *out = shift[x] ? HR_ORANGE : HR_GREEN;
            } else {
                *out = shift[x] ? HR_BLUE : HR_VIOLET;
            }
        }
    }
}

/* ---- public --------------------------------------------------------------*/

const char *apple2debug_view_name(int idx)
{
    static const char *const kNames[APPLE2VIEW_COUNT] = {
        "Text page 1",  "Text page 2",  "Lo-res page 1",
        "Lo-res page 2", "Hi-res page 1", "Hi-res page 2",
    };
    if (idx < 0 || idx >= APPLE2VIEW_COUNT)
        return NULL;
    return kNames[idx];
}

uint16_t apple2debug_view_base(apple2debug_view view)
{
    switch (view) {
    case APPLE2VIEW_TEXT_1:
    case APPLE2VIEW_LORES_1: return 0x0400;
    case APPLE2VIEW_TEXT_2:
    case APPLE2VIEW_LORES_2: return 0x0800;
    case APPLE2VIEW_HIRES_1: return 0x2000;
    case APPLE2VIEW_HIRES_2: return 0x4000;
    default:                 return 0;
    }
}

int apple2debug_render_view(apple2debug *d, apple2debug_view view,
                            uint32_t *dst)
{
    const uint16_t base = apple2debug_view_base(view);

    if (!d || !dst)
        return -1;

    switch (view) {
    case APPLE2VIEW_TEXT_1:
    case APPLE2VIEW_TEXT_2:
        render_text(d, base, dst);
        return 0;
    case APPLE2VIEW_LORES_1:
    case APPLE2VIEW_LORES_2:
        render_lores(d, base, dst);
        return 0;
    case APPLE2VIEW_HIRES_1:
    case APPLE2VIEW_HIRES_2:
        render_hires(d, base, dst);
        return 0;
    default:
        memset(dst, 0, (size_t)W * H * sizeof(*dst));
        return -1;
    }
}

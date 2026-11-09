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
#include <stdio.h>
#include <string.h>

#include "apple2debug.h"
#include "a2video.h"

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

uint16_t a2video_text_row_addr(uint16_t base, int row)
{
    return (uint16_t)(base + (row & 7) * 0x80 + (row >> 3) * 0x28);
}

uint16_t a2video_hires_row_addr(uint16_t base, int y)
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
 *
 * The font here is the ORIGINAL character generator: 64 glyphs, $20-$5F, so
 * control codes fold up into @A-Z[\]^_ and lowercase folds to uppercase.
 *
 * KNOWN DIVERGENCE: a //e with its alternate character set enabled really
 * does show lowercase, so this view can read APPLE //E where the machine's
 * own screen reads Apple //e. Rendering that faithfully means tracking the
 * ALTCHARSET soft switch, and this is deliberately a page inspector rather
 * than a second display emulator -- it decodes bytes at an address, not the
 * video hardware's current state. Folding to uppercase is what those bytes
 * mean with the primary character set, and it stays legible either way. */
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
        apple2debug_read(d, a2video_text_row_addr(base, row), line, TEXT_COLS);
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
        apple2debug_read(d, a2video_text_row_addr(base, row), line, TEXT_COLS);
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
    int y, x;

    for (y = 0; y < H; y++) {
        apple2debug_read(d, a2video_hires_row_addr(base, y), line, TEXT_COLS);
        for (x = 0; x < W; x++)
            dst[(size_t)y * W + x] = a2video_hires_pixel(line, x);
    }
}


/* ---- shared with the Video tab (a2video.h) ------------------------------*/

uint32_t a2video_lores_rgb(int n) { return kLoresPalette[n & 15]; }

const char *a2video_lores_name(int n)
{
    static const char *const k[16] = {
        "black", "deep red", "dark blue", "purple", "dark green", "grey",
        "medium blue", "light blue", "brown", "orange", "grey 2", "pink",
        "green", "yellow", "aqua", "white",
    };
    return k[n & 15];
}

int a2video_dlores_rotate(int n) { return ((n << 1) & 0xF) | ((n >> 3) & 1); }

int a2video_dhires_colour(int dots4)
{
    /* AppleWin's DoubleHiresPalIndex, as lo-res colour numbers */
    static const uint8_t k[16] = { 0, 2, 4, 6, 8, 10, 12, 14,
                                   1, 3, 5, 7, 9, 11, 13, 15 };
    return k[dots4 & 15];
}

uint32_t a2video_hires_pixel(const uint8_t line[40], int x)
{
    const int col = x / 7, bit = x % 7;
    const int on = (line[col] >> bit) & 1;
    const int shift = (line[col] & 0x80) != 0;
    int l = 0, r = 0;

    if (!on)
        return HR_BLACK;
    if (x > 0)
        l = (line[(x - 1) / 7] >> ((x - 1) % 7)) & 1;
    if (x + 1 < W)
        r = (line[(x + 1) / 7] >> ((x + 1) % 7)) & 1;
    if (l || r)
        return HR_WHITE;
    if (x & 1)
        return shift ? HR_ORANGE : HR_GREEN;
    return shift ? HR_BLUE : HR_VIOLET;
}

const char *a2video_hires_name(uint32_t rgb)
{
    switch (rgb & 0xFFFFFFu) {
    case HR_BLACK:  return "black";
    case HR_WHITE:  return "white";
    case HR_GREEN:  return "green";
    case HR_VIOLET: return "violet";
    case HR_ORANGE: return "orange";
    case HR_BLUE:   return "blue";
    default:        return "?";
    }
}

/* lowercase a-z, 5x7; written as pictures, converted on first use */
static const char *const kLower[26][7] = {
    {".....",".....",".###.","....#",".####","#...#",".####"},
    {"#....","#....","#.##.","##..#","#...#","##..#","#.##."},
    {".....",".....",".###.","#....","#....","#...#",".###."},
    {"....#","....#",".##.#","#..##","#...#","#..##",".##.#"},
    {".....",".....",".###.","#...#","#####","#....",".###."},
    {"..##.",".#..#",".#...","###..",".#...",".#...",".#..."},
    {".....",".####","#...#","#...#",".####","....#",".###."},
    {"#....","#....","#.##.","##..#","#...#","#...#","#...#"},
    {"..#..",".....",".##..","..#..","..#..","..#..",".###."},
    {"...#.",".....","..##.","...#.","...#.","#..#.",".##.."},
    {"#....","#....","#..#.","#.#..","##...","#.#..","#..#."},
    {".##..","..#..","..#..","..#..","..#..","..#..",".###."},
    {".....",".....","##.#.","#.#.#","#.#.#","#.#.#","#.#.#"},
    {".....",".....","#.##.","##..#","#...#","#...#","#...#"},
    {".....",".....",".###.","#...#","#...#","#...#",".###."},
    {".....",".....","#.##.","##..#","#...#","####.","#...."},
    {".....",".....",".##.#","#..##","#...#",".####","....#"},
    {".....",".....","#.##.","##..#","#....","#....","#...."},
    {".....",".....",".####","#....",".###.","....#","####."},
    {".#...",".#...","###..",".#...",".#...",".#..#","..##."},
    {".....",".....","#...#","#...#","#...#","#..##",".##.#"},
    {".....",".....","#...#","#...#","#...#",".#.#.","..#.."},
    {".....",".....","#...#","#...#","#.#.#","#.#.#",".#.#."},
    {".....",".....","#...#",".#.#.","..#..",".#.#.","#...#"},
    {".....","#...#","#...#","#...#",".####","....#",".###."},
    {".....",".....","#####","...#.","..#..",".#...","#####"},
};

static void lower_rows(int letter, uint8_t rows[7])
{
    int y, x;
    for (y = 0; y < 7; y++) {
        uint8_t v = 0;
        for (x = 0; x < 5; x++)
            if (kLower[letter][y][x] == '#')
                v |= (uint8_t)(1 << x);
        rows[y] = v;
    }
}

char a2video_glyph(uint8_t b, int alt, uint8_t rows[7], int *inverse, int *flash, int *mouse)
{
    uint8_t c = (uint8_t)(b & 0x7F);
    int inv = (b < 0x40), fl = 0, mt = 0;

    if (alt) {
        inv = (b < 0x40) || (b >= 0x60 && b < 0x80);
        if (b >= 0x40 && b < 0x60)
            mt = 1;
    } else {
        fl = (b >= 0x40 && b < 0x80);
    }
    if (inverse) *inverse = inv;
    if (flash) *flash = fl;
    if (mouse) *mouse = mt;

    if (mt) {
        static const uint8_t k[7] = { 0x15, 0x0A, 0x15, 0x0A, 0x15, 0x0A, 0x15 };
        memcpy(rows, k, 7);
        return '~';
    }
    if (alt && c >= 0x61 && c <= 0x7A) {
        lower_rows(c - 0x61, rows);
        return (char)c;
    }
    if (alt && (c == 0x60 || c > 0x7A))
        c = (uint8_t)(c - 0x20);        /* ` { | } ~ DEL: fold to @ [ \ ] ^ _ */
    if (c < 0x20)
        c = (uint8_t)(c + 0x40);
    if (!alt && c >= 0x60)
        c = (uint8_t)(c - 0x20);        /* the original generator has no lowercase */
    memcpy(rows, kFont5x7[c - 0x20], 7);
    return (char)c;
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

/* ---- inspector -----------------------------------------------------------*/

static const char *const kLoresNames[16] = {
    "black", "magenta", "dark blue", "purple", "dark green", "grey",
    "medium blue", "light blue", "brown", "orange", "grey", "pink",
    "green", "yellow", "aqua", "white",
};

static void inspect_text(apple2debug *d, uint16_t base, int page, int col,
                         int row, apple2debug_pixel *o)
{
    int inverse = 0;
    uint8_t v = 0;
    char ch;
    const char *kind;

    o->addr = (uint16_t)(a2video_text_row_addr(base, row) + col);
    apple2debug_read(d, o->addr, &v, 1);
    o->value = v;
    o->rx = col * CELL_W; o->ry = row * CELL_H; o->rw = CELL_W; o->rh = CELL_H;
    ch = (char)(glyph_index(v, &inverse) + 0x20);
    kind = v >= 0x80 ? "normal" : v >= 0x40 ? "flash" : "inverse";
    snprintf(o->text, sizeof(o->text),
             "Text page %d  row %d col %d\n$%04X = $%02X  '%c'  %s",
             page, row, col, o->addr, v, ch, kind);
}

static void inspect_lores(apple2debug *d, uint16_t base, int page, int col,
                          int row, int half, apple2debug_pixel *o)
{
    uint8_t v = 0;
    const int lo = 0, hi = 1;

    (void)lo;
    o->addr = (uint16_t)(a2video_text_row_addr(base, row) + col);
    apple2debug_read(d, o->addr, &v, 1);
    o->value = v;
    o->rx = col * CELL_W; o->ry = row * CELL_H + half * 4;
    o->rw = CELL_W; o->rh = 4;
    snprintf(o->text, sizeof(o->text),
             "Lo-res page %d  block %d,%d\n$%04X = $%02X  low nibble %X (%s) "
             "top, high nibble %X (%s) bottom \xE2\x80\x94 this block: %s",
             page, col, row * 2 + half, o->addr, v, v & 0x0F,
             kLoresNames[v & 0x0F], v >> 4, kLoresNames[v >> 4],
             half == hi ? "high nibble (bottom)" : "low nibble (top)");
}

static void inspect_hires(apple2debug *d, uint16_t base, int page, int x,
                          int y, apple2debug_pixel *o)
{
    uint8_t line[TEXT_COLS];
    const int col = x / 7, bit = x % 7;
    const uint8_t v = (apple2debug_read(d, a2video_hires_row_addr(base, y), line,
                                        TEXT_COLS), line[col]);
    const int lit = (v >> bit) & 1;
    const int shift = (v & 0x80) != 0;
    int left = 0, right = 0;
    const char *clr, *nb;

    if (x > 0)
        left = (line[(x - 1) / 7] >> ((x - 1) % 7)) & 1;
    if (x + 1 < W)
        right = (line[(x + 1) / 7] >> ((x + 1) % 7)) & 1;

    if (!lit)
        clr = "black";
    else if (left || right)
        clr = "white";
    else if (x & 1)
        clr = shift ? "orange" : "green";
    else
        clr = shift ? "blue" : "violet";
    nb = left && right ? "left/right" : left ? "left" : right ? "right"
                                                              : "none";

    o->addr = (uint16_t)(a2video_hires_row_addr(base, y) + col);
    o->value = v;
    o->rx = col * 7; o->ry = y; o->rw = 7; o->rh = 1;
    snprintf(o->text, sizeof(o->text),
             "Hi-res page %d  x %d y %d\n$%04X = $%02X  pixel bit %d = %d  "
             "palette bit 7 = %d (%s)\nneighbours lit: %s\ncolour: %s",
             page, x, y, o->addr, v, bit, lit, shift,
             shift ? "blue/orange" : "violet/green", nb, clr);
}

int apple2debug_view_inspect(apple2debug *d, apple2debug_view view, int x,
                             int y, apple2debug_pixel *out)
{
    const uint16_t base = apple2debug_view_base(view);

    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!d || view < 0 || view >= APPLE2VIEW_COUNT || x < 0 || y < 0 ||
        x >= W || y >= H)
        return -1;

    switch (view) {
    case APPLE2VIEW_TEXT_1:
    case APPLE2VIEW_TEXT_2:
        inspect_text(d, base, view == APPLE2VIEW_TEXT_1 ? 1 : 2, x / CELL_W,
                     y / CELL_H, out);
        break;
    case APPLE2VIEW_LORES_1:
    case APPLE2VIEW_LORES_2:
        inspect_lores(d, base, view == APPLE2VIEW_LORES_1 ? 1 : 2, x / CELL_W,
                      y / CELL_H, (y % CELL_H) >= 4, out);
        break;
    default:
        inspect_hires(d, base, view == APPLE2VIEW_HIRES_1 ? 1 : 2, x, y, out);
        break;
    }
    out->valid = 1;
    return 0;
}

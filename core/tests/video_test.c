/*
 * Exercises the debugger's Apple II video page decoders against a real
 * machine: writes known bytes into a page, renders it, and checks the pixels
 * that came out.
 *
 * The interesting part is the addressing. The Apple II's screen rows are
 * interleaved, so "row 1" is not 40 bytes after row 0 -- and a decoder that
 * gets that wrong still produces a plausible-looking picture, just scrambled.
 * These cases pin the three bands of the text layout and the extra scanline
 * interleave of hi-res, which is where such a bug would actually live.
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

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            g_fail++;                                            \
        }                                                        \
    } while (0)

#define W APPLE2VIEW_WIDTH
#define H APPLE2VIEW_HEIGHT

static uint32_t px(const uint32_t *fb, int x, int y)
{
    return fb[(size_t)y * W + x] & 0xFFFFFFu;
}

static int wait_paused(apple2debug *d, int ms)
{
    while (ms > 0) {
        if (apple2debug_is_paused(d)) return 1;
        usleep(5000);
        ms -= 5;
    }
    return 0;
}

static void fill(apple2debug *d, uint16_t base, int len, uint8_t value)
{
    uint8_t *buf = malloc((size_t)len);
    if (!buf) return;
    memset(buf, value, (size_t)len);
    apple2debug_write_mem(d, base, buf, len);
    free(buf);
}

int main(void)
{
    apple2session *s;
    apple2session_start_opts o;
    apple2debug *d;
    uint32_t *fb;
    int i;

    /* ---- pure lookups, no machine needed -------------------------------- */
    CHECK(apple2debug_view_base(APPLE2VIEW_TEXT_1) == 0x0400, "text 1 base");
    CHECK(apple2debug_view_base(APPLE2VIEW_TEXT_2) == 0x0800, "text 2 base");
    CHECK(apple2debug_view_base(APPLE2VIEW_LORES_1) == 0x0400, "lores 1 base");
    CHECK(apple2debug_view_base(APPLE2VIEW_HIRES_1) == 0x2000, "hires 1 base");
    CHECK(apple2debug_view_base(APPLE2VIEW_HIRES_2) == 0x4000, "hires 2 base");

    for (i = 0; i < APPLE2VIEW_COUNT; i++)
        CHECK(apple2debug_view_name(i) != NULL, "view name %d is NULL", i);
    CHECK(apple2debug_view_name(APPLE2VIEW_COUNT) == NULL,
          "view name past the end is not NULL");
    CHECK(apple2debug_view_name(-1) == NULL, "view name -1 is not NULL");

    s = apple2session_new(NULL);
    if (!s) {
        fprintf(stderr, "video: could not create the session\n");
        return 77;
    }
    apple2session_default_opts(s, &o);
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.slot7 = "Empty";
    if (apple2session_start(s, &o) != 0) {
        fprintf(stderr, "video: %s\n", apple2session_last_error(s));
        apple2session_free(s);
        return 77;
    }

    d = apple2session_debugger(s);
    CHECK(d != NULL, "debugger engine was not created");
    if (!d) { apple2session_free(s); return 1; }

    fb = malloc((size_t)W * H * sizeof(*fb));
    if (!fb) { apple2session_free(s); return 1; }

    usleep(300000);

    /* Paused, so the machine cannot scribble over the bytes under test. */
    apple2debug_pause(d);
    CHECK(wait_paused(d, 2000), "did not pause within 2s");

    /* ---- text: the three interleaved bands ------------------------------ */
    {
        /* $A0 is a normal space, so a cleared page renders all background. */
        const uint8_t inv_space = 0x20; /* < $40 -> inverse: a solid block */
        fill(d, 0x0400, 0x400, 0xA0);

        /* Row 0 is at the base, row 1 a whole 0x80 further on, and row 8 is
         * only 0x28 in -- the band boundary. Getting any of these wrong is
         * the classic Apple II screen bug. */
        apple2debug_write_mem(d, 0x0400, &inv_space, 1); /* row 0,  col 0 */
        apple2debug_write_mem(d, 0x0480, &inv_space, 1); /* row 1,  col 0 */
        apple2debug_write_mem(d, 0x0428, &inv_space, 1); /* row 8,  col 0 */
        apple2debug_write_mem(d, 0x0450, &inv_space, 1); /* row 16, col 0 */

        CHECK(apple2debug_render_view(d, APPLE2VIEW_TEXT_1, fb) == 0,
              "render text 1 failed");

        /* An inverse space is a filled cell: white where the rest is black. */
        CHECK(px(fb, 3, 3) == 0xFFFFFF, "row 0 not inverse");
        CHECK(px(fb, 3, 8 + 3) == 0xFFFFFF, "row 1 ($0480) not inverse");
        CHECK(px(fb, 3, 8 * 8 + 3) == 0xFFFFFF, "row 8 ($0428) not inverse");
        CHECK(px(fb, 3, 16 * 8 + 3) == 0xFFFFFF, "row 16 ($0450) not inverse");
        /* ...and the neighbours are not. */
        CHECK(px(fb, 3, 2 * 8 + 3) == 0x000000, "row 2 unexpectedly inverse");
        /* Column 1 starts at x = 7 (a 7-pixel cell). */
        CHECK(px(fb, 7 + 3, 3) == 0x000000,
              "row 0 col 1 unexpectedly inverse");
    }

    /* ---- lo-res: low nibble on top -------------------------------------- */
    {
        const uint8_t both = 0x0F; /* low = 15 (white), high = 0 (black) */
        fill(d, 0x0400, 0x400, 0x00);
        apple2debug_write_mem(d, 0x0400, &both, 1);

        CHECK(apple2debug_render_view(d, APPLE2VIEW_LORES_1, fb) == 0,
              "render lores 1 failed");
        CHECK(px(fb, 3, 1) == 0xFFFFFF, "lo-res top block not white");
        CHECK(px(fb, 3, 5) == 0x000000, "lo-res bottom block not black");
        CHECK(px(fb, 8, 1) == 0x000000, "lo-res spilled into the next column");
    }

    /* ---- hi-res: the extra scanline interleave, and colour --------------- */
    {
        const uint8_t lone_lo = 0x01; /* bit 0 only, high bit clear */
        const uint8_t lone_hi = 0x81; /* bit 0 only, high bit set   */
        const uint8_t pair = 0x03;    /* bits 0 and 1 -> white      */

        fill(d, 0x2000, 0x2000, 0x00);
        apple2debug_write_mem(d, 0x2000, &lone_lo, 1); /* y = 0  */
        apple2debug_write_mem(d, 0x2400, &lone_hi, 1); /* y = 1  */
        apple2debug_write_mem(d, 0x2080, &pair, 1);    /* y = 8  */

        CHECK(apple2debug_render_view(d, APPLE2VIEW_HIRES_1, fb) == 0,
              "render hires 1 failed");
        /* Column 0 is even: violet with the high bit clear, blue with it
         * set -- the whole point of that bit. */
        CHECK(px(fb, 0, 0) == 0xFF44FD, "y0 lone pixel not violet (got %06X)",
              px(fb, 0, 0));
        CHECK(px(fb, 0, 1) == 0x14CFFD,
              "y1 ($2400) lone pixel not blue (got %06X)", px(fb, 0, 1));
        /* Two adjacent lit pixels read as white, not as two colours. */
        CHECK(px(fb, 0, 8) == 0xFFFFFF, "y8 ($2080) pair not white");
        CHECK(px(fb, 1, 8) == 0xFFFFFF, "y8 second pixel of pair not white");
        CHECK(px(fb, 2, 8) == 0x000000, "y8 third pixel unexpectedly lit");
        /* An untouched scanline stays black. */
        CHECK(px(fb, 0, 2) == 0x000000, "y2 unexpectedly lit");
    }

    /* ---- an unknown view blanks rather than scribbling ------------------- */
    CHECK(apple2debug_render_view(d, (apple2debug_view)999, fb) == -1,
          "unknown view did not report an error");
    CHECK(px(fb, 10, 10) == 0x000000, "unknown view did not blank");

    apple2debug_resume(d);
    free(fb);
    apple2session_free(s);

    if (g_fail) {
        fprintf(stderr, "video: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("video: OK\n");
    return 0;
}

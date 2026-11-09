/*
 * a2video -- the pieces of the Apple II video decoders that the page viewer
 * (a2video.c's apple2debug_render_view) and the Video tab controller
 * (vidtab.c) share: the palettes, the character generator, address
 * arithmetic and the hi-res colour rule. Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_A2VIDEO_H
#define APPLE2_A2VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* text and lo-res: 24 rows in three interleaved bands of eight */
uint16_t a2video_text_row_addr(uint16_t base, int row);
/* hi-res: 8 scanlines interleaved within each of those rows */
uint16_t a2video_hires_row_addr(uint16_t base, int y);

/* the 16 lo-res colours (0xRRGGBB, no alpha) and their names */
uint32_t    a2video_lores_rgb(int n);
const char *a2video_lores_name(int n);
/* the colour lo-res nibble an aux nibble displays as in double lo-res (rotate left one bit) */
int         a2video_dlores_rotate(int nibble);
/* double hi-res: the lo-res colour index for 4 consecutive dots (first dot = bit 0) */
int         a2video_dhires_colour(int dots4);

/* hi-res pixel x (0..279) of a 40-byte scanline: 0xRRGGBB, and its colour name */
uint32_t    a2video_hires_pixel(const uint8_t line[40], int x);
const char *a2video_hires_name(uint32_t rgb);

/*
 * The glyph a screen byte shows. rows[0..6] are the 5x7 bitmap (bit 0 =
 * leftmost column). inverse: draw light on dark. flash: the byte flashes on a
 * real machine (drawn steady). alt: ALTCHARSET on (lowercase, MouseText).
 * mouse: the byte is a MouseText glyph (rows are a placeholder). Returns the
 * ASCII character it stands for.
 */
char a2video_glyph(uint8_t b, int alt, uint8_t rows[7], int *inverse, int *flash, int *mouse);

#ifdef __cplusplus
}
#endif

#endif

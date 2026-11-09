/*
 * The debugger's Video tab (Win32): a thin view over core/include/apple2vidtab.h. It is one
 * child window holding all its own controls, so the debugger shows, hides, places and pops it
 * out as a unit.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_WINDOWS_DBG_VIDEO_H
#define APPLE2_WINDOWS_DBG_VIDEO_H

#include <windows.h>

#include "apple2session.h"

/* "Show in Memory": the debugger jumps its Memory tab to bank (an apple2debug_bank) at lo..hi. */
typedef void (*dbg_video_show_memory_fn)(void *ctx, int bank, unsigned lo, unsigned hi);

/* The tab's page, a child of `parent`. ui and mono are the fonts it uses. */
HWND dbg_video_create(HWND parent, HFONT ui, HFONT mono, int dpi, apple2session *session,
                      dbg_video_show_memory_fn show_memory, void *ctx);
/* Move and size the page (it lays out its own controls). */
void dbg_video_place(HWND page, int x, int y, int w, int h);
/* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
void dbg_video_refresh(HWND page, int force);
/* The debugger's fonts or DPI changed. */
void dbg_video_fonts(HWND page, HFONT ui, HFONT mono, int dpi);
/* Capture hooks (APPLE2_DEBUGGER_VIEW: view index 0..5 = text1, text2, lores1, lores2, hires1,
 * hires2; APPLE2_VIDEO_PIN: a point of the zoomed picture). */
void dbg_video_env_view(HWND page, int index);
void dbg_video_env_pin(HWND page, int x, int y);

#endif

/*
 * Win32 helpers the Controllers and Input settings windows share, on top of the
 * DPI, font and text helpers the Help window already carries (help_compat.h):
 * a font on a window and its children, UTF-8 into a wide control, and a
 * window's outer size for a client size at its own DPI.
 *
 * The manifest makes the process per-monitor DPI aware (v2), so nothing is
 * scaled for us: every window lays itself out in 96-dpi units through
 * apple2_scale() and remakes its fonts on WM_DPICHANGED.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_WIN_COMMON_H
#define APPLE2_WIN_COMMON_H

#include "help/help_compat.h"

/* A font on a window and every child of it. */
static inline BOOL CALLBACK apple2_set_font_proc(HWND child, LPARAM font)
{
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static inline void apple2_set_font_all(HWND parent, HFONT font)
{
    EnumChildWindows(parent, apple2_set_font_proc, (LPARAM)font);
}

static inline void apple2_set_text_utf8(HWND ctrl, const char *utf8)
{
    wchar_t *wide = apple2_wide_dup(utf8);
    if (wide) { SetWindowTextW(ctrl, wide); free(wide); }
}

/* A window's outer size for a client size, at its own style and DPI (the
 * frame and caption grow with the DPI too: AdjustWindowRectExForDpi, Windows
 * 10 1607 and later, looked up). */
static inline SIZE apple2_outer_size(HWND hwnd, int clientw, int clienth, BOOL menu)
{
    typedef BOOL (WINAPI *adjust_fn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static adjust_fn adjust;
    static int looked;
    RECT rect = { 0, 0, clientw, clienth };
    const DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    const DWORD exstyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SIZE outer;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        adjust = user ? (adjust_fn)(void (*)(void))GetProcAddress(user, "AdjustWindowRectExForDpi") : NULL;
        looked = 1;
    }
    if (!adjust || !adjust(&rect, style, menu, exstyle, (UINT)apple2_window_dpi(hwnd)))
        AdjustWindowRectEx(&rect, style, menu, exstyle);
    outer.cx = rect.right - rect.left;
    outer.cy = rect.bottom - rect.top;
    return outer;
}

#endif /* APPLE2_WIN_COMMON_H */

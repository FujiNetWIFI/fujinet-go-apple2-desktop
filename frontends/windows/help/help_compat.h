/*
 * The few Win32 helpers the Help window needs (DPI, a scaled font, a UTF-8
 * message box). This frontend has no shared win_common.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_HELP_COMPAT_H
#define APPLE2_HELP_COMPAT_H

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static inline int apple2_system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d > 0 ? d : 96;
}

/* GetDpiForWindow is Windows 10 1607 and later: looked up, not imported. */
static inline int apple2_window_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    static dpi_fn fn;
    static int looked;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (dpi_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d) return (int)d;
    }
    return apple2_system_dpi();
}

static inline int apple2_scale(int v, int dpi)
{
    return MulDiv(v, dpi, 96);
}

/* The message font at dpi (system metrics come at the system DPI), at its
 * own size (px 0) or px pixels at 96 dpi; face NULL keeps the message
 * font's, otherwise a fixed-pitch face. */
static inline HFONT apple2_make_font(int dpi, int px, int weight, const char *face)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        lf = ncm.lfMessageFont;
    else
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    if (px > 0)
        lf.lfHeight = -MulDiv(px, dpi, 96);
    else
        lf.lfHeight = -MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), dpi, apple2_system_dpi());
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    if (face) {
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    }
    return CreateFontIndirectA(&lf);
}

static inline wchar_t *apple2_wide_dup(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, w, n);
    return w;
}

static inline int apple2_message(HWND owner, const char *text, const char *title, UINT flags)
{
    wchar_t *wt = apple2_wide_dup(text), *wc = apple2_wide_dup(title);
    int rc = MessageBoxW(owner, wt ? wt : L"", wc ? wc : L"", flags);
    free(wt);
    free(wc);
    return rc;
}

#endif /* APPLE2_HELP_COMPAT_H */

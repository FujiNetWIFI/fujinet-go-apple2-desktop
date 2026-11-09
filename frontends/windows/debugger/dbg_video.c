/*
 * The debugger's Video tab (Win32).
 *
 * Everything that decides what to show lives in the core (apple2vidtab.h): this file is
 * controls and a GDI picture. The picture is the controller's cached 0xFFRRGGBB buffer drawn
 * 1:1 with StretchDIBits (BI_RGB ignores the alpha byte), double buffered; a mouse move
 * redraws only the marks. Only the section showing is filled in. Every control is a child of
 * the page, so the debugger shows, hides, places and pops the tab out as one window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apple2debug.h"
#include "apple2vidtab.h"

#define PAGE_CLASS L"FujiNetGoApple2VideoPage"
#define PIC_CLASS L"FujiNetGoApple2VideoPic"
#define COL_CLASS L"FujiNetGoApple2VideoColours"

#define ACCENT RGB(0xf4, 0x43, 0x36)
#define MAX_OVERLAYS 16
#define MAX_ZOOMS 16
#define TOOL_MAX 32

enum {
    IDV_VIEW = 3000, IDV_ZOOM, IDV_FOLLOW, IDV_MODE, IDV_PAGE, IDV_MIXED, IDV_BREAK, IDV_SHOWMEM, IDV_STATUS, IDV_NOTE,
    IDV_SEC, IDV_PIXEL, IDV_UNPIN, IDV_SWITCHES, IDV_BRK_SW, IDV_BEAM, IDV_TIMING, IDV_PIC, IDV_COLS,
    IDV_LABEL_VIEW, IDV_LABEL_ZOOM, IDV_LABEL_MODE, IDV_LABEL_PAGE,
    IDV_OV0 = 3200
};

typedef struct {
    unsigned char hot[APPLE2VID_SW_COUNT];
    int nhot;
    uint64_t sig;
    int have_sig;
} listfill;

typedef struct {
    HWND page, view, zoom, follow, mode, pgsel, mixed, status, note, pic, sec;
    HWND ov[MAX_OVERLAYS];
    unsigned ov_bit[MAX_OVERLAYS];
    int nov;
    HWND label_view, label_zoom, label_mode, label_page;
    HWND pixel, brk, showmem, unpin, switches, brk_sw, cols, beam, timing;
    HFONT ui, mono;
    int dpi;
    apple2vid_tab *tab;
    dbg_video_show_memory_fn show_memory;
    void *ctx;
    int updating;
    int scroll_x, scroll_y;
    int tracking;
    int col_tracking;
    int zoom_val[MAX_ZOOMS], nzoom;
    listfill sf;
    BITMAPINFO bmi;
} vidwin;

#define S(v, n) MulDiv((n), (v)->dpi, 96)

/* ---- text ------------------------------------------------------------------------ */

/* UTF-8 to UTF-16; '\n' becomes CR LF (multi-line controls want it) */
static wchar_t *wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0), i;
    wchar_t *w = malloc(((size_t)n + 1) * sizeof *w), *o, *d;
    if (!w) return NULL;
    o = malloc(((size_t)n * 2 + 2) * sizeof *w);
    if (!o) {
        free(w);
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    d = o;
    for (i = 0; w[i]; i++) {
        if (w[i] == L'\n') *d++ = L'\r';
        *d++ = w[i];
    }
    *d = 0;
    free(w);
    return o;
}

/* a control's text, set only when it changed (no flicker on the live refresh) */
static void set_text(HWND h, const char *utf8)
{
    wchar_t *w = wide(utf8), cur[2048];
    if (!w) return;
    GetWindowTextW(h, cur, 2048);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
    free(w);
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static COLORREF rgb_ref(uint32_t rgb)
{
    return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

/* ---- the picture --------------------------------------------------------------------- */

static void pic_update_scroll(vidwin *v)
{
    RECT rc;
    SCROLLINFO si;
    int pw = 0, ph = 0;
    GetClientRect(v->pic, &rc);
    apple2vid_tab_picture(v->tab, &pw, &ph);
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = pw > 0 ? pw - 1 : 0;
    si.nPage = (UINT)rc.right;
    if (v->scroll_x > pw - rc.right) v->scroll_x = pw > rc.right ? pw - rc.right : 0;
    si.nPos = v->scroll_x;
    SetScrollInfo(v->pic, SB_HORZ, &si, TRUE);
    si.nMax = ph > 0 ? ph - 1 : 0;
    si.nPage = (UINT)rc.bottom;
    if (v->scroll_y > ph - rc.bottom) v->scroll_y = ph > rc.bottom ? ph - rc.bottom : 0;
    si.nPos = v->scroll_y;
    SetScrollInfo(v->pic, SB_VERT, &si, TRUE);
}

static void pic_paint(vidwin *v, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    RECT rc;
    HBITMAP bmp, old;
    int pw = 0, ph = 0, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 24));
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, bg);
    px = apple2vid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = apple2vid_tab_why(v->tab);
        wchar_t *w = wide(why && *why ? why : "Nothing to show");
        RECT tr = rc;
        InflateRect(&tr, -S(v, 10), -S(v, 10));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(140, 140, 148));
        SelectObject(mem, v->ui);
        if (w) DrawTextW(mem, w, -1, &tr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        free(w);
    } else {
        HPEN pen_h, pen_p, pen_k, oldpen;
        HGDIOBJ oldbr;
        v->bmi.bmiHeader.biWidth = pw;      /* the buffer is pw words per row, whatever the view */
        v->bmi.bmiHeader.biHeight = -ph;    /* top-down */
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchDIBits(mem, -v->scroll_x, -v->scroll_y, pw, ph, 0, 0, pw, ph, px, &v->bmi, DIB_RGB_COLORS, SRCCOPY);
        marks = apple2vid_tab_marks(v->tab, hover, pin);
        oldbr = SelectObject(mem, GetStockObject(NULL_BRUSH));
        pen_h = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        pen_p = CreatePen(PS_SOLID, 2, RGB(255, 217, 26));
        pen_k = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        if (marks & 1) {            /* hover: a white box just outside the pixel, over a dark one */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, hover[0] - v->scroll_x - 2, hover[1] - v->scroll_y - 2, hover[0] - v->scroll_x + hover[2] + 2,
                      hover[1] - v->scroll_y + hover[3] + 2);
            SelectObject(mem, pen_h);
            Rectangle(mem, hover[0] - v->scroll_x - 1, hover[1] - v->scroll_y - 1, hover[0] - v->scroll_x + hover[2] + 1,
                      hover[1] - v->scroll_y + hover[3] + 1);
            SelectObject(mem, oldpen);
        }
        if (marks & 2) {            /* pin: a thick yellow box */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, pin[0] - v->scroll_x - 3, pin[1] - v->scroll_y - 3, pin[0] - v->scroll_x + pin[2] + 3,
                      pin[1] - v->scroll_y + pin[3] + 3);
            SelectObject(mem, pen_p);
            Rectangle(mem, pin[0] - v->scroll_x - 1, pin[1] - v->scroll_y - 1, pin[0] - v->scroll_x + pin[2] + 1,
                      pin[1] - v->scroll_y + pin[3] + 1);
            SelectObject(mem, oldpen);
        }
        SelectObject(mem, oldbr);
        DeleteObject(pen_h);
        DeleteObject(pen_p);
        DeleteObject(pen_k);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    DeleteObject(bg);
    EndPaint(h, &ps);
}

static void update_pixel(vidwin *v)
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(v->tab);
    set_text(v->pixel, p && p->valid && p->text[0] ? p->text
                                                   : "Move over the picture to inspect a pixel.\n"
                                                     "Click to pin it; click again to unpin.\n"
                                                     "Double-click to break on writes to it.");
    if (v->cols) InvalidateRect(v->cols, NULL, FALSE);
}

static void set_note(vidwin *v, const char *text)
{
    set_text(v->note, text);
}

/* Break on writes to the inspected pixel's byte (and its aux byte), and say what was done. */
static void break_on_pixel(vidwin *v)
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(v->tab);
    char msg[200];
    int id = apple2vid_tab_break_on_pixel(v->tab);
    if (id >= 0 && p) {
        size_t len = (size_t)snprintf(msg, sizeof msg, "Break on writes to");
        if (p->main_addr >= 0) len += (size_t)snprintf(msg + len, sizeof msg - len, " $%04X", p->main_addr);
        if (p->aux_addr >= 0) len += (size_t)snprintf(msg + len, sizeof msg - len, " aux $%04X", p->aux_addr);
        snprintf(msg + len, sizeof msg - len, " (see Breakpoints)");
    } else {
        snprintf(msg, sizeof msg, "Nothing to break on: pin or hover a pixel inside the data area");
    }
    set_note(v, msg);
}

/* Show in Memory: the pixel's main address, or the aux bank when only aux applies. */
static void show_in_memory(vidwin *v)
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(v->tab);
    if (!p || !p->valid || (p->main_addr < 0 && p->aux_addr < 0)) {
        set_note(v, "Hover or click a pixel inside the data area first");
        return;
    }
    set_note(v, "");
    if (!v->show_memory) return;
    if (p->main_addr >= 0) v->show_memory(v->ctx, APPLE2_BANK_MAIN, (unsigned)p->main_addr, (unsigned)p->main_addr);
    else v->show_memory(v->ctx, APPLE2_BANK_AUX, (unsigned)p->aux_addr, (unsigned)p->aux_addr);
}

static LRESULT CALLBACK pic_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        pic_paint(v, h);
        return 0;
    case WM_SIZE:
        pic_update_scroll(v);
        return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        SCROLLINFO si;
        int bar = msg == WM_HSCROLL ? SB_HORZ : SB_VERT, pos;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_ALL;
        GetScrollInfo(h, bar, &si);
        pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos -= 16; break;
        case SB_LINERIGHT: pos += 16; break;
        case SB_PAGELEFT: pos -= (int)si.nPage; break;
        case SB_PAGERIGHT: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
        }
        if (pos < 0) pos = 0;
        if (bar == SB_HORZ) v->scroll_x = pos; else v->scroll_y = pos;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        v->scroll_y -= GET_WHEEL_DELTA_WPARAM(wp) / 3;
        if (v->scroll_y < 0) v->scroll_y = 0;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp) + v->scroll_x, y = (short)HIWORD(lp) + v->scroll_y;
        if (!v->tracking) {
            TRACKMOUSEEVENT t;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            v->tracking = 1;
        }
        if (apple2vid_tab_hover(v->tab, x, y)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        v->tracking = 0;
        if (apple2vid_tab_hover(v->tab, -1, -1)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        apple2vid_tab_hover(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        apple2vid_tab_pin(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    case WM_LBUTTONDBLCLK:
        apple2vid_tab_hover(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        break_on_pixel(v);
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- the Colours panel: the palette of the Decoded picture, with use counts --------------- */

#define PAL_ROW 22

static int pal_row_h(vidwin *v, const RECT *rc, int n)
{
    int rh = S(v, PAL_ROW);
    if (n > 0 && rc->bottom / n < rh) rh = rc->bottom / n;
    return rh < S(v, 12) ? S(v, 12) : rh;
}

static void col_paint(vidwin *v, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    HBITMAP bmp, old;
    RECT rc;
    const apple2vid_pal_entry *pe;
    int np = apple2vid_tab_palette(v->tab, &pe), i, rh;
    HFONT oldf;
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(mem, TRANSPARENT);
    oldf = SelectObject(mem, v->mono);
    rh = pal_row_h(v, &rc, np);
    for (i = 0; i < np; i++) {
        RECT r, row;
        char line[96];
        wchar_t *w;
        HBRUSH b = CreateSolidBrush(rgb_ref(pe[i].rgb));
        HBRUSH fb = CreateSolidBrush(RGB(0, 0, 0));
        row.left = 0;
        row.top = i * rh;
        row.right = rc.right;
        row.bottom = row.top + rh;
        if (pe[i].inspected) {              /* the inspected pixel's colour: an accent row */
            HBRUSH ab = CreateSolidBrush(RGB(0xf6, 0xd6, 0xd2));
            FillRect(mem, &row, ab);
            DeleteObject(ab);
            ab = CreateSolidBrush(ACCENT);
            FrameRect(mem, &row, ab);
            DeleteObject(ab);
        }
        r.left = S(v, 3);
        r.top = row.top + S(v, 2);
        r.right = r.left + rh - S(v, 4);
        r.bottom = r.top + rh - S(v, 4);
        FillRect(mem, &r, b);
        FrameRect(mem, &r, fb);
        DeleteObject(b);
        DeleteObject(fb);
        snprintf(line, sizeof line, "%-16s #%06X  %ld px", pe[i].name, pe[i].rgb & 0xFFFFFFu, pe[i].uses);
        w = wide(line);
        SetTextColor(mem, pe[i].inspected ? RGB(0, 0, 0) : GetSysColor(COLOR_WINDOWTEXT));
        if (w) TextOutW(mem, rh + S(v, 6), row.top + (rh - S(v, 15)) / 2, w, (int)wcslen(w));
        free(w);
    }
    if (np == 0) {
        static const wchar_t none[] = L"No palette to show";
        SetTextColor(mem, GetSysColor(COLOR_GRAYTEXT));
        TextOutW(mem, S(v, 4), S(v, 4), none, (int)wcslen(none));
    }
    SelectObject(mem, oldf);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK col_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        col_paint(v, h);
        return 0;
    case WM_SIZE:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- sections ----------------------------------------------------------------------- */

static void lv_cell(HWND lv, int row, int col, const char *text)
{
    wchar_t *w = wide(text);
    LVITEMW it;
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = w ? w : L"";
    if (col == 0) {
        it.mask = LVIF_TEXT;
        it.iItem = row;
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    } else {
        SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
    }
    free(w);
}

static void fill_switches(vidwin *v)
{
    const apple2vid_switch *sw;
    int n = apple2vid_tab_switches(v->tab, &sw), i, sel;
    uint64_t sig = 1469598103934665603ull;
    unsigned char hot[APPLE2VID_SW_COUNT];
    for (i = 0; i < n; i++) {
        int shown = sw[i].age < APPLE2VID_SW_FADE ? sw[i].age : APPLE2VID_SW_FADE;
        sig = fnv(sig, sw[i].name, strlen(sw[i].name));
        sig = fnv(sig, &sw[i].on, sizeof sw[i].on);
        sig = fnv(sig, &shown, sizeof shown);
        sig = fnv(sig, sw[i].meaning, strlen(sw[i].meaning));
    }
    if (v->sf.have_sig && v->sf.sig == sig) return;
    v->sf.sig = sig;
    v->sf.have_sig = 1;
    sel = (int)SendMessageW(v->switches, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    SendMessageW(v->switches, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v->switches, LVM_DELETEALLITEMS, 0, 0);
    memset(hot, 0, sizeof hot);
    for (i = 0; i < n && i < APPLE2VID_SW_COUNT; i++) {
        char b[160];
        lv_cell(v->switches, i, 0, sw[i].name);
        snprintf(b, sizeof b, "%s", sw[i].on ? "on" : "off");
        lv_cell(v->switches, i, 1, b);
        if (sw[i].age >= APPLE2VID_SW_FADE) snprintf(b, sizeof b, "%d+", APPLE2VID_SW_FADE);
        else snprintf(b, sizeof b, "%d", sw[i].age);
        lv_cell(v->switches, i, 2, b);
        snprintf(b, sizeof b, "$%04X", sw[i].set_addr);
        lv_cell(v->switches, i, 3, b);
        snprintf(b, sizeof b, "$%04X", sw[i].clr_addr);
        lv_cell(v->switches, i, 4, b);
        lv_cell(v->switches, i, 5, sw[i].meaning);
        hot[i] = (unsigned char)(sw[i].age < APPLE2VID_SW_FADE);
    }
    memcpy(v->sf.hot, hot, sizeof hot);
    v->sf.nhot = n < APPLE2VID_SW_COUNT ? n : APPLE2VID_SW_COUNT;
    if (sel >= 0 && sel < n) {
        LVITEMW it;
        memset(&it, 0, sizeof it);
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        it.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(v->switches, LVM_SETITEMSTATE, (WPARAM)sel, (LPARAM)&it);
    }
    SendMessageW(v->switches, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v->switches, NULL, TRUE);
}

static void break_on_switch(vidwin *v)
{
    const apple2vid_switch *sw;
    char msg[160];
    int n = apple2vid_tab_switches(v->tab, &sw);
    int sel = (int)SendMessageW(v->switches, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel < 0 || sel >= n) {
        set_note(v, "Select a switch first");
        return;
    }
    if (apple2vid_tab_break_on_switch(v->tab, sel) >= 0)
        snprintf(msg, sizeof msg, "Break on %s: read/write of $%04X and $%04X (see Breakpoints)", sw[sel].name,
                 sw[sel].set_addr, sw[sel].clr_addr);
    else
        snprintf(msg, sizeof msg, "Could not add the breakpoint");
    set_note(v, msg);
}

static void fill_timing(vidwin *v)
{
    const char *const *lines;
    int n = apple2vid_tab_timing(v->tab, &lines), i;
    char buf[2048];
    size_t len = 0;
    buf[0] = 0;
    for (i = 0; i < n && len < sizeof buf - 160; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) len += (size_t)snprintf(buf + len, sizeof buf - len, "%-16.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\n", lines[i]);
    }
    if (!n) snprintf(buf, sizeof buf, "No machine");
    set_text(v->timing, buf);
    SendMessageW(v->beam, PBM_SETPOS, (WPARAM)apple2vid_tab_beam_permille(v->tab), 0);
}

static int cur_section(vidwin *v)
{
    int s = (int)SendMessageW(v->sec, TCM_GETCURSEL, 0, 0);
    return s < 0 ? 0 : s;
}

static void show_section(vidwin *v)
{
    int s = cur_section(v);
    ShowWindow(v->pixel, s == APPLE2VID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->brk, s == APPLE2VID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->showmem, s == APPLE2VID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->unpin, s == APPLE2VID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->switches, s == APPLE2VID_S_SWITCHES ? SW_SHOW : SW_HIDE);
    ShowWindow(v->brk_sw, s == APPLE2VID_S_SWITCHES ? SW_SHOW : SW_HIDE);
    ShowWindow(v->cols, s == APPLE2VID_S_COLOURS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->beam, s == APPLE2VID_S_TIMING ? SW_SHOW : SW_HIDE);
    ShowWindow(v->timing, s == APPLE2VID_S_TIMING ? SW_SHOW : SW_HIDE);
}

/* ---- refresh -------------------------------------------------------------------------- */

#define R_RESET 0x8000u         /* the view or zoom changed: scroll back to the corner */

static void set_check(HWND h, int on)
{
    int want = on ? BST_CHECKED : BST_UNCHECKED;
    if ((int)SendMessageW(h, BM_GETCHECK, 0, 0) != want) SendMessageW(h, BM_SETCHECK, (WPARAM)want, 0);
}

static void set_sel(HWND h, int i)
{
    if ((int)SendMessageW(h, CB_GETCURSEL, 0, 0) != i) SendMessageW(h, CB_SETCURSEL, (WPARAM)i, 0);
}

/* The choosers track the controller: Follow screen moves mode, page and mixed under them. */
static void sync_controls(vidwin *v)
{
    int decoded = apple2vid_tab_view(v->tab) == APPLE2VID_V_DECODED, i;
    v->updating = 1;
    set_sel(v->view, apple2vid_tab_view(v->tab));
    set_check(v->follow, apple2vid_tab_follow(v->tab));
    set_sel(v->mode, apple2vid_tab_mode(v->tab));
    set_sel(v->pgsel, apple2vid_tab_page(v->tab) - 1);
    set_check(v->mixed, apple2vid_tab_mixed(v->tab));
    for (i = 0; i < v->nzoom; i++)
        if (v->zoom_val[i] == apple2vid_tab_zoom(v->tab)) set_sel(v->zoom, i);
    EnableWindow(v->follow, decoded);
    EnableWindow(v->mode, decoded);
    EnableWindow(v->pgsel, decoded);
    EnableWindow(v->mixed, decoded);
    v->updating = 0;
}

static void refresh_view(vidwin *v, unsigned what)
{
    if (what & (APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_OFF)) {
        RECT rc;
        GetClientRect(v->pic, &rc);
        if (rc.right > 4 && rc.bottom > 4) apple2vid_tab_set_viewport(v->tab, rc.right - 2, rc.bottom - 2);
        if (what & R_RESET) v->scroll_x = v->scroll_y = 0;
        pic_update_scroll(v);
        InvalidateRect(v->pic, NULL, FALSE);
    }
    if (what & APPLE2VID_TAB_TABLES) {
        set_text(v->status, apple2vid_tab_status(v->tab));
        sync_controls(v);
        switch (cur_section(v)) {
        case APPLE2VID_S_SWITCHES: fill_switches(v); break;
        case APPLE2VID_S_COLOURS: InvalidateRect(v->cols, NULL, FALSE); break;
        case APPLE2VID_S_TIMING: fill_timing(v); break;
        default: break;
        }
        update_pixel(v);
    }
}

void dbg_video_refresh(HWND page, int force)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    unsigned what;
    if (!v) return;
    what = apple2vid_tab_refresh(v->tab, force);
    if (force) {
        what |= APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES;
        v->sf.have_sig = 0;
    }
    refresh_view(v, what);
}

/* ---- layout --------------------------------------------------------------------------- */

static int text_w(vidwin *v, HFONT f, const wchar_t *s)
{
    HDC dc = GetDC(v->page);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz;
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    ReleaseDC(v->page, dc);
    return sz.cx;
}

static int btn_w(vidwin *v, HWND b)
{
    wchar_t lbl[64];
    GetWindowTextW(b, lbl, 64);
    return text_w(v, v->ui, lbl) + S(v, 28);
}

static void layout(vidwin *v)
{
    RECT rc, tr;
    int W, H, rw, lw, x, y, i, row_h = S(v, 24), lh = S(v, 18), gap = S(v, 6), mono_h;
    int right_x, tab_h, top, nt = 0;
    struct { HWND h; int w; int kind; } tool[TOOL_MAX];     /* kind 0 label, 1 combo, 2 button/checkbox */
    TEXTMETRICW tm;
    HDC dc;
    GetClientRect(v->page, &rc);
    W = rc.right;
    H = rc.bottom;
    if (W < 100 || H < 100) return;
    dc = GetDC(v->page);
    SelectObject(dc, v->mono);
    mono_h = GetTextMetricsW(dc, &tm) ? tm.tmHeight : S(v, 15);
    ReleaseDC(v->page, dc);

    /* toolbar, flowing: View, Zoom, Follow screen, Mode, Page, Mixed; then the overlay checkboxes */
#define TOOL(hwnd, width, k) do { tool[nt].h = (hwnd); tool[nt].w = (width); tool[nt].kind = (k); nt++; } while (0)
    TOOL(v->label_view, S(v, 36), 0);
    TOOL(v->view, S(v, 90), 1);
    TOOL(v->label_zoom, S(v, 40), 0);
    TOOL(v->zoom, S(v, 70), 1);
    TOOL(v->follow, text_w(v, v->ui, L"Follow screen") + S(v, 26), 2);
    TOOL(v->label_mode, S(v, 40), 0);
    TOOL(v->mode, S(v, 130), 1);
    TOOL(v->label_page, S(v, 38), 0);
    TOOL(v->pgsel, S(v, 50), 1);
    TOOL(v->mixed, text_w(v, v->ui, L"Mixed") + S(v, 26), 2);
#undef TOOL
    for (i = 0; i < v->nov && nt < TOOL_MAX; i++) {
        wchar_t lbl[64];
        GetWindowTextW(v->ov[i], lbl, 64);
        tool[nt].h = v->ov[i];
        tool[nt].w = text_w(v, v->ui, lbl) + S(v, 26);
        tool[nt].kind = 2;
        nt++;
    }
    x = 0;
    y = 0;
    for (i = 0; i < nt; i++) {
        int w = tool[i].w, hgt = tool[i].kind == 1 ? S(v, 300) : tool[i].kind == 2 ? row_h - S(v, 4) : lh, yy;
        int need = w + (tool[i].kind == 0 && i + 1 < nt ? tool[i + 1].w : 0);   /* a label stays with its chooser */
        if (x + need > W && x > 0) {
            x = 0;
            y += row_h;
        }
        yy = tool[i].kind == 0 ? y + (row_h - lh) / 2 : tool[i].kind == 2 ? y + S(v, 1) : y;
        MoveWindow(tool[i].h, x, yy, w, hgt, TRUE);
        x += w + (tool[i].kind == 0 ? 0 : gap);
    }
    y += row_h + gap;
    MoveWindow(v->status, 0, y, W, mono_h * 2 + S(v, 4), TRUE);
    y += mono_h * 2 + S(v, 8);
    top = y;

    /* the split: picture left, sections right */
    rw = S(v, 480);
    if (rw > W * 50 / 100) rw = W * 48 / 100;
    lw = W - rw - gap;
    right_x = lw + gap;
    MoveWindow(v->pic, 0, top, lw, H - top - lh - gap > 20 ? H - top - lh - gap : 20, TRUE);
    MoveWindow(v->note, 0, H - lh, W, lh, TRUE);

    MoveWindow(v->sec, right_x, top, rw, H - top - lh - gap, TRUE);
    SetRect(&tr, 0, 0, rw, H - top - lh - gap);
    SendMessageW(v->sec, TCM_ADJUSTRECT, FALSE, (LPARAM)&tr);
    tab_h = tr.top;
    {
        int cx = right_x + S(v, 6), cy = top + tab_h + S(v, 6), cw = rw - S(v, 12);
        int ch = (H - lh - gap) - cy - S(v, 6), bx = cx, by = cy + (ch > S(v, 60) ? ch - S(v, 28) : S(v, 32));
        static const int sw_[6] = { 80, 46, 50, 62, 62, 220 };
        MoveWindow(v->pixel, cx, cy, cw, ch - S(v, 34) > 20 ? ch - S(v, 34) : 20, TRUE);
        MoveWindow(v->brk, bx, by, btn_w(v, v->brk), S(v, 26), TRUE);
        bx += btn_w(v, v->brk) + gap;
        MoveWindow(v->showmem, bx, by, btn_w(v, v->showmem), S(v, 26), TRUE);
        bx += btn_w(v, v->showmem) + gap;
        MoveWindow(v->unpin, bx, by, btn_w(v, v->unpin), S(v, 26), TRUE);
        MoveWindow(v->switches, cx, cy, cw, ch - S(v, 34) > 20 ? ch - S(v, 34) : 20, TRUE);
        MoveWindow(v->brk_sw, cx, by, btn_w(v, v->brk_sw), S(v, 26), TRUE);
        MoveWindow(v->cols, cx, cy, cw, ch, TRUE);
        MoveWindow(v->beam, cx, cy, cw, S(v, 20), TRUE);
        MoveWindow(v->timing, cx, cy + S(v, 30), cw, ch - S(v, 30), TRUE);
        for (i = 0; i < 6; i++) SendMessageW(v->switches, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(v, sw_[i]));
    }
    pic_update_scroll(v);
}

/* ---- the page -------------------------------------------------------------------------- */

static HWND mk_in(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    if (font) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

#define mk(v, cls, text, style, id, font) mk_in((v)->page, cls, text, style, id, font)

static void on_overlay(vidwin *v)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < v->nov; i++)
        if (SendMessageW(v->ov[i], BM_GETCHECK, 0, 0) == BST_CHECKED) m |= v->ov_bit[i];
    apple2vid_tab_set_overlays(v->tab, m);
    refresh_view(v, APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES);
}

/* A mode, page or Mixed choice: the controller turns Follow off and keeps the rest. */
static void on_decoded_choice(vidwin *v, int id)
{
    if (id == IDV_MODE) {
        int m = (int)SendMessageW(v->mode, CB_GETCURSEL, 0, 0);
        if (m >= 0) apple2vid_tab_set_mode(v->tab, m);
    } else if (id == IDV_PAGE) {
        int p = (int)SendMessageW(v->pgsel, CB_GETCURSEL, 0, 0);
        if (p >= 0) apple2vid_tab_set_page(v->tab, p + 1);
    } else {
        apple2vid_tab_set_mixed(v->tab, SendMessageW(v->mixed, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    refresh_view(v, apple2vid_tab_refresh(v->tab, 1) | APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES);
}

static LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_SIZE:
        layout(v);
        refresh_view(v, APPLE2VID_TAB_PICTURE);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (v->updating) return 0;
        if (id == IDV_VIEW && code == CBN_SELCHANGE) {
            apple2vid_tab_set_view(v->tab, (int)SendMessageW(v->view, CB_GETCURSEL, 0, 0));
            dbg_video_refresh(h, 1);
            v->scroll_x = v->scroll_y = 0;
            pic_update_scroll(v);
        } else if (id == IDV_ZOOM && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(v->zoom, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < v->nzoom) apple2vid_tab_set_zoom(v->tab, v->zoom_val[i]);
            refresh_view(v, apple2vid_tab_refresh(v->tab, 1) | APPLE2VID_TAB_PICTURE | R_RESET);
        } else if (id == IDV_FOLLOW && code == BN_CLICKED) {
            apple2vid_tab_set_follow(v->tab, SendMessageW(v->follow, BM_GETCHECK, 0, 0) == BST_CHECKED);
            refresh_view(v, apple2vid_tab_refresh(v->tab, 1) | APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES);
        } else if ((id == IDV_MODE || id == IDV_PAGE) && code == CBN_SELCHANGE) {
            on_decoded_choice(v, id);
        } else if (id == IDV_MIXED && code == BN_CLICKED) {
            on_decoded_choice(v, id);
        } else if (id >= IDV_OV0 && id < IDV_OV0 + v->nov && code == BN_CLICKED) {
            on_overlay(v);
        } else if (id == IDV_BREAK && code == BN_CLICKED) {
            break_on_pixel(v);
        } else if (id == IDV_SHOWMEM && code == BN_CLICKED) {
            show_in_memory(v);
        } else if (id == IDV_BRK_SW && code == BN_CLICKED) {
            break_on_switch(v);
        } else if (id == IDV_UNPIN && code == BN_CLICKED) {
            apple2vid_tab_unpin(v->tab);
            InvalidateRect(v->pic, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDV_SEC && nh->code == TCN_SELCHANGE) {
            show_section(v);
            refresh_view(v, APPLE2VID_TAB_TABLES);
        } else if (nh->idFrom == IDV_SWITCHES && nh->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT || cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                int i = (int)cd->nmcd.dwItemSpec;
                int hot = i >= 0 && i < v->sf.nhot && v->sf.hot[i];
                if (hot) {                      /* changed lately: the accent tint */
                    cd->clrTextBk = RGB(0xf6, 0xd6, 0xd2);
                    cd->clrText = RGB(0, 0, 0);
                } else if (cd->nmcd.dwDrawStage != CDDS_ITEMPREPAINT && cd->iSubItem == 2) {
                    cd->clrText = GetSysColor(COLOR_GRAYTEXT);      /* the age fades to grey */
                }
                return cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT ? CDRF_NOTIFYSUBITEMDRAW : CDRF_DODEFAULT;
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        /* the read-only labels sit on the page colour */
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_DESTROY:
        apple2vid_tab_free(v->tab);
        free(v);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void register_classes(HINSTANCE inst)
{
    static int done;
    WNDCLASSW wc;
    if (done) return;
    done = 1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = page_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = PAGE_CLASS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = pic_proc;
    wc.lpszClassName = PIC_CLASS;
    wc.hbrBackground = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = col_proc;
    wc.lpszClassName = COL_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = 0;
    RegisterClassW(&wc);
}

static HWND mk_list(vidwin *v, int id, const wchar_t *const *names, int n)
{
    HWND lv = mk(v, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                 id, v->mono);
    LVCOLUMNW col;
    int i;
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    for (i = 0; i < n; i++) {
        col.pszText = (LPWSTR)names[i];
        col.cx = 80;
        SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
    return lv;
}

static void fill_combo(HWND cb, int n, const char *(*name)(int))
{
    int i;
    for (i = 0; i < n; i++) {
        wchar_t *w = wide(name(i));
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
}

HWND dbg_video_create(HWND parent, HFONT ui, HFONT mono, int dpi, apple2session *session,
                      dbg_video_show_memory_fn show_memory, void *ctx)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    vidwin *v = calloc(1, sizeof *v);
    int n, i;
    const apple2vid_overlay_info *od;
    const int *zv;
    TCITEMW item;
    static const wchar_t *const swcols[6] = { L"Switch", L"State", L"Age", L"Set", L"Clear", L"Meaning" };
    if (!v) return NULL;
    register_classes(inst);
    v->ui = ui;
    v->mono = mono;
    v->dpi = dpi > 0 ? dpi : 96;
    v->show_memory = show_memory;
    v->ctx = ctx;
    v->tab = apple2vid_tab_new(session);
    if (!v->tab) {
        free(v);
        return NULL;
    }
    memset(&v->bmi, 0, sizeof v->bmi);
    v->bmi.bmiHeader.biSize = sizeof v->bmi.bmiHeader;
    v->bmi.bmiHeader.biPlanes = 1;
    v->bmi.bmiHeader.biBitCount = 32;
    v->bmi.bmiHeader.biCompression = BI_RGB;
    v->page = CreateWindowExW(0, PAGE_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                              NULL, inst, NULL);
    SetWindowLongPtrW(v->page, GWLP_USERDATA, (LONG_PTR)v);

    v->label_view = mk(v, L"STATIC", L"View", SS_LEFT, IDV_LABEL_VIEW, ui);
    v->view = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_VIEW, ui);
    fill_combo(v->view, APPLE2VID_V_COUNT, apple2vid_view_name);
    v->label_zoom = mk(v, L"STATIC", L"Zoom", SS_LEFT, IDV_LABEL_ZOOM, ui);
    v->zoom = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_ZOOM, ui);
    zv = apple2vid_zooms(&n);
    for (i = 0; i < n && i < MAX_ZOOMS; i++) {
        wchar_t lbl[16];
        v->zoom_val[v->nzoom++] = zv[i];
        if (zv[i] == 0) wcscpy(lbl, L"Fit");
        else swprintf(lbl, 16, L"%d×", zv[i]);
        SendMessageW(v->zoom, CB_ADDSTRING, 0, (LPARAM)lbl);
    }
    v->follow = mk(v, L"BUTTON", L"Follow screen", BS_AUTOCHECKBOX | WS_TABSTOP, IDV_FOLLOW, ui);
    v->label_mode = mk(v, L"STATIC", L"Mode", SS_LEFT, IDV_LABEL_MODE, ui);
    v->mode = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_MODE, ui);
    fill_combo(v->mode, APPLE2VID_M_COUNT, apple2vid_mode_name);
    v->label_page = mk(v, L"STATIC", L"Page", SS_LEFT, IDV_LABEL_PAGE, ui);
    v->pgsel = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_PAGE, ui);
    SendMessageW(v->pgsel, CB_ADDSTRING, 0, (LPARAM)L"1");
    SendMessageW(v->pgsel, CB_ADDSTRING, 0, (LPARAM)L"2");
    v->mixed = mk(v, L"BUTTON", L"Mixed", BS_AUTOCHECKBOX | WS_TABSTOP, IDV_MIXED, ui);
    od = apple2vid_overlays(&n);
    for (i = 0; i < n && i < MAX_OVERLAYS; i++) {
        wchar_t *w = wide(od[i].label);
        v->ov[i] = mk(v, L"BUTTON", w, BS_AUTOCHECKBOX | WS_TABSTOP, IDV_OV0 + i, ui);
        v->ov_bit[i] = od[i].bit;
        v->nov++;
        free(w);
    }
    v->status = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_STATUS, mono);
    v->pic = CreateWindowExW(WS_EX_CLIENTEDGE, PIC_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL, 0, 0, 10, 10,
                             v->page, (HMENU)(INT_PTR)IDV_PIC, inst, NULL);
    SetWindowLongPtrW(v->pic, GWLP_USERDATA, (LONG_PTR)v);
    v->note = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDV_NOTE, ui);

    v->sec = mk(v, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDV_SEC, ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < APPLE2VID_S_COUNT; i++) {
        wchar_t *w = wide(apple2vid_section_name(i));
        item.pszText = w;
        SendMessageW(v->sec, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
        free(w);
    }
    SetWindowPos(v->sec, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* under the section controls */
    v->pixel = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_PIXEL, mono);
    v->brk = mk(v, L"BUTTON", L"Break on writes", BS_PUSHBUTTON | WS_TABSTOP, IDV_BREAK, ui);
    v->showmem = mk(v, L"BUTTON", L"Show in Memory", BS_PUSHBUTTON | WS_TABSTOP, IDV_SHOWMEM, ui);
    v->unpin = mk(v, L"BUTTON", L"Unpin", BS_PUSHBUTTON | WS_TABSTOP, IDV_UNPIN, ui);
    v->switches = mk_list(v, IDV_SWITCHES, swcols, 6);
    v->brk_sw = mk(v, L"BUTTON", L"Break on change", BS_PUSHBUTTON | WS_TABSTOP, IDV_BRK_SW, ui);
    v->cols = CreateWindowExW(0, COL_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, v->page,
                              (HMENU)(INT_PTR)IDV_COLS, inst, NULL);
    SetWindowLongPtrW(v->cols, GWLP_USERDATA, (LONG_PTR)v);
    v->beam = mk(v, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDV_BEAM, NULL);
    SendMessageW(v->beam, PBM_SETRANGE32, 0, 1000);
    v->timing = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_TIMING, mono);

    /* start from the controller's state (the APPLE2_DEBUGGER_VIDEO_* variables) */
    for (i = 0; i < v->nov; i++)
        SendMessageW(v->ov[i], BM_SETCHECK, (apple2vid_tab_overlays(v->tab) & v->ov_bit[i]) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(v->sec, TCM_SETCURSEL, (WPARAM)apple2vid_tab_env_section(v->tab), 0);
    sync_controls(v);
    show_section(v);
    return v->page;
}

void dbg_video_place(HWND page, int x, int y, int w, int h)
{
    MoveWindow(page, x, y, w > 1 ? w : 1, h > 1 ? h : 1, TRUE);
}

void dbg_video_fonts(HWND page, HFONT ui, HFONT mono, int dpi)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (!v) return;
    v->ui = ui;
    v->mono = mono;
    v->dpi = dpi > 0 ? dpi : 96;
    layout(v);
    InvalidateRect(v->page, NULL, TRUE);
}

void dbg_video_env_view(HWND page, int index)
{
    static const int modes[6] = { APPLE2VID_M_TEXT40, APPLE2VID_M_TEXT40, APPLE2VID_M_LORES, APPLE2VID_M_LORES,
                                  APPLE2VID_M_HIRES, APPLE2VID_M_HIRES };
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (!v || index < 0 || index > 5) return;
    apple2vid_tab_set_view(v->tab, APPLE2VID_V_DECODED);
    apple2vid_tab_set_mode(v->tab, modes[index]);
    apple2vid_tab_set_page(v->tab, index % 2 + 1);
    dbg_video_refresh(page, 1);
}

void dbg_video_env_pin(HWND page, int x, int y)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (!v) return;
    dbg_video_refresh(page, 1);
    apple2vid_tab_hover(v->tab, x, y);
    apple2vid_tab_pin(v->tab, x, y);
    InvalidateRect(v->pic, NULL, FALSE);
    update_pixel(v);
}

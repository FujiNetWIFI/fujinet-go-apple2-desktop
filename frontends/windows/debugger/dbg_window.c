/*
 * Debugger window (Win32): run control, 6502 registers, disassembly with
 * click-to-toggle breakpoints, and a memory view -- the same shape as the
 * GNOME and KDE debuggers, over the same shared engine (core/debugger), built
 * from plain common controls.
 *
 * The engine's rule is that only the emulator thread touches AppleWin, and
 * its stop callback therefore FIRES ON THE EMULATOR THREAD. It is handed to
 * the UI thread with PostMessage (the Win32 analog of g_idle_add /
 * QMetaObject::invokeMethod) before any control is touched.
 *
 * Keys match the rest of the family: F5 pause/continue, F7 step into,
 * F8 step over, Shift+F8 step out.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apple2debug.h"

#define DISASM_LINES 24
/* 16 columns, where the GNOME and KDE windows use 8. Those two are sized by
 * their window manager and a 16-column dump is wide enough to squeeze the
 * disassembly pane out entirely under a tiling one; this window sets its own
 * size, so the constraint does not apply and the wider dump is simply more
 * useful. */
#define MEM_COLS 16
#define MEM_ROWS 24

#define WM_DBG_STOPPED (WM_APP + 1) /* wp: reason, lp: pc */
#define WM_DBG_ACCEPT  (WM_APP + 2) /* wp: control id -- Enter in an edit */

#define TIMER_REFRESH 1

enum {
    IDC_PAUSE = 1000, IDC_STEP_IN, IDC_STEP_OVER, IDC_STEP_OUT,
    IDC_STATUS, IDC_DISASM, IDC_REGS, IDC_REG_LABEL,
    IDC_MEM_LABEL, IDC_MEM_ADDR, IDC_MEM_VIEW,
    IDC_VIEW_PICK, IDC_VIEW_PIC,
    IDC_LABEL_FIRST /* statics that only carry text */
};

typedef struct {
    HWND hwnd;
    HWND pause_btn, step_in, step_over, step_out, status;
    HWND disasm;
    HWND reg_label, regs;
    HWND mem_label, mem_addr, mem_view;
    HWND view_pick, view_pic;
    uint32_t *view_fb;

    HFONT mono;
    HACCEL accel;

    apple2session *session;
    apple2debug *dbg;

    uint16_t disasm_base;
    uint16_t mem_base;
    int follow_pc;

    /* Address per disassembly row, so a click resolves to an address without
     * parsing the rendered text back out of the list box. */
    uint16_t line_addr[DISASM_LINES];
    int line_count;
} debugger;

static debugger *g_dbg;

/* ---- video page view ------------------------------------------------------
 * A small owner-drawn window: the engine's XRGB8888 is byte-for-byte what a
 * 32bpp BI_RGB DIB wants, so StretchDIBits reads the buffer as it stands --
 * the same deal the main window's display gets. */

static LRESULT CALLBACK pixels_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = (debugger *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT c;
        BITMAPINFO bmi;

        GetClientRect(hwnd, &c);
        if (d && d->view_fb) {
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
            bmi.bmiHeader.biWidth = APPLE2VIEW_WIDTH;
            bmi.bmiHeader.biHeight = -APPLE2VIEW_HEIGHT; /* top-down */
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            /* Nearest-neighbour: this is a pixel inspector, so smoothing it
             * would hide the thing being inspected. */
            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchDIBits(hdc, 0, 0, c.right, c.bottom, 0, 0,
                          APPLE2VIEW_WIDTH, APPLE2VIEW_HEIGHT, d->view_fb,
                          &bmi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Decode whichever page the combo names. This shows a PAGE, not what the
 * machine is currently displaying -- which is the point: it is how you see the
 * buffer a program is drawing into before it flips to it. */
static void render_video(debugger *d)
{
    int sel;

    if (!d->dbg || !d->view_fb || !d->view_pic)
        return;
    sel = (int)SendMessageA(d->view_pick, CB_GETCURSEL, 0, 0);
    if (sel < 0)
        sel = 0;
    if (apple2debug_render_view(d->dbg, (apple2debug_view)sel,
                                d->view_fb) != 0)
        return;
    InvalidateRect(d->view_pic, NULL, FALSE);
}

/* ---- rendering ----------------------------------------------------------- */

static void render_regs(debugger *d)
{
    /* Bit 7 is N, bit 0 is C; a set flag shows in upper case and a clear one
     * as a dot, the way every 6502 monitor has always done it. */
    static const char kFlag[] = "NV-BDIZC";
    apple2debug_regs r;
    char flags[9];
    char text[256];
    int i;

    apple2debug_get_regs(d->dbg, &r);
    for (i = 0; i < 8; i++)
        flags[i] = (r.ps & (0x80 >> i)) ? kFlag[i] : '.';
    flags[8] = '\0';

    /* SP is a full address because that is what AppleWin stores: the 6502's
     * 8-bit stack pointer plus the $0100 page base. */
    snprintf(text, sizeof(text),
             "PC  $%04X\r\n"
             "A   $%02X      X   $%02X\r\n"
             "Y   $%02X      SP  $%04X\r\n"
             "P   $%02X   %s%s",
             r.pc, r.a, r.x, r.y, r.sp, r.ps, flags,
             r.jammed ? "\r\n\r\nCPU JAMMED" : "");
    SetWindowTextA(d->regs, text);
}

static void render_disasm(debugger *d)
{
    apple2dasm_line lines[DISASM_LINES];
    apple2debug_regs r;
    uint16_t next = 0;
    int i, n;

    apple2debug_get_regs(d->dbg, &r);
    if (d->follow_pc) {
        d->disasm_base = r.pc;
        d->follow_pc = 0;
    }

    n = apple2debug_disassemble(d->dbg, d->disasm_base, DISASM_LINES, lines,
                                &next);

    /* Redrawing off, or the list box repaints once per inserted row. */
    SendMessageA(d->disasm, WM_SETREDRAW, FALSE, 0);
    SendMessageA(d->disasm, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) {
        char bytes[16];
        char text[128];
        int b;

        bytes[0] = '\0';
        for (b = 0; b < lines[i].len; b++)
            snprintf(bytes + b * 3, sizeof(bytes) - (size_t)b * 3, "%02X ",
                     lines[i].bytes[b]);

        snprintf(text, sizeof(text), "%c%c $%04X  %-9s %-16s%s%s",
                 apple2debug_bp_is_set(d->dbg, lines[i].addr) ? '*' : ' ',
                 lines[i].addr == r.pc ? '>' : ' ', lines[i].addr, bytes,
                 lines[i].text, lines[i].symbol ? "  ; " : "",
                 lines[i].symbol ? lines[i].symbol : "");
        SendMessageA(d->disasm, LB_ADDSTRING, 0, (LPARAM)text);
        d->line_addr[i] = lines[i].addr;
    }
    d->line_count = n;
    SendMessageA(d->disasm, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->disasm, NULL, TRUE);
}

static void render_mem(debugger *d)
{
    char text[MEM_ROWS * (12 + MEM_COLS * 4) + 64];
    uint8_t row[MEM_COLS];
    size_t len = 0;
    int r, c;

    text[0] = '\0';
    for (r = 0; r < MEM_ROWS; r++) {
        uint16_t addr = (uint16_t)(d->mem_base + r * MEM_COLS);
        apple2debug_read_mem(d->dbg, addr, row, MEM_COLS);
        len += (size_t)snprintf(text + len, sizeof(text) - len, "$%04X ", addr);
        for (c = 0; c < MEM_COLS; c++)
            len += (size_t)snprintf(text + len, sizeof(text) - len, "%02X ",
                                    row[c]);
        len += (size_t)snprintf(text + len, sizeof(text) - len, " ");
        for (c = 0; c < MEM_COLS; c++) {
            /* Apple II text carries the high bit; mask it before deciding
             * whether a byte is printable, or nothing ever looks like text. */
            uint8_t ch = row[c] & 0x7F;
            len += (size_t)snprintf(text + len, sizeof(text) - len, "%c",
                                    (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.');
        }
        len += (size_t)snprintf(text + len, sizeof(text) - len, "\r\n");
    }
    SetWindowTextA(d->mem_view, text);
}

static void refresh_all(debugger *d)
{
    if (!d->dbg)
        return;
    SetWindowTextA(d->pause_btn,
                   apple2debug_is_paused(d->dbg) ? "Continue" : "Pause");
    render_regs(d);
    render_disasm(d);
    render_mem(d);
    render_video(d);
}

/* ---- actions ------------------------------------------------------------- */

static void toggle_run(debugger *d)
{
    if (!d->dbg)
        return;
    if (apple2debug_is_paused(d->dbg)) {
        apple2debug_resume(d->dbg);
        SetWindowTextA(d->status, "Running");
        refresh_all(d);
    } else {
        apple2debug_pause(d->dbg);
    }
}

/* "$C000", "C000" or a symbol name. */
static int parse_addr(debugger *d, const char *text, uint16_t *out)
{
    char buf[128];
    char *p = buf;
    char *end;
    unsigned long v;
    uint16_t addr;

    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '$')
        p++;
    if (*p) {
        v = strtoul(p, &end, 16);
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
            end++;
        if (*end == '\0' && v <= 0xFFFF) {
            *out = (uint16_t)v;
            return 1;
        }
    }
    if (apple2debug_symbol_find(d->dbg, text, &addr)) {
        *out = addr;
        return 1;
    }
    return 0;
}

static void on_accept(debugger *d, int id)
{
    char buf[128];
    uint16_t addr;

    if (id != IDC_MEM_ADDR)
        return;
    GetWindowTextA(d->mem_addr, buf, sizeof(buf));
    if (parse_addr(d, buf, &addr)) {
        d->mem_base = addr;
        render_mem(d);
    }
}

static void on_stopped(debugger *d, apple2debug_stop_reason reason,
                       uint16_t pc)
{
    static const char *const kReason[] = {"Paused", "Breakpoint", "Stepped",
                                          "Ran to address", "CPU JAMMED"};
    const char *what = (reason >= 0 && reason <= APPLE2DBG_STOP_JAMMED)
                           ? kReason[reason]
                           : "Stopped";
    const char *sym = apple2debug_symbol_at(d->dbg, pc);
    char buf[192];

    if (sym)
        snprintf(buf, sizeof(buf), "%s at $%04X  %s", what, pc, sym);
    else
        snprintf(buf, sizeof(buf), "%s at $%04X", what, pc);
    SetWindowTextA(d->status, buf);
    d->follow_pc = 1;
    refresh_all(d);
}

/* EMULATOR THREAD. Does nothing but hand the event to the UI thread. */
static void stop_trampoline(apple2debug_stop_reason reason, uint16_t pc,
                            void *user)
{
    debugger *d = (debugger *)user;
    PostMessageA(d->hwnd, WM_DBG_STOPPED, (WPARAM)reason, (LPARAM)pc);
}

/* ---- construction -------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value" (and must not beep
 * its way through the default handler). */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetParent(hwnd), WM_DBG_ACCEPT,
                     (WPARAM)GetWindowLongPtrA(hwnd, GWLP_ID), 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN)
        return 0;
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

static HWND child(debugger *d, const char *cls, const char *text, DWORD style,
                  int id)
{
    HWND h = CreateWindowExA(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, d->hwnd,
        (HMENU)(INT_PTR)id,
        (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)d->mono, TRUE);
    return h;
}

static HWND mono_view(debugger *d, int id, DWORD extra)
{
    return child(d, "EDIT", "",
                 WS_BORDER | ES_MULTILINE | ES_READONLY | extra, id);
}

static void build_controls(debugger *d)
{
    int i;

    d->pause_btn = child(d, "BUTTON", "Pause", BS_PUSHBUTTON, IDC_PAUSE);
    d->step_in = child(d, "BUTTON", "Into (F7)", BS_PUSHBUTTON, IDC_STEP_IN);
    d->step_over = child(d, "BUTTON", "Over (F8)", BS_PUSHBUTTON,
                         IDC_STEP_OVER);
    d->step_out = child(d, "BUTTON", "Out (Shift+F8)", BS_PUSHBUTTON,
                        IDC_STEP_OUT);
    d->status = child(d, "STATIC", "Running", SS_LEFT, IDC_STATUS);

    /* A list box, so a click resolves to a row directly. Its tooltip lives in
     * the label above it -- list boxes do not carry one. */
    d->disasm = child(d, "LISTBOX", "",
                      WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                      IDC_DISASM);

    d->reg_label = child(d, "STATIC", "Registers", SS_LEFT, IDC_REG_LABEL);
    d->regs = mono_view(d, IDC_REGS, 0);

    d->mem_label = child(d, "STATIC", "Memory (address or symbol, Enter)",
                         SS_LEFT, IDC_MEM_LABEL);
    d->mem_addr = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL,
                        IDC_MEM_ADDR);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(d->mem_addr, GWLP_WNDPROC,
                                             (LONG_PTR)edit_subclass);
    d->mem_view = mono_view(d, IDC_MEM_VIEW, WS_VSCROLL | ES_AUTOVSCROLL);

    /* Video page viewer. The page list comes from the engine, so a view added
     * there shows up here with no edit. */
    d->view_pick = child(d, "COMBOBOX", "",
                         CBS_DROPDOWNLIST | WS_VSCROLL, IDC_VIEW_PICK);
    for (i = 0; apple2debug_view_name(i); i++)
        SendMessageA(d->view_pick, CB_ADDSTRING, 0,
                     (LPARAM)apple2debug_view_name(i));
    SendMessageA(d->view_pick, CB_SETCURSEL, 0, 0);

    d->view_fb = (uint32_t *)calloc((size_t)APPLE2VIEW_WIDTH *
                                        APPLE2VIEW_HEIGHT,
                                    sizeof(uint32_t));
    d->view_pic = child(d, "Apple2DbgPixels", "", 0, IDC_VIEW_PIC);
    SetWindowLongPtrA(d->view_pic, GWLP_USERDATA, (LONG_PTR)d);
}

static void layout(debugger *d)
{
    RECT client;
    int y = 8, bx = 8;
    int left_w, rx, rw, ry, body_y, body_h;

    GetClientRect(d->hwnd, &client);

    MoveWindow(d->pause_btn, bx, y, 100, 26, TRUE);      bx += 106;
    MoveWindow(d->step_in, bx, y, 100, 26, TRUE);        bx += 106;
    MoveWindow(d->step_over, bx, y, 100, 26, TRUE);      bx += 106;
    MoveWindow(d->step_out, bx, y, 130, 26, TRUE);       bx += 138;
    MoveWindow(d->status, bx, y + 5, client.right - bx - 8, 20, TRUE);

    body_y = 44;
    body_h = client.bottom - body_y - 8;
    if (body_h < 100)
        body_h = 100;

    /* Disassembly on the left, registers and memory stacked on the right --
     * the same split the GNOME and KDE windows use. */
    left_w = (client.right - 24) * 45 / 100;
    if (left_w < 380)
        left_w = 380;
    MoveWindow(d->disasm, 8, body_y, left_w, body_h, TRUE);

    rx = 8 + left_w + 8;
    rw = client.right - rx - 8;
    if (rw < 40)
        rw = 40;
    ry = body_y;

    MoveWindow(d->reg_label, rx, ry, rw, 18, TRUE);
    ry += 20;
    MoveWindow(d->regs, rx, ry, rw, 86, TRUE);
    ry += 94;
    MoveWindow(d->mem_label, rx, ry, rw, 18, TRUE);
    ry += 20;
    MoveWindow(d->mem_addr, rx, ry, rw, 22, TRUE);
    ry += 28;

    /* The memory dump takes what is left after the video pane below it, which
     * is a fixed 280x192 plus its combo -- so it can never be squeezed out.
     * (Stacking it and letting the memory view take the slack is exactly how
     * it got pushed off the bottom in the Qt window.) */
    {
        const int video_h = APPLE2VIEW_HEIGHT + 30;
        int mem_h = body_y + body_h - ry - video_h - 8;
        if (mem_h < 60)
            mem_h = 60;
        MoveWindow(d->mem_view, rx, ry, rw, mem_h, TRUE);
        ry += mem_h + 8;
        MoveWindow(d->view_pick, rx, ry, rw, 200, TRUE);
        ry += 26;
        MoveWindow(d->view_pic, rx, ry, APPLE2VIEW_WIDTH, APPLE2VIEW_HEIGHT,
                   TRUE);
    }
}

/* ---- window proc --------------------------------------------------------- */

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;

    if (!d || d->hwnd != hwnd)
        return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_TIMER:
        /* While the machine runs, keep the registers live but leave the
         * disassembly and memory alone: they would be stale before they were
         * drawn, and re-rendering them every tick fights with scrolling. */
        if (wp == TIMER_REFRESH && d->dbg && !apple2debug_is_paused(d->dbg))
            render_regs(d);
        return 0;
    case WM_DBG_STOPPED:
        on_stopped(d, (apple2debug_stop_reason)wp, (uint16_t)lp);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PAUSE:     toggle_run(d); return 0;
        case IDC_STEP_IN:   if (d->dbg) apple2debug_step_into(d->dbg); return 0;
        case IDC_STEP_OVER: if (d->dbg) apple2debug_step_over(d->dbg); return 0;
        case IDC_STEP_OUT:  if (d->dbg) apple2debug_step_out(d->dbg); return 0;
        case IDC_VIEW_PICK:
            if (HIWORD(wp) == CBN_SELCHANGE)
                render_video(d);
            return 0;
        case IDC_DISASM:
            if (HIWORD(wp) == LBN_SELCHANGE && d->dbg) {
                int sel = (int)SendMessageA(d->disasm, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < d->line_count) {
                    apple2debug_bp_toggle(d->dbg, d->line_addr[sel]);
                    render_disasm(d);
                }
            }
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        if (d->dbg) {
            /* Leave the machine running, but stop calling into a dead
             * window -- and do not strand it paused. */
            apple2debug_set_stop_callback(d->dbg, NULL, NULL);
            apple2debug_resume(d->dbg);
        }
        if (d->accel)
            DestroyAcceleratorTable(d->accel);
        free(d->view_fb);
        DeleteObject(d->mono);
        free(d);
        g_dbg = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points -------------------------------------------------------- */

void apple2_debugger_show(HWND parent, apple2session *session)
{
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    WNDCLASSA wc;
    debugger *d;
    ACCEL accels[4];

    if (g_dbg) {
        ShowWindow(g_dbg->hwnd, SW_SHOW);
        SetForegroundWindow(g_dbg->hwnd);
        return;
    }

    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = pixels_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "Apple2DbgPixels";
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dbg_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "Apple2DebuggerWindow";
    RegisterClassA(&wc);

    d = (debugger *)calloc(1, sizeof(*d));
    if (!d)
        return;
    d->session = session;
    d->dbg = apple2session_debugger(session);
    d->follow_pc = 1;
    d->mem_base = 0x0400; /* text page 1, as in the other frontends */
    g_dbg = d;

    d->hwnd = CreateWindowExA(0, "Apple2DebuggerWindow", "Debugger",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1240, 900, NULL, NULL, inst, NULL);
    if (!d->hwnd) {
        free(d);
        g_dbg = NULL;
        return;
    }

    d->mono = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          FIXED_PITCH | FF_MODERN, "Consolas");

    build_controls(d);

    /* F5/F7/F8/Shift+F8 work wherever the focus is inside the window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;
    accels[0].cmd = IDC_PAUSE;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;
    accels[1].cmd = IDC_STEP_IN;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;
    accels[2].cmd = IDC_STEP_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;
    accels[3].cmd = IDC_STEP_OUT;
    d->accel = CreateAcceleratorTableA(accels, 4);

    if (d->dbg) {
        apple2debug_set_stop_callback(d->dbg, stop_trampoline, d);
        /* Open paused on purpose: a debugger that opens on a running machine
         * shows a disassembly that is stale before it is drawn. */
        apple2debug_pause(d->dbg);
    } else {
        SetWindowTextA(d->status, "No debugger (the session is not running)");
    }

    /* Dev affordance, like APPLE2_OPEN_DEBUGGER: land on a given video page
     * (an index into apple2debug_view_name) instead of text page 1. */
    {
        const char *v = getenv("APPLE2_DEBUGGER_VIEW");
        if (v && *v) {
            const int idx = atoi(v);
            if (idx >= 0 && idx < APPLE2VIEW_COUNT)
                SendMessageA(d->view_pick, CB_SETCURSEL, (WPARAM)idx, 0);
        }
    }

    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    layout(d);
    refresh_all(d);
    ShowWindow(d->hwnd, SW_SHOW);
}

int apple2_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel)
        return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd))
        return 0;
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}

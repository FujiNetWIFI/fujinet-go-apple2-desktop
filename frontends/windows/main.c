/*
 * FujiNet Go Apple II -- native Win32 frontend.
 *
 * Mirrors the GTK/Qt/AppKit frontends over the shared apple2session API: a
 * GDI-blitted display letterboxed to the chosen aspect, a present thread that
 * feeds the session's vsync phase-lock from DwmFlush (the Windows analog of
 * the GTK frame clock / CVDisplayLink), full keyboard mapping, menus for the
 * machine and slot options, media and ROM import, and the FujiNet
 * configuration (default browser) and console-log windows. Gamepads are
 * handled inside the core's SDL thread.
 *
 * Two things differ from the ADAM frontend this is modelled on:
 *
 *  - the frame needs no conversion. The session hands out XRGB8888, which is
 *    byte-for-byte what a 32bpp BI_RGB DIB wants, so the present thread just
 *    copies it and StretchDIBits blits it. (ADAM's core produces RGB565 and
 *    its frontend expands every pixel by hand.)
 *  - the geometry is not fixed. A VidHD card in slot 3 switches the core from
 *    560x384 to 640x400 at runtime, so the frame's own dimensions drive the
 *    letterbox rather than a compile-time constant.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apple2session.h"
#include "debugger/dbg_window.h"
#include "resource.h"

#define FB_MAX_W APPLE2SESSION_FB_MAX_WIDTH
#define FB_MAX_H APPLE2SESSION_FB_MAX_HEIGHT

#define APP_TITLE "FujiNet Go Apple II"

enum { ASPECT_TV43 = 0, ASPECT_SQUARE, ASPECT_INTEGER };

static apple2session *g_session;
static HWND g_hwnd;
static HWND g_log_window;
static HWND g_log_edit;
static HMENU g_menu;

/* Frame hand-off: the present thread refreshes this under the lock, WM_PAINT
 * blits it. The pixels need no conversion (see the file comment), so the
 * buffer is exactly what StretchDIBits reads. */
static CRITICAL_SECTION g_fb_lock;
static uint32_t g_fb[FB_MAX_W * FB_MAX_H];
static int g_fb_w, g_fb_h;
static uint64_t g_serial;

static pthread_t g_present_thread;
static volatile int g_present_run;
static int g_aspect_mode;
static int g_smooth;
static int g_scanlines;
static int g_fullscreen;
/* .length is filled in at startup rather than in the initializer: only the
 * first field would be named, and -Wextra rightly complains about the rest. */
static WINDOWPLACEMENT g_prev_placement;

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- present thread: vsync pacing + frame pickup ------------------------- */

static void *present_main(void *arg)
{
    (void)arg;
    while (g_present_run) {
        BOOL composited = FALSE;
        DwmIsCompositionEnabled(&composited);
        if (composited && SUCCEEDED(DwmFlush())) {
            /* DwmFlush returned at the compositor's vsync. */
        } else {
            Sleep(16); /* no DWM composition (RDP, safe mode): wall clock */
        }
        apple2session_notify_vsync(g_session, monotonic_ns());

        EnterCriticalSection(&g_fb_lock);
        {
            int w = 0, h = 0;
            int changed =
                apple2session_copy_frame(g_session, g_fb, &g_serial, &w, &h);
            if (changed) {
                g_fb_w = w;
                g_fb_h = h;
            }
            LeaveCriticalSection(&g_fb_lock);
            if (changed)
                InvalidateRect(g_hwnd, NULL, FALSE);
        }
    }
    return NULL;
}

/* ---- display ------------------------------------------------------------- */

static RECT dest_rect(int cw, int ch, int fbw, int fbh)
{
    double dw, dh;
    RECT r;

    if (g_aspect_mode == ASPECT_INTEGER) {
        int scale = (int)((cw / fbw < ch / fbh) ? cw / fbw : ch / fbh);
        if (scale < 1) scale = 1;
        dw = (double)scale * fbw;
        dh = (double)scale * fbh;
    } else {
        double aspect =
            g_aspect_mode == ASPECT_TV43 ? 4.0 / 3.0 : (double)fbw / fbh;
        if ((double)cw / ch > aspect) {
            dh = ch;
            dw = ch * aspect;
        } else {
            dw = cw;
            dh = cw / aspect;
        }
    }
    r.left = (LONG)((cw - dw) / 2);
    r.top = (LONG)((ch - dh) / 2);
    r.right = r.left + (LONG)dw;
    r.bottom = r.top + (LONG)dh;
    return r;
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    BITMAPINFO bmi;
    RECT d;
    int fbw, fbh;

    GetClientRect(hwnd, &client);
    FillRect(hdc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));

    EnterCriticalSection(&g_fb_lock);
    fbw = g_fb_w;
    fbh = g_fb_h;
    if (fbw > 0 && fbh > 0 && client.right > 0 && client.bottom > 0) {
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = fbw;
        bmi.bmiHeader.biHeight = -fbh; /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        d = dest_rect(client.right, client.bottom, fbw, fbh);
        SetStretchBltMode(hdc, g_smooth ? HALFTONE : COLORONCOLOR);
        StretchDIBits(hdc, d.left, d.top, d.right - d.left, d.bottom - d.top,
                      0, 0, fbw, fbh, g_fb, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }
    LeaveCriticalSection(&g_fb_lock);

    EndPaint(hwnd, &ps);
}

/* ---- keyboard ------------------------------------------------------------
 * Everything is driven from WM_KEYDOWN/WM_KEYUP rather than WM_CHAR: the core
 * tracks Open Apple and Closed Apple as HELD modifiers, so every key needs a
 * matching release, and WM_CHAR has none. The printable character is obtained
 * from ToUnicode() instead, which applies the user's layout and shift state
 * exactly as TranslateMessage would have. */

/* Win32 virtual key -> X11/xkb keysym, for the keys apple2_key_from_event
 * resolves by keycode rather than by character. */
static uint32_t keysym_for_vk(WPARAM vk, LPARAM lparam)
{
    switch (vk) {
    case VK_RETURN:
        /* Numpad Enter sets the extended-key bit; both are Return here. */
        return 0xFF0D;
    case VK_ESCAPE:   return 0xFF1B;
    case VK_TAB:      return 0xFF09;
    case VK_BACK:     return 0xFF08;
    case VK_DELETE:   return 0xFFFF;
    case VK_LEFT:     return 0xFF51;
    case VK_UP:       return 0xFF52;
    case VK_RIGHT:    return 0xFF53;
    case VK_DOWN:     return 0xFF54;
    /* The Apple keys. Windows reports both Alts as VK_MENU and distinguishes
     * them by the extended-key bit (bit 24 of lParam), which is set for the
     * right-hand one. Left Alt is Open Apple, right Alt is Closed Apple. */
    case VK_MENU:     return (lparam & (1 << 24)) ? 0xFFEA : 0xFFE9;
    case VK_LMENU:    return 0xFFE9;
    case VK_RMENU:    return 0xFFEA;
    /* A Mac keyboard's Command keys land on the Windows keys, and they are
     * physically the Apple keys. */
    case VK_LWIN:     return 0xFFE7;
    case VK_RWIN:     return 0xFFE8;
    default:          return 0;
    }
}

/* The character this key would type, ignoring Ctrl. Ctrl is masked out of the
 * keyboard state because ToUnicode would otherwise fold it into a control
 * code (Ctrl+A -> 0x01), and the mapping table wants the plain character --
 * it forces the character to 0 for Ctrl combos itself, after deriving the
 * keycode from it. */
static uint32_t translated_char(WPARAM vk, LPARAM lparam)
{
    BYTE state[256];
    WCHAR buf[8];
    int n;

    if (!GetKeyboardState(state))
        return 0;
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
    /* Alt is Open/Closed Apple, not a compose modifier. */
    state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = 0;

    /* Flag bit 2 keeps ToUnicode from consuming pending dead-key state, so
     * probing a keystroke here does not disturb the next one. */
    n = ToUnicode((UINT)vk, (UINT)((lparam >> 16) & 0xFF), state, buf,
                  (int)(sizeof(buf) / sizeof(buf[0])), 0x04);
    return n > 0 ? (uint32_t)buf[0] : 0;
}

static void toggle_fullscreen(HWND hwnd);
static void open_debugger(void);

static void forward_key(WPARAM vk, LPARAM lparam, int down)
{
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    uint32_t unicode = 0;
    uint32_t keysym = keysym_for_vk(vk, lparam);

    if (keysym == 0) {
        unicode = translated_char(vk, lparam);
        keysym = unicode;
    }
    if (keysym == 0)
        return;
    apple2session_key(g_session, down, keysym, unicode, ctrl, shift);
}

static int on_keydown(HWND hwnd, WPARAM vk, LPARAM lparam)
{
    /* Auto-repeat (lParam bit 30 = key was already down): the core latches a
     * key until the machine reads it, so a repeat storm would jam it. Let the
     * Apple II's own repeat handle it. */
    if (lparam & (1 << 30))
        return 1;

    switch (vk) {
    case VK_F10:
        return 0; /* let the system open the menu */
    case VK_F11:
        toggle_fullscreen(hwnd);
        return 1;
    case VK_F12:
        open_debugger();
        return 1;
    case VK_F4:
        /* Alt+F4 stays a window close. */
        if (GetKeyState(VK_MENU) & 0x8000)
            return 0;
        break;
    default:
        break;
    }

    forward_key(vk, lparam, 1);
    return 1;
}

/* ---- settings ------------------------------------------------------------ */

static void apply_display_settings(void)
{
    g_aspect_mode = apple2session_get_int(g_session, "aspect_mode",
                                          ASPECT_TV43);
    g_smooth = apple2session_get_int(g_session, "smooth_scaling", 0);
    g_scanlines = apple2session_get_int(g_session, "scanlines", 1);
}

static void restart_session(void)
{
    apple2session_start_opts opts;
    apple2session_stop(g_session);
    apple2session_default_opts(g_session, &opts);
    if (apple2session_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, apple2session_last_error(g_session), APP_TITLE,
                    MB_ICONWARNING);
}

/* ---- menus ---------------------------------------------------------------
 * The machine, video and slot menus are generated from the option names the
 * core registers, so adding a card upstream shows up here with no edit. */

typedef struct {
    const char *label;   /* menu title */
    const char *key;     /* settings key */
    const char *(*name_at)(int);
    UINT base;           /* first command id */
} option_menu;

static const option_menu g_option_menus[] = {
    {"&Model",  "machine",    apple2session_machine_name,    IDM_MACHINE_BASE},
    {"&Video",  "video_mode", apple2session_video_mode_name, IDM_VIDEO_BASE},
    {"Slot &3", "slot3",      apple2session_slot3_name,      IDM_SLOT3_BASE},
    {"Slot &4", "slot4",      apple2session_slot4_name,      IDM_SLOT4_BASE},
    {"Slot &5", "slot5",      apple2session_slot5_name,      IDM_SLOT5_BASE},
    {"Slot &6", "slot6",      apple2session_slot6_name,      IDM_SLOT6_BASE},
    {"Slot &7", "slot7",      apple2session_slot7_name,      IDM_SLOT7_BASE},
};
#define OPTION_MENU_COUNT \
    ((int)(sizeof(g_option_menus) / sizeof(g_option_menus[0])))

/* The option menu owning cmd, or NULL. */
static const option_menu *option_menu_for(UINT cmd, int *idx_out)
{
    int i;
    for (i = 0; i < OPTION_MENU_COUNT; i++) {
        const option_menu *m = &g_option_menus[i];
        if (cmd >= m->base && cmd < m->base + IDM_OPT_SPAN) {
            *idx_out = (int)(cmd - m->base);
            return m;
        }
    }
    return NULL;
}

/* Which entry of m is currently selected. Defaults to the first, which is
 * also what apple2session_default_opts falls back to. */
static int option_current_index(const option_menu *m)
{
    const char *current = apple2session_get_str(g_session, m->key, NULL);
    int i;
    if (!current)
        return 0;
    for (i = 0; i < IDM_OPT_SPAN; i++) {
        const char *name = m->name_at(i);
        if (!name)
            break;
        if (strcmp(name, current) == 0)
            return i;
    }
    return 0;
}

static void sync_menu_checks(void)
{
    int i;
    for (i = 0; i < OPTION_MENU_COUNT; i++) {
        const option_menu *m = &g_option_menus[i];
        int count = 0, sel;
        while (m->name_at(count))
            count++;
        if (count == 0)
            continue;
        sel = option_current_index(m);
        CheckMenuRadioItem(g_menu, m->base, m->base + (UINT)count - 1,
                           m->base + (UINT)sel, MF_BYCOMMAND);
    }
    CheckMenuRadioItem(g_menu, IDM_ASPECT_BASE, IDM_ASPECT_BASE + 2,
                       IDM_ASPECT_BASE + (UINT)g_aspect_mode, MF_BYCOMMAND);
    CheckMenuItem(g_menu, IDM_SCANLINES,
                  MF_BYCOMMAND | (g_scanlines ? MF_CHECKED : MF_UNCHECKED));
}

static HMENU build_option_menu(const option_menu *m)
{
    HMENU menu = CreatePopupMenu();
    int i;
    for (i = 0; i < IDM_OPT_SPAN; i++) {
        const char *name = m->name_at(i);
        if (!name)
            break;
        AppendMenuA(menu, MF_STRING, m->base + (UINT)i, name);
    }
    return menu;
}

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU media = CreatePopupMenu();
    HMENU fujinet = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU aspect = CreatePopupMenu();
    HMENU help = CreatePopupMenu();
    int i;

    AppendMenuA(machine, MF_STRING, IDM_RESET, "&Reset");
    AppendMenuA(machine, MF_STRING, IDM_POWER_CYCLE, "&Power Cycle");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < OPTION_MENU_COUNT; i++)
        AppendMenuA(machine, MF_POPUP,
                    (UINT_PTR)build_option_menu(&g_option_menus[i]),
                    g_option_menus[i].label);
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");

    AppendMenuA(media, MF_STRING, IDM_IMPORT_MEDIA, "Import &Disk Image...");
    AppendMenuA(media, MF_STRING, IDM_IMPORT_ROMS, "Import System &ROMs...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)media, "Me&dia");

    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_CONFIG, "&Configuration...");
    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_LOG, "Console &Log...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fujinet, "&FujiNet");

    AppendMenuA(aspect, MF_STRING, IDM_ASPECT_BASE + 0, "&TV (4:3)");
    AppendMenuA(aspect, MF_STRING, IDM_ASPECT_BASE + 1, "&Square pixels");
    AppendMenuA(aspect, MF_STRING, IDM_ASPECT_BASE + 2, "&Integer scale");
    AppendMenuA(view, MF_POPUP, (UINT_PTR)aspect, "&Aspect Ratio");
    AppendMenuA(view, MF_STRING, IDM_SCANLINES, "Scan&lines");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tF11");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    return bar;
}

/* ---- menu actions -------------------------------------------------------- */

static int open_file(HWND hwnd, const char *filter, char *out, int outsz,
                     DWORD extra_flags)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = (DWORD)outsz;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                extra_flags;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void import_media(HWND hwnd)
{
    char src[MAX_PATH], dest[1024];
    if (!open_file(hwnd,
                   "Apple II disk images "
                   "(*.dsk;*.do;*.po;*.2mg;*.nib;*.woz;*.hdv)\0"
                   "*.dsk;*.do;*.po;*.2mg;*.nib;*.woz;*.hdv\0"
                   "All files (*.*)\0*.*\0",
                   src, sizeof(src), 0))
        return;
    if (apple2session_import_media(g_session, src, dest, sizeof(dest)) != 0) {
        MessageBoxA(hwnd, apple2session_last_error(g_session), APP_TITLE,
                    MB_ICONWARNING);
        return;
    }
    MessageBoxA(hwnd,
                "Copied to the FujiNet SD folder.\n\n"
                "Mount it from FujiNet > Configuration.",
                APP_TITLE, MB_ICONINFORMATION);
}

/* GetOpenFileName with OFN_ALLOWMULTISELECT returns the directory followed by
 * NUL-separated file names, all in one buffer; a single selection is returned
 * as a plain path with no separator. Walk both shapes. */
static void import_roms(HWND hwnd)
{
    char buf[8192], msg[128];
    const char *dir;
    const char *p;
    int ok = 0;

    if (!open_file(hwnd,
                   "Apple II system ROMs (*.rom;*.bin)\0*.rom;*.bin\0"
                   "All files (*.*)\0*.*\0",
                   buf, sizeof(buf), OFN_ALLOWMULTISELECT))
        return;

    dir = buf;
    p = dir + strlen(dir) + 1;
    if (*p == '\0') {
        /* Single selection: buf is the whole path. */
        char dest[1024];
        if (apple2session_import_rom(g_session, buf, dest, sizeof(dest)) == 0)
            ok = 1;
    } else {
        for (; *p; p += strlen(p) + 1) {
            char path[MAX_PATH * 2], dest[1024];
            snprintf(path, sizeof(path), "%s\\%s", dir, p);
            if (apple2session_import_rom(g_session, path, dest,
                                         sizeof(dest)) == 0)
                ok++;
        }
    }

    if (ok == 0) {
        MessageBoxA(hwnd, apple2session_last_error(g_session), APP_TITLE,
                    MB_ICONWARNING);
        return;
    }
    restart_session();
    snprintf(msg, sizeof(msg), "Imported %d ROM file%s (session restarted).",
             ok, ok == 1 ? "" : "s");
    MessageBoxA(hwnd, msg, APP_TITLE, MB_ICONINFORMATION);
}

static void show_fujinet_config(void)
{
    ShellExecuteA(NULL, "open", apple2session_fujinet_webui_url(g_session),
                  NULL, NULL, SW_SHOWNORMAL);
}

/* ---- console log window -------------------------------------------------- */

#define LOG_TIMER_ID 1

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        MoveWindow(g_log_edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_TIMER: {
        static char buf[128 * 1024];
        int n = apple2session_fujinet_copy_log(g_session, buf, sizeof(buf));
        SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, LOG_TIMER_ID);
        DestroyWindow(hwnd);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    static int registered;
    if (g_log_window) {
        SetForegroundWindow(g_log_window);
        return;
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = log_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "Apple2LogWindow";
        RegisterClassA(&wc);
        registered = 1;
    }
    g_log_window = CreateWindowA(
        "Apple2LogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 560, NULL, NULL, inst, NULL);
    g_log_edit = CreateWindowA(
        "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, 0, 0, g_log_window, NULL, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT,
                 (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
    SetTimer(g_log_window, LOG_TIMER_ID, 1000, NULL);
    ShowWindow(g_log_window, SW_SHOW);
}

static void open_debugger(void)
{
    apple2_debugger_show(g_hwnd, g_session);
}

/* ---- fullscreen ---------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetWindowPlacement(hwnd, &g_prev_placement) &&
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY),
                           &mi)) {
            SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetMenu(hwnd, g_menu);
        SetWindowPlacement(hwnd, &g_prev_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
}

/* ---- window -------------------------------------------------------------- */

static void on_command(HWND hwnd, UINT cmd)
{
    const option_menu *m;
    int idx;

    m = option_menu_for(cmd, &idx);
    if (m) {
        const char *name = m->name_at(idx);
        if (name && strcmp(name, apple2session_get_str(g_session, m->key,
                                                       "")) != 0) {
            apple2session_set_str(g_session, m->key, name);
            /* Every one of these is a libretro core option, read when the
             * core loads: they only take effect across a restart. */
            restart_session();
            sync_menu_checks();
        }
        return;
    }
    if (cmd >= IDM_ASPECT_BASE && cmd <= IDM_ASPECT_BASE + 2) {
        g_aspect_mode = (int)(cmd - IDM_ASPECT_BASE);
        apple2session_set_int(g_session, "aspect_mode", g_aspect_mode);
        sync_menu_checks();
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    switch (cmd) {
    case IDM_RESET:       apple2session_reset(g_session, 0); break;
    case IDM_POWER_CYCLE: apple2session_reset(g_session, 1); break;
    case IDM_IMPORT_MEDIA: import_media(hwnd); break;
    case IDM_IMPORT_ROMS:  import_roms(hwnd); break;
    case IDM_FUJINET_CONFIG: show_fujinet_config(); break;
    case IDM_FUJINET_LOG:
        show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
        break;
    case IDM_SCANLINES:
        g_scanlines = !g_scanlines;
        g_smooth = g_scanlines;
        apple2session_set_int(g_session, "scanlines", g_scanlines);
        /* Smoothing is no longer its own option; it just follows scanlines,
         * which is a libretro core option and needs a restart. */
        apple2session_set_int(g_session, "smooth_scaling", g_smooth);
        restart_session();
        sync_menu_checks();
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case IDM_FULLSCREEN: toggle_fullscreen(hwnd); break;
    case IDM_DEBUGGER:   open_debugger(); break;
    case IDM_ABOUT:
        MessageBoxA(hwnd,
                    APP_TITLE " " APPLE2_VERSION_STRING "\n\n"
                    "Self-contained Apple II with built-in FujiNet.\n"
                    "Emulation by AppleWin; FujiNet runs in-process over\n"
                    "SmartPort-over-SLIP.\n\n"
                    "Copyright (C) 2026 Thomas Cherryhomes\n"
                    "GPL-3.0-or-later -- https://fujinet.online/",
                    "About " APP_TITLE, MB_ICONINFORMATION);
        break;
    case IDM_EXIT: DestroyWindow(hwnd); break;
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1; /* paint() clears; avoid flicker */
    case WM_SIZE:
        /* A resize only invalidates the newly exposed strips, and BeginPaint
         * clips the repaint to them -- the rest of the client area would keep
         * the image blitted at the old size (visible as a ghost after
         * maximize or F11). Ask for the whole client area instead. */
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (on_keydown(hwnd, wp, lp))
            return 0;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        forward_key(wp, lp, 0);
        return 0;
    case WM_SYSCHAR:
        /* Alt is Open/Closed Apple, not a menu accelerator: swallow the
         * synthesised WM_SYSCHAR so Alt+key does not ding and does not
         * activate the menu bar. F10 still opens it (see on_keydown). */
        return 0;
    case WM_KILLFOCUS:
        /* Releases are delivered to the focused window, so a chord that
         * loses focus mid-press (Alt+Tab, in particular) would leave Open
         * Apple stuck down. Lift both Apple keys on the way out. */
        apple2session_key(g_session, 0, 0xFFE9, 0, 0, 0);
        apple2session_key(g_session, 0, 0xFFEA, 0, 0, 0);
        return 0;
    case WM_COMMAND:
        on_command(hwnd, LOWORD(wp));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEXA wc;
    MSG msg;
    apple2session_start_opts opts;
    (void)prev;
    (void)cmd;

    SetProcessDPIAware();
    InitializeCriticalSection(&g_fb_lock);
    g_prev_placement.length = sizeof(g_prev_placement);

    g_session = apple2session_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session.", APP_TITLE,
                    MB_ICONERROR);
        return 1;
    }
    apple2session_default_opts(g_session, &opts);
    if (apple2session_start(g_session, &opts) != 0)
        MessageBoxA(NULL, apple2session_last_error(g_session), APP_TITLE,
                    MB_ICONWARNING);
    apply_display_settings();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    /* Redraw the whole client area on any size change; the display is
     * stretched to fit, so a partial repaint leaves stale pixels. */
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "Apple2MainWindow";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExA(&wc);

    g_menu = build_menu();
    g_hwnd = CreateWindowExA(0, "Apple2MainWindow", APP_TITLE,
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1120, 900, NULL, g_menu, inst, NULL);
    if (!g_hwnd)
        return 1;
    sync_menu_checks();
    ShowWindow(g_hwnd, show);

    g_present_run = 1;
    pthread_create(&g_present_thread, NULL, present_main, NULL);

    /* Developer affordance, matching the other frontends: open the debugger
     * alongside the main window, which is what you want when the app dies
     * before you can reach a menu. */
    if (getenv("APPLE2_OPEN_DEBUGGER"))
        open_debugger();

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        /* The debugger's F5/F7/F8 accelerators get first refusal while its
         * window (or one of its fields) has the focus. */
        if (apple2_debugger_pretranslate(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    g_present_run = 0;
    pthread_join(g_present_thread, NULL);
    apple2session_free(g_session);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}

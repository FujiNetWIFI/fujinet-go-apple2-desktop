/*
 * help_window.c -- the Win32 Help window: the topics on the left, with a
 * search that lists every section having all the words typed; the topic on
 * the right in a RichEdit, filled a run at a time from its blocks (tables and
 * bullets on tab stops with a hanging indent), its matches highlighted when
 * it came from a search.
 *
 * Msftedit.dll is loaded when the window is first made, not linked: every
 * Windows has it, and the release checks the import table.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "help_window.h"

#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>

#include <string.h>

#include "apple2help.h"

#define ME APPLE2HELP_WINDOWS
#define HELP_CLASS "FujiNetGoApple2Help"
#define IDC_SEARCH 100
#define IDC_LIST 101
#define IDC_VIEW 102
#define SIDE_W 250
#define MAX_HITS 64
#define MAX_LINKS 256
#define MAX_CONTEXTS 8

enum { ST_TEXT, ST_BOLD, ST_ITALIC, ST_CODE, ST_KBD, ST_LINK, ST_TITLE, ST_H2, ST_H3, ST_HEAD, ST_PRE };

typedef struct {
    LONG min, max;
    const char *target;
} link_range;

static HWND g_win, g_owner, g_search, g_list, g_view;
static HFONT g_font;
static int g_dpi = 96, g_twips = 200;
static wchar_t g_face[LF_FACESIZE] = L"Segoe UI";
static int g_topic = -1, g_block = -1, g_filling, g_searching, g_nhits;
static char g_query[256];
static LONG *g_block_cp;            /* where each block of the topic starts */
static link_range g_links[MAX_LINKS];
static int g_nlinks;
static apple2help_hit g_hits[MAX_HITS];
static struct {
    HWND hwnd;
    char context[48];
    int f1;
} g_contexts[MAX_CONTEXTS];

static int S(int v)
{
    return apple2_scale(v, g_dpi);
}

/* UTF-8 into a wide buffer of n, always terminated. */
static void help_wide(const char *utf8, wchar_t *w, int n)
{
    w[0] = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, w, n) <= 0) w[n - 1] = 0;
}

static COLORREF mix(COLORREF a, COLORREF b, int pct)
{
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * pct / 100,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * pct / 100,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * pct / 100);
}

/* ---- contexts ----------------------------------------------------------------------- */

void apple2_help_set_context(HWND window, const char *context, int f1)
{
    int i, slot = -1;
    for (i = 0; i < MAX_CONTEXTS; i++) {
        if (g_contexts[i].hwnd == window) { slot = i; break; }
        if (slot < 0 && (!g_contexts[i].hwnd || !IsWindow(g_contexts[i].hwnd))) slot = i;
    }
    if (slot < 0) return;
    g_contexts[slot].hwnd = window;
    snprintf(g_contexts[slot].context, sizeof g_contexts[slot].context, "%s", context ? context : "main");
    g_contexts[slot].f1 = f1;
}

static int find_context(HWND window)
{
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (window && g_contexts[i].hwnd == window) return i;
    return -1;
}

void apple2_help_for(HWND window)
{
    const int c = find_context(window);
    if (g_win && window == g_win) {
        SetFocus(g_search);
        SendMessageW(g_search, EM_SETSEL, 0, -1);
        return;
    }
    apple2_help_show(window, c >= 0 ? g_contexts[c].context : "main");
}

/* ---- the page ------------------------------------------------------------------------- */

static LONG text_end(void)
{
    GETTEXTLENGTHEX gl = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
    return (LONG)SendMessageW(g_view, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
}

static void style(CHARFORMAT2W *cf, int st)
{
    memset(cf, 0, sizeof *cf);
    cf->cbSize = sizeof *cf;
    cf->dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_LINK | CFM_BACKCOLOR | CFM_COLOR;
    cf->dwEffects = CFE_AUTOCOLOR | CFE_AUTOBACKCOLOR;
    cf->yHeight = g_twips;
    wcsncpy(cf->szFaceName, g_face, LF_FACESIZE - 1);
    switch (st) {
    case ST_BOLD: case ST_HEAD: cf->dwEffects |= CFE_BOLD; break;
    case ST_ITALIC: cf->dwEffects |= CFE_ITALIC; break;
    case ST_LINK: cf->dwEffects |= CFE_LINK; break;
    case ST_TITLE: cf->dwEffects |= CFE_BOLD; cf->yHeight = g_twips * 17 / 10; break;
    case ST_H2: cf->dwEffects |= CFE_BOLD; cf->yHeight = g_twips * 13 / 10; break;
    case ST_H3: cf->dwEffects |= CFE_BOLD; cf->yHeight = g_twips * 11 / 10; break;
    case ST_CODE: case ST_KBD: case ST_PRE:
        wcsncpy(cf->szFaceName, L"Consolas", LF_FACESIZE - 1);
        cf->dwEffects &= ~(DWORD)CFE_AUTOBACKCOLOR;
        cf->crBackColor = mix(GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT), 12);
        if (st == ST_KBD) cf->dwEffects |= CFE_BOLD;
        break;
    default: break;
    }
}

/* text at the end in a style; where it starts */
static LONG put(const char *utf8, int st)
{
    CHARFORMAT2W cf;
    CHARRANGE cr;
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    LONG at = text_end();
    if (!w) return at;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    for (int i = 0; w[i]; i++)
        if (w[i] == L'\n') w[i] = L'\r';
    cr.cpMin = cr.cpMax = at;
    SendMessageW(g_view, EM_EXSETSEL, 0, (LPARAM)&cr);
    style(&cf, st);
    SendMessageW(g_view, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(g_view, EM_REPLACESEL, FALSE, (LPARAM)w);
    free(w);
    return at;
}

/* the paragraphs from start on: indents, tab stops and spacing in twips */
static void para(LONG start, LONG indent, LONG offset, const LONG *tabs, int ntabs, LONG before, LONG after)
{
    PARAFORMAT2 pf;
    CHARRANGE cr = { start, text_end() };
    SendMessageW(g_view, EM_EXSETSEL, 0, (LPARAM)&cr);
    memset(&pf, 0, sizeof pf);
    pf.cbSize = sizeof pf;
    pf.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_TABSTOPS | PFM_SPACEBEFORE | PFM_SPACEAFTER;
    pf.dxStartIndent = indent;
    pf.dxOffset = offset;
    pf.cTabCount = (SHORT)(ntabs < MAX_TAB_STOPS ? ntabs : MAX_TAB_STOPS);
    for (int i = 0; i < pf.cTabCount; i++) pf.rgxTabs[i] = tabs[i];
    pf.dySpaceBefore = before;
    pf.dySpaceAfter = after;
    SendMessageW(g_view, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

static void put_spans(const apple2help_block *b, int base)
{
    for (int i = 0; i < b->nspans; i++) {
        const apple2help_span *sp = &b->spans[i];
        int st = base;
        LONG at;
        switch (sp->kind) {
        case APPLE2HELP_CELL: put("\t", base); continue;
        case APPLE2HELP_BOLD: st = ST_BOLD; break;
        case APPLE2HELP_ITALIC: st = ST_ITALIC; break;
        case APPLE2HELP_CODE: st = ST_CODE; break;
        case APPLE2HELP_KBD: st = ST_KBD; break;
        case APPLE2HELP_LINK: st = ST_LINK; break;
        default: break;
        }
        if (st == ST_CODE || st == ST_KBD) put("\xe2\x80\x89", st);
        at = put(apple2help_span_text(sp, ME), st);
        if (st == ST_LINK && g_nlinks < MAX_LINKS) {
            g_links[g_nlinks].min = at;
            g_links[g_nlinks].max = text_end();
            g_links[g_nlinks++].target = sp->target;
        }
        if (st == ST_CODE || st == ST_KBD) put("\xe2\x80\x89", st);
    }
}

/* every word of the query, highlighted */
static void highlight(void)
{
    char words[256], *w = words, *end;
    snprintf(words, sizeof words, "%s", g_query);
    for (; *w; w = end) {
        wchar_t ww[128];
        FINDTEXTEXW ft;
        while (*w == ' ') w++;
        for (end = w; *end && *end != ' '; end++) {}
        if (*end) *end++ = '\0';
        if (!*w) continue;
        help_wide(w, ww, 128);
        ft.chrg.cpMin = 0;
        ft.chrg.cpMax = -1;
        ft.lpstrText = ww;
        while (SendMessageW(g_view, EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&ft) >= 0) {
            CHARFORMAT2W cf;
            CHARRANGE cr = ft.chrgText;
            if (cr.cpMax <= cr.cpMin) break;
            SendMessageW(g_view, EM_EXSETSEL, 0, (LPARAM)&cr);
            memset(&cf, 0, sizeof cf);
            cf.cbSize = sizeof cf;
            cf.dwMask = CFM_BACKCOLOR | CFM_COLOR;
            cf.crBackColor = RGB(0xf8, 0xe4, 0x5c);
            cf.crTextColor = RGB(0, 0, 0);
            SendMessageW(g_view, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
            ft.chrg.cpMin = cr.cpMax;
        }
    }
}

/* character cp's line at the top */
static void scroll_to_cp(LONG cp)
{
    const int line = (int)SendMessageW(g_view, EM_LINEFROMCHAR, (WPARAM)cp, 0);
    const int first = (int)SendMessageW(g_view, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(g_view, EM_LINESCROLL, 0, line - first);
}

/* the block's heading at the top */
static void scroll_to_block(void)
{
    const apple2help_topic *tp = apple2help_topic_at(g_topic);
    scroll_to_cp((tp && g_block >= 0 && g_block < tp->nblocks) ? g_block_cp[g_block] : 0);
}

static void render(void)
{
    const apple2help_topic *tp = apple2help_topic_at(g_topic);
    CHARRANGE home = { 0, 0 };
    LONG start, tabs[MAX_TAB_STOPS];
    if (!tp) return;
    SendMessageW(g_view, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(g_view, L"");
    g_nlinks = 0;
    free(g_block_cp);
    g_block_cp = calloc((size_t)(tp->nblocks ? tp->nblocks : 1), sizeof *g_block_cp);

    start = put(tp->title, ST_TITLE);
    put("\r", ST_TITLE);
    para(start, 0, 0, NULL, 0, 0, 120);
    for (int i = 0; i < tp->nblocks; i++) {
        const apple2help_block *b = &tp->blocks[i];
        start = text_end();
        if (g_block_cp) g_block_cp[i] = start;
        if (!(b->platforms & ME)) continue;
        switch (b->kind) {
        case APPLE2HELP_H2:
        case APPLE2HELP_H3:
            put(apple2help_heading(b), b->kind == APPLE2HELP_H2 ? ST_H2 : ST_H3);
            put("\r", ST_TEXT);
            para(start, 0, 0, NULL, 0, b->kind == APPLE2HELP_H2 ? 280 : 160, 80);
            break;
        case APPLE2HELP_BULLET:
        case APPLE2HELP_BULLET2:
        case APPLE2HELP_NUMBERED: {
            const LONG base = b->kind == APPLE2HELP_BULLET2 ? 360 : 0;
            char mark[16];
            if (b->kind == APPLE2HELP_NUMBERED) snprintf(mark, sizeof mark, "%d.\t", b->number);
            else snprintf(mark, sizeof mark, "%s\t", b->kind == APPLE2HELP_BULLET ? "\xe2\x80\xa2" : "\xe2\x97\xa6");
            put(mark, ST_TEXT);
            put_spans(b, ST_TEXT);
            put("\r", ST_TEXT);
            tabs[0] = base + 360;
            para(start, base, 360, tabs, 1, 0, 60);
            break;
        }
        case APPLE2HELP_PRE:
            put(b->spans[0].text, ST_PRE);
            put("\r", ST_TEXT);
            para(start, 360, 0, NULL, 0, 60, 120);
            break;
        case APPLE2HELP_TABLE_HEAD:
        case APPLE2HELP_TABLE_ROW: {
            LONG at = 0;
            int n = b->ncol_em < MAX_TAB_STOPS ? b->ncol_em : MAX_TAB_STOPS;
            for (int k = 0; k < n; k++) tabs[k] = at += (LONG)b->col_em[k] * g_twips;
            put_spans(b, b->kind == APPLE2HELP_TABLE_HEAD ? ST_HEAD : ST_TEXT);
            put("\r", ST_TEXT);
            para(start, 0, at, tabs, n, b->kind == APPLE2HELP_TABLE_HEAD ? 60 : 0, 40);
            break;
        }
        case APPLE2HELP_NOTE:
            put_spans(b, ST_TEXT);
            put("\r", ST_TEXT);
            para(start, 360, 0, NULL, 0, 60, 120);
            break;
        default:
            put_spans(b, ST_TEXT);
            put("\r", ST_TEXT);
            para(start, 0, 0, NULL, 0, 0, 120);
            break;
        }
    }
    highlight();
    SendMessageW(g_view, EM_EXSETSEL, 0, (LPARAM)&home);
    SendMessageW(g_view, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_view, NULL, TRUE);
    scroll_to_block();
}

static void show_topic(int topic, int block, const char *query)
{
    if (!apple2help_topic_at(topic)) return;
    g_block = block;
    if (topic != g_topic || strcmp(query, g_query) != 0) {
        g_topic = topic;
        snprintf(g_query, sizeof g_query, "%s", query);
        render();
    } else {
        scroll_to_block();
    }
}

static void follow_link(LONG cp)
{
    for (int i = 0; i < g_nlinks; i++) {
        if (cp < g_links[i].min || cp >= g_links[i].max) continue;
        if (g_links[i].target[0] == '#') apple2_help_show(g_win, g_links[i].target);
        else ShellExecuteA(g_win, "open", g_links[i].target, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
}

/* ---- the list --------------------------------------------------------------------------- */

static void search_text(char *dst, int dstsz)
{
    wchar_t w[256];
    char *p;
    GetWindowTextW(g_search, w, 256);
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, dstsz, NULL, NULL) <= 0) dst[0] = '\0';
    for (p = dst; *p == ' '; p++) {}
    memmove(dst, p, strlen(p) + 1);
    for (size_t n = strlen(dst); n && dst[n - 1] == ' '; n--) dst[n - 1] = '\0';
}

static void add_row(const char *utf8)
{
    wchar_t w[512];
    help_wide(utf8, w, 512);
    SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)w);
}

static void fill_list(void)
{
    char q[256], row[512];
    search_text(q, sizeof q);
    g_filling = 1;
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    g_searching = q[0] != '\0';
    if (!g_searching) {
        for (int t = 0; t < apple2help_topic_count(); t++) add_row(apple2help_topic_at(t)->title);
        if (g_topic >= 0) SendMessageW(g_list, LB_SETCURSEL, (WPARAM)g_topic, 0);
    } else {
        g_nhits = apple2help_search(ME, q, g_hits, MAX_HITS);
        for (int i = 0; i < g_nhits; i++) {
            const apple2help_topic *tp = apple2help_topic_at(g_hits[i].topic);
            if (g_hits[i].block < 0) snprintf(row, sizeof row, "%s", tp->title);
            else snprintf(row, sizeof row, "%s \xc2\xb7 %s", apple2help_heading(&tp->blocks[g_hits[i].block]),
                          tp->title);
            add_row(row);
        }
        if (!g_nhits) add_row("No matches");
    }
    g_filling = 0;
}

static void on_select(void)
{
    char q[256];
    const int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (g_filling || i < 0) return;
    search_text(q, sizeof q);
    if (!g_searching) show_topic(i, -1, "");
    else if (i < g_nhits) show_topic(g_hits[i].topic, g_hits[i].block, q);
}

static void select_row(int i)
{
    if (i < 0 || i >= (int)SendMessageW(g_list, LB_GETCOUNT, 0, 0)) return;
    SendMessageW(g_list, LB_SETCURSEL, (WPARAM)i, 0);
    on_select();
}

/* ---- the window --------------------------------------------------------------------------- */

static void layout(void)
{
    RECT c;
    POINTL corner = { 1, 1 };
    const int side = S(SIDE_W), eh = S(24);
    /* resized, the page keeps its place: what was at its top stays there */
    const LONG top = g_topic >= 0 && IsWindowVisible(g_win)
        ? (LONG)SendMessageW(g_view, EM_CHARFROMPOS, 0, (LPARAM)&corner) : -1;
    GetClientRect(g_win, &c);
    MoveWindow(g_search, S(8), S(8), side - S(12), eh, TRUE);
    MoveWindow(g_list, S(8), S(8) + eh + S(6), side - S(12), c.bottom - (S(8) + eh + S(6)) - S(8), TRUE);
    MoveWindow(g_view, side + S(4), 0, c.right - side - S(4) > 1 ? c.right - side - S(4) : 1, c.bottom, TRUE);
    if (top > 0) scroll_to_cp(top);
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    const int sys = apple2_system_dpi();
    if (g_font) DeleteObject(g_font);
    g_font = apple2_make_font(g_dpi, 0, FW_NORMAL, NULL);
    SendMessageW(g_search, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    /* the page in the message font, a point larger; RichEdit sizes it at
     * the system DPI, so a monitor of another is a zoom */
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        wcsncpy(g_face, ncm.lfMessageFont.lfFaceName, LF_FACESIZE - 1);
        g_twips = abs(ncm.lfMessageFont.lfHeight) * 1440 / sys + 20;
    }
    if (g_twips < 180) g_twips = 180;
    SendMessageW(g_view, EM_SETZOOM, g_dpi == sys ? 0 : (WPARAM)g_dpi, g_dpi == sys ? 0 : (LPARAM)sys);
    SendMessageW(g_view, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(16), S(16)));
}

static LRESULT CALLBACK help_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SEARCH && HIWORD(wp) == EN_CHANGE) {
            fill_list();
            return 0;
        }
        if (LOWORD(wp) == IDC_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            on_select();
            return 0;
        }
        break;
    case WM_NOTIFY: {
        const NMHDR *n = (const NMHDR *)lp;
        if (n->hwndFrom == g_view && n->code == EN_LINK) {
            const ENLINK *e = (const ENLINK *)lp;
            if (e->msg == WM_LBUTTONUP) follow_link(e->chrg.cpMin);
            return 0;
        }
        break;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        g_dpi = HIWORD(wp);
        make_fonts();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        make_fonts();
        render();
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        if (g_owner && IsWindow(g_owner)) SetForegroundWindow(g_owner);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int create_window(HWND owner)
{
    static HMODULE richedit;
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSEXA wc;
    RECT pr;

    if (!richedit) richedit = LoadLibraryW(L"Msftedit.dll");
    if (!richedit) {
        apple2_message(owner, "The Help needs Msftedit.dll, which is part of Windows.", "Help",
                      MB_ICONERROR | MB_OK);
        return 0;
    }
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = help_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = HELP_CLASS;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(101));
    RegisterClassExA(&wc);

    /* owned by the main window (not a popup: its keys are its own) */
    while (owner && GetWindow(owner, GW_OWNER)) owner = GetWindow(owner, GW_OWNER);
    g_owner = owner;
    g_dpi = owner ? apple2_window_dpi(owner) : apple2_system_dpi();
    if (owner) GetWindowRect(owner, &pr);
    else SetRect(&pr, 0, 0, 0, 0);
    g_win = CreateWindowExA(WS_EX_CONTROLPARENT, HELP_CLASS, "FujiNet Go Apple II Help",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, pr.left + S(80), pr.top + S(60), S(980),
                            S(720), owner, NULL, inst, NULL);
    if (!g_win) return 0;
    if (apple2_window_dpi(g_win) != g_dpi) {
        g_dpi = apple2_window_dpi(g_win);
        SetWindowPos(g_win, NULL, 0, 0, S(980), S(720), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_SEARCH, inst, NULL);
    SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search Help");
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_LIST, inst, NULL);
    g_view = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                 ES_AUTOVSCROLL | ES_NOHIDESEL,
                             0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_VIEW, inst, NULL);
    SendMessageW(g_view, EM_SETEVENTMASK, 0, ENM_LINK);
    SendMessageW(g_view, EM_AUTOURLDETECT, FALSE, 0);
    make_fonts();
    layout();
    fill_list();
    return 1;
}

void apple2_help_show(HWND owner, const char *id)
{
    int t = 0, b = -1;
    if (!g_win && !create_window(owner)) return;
    if (id && id[0] == '?') {
        wchar_t w[256];
        help_wide(id + 1, w, 256);
        SetWindowTextW(g_search, w);        /* EN_CHANGE fills the list */
        if (g_nhits) select_row(0);
    } else if (id || g_topic < 0) {
        if (!id || apple2help_find(id, &t, &b) != 0) {
            t = 0;
            b = -1;
        }
        if (GetWindowTextLengthW(g_search)) SetWindowTextW(g_search, L"");
        show_topic(t, b, "");
        g_filling = 1;
        SendMessageW(g_list, LB_SETCURSEL, (WPARAM)t, 0);
        g_filling = 0;
    }
    ShowWindow(g_win, IsIconic(g_win) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_win);
    /* the lines are only right once it is shown at its size */
    UpdateWindow(g_win);
    scroll_to_block();
}

int apple2_help_pretranslate(MSG *msg)
{
    HWND root;
    int ctrl, shift, alt, c;
    if (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) return 0;
    root = msg->hwnd ? GetAncestor(msg->hwnd, GA_ROOT) : NULL;
    if (!root) return 0;
    ctrl = GetKeyState(VK_CONTROL) < 0;
    shift = GetKeyState(VK_SHIFT) < 0;
    alt = GetKeyState(VK_MENU) < 0;
    if (ctrl && shift && !alt && msg->wParam == 'H') {
        if (!(msg->lParam & (1L << 30))) apple2_help_for(root);
        return 1;
    }
    c = find_context(root);
    if (msg->wParam == VK_F1 && !ctrl && !shift && !alt && c >= 0 && g_contexts[c].f1) {
        if (!(msg->lParam & (1L << 30))) apple2_help_for(root);
        return 1;
    }
    if (!g_win || root != g_win || msg->message != WM_KEYDOWN) return 0;

    /* the Help's own keys */
    if (ctrl && !shift && msg->wParam == 'F') {
        apple2_help_for(g_win);
        return 1;
    }
    if (msg->wParam == VK_TAB && !ctrl && !alt) {
        HWND order[3] = { g_search, g_list, g_view }, focus = GetFocus();
        int i = 0;
        while (i < 3 && order[i] != focus) i++;
        SetFocus(order[((i < 3 ? i : 0) + (shift ? 2 : 1)) % 3]);
        return 1;
    }
    if (msg->hwnd == g_search) {
        switch (msg->wParam) {
        case VK_RETURN:
            if (g_searching && g_nhits) select_row(0);
            return 1;
        case VK_DOWN:
            SetFocus(g_list);
            if (SendMessageW(g_list, LB_GETCURSEL, 0, 0) < 0) select_row(0);
            return 1;
        case VK_ESCAPE:
            if (GetWindowTextLengthW(g_search)) SetWindowTextW(g_search, L"");
            return 1;
        default:
            break;
        }
    }
    return 0;
}

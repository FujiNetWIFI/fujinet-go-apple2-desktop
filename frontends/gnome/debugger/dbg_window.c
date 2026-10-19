/*
 * The GNOME frontend's debugger window: run control, and two tabs. CPU has
 * the 6502 registers, disassembly with click-to-toggle breakpoints, a memory
 * view and a video page; FujiNet has the SmartPort packets the card and
 * FujiNet exchange, both ways, and the card's state.
 *
 * All the machine state comes from the shared engine (core/debugger), which
 * owns the rule that only the emulator thread touches AppleWin. This file is
 * windowing and painting -- the one thing it must get right is that the
 * engine's stop callback fires ON THE EMULATOR THREAD, so it is marshalled
 * onto the main loop with g_idle_add before any widget is touched.
 *
 * Keys match the rest of the family: F5 pause/continue, F7 step into,
 * F8 step over, Shift+F8 step out.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "apple2debug.h"

#define DISASM_LINES 24
#define MEM_ROWS     16
#define MEM_COLS     16
#define TRACE_MAX    512

enum { TAB_CPU, TAB_FUJINET, TAB_COUNT };

/* APPLE2_DEBUGGER_TAB takes a page number or the start of a name. */
static const char *const k_tab_names[TAB_COUNT] = { "cpu", "fujinet" };
static const char *const k_tab_labels[TAB_COUNT] = { "CPU", "FujiNet" };

typedef struct {
    GtkWindow *win;
    apple2session *session;
    apple2debug *dbg;

    GtkButton *btn_run;
    GtkLabel *status;
    GtkLabel *regs;
    GtkListBox *disasm;
    GtkLabel *mem;
    GtkEntry *mem_entry;
    GtkDropDown *view_pick;
    GtkPicture *view_pic;
    uint32_t *view_fb;

    uint16_t disasm_at;   /* address the listing currently starts at */
    uint16_t mem_at;
    int follow_pc;        /* re-centre the listing on PC when it stops */

    GtkNotebook *notebook;
    GtkLabel *fuji_note;
    GtkTextView *fuji_text;
    GtkTextView *card_text;
    int fuji_count;       /* what the log shows: rebuilt only on a change */
    uint64_t fuji_last;
    guint timer;
} DbgWindow;

static DbgWindow *g_dbg; /* one window per process */

/* The engine's XRGB8888 is a uint32 0x00RRGGBB, so on a little-endian host
 * the bytes run B,G,R,X -- exactly GDK_MEMORY_B8G8R8X8, with no conversion.
 * Same reasoning as display.c, which paints the machine's own frame. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define APPLE2_GDK_FORMAT GDK_MEMORY_X8R8G8B8
#else
#define APPLE2_GDK_FORMAT GDK_MEMORY_B8G8R8X8
#endif

/* ---- text views ---------------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *v = g_utf8_make_valid(text, -1), *s, *d;
    for (s = d = v; *s; s++)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') *d++ = *s;
    *d = '\0';
    return v;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

static gboolean at_end(GtkAdjustment *adj)
{
    return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
           gtk_adjustment_get_upper(adj) - 2.0;
}

/* The log follows its end while its reader is there. New text grows the
 * view only once it is laid out (or its tab first shows), so the end is
 * pinned whenever the view's height changes; whether to follow is decided
 * as text arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    g_signal_connect(vadj(view), "changed", G_CALLBACK(on_log_resized), view);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 || at_end(adj)));
}

static void log_to_end(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    on_log_resized(vadj(view), view);
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextMark *mark;
    GtkTextIter it;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    gtk_text_buffer_set_text(b, clean, -1);
    g_free(clean);
    if (follow) {
        log_to_end(view);
        return;
    }
    if (top <= 0) return;
    /* scroll_to_mark waits for the new text's layout */
    gtk_text_buffer_get_iter_at_line(b, &it, top);
    mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
    gtk_text_buffer_delete_mark(b, mark);
}

/* ---- rendering ----------------------------------------------------------- */

/* Decode whichever video page the dropdown names. This shows a PAGE, not what
 * the machine is currently displaying -- which is the point: it is how you see
 * the buffer a program is drawing into before it flips to it. */
static void render_video(DbgWindow *w)
{
    GdkTexture *tex;
    GBytes *bytes;
    guint sel;

    if (!w->view_fb || !w->view_pic)
        return;
    sel = gtk_drop_down_get_selected(w->view_pick);
    if (sel == GTK_INVALID_LIST_POSITION)
        sel = 0;
    if (apple2debug_render_view(w->dbg, (apple2debug_view)sel, w->view_fb) != 0)
        return;

    bytes = g_bytes_new(w->view_fb, (gsize)APPLE2VIEW_WIDTH *
                                        APPLE2VIEW_HEIGHT * sizeof(uint32_t));
    tex = gdk_memory_texture_new(APPLE2VIEW_WIDTH, APPLE2VIEW_HEIGHT,
                                 APPLE2_GDK_FORMAT, bytes,
                                 (gsize)APPLE2VIEW_WIDTH * sizeof(uint32_t));
    g_bytes_unref(bytes);
    gtk_picture_set_paintable(w->view_pic, GDK_PAINTABLE(tex));
    g_object_unref(tex);
}

static void on_view_changed(GObject *obj, GParamSpec *spec, gpointer user_data)
{
    (void)obj; (void)spec;
    render_video((DbgWindow *)user_data);
}

static void render_regs(DbgWindow *w)
{
    apple2debug_regs r;
    char buf[512];
    static const char k_flag[] = "NV-BDIZC";
    char flags[9];
    int i;

    apple2debug_get_regs(w->dbg, &r);

    /* Bit 7 is N, bit 0 is C; show a set flag in upper case, a clear one as
     * a dot, which is how every 6502 monitor has ever done it.
     * SP is printed as a full address because that is what AppleWin stores:
     * the 6502's 8-bit stack pointer plus the $0100 page base. */
    for (i = 0; i < 8; i++)
        flags[i] = (r.ps & (0x80 >> i)) ? k_flag[i] : '.';
    flags[8] = '\0';

    g_snprintf(buf, sizeof(buf),
               "<tt>"
               "PC  <b>$%04X</b>\n"
               "A   $%02X      X   $%02X\n"
               "Y   $%02X      SP  $%04X\n"
               "P   $%02X   %s\n"
               "%s</tt>",
               r.pc, r.a, r.x, r.y, r.sp, r.ps, flags,
               r.jammed ? "\n<b>CPU JAMMED</b>" : "");
    gtk_label_set_markup(w->regs, buf);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row,
                             gpointer user_data);

static void render_disasm(DbgWindow *w)
{
    apple2dasm_line lines[DISASM_LINES];
    apple2debug_regs r;
    uint16_t next;
    int n, i;
    GtkWidget *child;

    apple2debug_get_regs(w->dbg, &r);
    if (w->follow_pc) {
        w->disasm_at = r.pc;
        w->follow_pc = 0;
    }

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->disasm))))
        gtk_list_box_remove(w->disasm, child);

    n = apple2debug_disassemble(w->dbg, w->disasm_at, DISASM_LINES, lines,
                                &next);
    for (i = 0; i < n; i++) {
        char bytes[16] = "";
        char text[256];
        GtkWidget *label, *row;
        int b;

        for (b = 0; b < lines[i].len; b++)
            g_snprintf(bytes + b * 3, sizeof(bytes) - b * 3, "%02X ",
                       lines[i].bytes[b]);

        g_snprintf(text, sizeof(text),
                   "<tt>%s%s $%04X  %-9s %-16s%s%s</tt>",
                   apple2debug_bp_is_set(w->dbg, lines[i].addr) ? "●" : " ",
                   lines[i].addr == r.pc ? "▶" : " ",
                   lines[i].addr, bytes, lines[i].text,
                   lines[i].symbol ? "  ; " : "",
                   lines[i].symbol ? lines[i].symbol : "");

        label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(label), text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_margin_start(label, 6);
        gtk_widget_set_margin_end(label, 6);

        row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        /* Stash the address so the click handler knows what to toggle. */
        g_object_set_data(G_OBJECT(row), "addr",
                          GUINT_TO_POINTER((guint)lines[i].addr));
        gtk_list_box_append(w->disasm, row);
    }
}

static void render_mem(DbgWindow *w)
{
    GString *s = g_string_new("<tt>");
    uint8_t row[MEM_COLS];
    int r, c;

    for (r = 0; r < MEM_ROWS; r++) {
        const uint16_t addr = (uint16_t)(w->mem_at + r * MEM_COLS);
        apple2debug_read_mem(w->dbg, addr, row, MEM_COLS);
        g_string_append_printf(s, "$%04X ", addr);
        for (c = 0; c < MEM_COLS; c++)
            g_string_append_printf(s, "%02X ", row[c]);
        g_string_append(s, " ");
        for (c = 0; c < MEM_COLS; c++) {
            /* Apple II text has the high bit set; mask it before deciding
             * whether a byte is printable, or nothing ever looks like text. */
            const uint8_t ch = row[c] & 0x7F;
            g_string_append_c(s, (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.');
        }
        g_string_append_c(s, '\n');
    }
    g_string_append(s, "</tt>");
    gtk_label_set_markup(w->mem, s->str);
    g_string_free(s, TRUE);
}

/* The note, the card's state, and the log -- rebuilt only when it changed
 * (or on force), so a reader scrolled up stays where they are. */
static void refresh_fujinet(DbgWindow *w, gboolean force)
{
    static apple2_fuji_packet p[TRACE_MAX];
    static char card[4096];
    const int slot = apple2session_fujinet_slot(w->session);
    GString *t;
    char note[200];
    int n = apple2debug_fujinet_trace(w->dbg, p, TRACE_MAX), i;

    if (!slot)
        g_strlcpy(note, "FujiNet is off (Preferences \xe2\x96\xb8 Slots).", sizeof note);
    else if (!apple2session_fujinet_running(w->session))
        g_snprintf(note, sizeof note, "FujiNet's card is in slot %d, but FujiNet is not running.",
                   slot);
    else
        g_snprintf(note, sizeof note, "FujiNet is on the slot %d SmartPort card%s.", slot,
                   apple2session_fujinet_link(w->session) ? ", connected" : ", not connected");
    gtk_label_set_text(w->fuji_note, note);
    apple2debug_fujinet_state(w->dbg, card, sizeof card);
    set_text_at(w->card_text, card, FALSE);

    if (!force && n == w->fuji_count && (n == 0 || p[n - 1].serial == w->fuji_last))
        return;
    w->fuji_count = n;
    w->fuji_last = n > 0 ? p[n - 1].serial : 0;
    log_note_reader(w->fuji_text);
    t = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        int k;
        g_string_append_printf(t, "%s%s\n     ", i ? "\n" : "", p[i].text);
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            g_string_append_printf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) g_string_append(t, "\xe2\x80\xa6");
    }
    if (n == 0)
        g_string_append(t, "(no SmartPort packets since the trace was cleared)");
    set_text_at(w->fuji_text, t->str, force || following(w->fuji_text));
    g_string_free(t, TRUE);
}

static void refresh_all(DbgWindow *w)
{
    const int paused = apple2debug_is_paused(w->dbg);
    gtk_button_set_label(w->btn_run, paused ? "Continue" : "Pause");
    render_regs(w);
    render_disasm(w);
    render_mem(w);
    render_video(w);
    if (gtk_notebook_get_current_page(w->notebook) == TAB_FUJINET)
        refresh_fujinet(w, FALSE);
}

/* ---- the stop callback --------------------------------------------------- */

typedef struct {
    DbgWindow *w;
    apple2debug_stop_reason reason;
    uint16_t pc;
} StopEvent;

static gboolean on_stop_main(gpointer data)
{
    StopEvent *e = data;
    static const char *const k_reason[] = {
        "Paused", "Breakpoint", "Stepped", "Ran to address", "CPU JAMMED"
    };
    char msg[128];

    if (g_dbg == e->w) { /* the window may have gone away meanwhile */
        g_snprintf(msg, sizeof(msg), "%s at $%04X",
                   e->reason <= APPLE2DBG_STOP_JAMMED ? k_reason[e->reason]
                                                      : "Stopped",
                   e->pc);
        gtk_label_set_text(e->w->status, msg);
        e->w->follow_pc = 1;
        refresh_all(e->w);
    }
    g_free(e);
    return G_SOURCE_REMOVE;
}

/* EMULATOR THREAD. Does nothing but hand the event to the main loop. */
static void on_stop(apple2debug_stop_reason reason, uint16_t pc, void *user)
{
    StopEvent *e = g_new0(StopEvent, 1);
    e->w = user;
    e->reason = reason;
    e->pc = pc;
    g_idle_add(on_stop_main, e);
}

/* ---- actions -------------------------------------------------------------- */

static void act_run(GtkButton *b, gpointer user_data)
{
    DbgWindow *w = user_data;
    (void)b;
    if (apple2debug_is_paused(w->dbg)) {
        apple2debug_resume(w->dbg);
        gtk_label_set_text(w->status, "Running");
        refresh_all(w);
    } else {
        apple2debug_pause(w->dbg);
    }
}

static void act_step_into(GtkButton *b, gpointer u)
{ (void)b; apple2debug_step_into(((DbgWindow *)u)->dbg); }
static void act_step_over(GtkButton *b, gpointer u)
{ (void)b; apple2debug_step_over(((DbgWindow *)u)->dbg); }
static void act_step_out(GtkButton *b, gpointer u)
{ (void)b; apple2debug_step_out(((DbgWindow *)u)->dbg); }

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row,
                             gpointer user_data)
{
    DbgWindow *w = user_data;
    const guint addr =
        GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "addr"));
    (void)box;
    apple2debug_bp_toggle(w->dbg, (uint16_t)addr);
    render_disasm(w);
}

static void on_mem_addr(GtkEntry *entry, gpointer user_data)
{
    DbgWindow *w = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    unsigned addr = 0;
    uint16_t sym = 0;

    while (*text == '$' || *text == ' ') text++;
    if (apple2debug_symbol_find(w->dbg, text, &sym))
        addr = sym;
    else if (sscanf(text, "%x", &addr) != 1)
        return;

    w->mem_at = (uint16_t)addr;
    render_mem(w);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType state, gpointer user_data)
{
    DbgWindow *w = user_data;
    (void)c;
    (void)code;

    switch (keyval) {
    case GDK_KEY_F5:
        act_run(NULL, w);
        return TRUE;
    case GDK_KEY_F7:
        apple2debug_step_into(w->dbg);
        return TRUE;
    case GDK_KEY_F8:
        if (state & GDK_SHIFT_MASK)
            apple2debug_step_out(w->dbg);
        else
            apple2debug_step_over(w->dbg);
        return TRUE;
    default:
        return FALSE;
    }
}

static void on_fuji_clear(GtkButton *b, gpointer user_data)
{
    DbgWindow *w = user_data;
    (void)b;
    apple2debug_fujinet_trace_clear(w->dbg);
    refresh_fujinet(w, TRUE);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num,
                           gpointer user_data)
{
    (void)nb; (void)page;
    if (num == TAB_FUJINET)
        refresh_fujinet(user_data, FALSE);
}

/* The FujiNet tab is live: twice a second while it shows. */
static gboolean on_tick(gpointer user_data)
{
    DbgWindow *w = user_data;
    if (gtk_widget_get_visible(GTK_WIDGET(w->win)) &&
        gtk_notebook_get_current_page(w->notebook) == TAB_FUJINET)
        refresh_fujinet(w, FALSE);
    return G_SOURCE_CONTINUE;
}

static void on_destroy(GtkWidget *widget, gpointer user_data)
{
    DbgWindow *w = user_data;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    /* Leave the machine running, but stop calling into a freed window. */
    apple2debug_set_stop_callback(w->dbg, NULL, NULL);
    apple2debug_resume(w->dbg);
    if (g_dbg == w) g_dbg = NULL;
    g_free(w->view_fb);
    g_free(w);
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *tool_button(const char *label, const char *tooltip,
                              GCallback cb, DbgWindow *w)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(b, tooltip);
    g_signal_connect(b, "clicked", cb, w);
    return b;
}

static GtkWidget *mono_view(GtkTextView **out)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *title_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

/* The FujiNet tab: the packets on the left (a note and Clear over them),
 * the card on the right. */
static GtkWidget *build_fujinet(DbgWindow *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");

    w->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->fuji_note), "dim-label");
    gtk_label_set_wrap(w->fuji_note, TRUE);
    gtk_label_set_xalign(w->fuji_note, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(w->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), title_label("SmartPort packets (SLIP-decoded)"));
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&w->fuji_text));
    make_log(w->fuji_text);

    gtk_box_append(GTK_BOX(right), title_label("SmartPort card"));
    gtk_box_append(GTK_BOX(right), mono_view(&w->card_text));

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 620, -1);
    gtk_widget_set_size_request(right, 320, -1);
    gtk_widget_set_margin_top(paned, 8);
    gtk_widget_set_margin_bottom(paned, 8);
    gtk_widget_set_margin_start(paned, 8);
    gtk_widget_set_margin_end(paned, 8);
    w->fuji_count = -1;
    return paned;
}

/* APPLE2_DEBUGGER_TAB: a page number, or the start of a tab's name. */
static int tab_from_env(void)
{
    const char *tab = getenv("APPLE2_DEBUGGER_TAB");
    char *end;
    long n;
    int i;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < TAB_COUNT) ? (int)n : -1;
    for (i = 0; i < TAB_COUNT; i++)
        if (g_ascii_strncasecmp(tab, k_tab_names[i], strlen(tab)) == 0) return i;
    return -1;
}

void apple2_debugger_show(GtkWindow *parent, apple2session *session)
{
    DbgWindow *w;
    GtkWidget *header, *tbview, *split, *left, *right, *scroll, *membox;
    GtkEventController *keys;
    int tab;

    if (g_dbg) {
        gtk_window_present(g_dbg->win);
        return;
    }

    w = g_new0(DbgWindow, 1);
    w->session = session;
    w->dbg = apple2session_debugger(session);
    if (!w->dbg) {
        g_free(w);
        return;
    }
    g_dbg = w;

    w->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_default_size(w->win, 1180, 780);
    gtk_window_set_transient_for(w->win, parent);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    header = adw_header_bar_new();
    w->btn_run = GTK_BUTTON(tool_button("Pause", "Pause / Continue (F5)",
                                        G_CALLBACK(act_run), w));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(w->btn_run));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header),
        tool_button("Into", "Step into (F7)", G_CALLBACK(act_step_into), w));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header),
        tool_button("Over", "Step over (F8)", G_CALLBACK(act_step_over), w));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header),
        tool_button("Out", "Step out (Shift+F8)", G_CALLBACK(act_step_out), w));

    w->status = GTK_LABEL(gtk_label_new("Running"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(w->status));

    /* Disassembly on the left, registers and memory on the right. */
    w->disasm = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->disasm, GTK_SELECTION_SINGLE);
    g_signal_connect(w->disasm, "row-activated", G_CALLBACK(on_row_activated),
                     w);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(w->disasm));
    gtk_widget_set_vexpand(scroll, TRUE);

    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    {
        GtkWidget *hint = gtk_label_new("Click a line to toggle a breakpoint");
        gtk_widget_add_css_class(hint, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
        gtk_widget_set_margin_start(hint, 6);
        gtk_box_append(GTK_BOX(left), hint);
    }
    gtk_box_append(GTK_BOX(left), scroll);

    w->regs = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(w->regs, 0.0f);
    gtk_widget_set_margin_start(GTK_WIDGET(w->regs), 12);
    gtk_widget_set_margin_top(GTK_WIDGET(w->regs), 6);

    w->mem = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(w->mem, 0.0f);
    gtk_widget_set_margin_start(GTK_WIDGET(w->mem), 12);

    w->mem_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->mem_entry, "Address or symbol");
    g_signal_connect(w->mem_entry, "activate", G_CALLBACK(on_mem_addr), w);

    membox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(membox), GTK_WIDGET(w->mem_entry));
    gtk_box_append(GTK_BOX(membox), GTK_WIDGET(w->mem));

    /* Video page viewer. The list of pages comes from the engine, so a view
     * added there shows up here with no edit. */
    {
        GtkStringList *views = gtk_string_list_new(NULL);
        GtkWidget *videobox;
        int i;

        for (i = 0; apple2debug_view_name(i); i++)
            gtk_string_list_append(views, apple2debug_view_name(i));

        w->view_pick = GTK_DROP_DOWN(
            gtk_drop_down_new(G_LIST_MODEL(views), NULL));
        g_signal_connect(w->view_pick, "notify::selected",
                         G_CALLBACK(on_view_changed), w);

        w->view_fb = g_new0(uint32_t, (gsize)APPLE2VIEW_WIDTH *
                                          APPLE2VIEW_HEIGHT);
        w->view_pic = GTK_PICTURE(gtk_picture_new());
        /* Nearest-neighbour and a fixed size: this is a pixel inspector, so
         * smoothing it would hide the thing being inspected. */
        gtk_picture_set_content_fit(w->view_pic, GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_set_size_request(GTK_WIDGET(w->view_pic),
                                    APPLE2VIEW_WIDTH, APPLE2VIEW_HEIGHT);

        videobox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_append(GTK_BOX(videobox), GTK_WIDGET(w->view_pick));
        gtk_box_append(GTK_BOX(videobox), GTK_WIDGET(w->view_pic));

        right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_end(right, 12);
        gtk_box_append(GTK_BOX(right), GTK_WIDGET(w->regs));
        gtk_box_append(GTK_BOX(right), membox);
        gtk_box_append(GTK_BOX(right), videobox);
    }

    {
        /* Scrolled: the right column carries registers, memory and a 280x192
         * video page, which together want more height than a small window
         * has. Without this the video pane is what gets squeezed to nothing. */
        GtkWidget *rscroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rscroll), right);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rscroll),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_paned_set_start_child(GTK_PANED(split), left);
        gtk_paned_set_end_child(GTK_PANED(split), rscroll);
        gtk_paned_set_position(GTK_PANED(split), 470);
        gtk_paned_set_resize_start_child(GTK_PANED(split), TRUE);
        gtk_paned_set_resize_end_child(GTK_PANED(split), FALSE);
    }

    /* CPU is the view above as it stands; FujiNet follows it. */
    w->notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_append_page(w->notebook, split,
                             gtk_label_new(k_tab_labels[TAB_CPU]));
    gtk_notebook_append_page(w->notebook, build_fujinet(w),
                             gtk_label_new(k_tab_labels[TAB_FUJINET]));
    tab = tab_from_env();
    if (tab >= 0) gtk_notebook_set_current_page(w->notebook, tab);
    g_signal_connect(w->notebook, "switch-page", G_CALLBACK(on_switch_page), w);

    tbview = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tbview), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tbview),
                                 GTK_WIDGET(w->notebook));
    adw_window_set_content(ADW_WINDOW(w->win), tbview);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    apple2debug_set_stop_callback(w->dbg, on_stop, w);

    /* Open paused: a debugger that opens on a running machine shows a
     * disassembly that is already stale by the time it is drawn. */
    w->mem_at = 0x0400; /* the text page: something recognisable to land on */
    w->follow_pc = 1;
    /* Dev affordance, like APPLE2_OPEN_DEBUGGER: land on a given video page
     * (an index into apple2debug_view_name) instead of text page 1. */
    {
        const char *v = getenv("APPLE2_DEBUGGER_VIEW");
        if (v && *v) {
            int idx = atoi(v);
            if (idx >= 0 && idx < APPLE2VIEW_COUNT)
                gtk_drop_down_set_selected(w->view_pick, (guint)idx);
        }
    }
    apple2debug_pause(w->dbg);
    refresh_all(w);
    w->timer = g_timeout_add(500, on_tick, w);

    gtk_window_present(w->win);
}

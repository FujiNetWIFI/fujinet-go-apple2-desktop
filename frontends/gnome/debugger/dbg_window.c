/*
 * The GNOME frontend's debugger window: run control, 6502 registers,
 * disassembly with click-to-toggle breakpoints, and a memory view.
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

    uint16_t disasm_at;   /* address the listing currently starts at */
    uint16_t mem_at;
    int follow_pc;        /* re-centre the listing on PC when it stops */
} DbgWindow;

static DbgWindow *g_dbg; /* one window per process */

/* ---- rendering ----------------------------------------------------------- */

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

static void refresh_all(DbgWindow *w)
{
    const int paused = apple2debug_is_paused(w->dbg);
    gtk_button_set_label(w->btn_run, paused ? "Continue" : "Pause");
    render_regs(w);
    render_disasm(w);
    render_mem(w);
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

static void on_destroy(GtkWidget *widget, gpointer user_data)
{
    DbgWindow *w = user_data;
    (void)widget;
    /* Leave the machine running, but stop calling into a freed window. */
    apple2debug_set_stop_callback(w->dbg, NULL, NULL);
    apple2debug_resume(w->dbg);
    if (g_dbg == w) g_dbg = NULL;
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

void apple2_debugger_show(GtkWindow *parent, apple2session *session)
{
    DbgWindow *w;
    GtkWidget *header, *tbview, *split, *left, *right, *scroll, *membox;
    GtkEventController *keys;

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
    gtk_window_set_default_size(w->win, 900, 680);
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

    right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_end(right, 12);
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(w->regs));
    gtk_box_append(GTK_BOX(right), membox);

    split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(split), left);
    gtk_paned_set_end_child(GTK_PANED(split), right);
    gtk_paned_set_position(GTK_PANED(split), 480);

    tbview = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tbview), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tbview), split);
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
    apple2debug_pause(w->dbg);
    refresh_all(w);

    gtk_window_present(w->win);
}

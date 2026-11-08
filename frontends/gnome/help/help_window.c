/*
 * The Help window: the topics on the left, with a search that lists every
 * section having all the words typed; the topic on the right, a widget per
 * block (labels with Pango markup, grids for tables), its matches
 * highlighted when it came from a search.
 *
 * One window, hidden (not destroyed) when closed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "help_window.h"

#include "../window.h"
#include "apple2help.h"

#include <string.h>

#define ME APPLE2HELP_GNOME
#define MAX_HITS 64
#define THIN "\xe2\x80\x89"

typedef struct {
    GtkWindow *win;
    AdwNavigationSplitView *split;
    AdwNavigationPage *content;
    GtkSearchEntry *search;
    GtkListBox *list;
    GtkScrolledWindow *scroll;
    GtkWidget *clamp;
    GtkWidget **blocks;         /* each block's widget on the page shown */
    int topic, block;
    char *query;                /* what the page highlights */
    gboolean filling;
    guint scroll_tick;
    double scroll_to;           /* where the section was last frame */
    int scroll_frames;
} Help;

static Help *g_help;

static void show_topic(Help *h, int topic, int block, const char *query);

/* ---- markup ------------------------------------------------------------------ */

static void escaped(GString *m, const char *text, gssize len)
{
    char *e = g_markup_escape_text(text, len);
    g_string_append(m, e);
    g_free(e);
}

/* text, with query's words highlighted */
static void marked(GString *m, const char *text, const char *query)
{
    int from = 0, at, len;
    while (query && apple2help_match(text, query, from, &at, &len)) {
        escaped(m, text + from, at - from);
        g_string_append(m, "<span background=\"#f8e45c\" foreground=\"#000000\">");
        escaped(m, text + at, len);
        g_string_append(m, "</span>");
        from = at + len;
    }
    escaped(m, text + from, -1);
}

static void span_markup(GString *m, const apple2help_span *sp, const char *query)
{
    const char *text = apple2help_span_text(sp, ME);
    switch (sp->kind) {
    case APPLE2HELP_BOLD:
        g_string_append(m, "<b>");
        marked(m, text, query);
        g_string_append(m, "</b>");
        break;
    case APPLE2HELP_ITALIC:
        g_string_append(m, "<i>");
        marked(m, text, query);
        g_string_append(m, "</i>");
        break;
    case APPLE2HELP_CODE:
    case APPLE2HELP_KBD:
        g_string_append_printf(m, "<span font_family=\"monospace\" background=\"#808080\" "
                               "bgalpha=\"22%%\"%s>" THIN,
                               sp->kind == APPLE2HELP_KBD ? " weight=\"bold\"" : "");
        marked(m, text, query);
        g_string_append(m, THIN "</span>");
        break;
    case APPLE2HELP_LINK:
        g_string_append(m, "<a href=\"");
        escaped(m, sp->target, -1);
        g_string_append(m, "\">");
        escaped(m, text, -1);
        g_string_append(m, "</a>");
        break;
    default:
        marked(m, text, query);
        break;
    }
}

/* spans [from, to) */
static char *markup(const apple2help_span *spans, int from, int to, const char *query)
{
    GString *m = g_string_new(NULL);
    for (int i = from; i < to; i++)
        span_markup(m, &spans[i], query);
    return g_string_free(m, FALSE);
}

/* ---- the page ------------------------------------------------------------------ */

static gboolean on_link(GtkLabel *label, const char *uri, gpointer ud)
{
    (void)ud;
    if (uri[0] == '#') {
        apple2_help_show(NULL, uri);
    } else {
        GtkUriLauncher *l = gtk_uri_launcher_new(uri);
        gtk_uri_launcher_launch(l, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(label))),
                                NULL, NULL, NULL);
        g_object_unref(l);
    }
    return TRUE;
}

static GtkWidget *text_label(char *markup_text, gboolean wrap)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup_text);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(l), wrap);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    g_signal_connect(l, "activate-link", G_CALLBACK(on_link), NULL);
    g_free(markup_text);
    return l;
}

/* "•" or "2." beside the item's text */
static GtkWidget *item(const char *mark, GtkWidget *text, int indent)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *m = gtk_label_new(mark);
    gtk_label_set_width_chars(GTK_LABEL(m), 2);
    gtk_label_set_xalign(GTK_LABEL(m), 1.0f);
    gtk_widget_set_valign(m, GTK_ALIGN_START);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_margin_start(row, indent);
    gtk_box_append(GTK_BOX(row), m);
    gtk_box_append(GTK_BOX(row), text);
    return row;
}

static void table_row(GtkGrid *grid, int y, const apple2help_block *b, const char *query)
{
    int from = 0, col = 0;
    for (int i = 0; i <= b->nspans; i++) {
        if (i < b->nspans && b->spans[i].kind != APPLE2HELP_CELL)
            continue;
        char *mk = markup(b->spans, from, i, b->kind == APPLE2HELP_TABLE_HEAD ? NULL : query);
        if (b->kind == APPLE2HELP_TABLE_HEAD) {
            char *bold = g_strdup_printf("<b>%s</b>", mk);
            g_free(mk);
            mk = bold;
        }
        GtkWidget *cell = text_label(mk, TRUE);
        gtk_widget_set_valign(cell, GTK_ALIGN_START);
        if (i == b->nspans)
            gtk_widget_set_hexpand(cell, TRUE);
        else
            gtk_label_set_max_width_chars(GTK_LABEL(cell), 32);
        gtk_grid_attach(grid, cell, col++, y, 1, 1);
        from = i + 1;
    }
}

static GtkWidget *block_widget(const apple2help_block *b, const char *query)
{
    GtkWidget *w;
    switch (b->kind) {
    case APPLE2HELP_H2:
    case APPLE2HELP_H3:
        w = text_label(g_markup_escape_text(apple2help_heading(b), -1), TRUE);
        gtk_widget_add_css_class(w, b->kind == APPLE2HELP_H2 ? "title-3" : "heading");
        gtk_widget_set_margin_top(w, b->kind == APPLE2HELP_H2 ? 12 : 6);
        return w;
    case APPLE2HELP_BULLET:
    case APPLE2HELP_BULLET2:
        return item(b->kind == APPLE2HELP_BULLET ? "\xe2\x80\xa2" : "\xe2\x97\xa6",
                    text_label(markup(b->spans, 0, b->nspans, query), TRUE),
                    b->kind == APPLE2HELP_BULLET2 ? 24 : 0);
    case APPLE2HELP_NUMBERED: {
        char n[16];
        g_snprintf(n, sizeof n, "%d.", b->number);
        return item(n, text_label(markup(b->spans, 0, b->nspans, query), TRUE), 0);
    }
    case APPLE2HELP_NOTE:
    case APPLE2HELP_PRE: {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        if (b->kind == APPLE2HELP_PRE) {
            w = text_label(g_markup_escape_text(b->spans[0].text, -1), FALSE);
            gtk_widget_add_css_class(w, "monospace");
            gtk_label_set_selectable(GTK_LABEL(w), TRUE);
        } else {
            w = text_label(markup(b->spans, 0, b->nspans, query), TRUE);
        }
        gtk_widget_set_margin_top(w, 10);
        gtk_widget_set_margin_bottom(w, 10);
        gtk_widget_set_margin_start(w, 12);
        gtk_widget_set_margin_end(w, 12);
        gtk_box_append(GTK_BOX(box), w);
        gtk_widget_add_css_class(box, "card");
        return box;
    }
    default:
        return text_label(markup(b->spans, 0, b->nspans, query), TRUE);
    }
}

/* The section's heading at the top, once the labels above it have wrapped
 * and the page is long enough to get there: until where it is stops moving. */
static gboolean scroll_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer ud)
{
    Help *h = ud;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(h->scroll);
    const apple2help_topic *tp = apple2help_topic_at(h->topic);
    GtkWidget *target = (tp && h->block >= 0 && h->block < tp->nblocks) ? h->blocks[h->block] : NULL;
    graphene_point_t p, origin = GRAPHENE_POINT_INIT(0, 0);
    double y = 0;
    (void)widget; (void)clock;
    if (target && gtk_widget_get_height(target) > 0
        && gtk_widget_compute_point(target, h->clamp, &origin, &p))
        y = p.y - 12;
    else if (target && h->scroll_frames < 60) {
        h->scroll_frames++;
        return G_SOURCE_CONTINUE;
    }
    gtk_adjustment_set_value(adj, y);
    if (++h->scroll_frames < 60 && (y != h->scroll_to || h->scroll_frames < 3)) {
        h->scroll_to = y;
        return G_SOURCE_CONTINUE;
    }
    h->scroll_tick = 0;
    return G_SOURCE_REMOVE;
}

static void build_page(Help *h, const char *query)
{
    const apple2help_topic *tp = apple2help_topic_at(h->topic);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *title;
    GtkGrid *grid = NULL;
    int y = 0;

    g_free(h->blocks);
    h->blocks = g_new0(GtkWidget *, tp->nblocks ? tp->nblocks : 1);
    title = text_label(g_markup_escape_text(tp->title, -1), TRUE);
    gtk_widget_add_css_class(title, "title-1");
    gtk_box_append(GTK_BOX(box), title);
    for (int i = 0; i < tp->nblocks; i++) {
        const apple2help_block *b = &tp->blocks[i];
        if (!(b->platforms & ME))
            continue;
        if (b->kind == APPLE2HELP_TABLE_HEAD || b->kind == APPLE2HELP_TABLE_ROW) {
            if (b->kind == APPLE2HELP_TABLE_HEAD || !grid) {
                grid = GTK_GRID(gtk_grid_new());
                gtk_grid_set_column_spacing(grid, 24);
                gtk_grid_set_row_spacing(grid, 6);
                gtk_box_append(GTK_BOX(box), GTK_WIDGET(grid));
                y = 0;
            }
            table_row(grid, y++, b, query);
            h->blocks[i] = GTK_WIDGET(grid);
            continue;
        }
        grid = NULL;
        h->blocks[i] = block_widget(b, query);
        gtk_box_append(GTK_BOX(box), h->blocks[i]);
    }
    adw_clamp_set_child(ADW_CLAMP(h->clamp), box);
    adw_navigation_page_set_title(h->content, tp->title);
}

static void show_topic(Help *h, int topic, int block, const char *query)
{
    if (!apple2help_topic_at(topic))
        return;
    if (query && !*query)
        query = NULL;
    if (topic != h->topic || g_strcmp0(query, h->query) != 0) {
        h->topic = topic;
        g_free(h->query);
        h->query = g_strdup(query);
        build_page(h, query);
    }
    h->block = block;
    h->scroll_to = -1;
    h->scroll_frames = 0;
    if (!h->scroll_tick)
        h->scroll_tick = gtk_widget_add_tick_callback(GTK_WIDGET(h->scroll), scroll_tick, h, NULL);
    adw_navigation_split_view_set_show_content(h->split, TRUE);
}

/* ---- the list ---------------------------------------------------------------------- */

static GtkWidget *list_row(const char *title, const char *sub, int topic, int block)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *l = gtk_label_new(title);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), l);
    if (sub) {
        GtkWidget *s = gtk_label_new(sub);
        gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(s, "caption");
        gtk_widget_add_css_class(s, "dim-label");
        gtk_box_append(GTK_BOX(box), s);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data(G_OBJECT(row), "apple2-topic", GINT_TO_POINTER(topic));
    g_object_set_data(G_OBJECT(row), "apple2-block", GINT_TO_POINTER(block));
    return row;
}

static void fill_list(Help *h)
{
    const char *q = gtk_editable_get_text(GTK_EDITABLE(h->search));
    h->filling = TRUE;
    gtk_list_box_remove_all(h->list);
    if (!*q) {
        for (int t = 0; t < apple2help_topic_count(); t++) {
            GtkWidget *row = list_row(apple2help_topic_at(t)->title, NULL, t, -1);
            gtk_list_box_append(h->list, row);
            if (t == h->topic)
                gtk_list_box_select_row(h->list, GTK_LIST_BOX_ROW(row));
        }
    } else {
        apple2help_hit hits[MAX_HITS];
        int n = apple2help_search(ME, q, hits, MAX_HITS);
        for (int i = 0; i < n; i++) {
            const apple2help_topic *tp = apple2help_topic_at(hits[i].topic);
            gtk_list_box_append(h->list, hits[i].block < 0
                ? list_row(tp->title, NULL, hits[i].topic, -1)
                : list_row(apple2help_heading(&tp->blocks[hits[i].block]), tp->title,
                           hits[i].topic, hits[i].block));
        }
    }
    h->filling = FALSE;
}

static void on_row_selected(GtkListBox *list, GtkListBoxRow *row, gpointer ud)
{
    Help *h = ud;
    (void)list;
    if (h->filling || !row)
        return;
    show_topic(h, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "apple2-topic")),
               GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "apple2-block")),
               gtk_editable_get_text(GTK_EDITABLE(h->search)));
}

static void on_search_changed(GtkSearchEntry *e, gpointer ud)
{
    (void)e;
    fill_list(ud);
}

/* Enter: the best match */
static void on_search_activate(GtkSearchEntry *e, gpointer ud)
{
    Help *h = ud;
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(h->list, 0);
    (void)e;
    if (row)
        gtk_list_box_select_row(h->list, row);
}

static void on_stop_search(GtkSearchEntry *e, gpointer ud)
{
    (void)ud;
    gtk_editable_set_text(GTK_EDITABLE(e), "");
}

/* Down from the search goes into the list */
static gboolean on_search_key(GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType st, gpointer ud)
{
    Help *h = ud;
    GtkListBoxRow *row;
    (void)c; (void)code; (void)st;
    if (keyval != GDK_KEY_Down || !(row = gtk_list_box_get_row_at_index(h->list, 0)))
        return FALSE;
    gtk_widget_grab_focus(GTK_WIDGET(row));
    return TRUE;
}

static gboolean focus_search(GtkWidget *w, GVariant *args, gpointer ud)
{
    Help *h = ud;
    (void)w; (void)args;
    gtk_widget_grab_focus(GTK_WIDGET(h->search));
    return TRUE;
}

/* ---- the window ------------------------------------------------------------------- */

static void build(GtkWindow *parent)
{
    Help *h = g_new0(Help, 1);
    GtkWidget *side, *side_tv, *list_scroll, *content_tv, *placeholder;
    GtkEventController *keys;
    GtkEventController *shortcuts;
    AdwBreakpoint *bp;
    GValue collapsed = G_VALUE_INIT;

    g_help = h;
    h->topic = -1;
    h->block = -1;
    h->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(h->win, "FujiNet Go Apple II Help");
    gtk_window_set_default_size(h->win, 980, 720);
    gtk_widget_set_size_request(GTK_WIDGET(h->win), 360, 300);
    gtk_window_set_hide_on_close(h->win, TRUE);
    if (parent)
        gtk_window_set_application(h->win, gtk_window_get_application(parent));

    h->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(h->search, "Search Help");
    gtk_search_entry_set_key_capture_widget(h->search, GTK_WIDGET(h->win));
    gtk_widget_set_margin_start(GTK_WIDGET(h->search), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(h->search), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(h->search), 6);
    g_signal_connect(h->search, "search-changed", G_CALLBACK(on_search_changed), h);
    g_signal_connect(h->search, "activate", G_CALLBACK(on_search_activate), h);
    g_signal_connect(h->search, "stop-search", G_CALLBACK(on_stop_search), h);
    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_search_key), h);
    gtk_widget_add_controller(GTK_WIDGET(h->search), keys);

    h->list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(h->list), "navigation-sidebar");
    placeholder = gtk_label_new("No matches");
    gtk_widget_add_css_class(placeholder, "dim-label");
    gtk_widget_set_margin_top(placeholder, 24);
    gtk_list_box_set_placeholder(h->list, placeholder);
    g_signal_connect(h->list, "row-selected", G_CALLBACK(on_row_selected), h);
    list_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll), GTK_WIDGET(h->list));
    gtk_widget_set_vexpand(list_scroll, TRUE);

    side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(side), GTK_WIDGET(h->search));
    gtk_box_append(GTK_BOX(side), list_scroll);
    side_tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(side_tv), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(side_tv), side);

    h->clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(h->clamp), 760);
    gtk_widget_set_margin_top(h->clamp, 12);
    gtk_widget_set_margin_bottom(h->clamp, 24);
    gtk_widget_set_margin_start(h->clamp, 24);
    gtk_widget_set_margin_end(h->clamp, 24);
    h->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(h->scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(h->scroll, h->clamp);
    content_tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content_tv), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content_tv), GTK_WIDGET(h->scroll));

    h->split = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
    adw_navigation_split_view_set_min_sidebar_width(h->split, 260);
    adw_navigation_split_view_set_sidebar(h->split, adw_navigation_page_new(side_tv, "Help"));
    h->content = adw_navigation_page_new(content_tv, "Help");
    adw_navigation_split_view_set_content(h->split, h->content);
    adw_window_set_content(ADW_WINDOW(h->win), GTK_WIDGET(h->split));

    bp = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 560sp"));
    g_value_init(&collapsed, G_TYPE_BOOLEAN);
    g_value_set_boolean(&collapsed, TRUE);
    adw_breakpoint_add_setter(bp, G_OBJECT(h->split), "collapsed", &collapsed);
    g_value_unset(&collapsed);
    adw_window_add_breakpoint(ADW_WINDOW(h->win), bp);

    shortcuts = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts),
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>f"),
                         gtk_callback_action_new(focus_search, h, NULL)));
    gtk_widget_add_controller(GTK_WIDGET(h->win), shortcuts);

    fill_list(h);
}

void apple2_help_show(GtkWindow *parent, const char *id)
{
    int t = 0, b = -1;
    if (!g_help)
        build(parent);
    if (id && id[0] == '?') {
        /* a search, and its best match */
        gtk_editable_set_text(GTK_EDITABLE(g_help->search), id + 1);
        fill_list(g_help);
        on_search_activate(g_help->search, g_help);
    } else if (id || g_help->topic < 0) {
        if (!id || apple2help_find(id, &t, &b) != 0) {
            t = 0;
            b = -1;
        }
        if (*gtk_editable_get_text(GTK_EDITABLE(g_help->search)))
            gtk_editable_set_text(GTK_EDITABLE(g_help->search), "");
        show_topic(g_help, t, b, NULL);
        fill_list(g_help);      /* now, not after the search's delay */
    }
    gtk_window_present(g_help->win);
}

void apple2_help_set_context(GtkWindow *window, const char *context)
{
    g_object_set_data_full(G_OBJECT(window), "apple2-help-context", g_strdup(context), g_free);
}

const char *apple2_help_get_context(GtkWindow *window)
{
    const char *c;
    if (APPLE2_IS_WINDOW(window))
        return apple2_window_help_context(APPLE2_WINDOW(window));
    c = window ? g_object_get_data(G_OBJECT(window), "apple2-help-context") : NULL;
    return c ? c : "main";
}

void apple2_help_for(GtkWindow *window)
{
    if (g_help && window == g_help->win) {
        gtk_widget_grab_focus(GTK_WIDGET(g_help->search));
        return;
    }
    apple2_help_show(window, apple2_help_get_context(window));
}

static void on_help_clicked(GtkButton *b, gpointer ud)
{
    GtkWindow *win = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b)));
    (void)ud;
    const char *ctx = g_object_get_data(G_OBJECT(b), "apple2-help-context");
    apple2_help_show(win, ctx ? ctx : apple2_help_get_context(win));
}

GtkWidget *apple2_help_button(const char *context)
{
    GtkWidget *b = gtk_button_new_from_icon_name("help-browser-symbolic");
    gtk_widget_set_tooltip_text(b, "Help");
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(b), "apple2-help-context", g_strdup(context), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(on_help_clicked), NULL);
    return b;
}

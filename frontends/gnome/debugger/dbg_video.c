/*
 * The debugger's Video tab (GTK4/cairo).
 *
 * Everything that decides what to show lives in the core (apple2vidtab.h): this file is
 * widgets. The picture is drawn from the controller's cached buffer (0xFFRRGGBB, drawn as
 * CAIRO_FORMAT_RGB24 with a nearest filter), so a mouse move redraws only the marks. Only the
 * section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <string.h>

#include "apple2debug.h"
#include "apple2vidtab.h"

#define MAX_OVERLAYS 16
#define MAX_PAL 24

typedef struct {
    apple2vid_tab *tab;
    DbgVideoMemoryFn show_memory;
    gpointer user;
    GtkDropDown *view, *zoom, *mode, *pagesel;
    GtkCheckButton *follow, *mixed;
    GtkCheckButton *ov[MAX_OVERLAYS];
    int nov;
    GtkLabel *status, *note;
    GtkDrawingArea *pic;
    GtkWidget *pic_scroll;
    int vp_w, vp_h;
    GtkNotebook *sections;
    GtkLabel *pixel_text, *pixel_note;
    /* switches */
    GtkLabel *sw_cell[APPLE2VID_SW_COUNT][5];
    /* colours */
    GtkDrawingArea *pal_box[MAX_PAL];
    GtkLabel *pal_lbl[MAX_PAL];
    GtkLabel *timing_text;
    GtkProgressBar *beam;
    gboolean updating, primed, was_mapped;
} Vid;

static GtkWidget *mono_label(void)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "monospace");
    return l;
}

static GtkWidget *padded_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    return box;
}

static void set_rgb(cairo_t *cr, uint32_t rgb)
{
    cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
}

/* ---- the picture ---------------------------------------------------------- */

static void draw_message(cairo_t *cr, int w, int h, const char *msg)
{
    cairo_text_extents_t ext;
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.68);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_text_extents(cr, msg, &ext);
    cairo_move_to(cr, (w - ext.width) / 2 - ext.x_bearing, (h - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(cr, msg);
}

/* A pixel can be one device pixel wide: outline it from outside, dark under light. */
static void draw_mark(cairo_t *cr, const int r[4], double red, double green, double blue)
{
    cairo_set_line_width(cr, 3);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
    cairo_rectangle(cr, r[0] - 1.5, r[1] - 1.5, r[2] + 3, r[3] + 3);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, red, green, blue);
    cairo_rectangle(cr, r[0] - 1.5, r[1] - 1.5, r[2] + 3, r[3] + 3);
    cairo_stroke(cr);
}

static void draw_pic(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    int pw, ph, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    (void)area;
    cairo_set_source_rgb(cr, 0.07, 0.07, 0.09);
    cairo_paint(cr);
    px = apple2vid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = apple2vid_tab_why(v->tab);
        draw_message(cr, w, h, why && *why ? why : "Nothing to show");
        return;
    }
    {
        cairo_surface_t *s = cairo_image_surface_create_for_data((unsigned char *)px, CAIRO_FORMAT_RGB24,
                                                                 pw, ph, pw * 4);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_surface_destroy(s);
    }
    marks = apple2vid_tab_marks(v->tab, hover, pin);
    if (marks & 1) draw_mark(cr, hover, 1, 1, 1);
    if (marks & 2) draw_mark(cr, pin, 1.0, 0.85, 0.1);
}

static void fill_colours(Vid *v);

static void update_pixel_section(Vid *v)
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(v->tab);
    gtk_label_set_text(v->pixel_text, p ? p->text
        : "Move over the picture to inspect a pixel.\nClick to pin it (click again to unpin);\n"
          "double-click to break on writes to it.");
    if (gtk_notebook_get_current_page(v->sections) == APPLE2VID_S_COLOURS) fill_colours(v);
}

static void do_break_pixel(Vid *v)
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(v->tab);
    int id = apple2vid_tab_break_on_pixel(v->tab);
    char buf[160];
    if (id >= 0 && p) {
        if (p->main_addr >= 0 && p->aux_addr >= 0)
            g_snprintf(buf, sizeof buf, "Break on writes to $%04X (main) and $%04X (aux)",
                       p->main_addr, p->aux_addr);
        else if (p->main_addr >= 0)
            g_snprintf(buf, sizeof buf, "Break on writes to $%04X", p->main_addr);
        else
            g_snprintf(buf, sizeof buf, "Break on writes to $%04X (aux)", p->aux_addr);
    } else {
        g_snprintf(buf, sizeof buf, "Nothing to break on");
    }
    gtk_label_set_text(v->note, buf);
    gtk_label_set_text(v->pixel_note, buf);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (apple2vid_tab_hover(v->tab, (int)x, (int)y)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_leave(GtkEventControllerMotion *c, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (apple2vid_tab_hover(v->tab, -1, -1)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)g;
    if (n == 1) {
        apple2vid_tab_pin(v->tab, (int)x, (int)y);
    } else if (n == 2) {
        /* the first click of the pair toggled the pin; the second restores it */
        apple2vid_tab_hover(v->tab, (int)x, (int)y);
        apple2vid_tab_pin(v->tab, (int)x, (int)y);
        do_break_pixel(v);
    }
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

/* ---- sections ------------------------------------------------------------------ */

static void fill_switches(Vid *v)
{
    const apple2vid_switch *sw;
    int n = apple2vid_tab_switches(v->tab, &sw), i, k;
    for (i = 0; i < APPLE2VID_SW_COUNT; i++) {
        char buf[5][96];
        if (i < n) {
            g_snprintf(buf[0], sizeof buf[0], "%s", sw[i].name);
            g_snprintf(buf[1], sizeof buf[1], "%s", sw[i].on ? "on" : "off");
            if (sw[i].age >= APPLE2VID_SW_FADE) g_snprintf(buf[2], sizeof buf[2], "-");
            else g_snprintf(buf[2], sizeof buf[2], "%d", sw[i].age);
            g_snprintf(buf[3], sizeof buf[3], "$%04X / $%04X", sw[i].set_addr, sw[i].clr_addr);
            g_snprintf(buf[4], sizeof buf[4], "%s", sw[i].meaning);
        } else {
            for (k = 0; k < 5; k++) g_snprintf(buf[k], sizeof buf[k], "-");
        }
        for (k = 0; k < 5; k++) {
            gtk_label_set_text(v->sw_cell[i][k], buf[k]);
            if (i < n && sw[i].age >= APPLE2VID_SW_FADE)
                gtk_widget_add_css_class(GTK_WIDGET(v->sw_cell[i][k]), "dim-label");
            else
                gtk_widget_remove_css_class(GTK_WIDGET(v->sw_cell[i][k]), "dim-label");
        }
    }
}

static void on_switch_break(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "idx"));
    const apple2vid_switch *sw;
    int id = apple2vid_tab_break_on_switch(v->tab, i), n = apple2vid_tab_switches(v->tab, &sw);
    char buf[120];
    if (id >= 0 && i < n)
        g_snprintf(buf, sizeof buf, "Break on access to $%04X and $%04X (%s)", sw[i].set_addr,
                   sw[i].clr_addr, sw[i].name);
    else
        g_snprintf(buf, sizeof buf, "Could not add a breakpoint");
    gtk_label_set_text(v->note, buf);
}

static void draw_swatch(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    const apple2vid_pal_entry *e;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "idx"));
    (void)w; (void)h;
    if (apple2vid_tab_palette(v->tab, &e) <= i) return;
    set_rgb(cr, e[i].rgb);
    cairo_rectangle(cr, 0, 0, 22, 22);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, 21, 21);
    cairo_stroke(cr);
    if (e[i].inspected) {
        cairo_set_source_rgb(cr, 1, 0.85, 0.1);
        cairo_set_line_width(cr, 2);
        cairo_rectangle(cr, 1, 1, 20, 20);
        cairo_stroke(cr);
    }
}

static void fill_colours(Vid *v)
{
    const apple2vid_pal_entry *e;
    int n = apple2vid_tab_palette(v->tab, &e), i;
    for (i = 0; i < MAX_PAL; i++) {
        char buf[112];
        if (i < n) {
            g_snprintf(buf, sizeof buf, "%c %-18s #%06X  %ld pixels", e[i].inspected ? '>' : ' ', e[i].name,
                       e[i].rgb & 0xffffff, e[i].uses);
            gtk_label_set_text(v->pal_lbl[i], buf);
            gtk_widget_set_visible(GTK_WIDGET(v->pal_lbl[i]), TRUE);
            gtk_widget_set_visible(GTK_WIDGET(v->pal_box[i]), TRUE);
            gtk_widget_queue_draw(GTK_WIDGET(v->pal_box[i]));
        } else {
            gtk_widget_set_visible(GTK_WIDGET(v->pal_lbl[i]), FALSE);
            gtk_widget_set_visible(GTK_WIDGET(v->pal_box[i]), FALSE);
        }
    }
}

static void fill_timing(Vid *v)
{
    const char *const *lines;
    int n = apple2vid_tab_timing(v->tab, &lines), i;
    GString *s = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) g_string_append_printf(s, "%-14.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else g_string_append_printf(s, "%s\n", lines[i]);
    }
    gtk_label_set_text(v->timing_text, s->len ? s->str : "The machine is not running.");
    g_string_free(s, TRUE);
    gtk_progress_bar_set_fraction(v->beam, apple2vid_tab_beam_permille(v->tab) / 1000.0);
}

/* ---- refresh ------------------------------------------------------------------- */

static void fill_section(Vid *v, int sec)
{
    switch (sec) {
    case APPLE2VID_S_SWITCHES: fill_switches(v); break;
    case APPLE2VID_S_COLOURS: fill_colours(v); break;
    case APPLE2VID_S_TIMING: fill_timing(v); break;
    default: update_pixel_section(v); break;
    }
}

static void refresh_view(Vid *v, unsigned what)
{
    if (what & (APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_OFF)) {
        int pw = 0, ph = 0;
        if (apple2vid_tab_picture(v->tab, &pw, &ph)) {
            gtk_drawing_area_set_content_width(v->pic, pw);
            gtk_drawing_area_set_content_height(v->pic, ph);
        } else {
            gtk_drawing_area_set_content_width(v->pic, v->vp_w > 4 ? v->vp_w : 480);
            gtk_drawing_area_set_content_height(v->pic, v->vp_h > 4 ? v->vp_h : 300);
        }
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    }
    if (what & APPLE2VID_TAB_TABLES) {
        gtk_label_set_text(v->status, apple2vid_tab_status(v->tab));
        fill_section(v, gtk_notebook_get_current_page(v->sections));
    }
}

/* The mode, page and Mixed boxes follow the controller (Follow screen moves them). */
static void sync_controls(Vid *v)
{
    gboolean decoded = apple2vid_tab_view(v->tab) == APPLE2VID_V_DECODED;
    v->updating = TRUE;
    if ((int)gtk_drop_down_get_selected(v->view) != apple2vid_tab_view(v->tab))
        gtk_drop_down_set_selected(v->view, (guint)apple2vid_tab_view(v->tab));
    if (gtk_check_button_get_active(v->follow) != (apple2vid_tab_follow(v->tab) != 0))
        gtk_check_button_set_active(v->follow, apple2vid_tab_follow(v->tab) != 0);
    if ((int)gtk_drop_down_get_selected(v->mode) != apple2vid_tab_mode(v->tab))
        gtk_drop_down_set_selected(v->mode, (guint)apple2vid_tab_mode(v->tab));
    if ((int)gtk_drop_down_get_selected(v->pagesel) != apple2vid_tab_page(v->tab) - 1)
        gtk_drop_down_set_selected(v->pagesel, (guint)(apple2vid_tab_page(v->tab) - 1));
    if (gtk_check_button_get_active(v->mixed) != (apple2vid_tab_mixed(v->tab) != 0))
        gtk_check_button_set_active(v->mixed, apple2vid_tab_mixed(v->tab) != 0);
    v->updating = FALSE;
    gtk_widget_set_sensitive(GTK_WIDGET(v->follow), decoded);
    gtk_widget_set_sensitive(GTK_WIDGET(v->mode), decoded);
    gtk_widget_set_sensitive(GTK_WIDGET(v->pagesel), decoded);
    gtk_widget_set_sensitive(GTK_WIDGET(v->mixed), decoded);
}

static void sync_viewport(Vid *v)
{
    int w = gtk_widget_get_width(v->pic_scroll), h = gtk_widget_get_height(v->pic_scroll);
    if (w > 4 && h > 4 && (w != v->vp_w || h != v->vp_h)) {
        v->vp_w = w;
        v->vp_h = h;
        apple2vid_tab_set_viewport(v->tab, w - 2, h - 2);
    }
}

void dbg_video_refresh(GtkWidget *page, gboolean force)
{
    Vid *v = g_object_get_data(G_OBJECT(page), "vid");
    gboolean mapped;
    unsigned what;
    if (!v) return;
    mapped = gtk_widget_get_mapped(page);
    if (mapped && !v->was_mapped) force = TRUE;      /* just shown */
    v->was_mapped = mapped;
    if (!v->primed) force = TRUE;
    v->primed = TRUE;
    sync_viewport(v);
    what = apple2vid_tab_refresh(v->tab, force);
    if (force) what |= APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES;
    sync_controls(v);
    refresh_view(v, what);
}

/* The picture's room changed (window resized, split dragged): Fit follows it. */
static gboolean on_tick(GtkWidget *w, GdkFrameClock *fc, gpointer ud)
{
    Vid *v = ud;
    int pw = gtk_widget_get_width(w), ph = gtk_widget_get_height(w);
    (void)fc;
    if (pw > 4 && ph > 4 && (pw != v->vp_w || ph != v->vp_h)) {
        sync_viewport(v);
        if (v->primed && apple2vid_tab_zoom(v->tab) == 0)
            refresh_view(v, apple2vid_tab_refresh(v->tab, TRUE) | APPLE2VID_TAB_PICTURE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- controls ------------------------------------------------------------------- */

static void refresh_all(Vid *v)
{
    unsigned what = apple2vid_tab_refresh(v->tab, TRUE) | APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES;
    sync_controls(v);
    refresh_view(v, what);
}

static void on_view(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    (void)o; (void)p;
    if (v->updating) return;
    apple2vid_tab_set_view(v->tab, (int)gtk_drop_down_get_selected(v->view));
    refresh_all(v);
}

static void on_zoom(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    int n;
    const int *z = apple2vid_zooms(&n);
    guint i = gtk_drop_down_get_selected(v->zoom);
    (void)o; (void)p;
    if (v->updating || (int)i >= n) return;
    sync_viewport(v);
    apple2vid_tab_set_zoom(v->tab, z[i]);
    refresh_all(v);
}

static void on_follow(GtkCheckButton *b, gpointer ud)
{
    Vid *v = ud;
    (void)b;
    if (v->updating) return;
    apple2vid_tab_set_follow(v->tab, gtk_check_button_get_active(v->follow));
    refresh_all(v);
}

static void on_mode(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    (void)o; (void)p;
    if (v->updating) return;
    apple2vid_tab_set_mode(v->tab, (int)gtk_drop_down_get_selected(v->mode));
    refresh_all(v);
}

static void on_page(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    (void)o; (void)p;
    if (v->updating) return;
    apple2vid_tab_set_page(v->tab, (int)gtk_drop_down_get_selected(v->pagesel) + 1);
    refresh_all(v);
}

static void on_mixed(GtkCheckButton *b, gpointer ud)
{
    Vid *v = ud;
    (void)b;
    if (v->updating) return;
    apple2vid_tab_set_mixed(v->tab, gtk_check_button_get_active(v->mixed));
    refresh_all(v);
}

static void on_overlay(GtkCheckButton *b, gpointer ud)
{
    Vid *v = ud;
    unsigned m = 0;
    int n, i;
    const apple2vid_overlay_info *d = apple2vid_overlays(&n);
    (void)b;
    if (v->updating) return;
    for (i = 0; i < n && i < v->nov; i++)
        if (gtk_check_button_get_active(v->ov[i])) m |= d[i].bit;
    apple2vid_tab_set_overlays(v->tab, m);
    refresh_all(v);
}

static void on_unpin(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    (void)b;
    apple2vid_tab_unpin(v->tab);
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

static void on_break(GtkButton *b, gpointer ud)
{
    (void)b;
    do_break_pixel(ud);
}

static void on_show_memory(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    const apple2vid_pixel *p = apple2vid_tab_pixel(v->tab);
    (void)b;
    if (!p || !p->valid || (p->main_addr < 0 && p->aux_addr < 0)) {
        gtk_label_set_text(v->pixel_note, "Point at the picture first");
        return;
    }
    if (!v->show_memory) return;
    if (p->main_addr >= 0)
        v->show_memory(APPLE2_BANK_MAIN, p->main_addr, v->user);
    else
        v->show_memory(APPLE2_BANK_AUX, p->aux_addr, v->user);
}

static void on_section(GtkNotebook *nb, GtkWidget *page, guint n, gpointer ud)
{
    Vid *v = ud;
    (void)nb; (void)page;
    fill_section(v, (int)n);
}

static void on_destroy(GtkWidget *w, gpointer ud)
{
    Vid *v = ud;
    (void)w;
    apple2vid_tab_free(v->tab);
    g_free(v);
}

void dbg_video_select(GtkWidget *page, int mode, int page_no)
{
    Vid *v = g_object_get_data(G_OBJECT(page), "vid");
    if (!v) return;
    apple2vid_tab_set_view(v->tab, APPLE2VID_V_DECODED);
    apple2vid_tab_set_mode(v->tab, mode);
    apple2vid_tab_set_page(v->tab, page_no);
    refresh_all(v);
}

void dbg_video_pin(GtkWidget *page, int x, int y)
{
    Vid *v = g_object_get_data(G_OBJECT(page), "vid");
    int pw, ph;
    if (!v) return;
    apple2vid_tab_refresh(v->tab, TRUE);
    (void)apple2vid_tab_picture(v->tab, &pw, &ph);
    apple2vid_tab_pin(v->tab, x, y);
    refresh_all(v);
    update_pixel_section(v);
}

static GtkWidget *drop_of(const char *const *items, int n, GtkDropDown **out)
{
    GtkStringList *l = gtk_string_list_new(NULL);
    int i;
    for (i = 0; i < n; i++) gtk_string_list_append(l, items[i]);
    *out = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(l), NULL));
    return GTK_WIDGET(*out);
}

GtkWidget *dbg_video_new(apple2session *session, DbgVideoMemoryFn show_memory, gpointer user)
{
    Vid *v = g_new0(Vid, 1);
    GtkWidget *root, *left, *bar, *bar2, *right, *box, *btns, *b1, *b2, *b3, *scroll, *ovbtn, *pop, *ovbox,
              *grid;
    GtkEventController *motion;
    GtkGesture *click;
    const apple2vid_overlay_info *od;
    const int *zv;
    const char *views[APPLE2VID_V_COUNT], *modes[APPLE2VID_M_COUNT], *zlabels[24];
    static const char *const pages[] = { "Page 1", "Page 2" };
    char zbuf[24][8];
    int n, nz, i, k;

    v->tab = apple2vid_tab_new(session);
    v->show_memory = show_memory;
    v->user = user;

    root = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set_data(G_OBJECT(root), "vid", v);
    g_signal_connect(root, "destroy", G_CALLBACK(on_destroy), v);

    /* ---- left: toolbars, status, picture */
    left = padded_box();
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (i = 0; i < APPLE2VID_V_COUNT; i++) views[i] = apple2vid_view_name(i);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("View"));
    gtk_box_append(GTK_BOX(bar), drop_of(views, APPLE2VID_V_COUNT, &v->view));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->view),
        "Screen: the last finished frame, as the display showed it.\n"
        "Decoded: video memory through the soft switches, right now.");
    zv = apple2vid_zooms(&nz);
    if (nz > 23) nz = 23;
    for (i = 0; i < nz; i++) {
        if (zv[i] == 0) g_strlcpy(zbuf[i], "Fit", sizeof zbuf[i]);
        else g_snprintf(zbuf[i], sizeof zbuf[i], "%d\xc3\x97", zv[i]);
        zlabels[i] = zbuf[i];
    }
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Zoom"));
    gtk_box_append(GTK_BOX(bar), drop_of(zlabels, nz, &v->zoom));

    od = apple2vid_overlays(&n);
    if (n > MAX_OVERLAYS) n = MAX_OVERLAYS;
    v->nov = n;
    ovbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    for (i = 0; i < n; i++) {
        v->ov[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(od[i].label));
        g_signal_connect(v->ov[i], "toggled", G_CALLBACK(on_overlay), v);
        gtk_box_append(GTK_BOX(ovbox), GTK_WIDGET(v->ov[i]));
    }
    pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), ovbox);
    ovbtn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(ovbtn), "Overlays");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(ovbtn), pop);
    gtk_box_append(GTK_BOX(bar), ovbtn);
    gtk_box_append(GTK_BOX(left), bar);

    bar2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    v->follow = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow screen"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->follow),
        "Decoded tracks the mode, page and MIXED the machine is displaying.\n"
        "Choosing a mode, page or Mixed below turns this off.");
    for (i = 0; i < APPLE2VID_M_COUNT; i++) modes[i] = apple2vid_mode_name(i);
    gtk_box_append(GTK_BOX(bar2), GTK_WIDGET(v->follow));
    gtk_box_append(GTK_BOX(bar2), drop_of(modes, APPLE2VID_M_COUNT, &v->mode));
    gtk_box_append(GTK_BOX(bar2), drop_of(pages, 2, &v->pagesel));
    v->mixed = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Mixed"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->mixed), "Text in the bottom four lines (lines 160-191)");
    gtk_box_append(GTK_BOX(bar2), GTK_WIDGET(v->mixed));
    gtk_box_append(GTK_BOX(left), bar2);

    v->status = GTK_LABEL(mono_label());
    gtk_label_set_selectable(v->status, FALSE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->status));

    v->pic = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_draw_func(v->pic, draw_pic, v, NULL);
    gtk_drawing_area_set_content_width(v->pic, 560);
    gtk_drawing_area_set_content_height(v->pic, 192);
    gtk_widget_set_halign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), motion);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), GTK_EVENT_CONTROLLER(click));
    scroll = gtk_scrolled_window_new();
    v->pic_scroll = scroll;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->pic));
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_tick_callback(scroll, on_tick, v, NULL);
    gtk_box_append(GTK_BOX(left), scroll);

    v->note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->note, 0);
    gtk_widget_add_css_class(GTK_WIDGET(v->note), "apple2-accent-text");
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->note));

    /* ---- right: the four sections, in apple2vid_section_name order */
    right = gtk_notebook_new();
    v->sections = GTK_NOTEBOOK(right);
    gtk_widget_set_size_request(right, 440, -1);

    /* Pixel */
    box = padded_box();
    v->pixel_text = GTK_LABEL(mono_label());
    gtk_label_set_wrap(v->pixel_text, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_text));
    v->pixel_note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->pixel_note, 0);
    gtk_widget_add_css_class(GTK_WIDGET(v->pixel_note), "apple2-accent-text");
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_note));
    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b1 = gtk_button_new_with_label("Break on writes");
    b2 = gtk_button_new_with_label("Show in Memory");
    b3 = gtk_button_new_with_label("Unpin");
    gtk_widget_set_tooltip_text(b1, "Break when the CPU writes the byte of the inspected pixel");
    gtk_widget_set_tooltip_text(b2, "Show the inspected pixel's byte in the Memory tab "
                                    "(the aux bank when only aux applies)");
    g_signal_connect(b1, "clicked", G_CALLBACK(on_break), v);
    g_signal_connect(b2, "clicked", G_CALLBACK(on_show_memory), v);
    g_signal_connect(b3, "clicked", G_CALLBACK(on_unpin), v);
    gtk_box_append(GTK_BOX(btns), b1);
    gtk_box_append(GTK_BOX(btns), b2);
    gtk_box_append(GTK_BOX(btns), b3);
    gtk_box_append(GTK_BOX(box), btns);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(apple2vid_section_name(APPLE2VID_S_PIXEL)));

    /* Switches */
    box = padded_box();
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    {
        static const char *const heads[5] = { "Switch", "State", "Age", "Set / clear", "Meaning" };
        for (k = 0; k < 5; k++) {
            GtkWidget *h = gtk_label_new(heads[k]);
            gtk_label_set_xalign(GTK_LABEL(h), 0);
            gtk_widget_add_css_class(h, "heading");
            gtk_grid_attach(GTK_GRID(grid), h, k, 0, 1, 1);
        }
    }
    for (i = 0; i < APPLE2VID_SW_COUNT; i++) {
        GtkWidget *bk = gtk_button_new_with_label("Break on change");
        for (k = 0; k < 5; k++) {
            v->sw_cell[i][k] = GTK_LABEL(mono_label());
            gtk_label_set_selectable(v->sw_cell[i][k], FALSE);
            gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v->sw_cell[i][k]), k, i + 1, 1, 1);
        }
        g_object_set_data(G_OBJECT(bk), "idx", GINT_TO_POINTER(i));
        gtk_widget_add_css_class(bk, "flat");
        gtk_widget_set_tooltip_text(bk, "Break on a read or write of either address of this switch");
        g_signal_connect(bk, "clicked", G_CALLBACK(on_switch_break), v);
        gtk_grid_attach(GTK_GRID(grid), bk, 5, i + 1, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(apple2vid_section_name(APPLE2VID_S_SWITCHES)));

    /* Colours */
    box = padded_box();
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    for (i = 0; i < MAX_PAL; i++) {
        v->pal_box[i] = GTK_DRAWING_AREA(gtk_drawing_area_new());
        gtk_drawing_area_set_content_width(v->pal_box[i], 22);
        gtk_drawing_area_set_content_height(v->pal_box[i], 22);
        g_object_set_data(G_OBJECT(v->pal_box[i]), "idx", GINT_TO_POINTER(i));
        gtk_drawing_area_set_draw_func(v->pal_box[i], draw_swatch, v, NULL);
        v->pal_lbl[i] = GTK_LABEL(mono_label());
        gtk_label_set_selectable(v->pal_lbl[i], FALSE);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v->pal_box[i]), 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v->pal_lbl[i]), 1, i, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(apple2vid_section_name(APPLE2VID_S_COLOURS)));

    /* Timing */
    box = padded_box();
    v->beam = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_text(v->beam, "beam");
    gtk_progress_bar_set_show_text(v->beam, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->beam));
    v->timing_text = GTK_LABEL(mono_label());
    gtk_label_set_selectable(v->timing_text, FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->timing_text));
    gtk_notebook_append_page(v->sections, box, gtk_label_new(apple2vid_section_name(APPLE2VID_S_TIMING)));

    gtk_paned_set_start_child(GTK_PANED(root), left);
    gtk_paned_set_end_child(GTK_PANED(root), right);
    gtk_paned_set_resize_start_child(GTK_PANED(root), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(root), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(root), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(root), FALSE);

    /* start from the controller's state (the APPLE2_DEBUGGER_VIDEO_* variables) */
    v->updating = TRUE;
    for (i = 0; i < nz; i++)
        if (zv[i] == apple2vid_tab_zoom(v->tab)) gtk_drop_down_set_selected(v->zoom, (guint)i);
    for (i = 0; i < v->nov; i++)
        gtk_check_button_set_active(v->ov[i], (apple2vid_tab_overlays(v->tab) & od[i].bit) != 0);
    gtk_notebook_set_current_page(v->sections, apple2vid_tab_env_section(v->tab));
    v->updating = FALSE;
    sync_controls(v);
    update_pixel_section(v);
    g_signal_connect(v->view, "notify::selected", G_CALLBACK(on_view), v);
    g_signal_connect(v->zoom, "notify::selected", G_CALLBACK(on_zoom), v);
    g_signal_connect(v->follow, "toggled", G_CALLBACK(on_follow), v);
    g_signal_connect(v->mode, "notify::selected", G_CALLBACK(on_mode), v);
    g_signal_connect(v->pagesel, "notify::selected", G_CALLBACK(on_page), v);
    g_signal_connect(v->mixed, "toggled", G_CALLBACK(on_mixed), v);
    g_signal_connect(v->sections, "switch-page", G_CALLBACK(on_section), v);
    return root;
}

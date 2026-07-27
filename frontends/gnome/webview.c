/*
 * FujiNet configuration window (embedded WebKitGTK view of the FujiNet web
 * admin at 127.0.0.1:8000, matching the Android app) and a console log
 * window streaming the runtime's captured output.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "webview.h"

#ifdef HAVE_WEBKIT
#include <webkit/webkit.h>
#endif

/* ---- configuration (web UI) --------------------------------------------- */

void apple2_fujinet_config_show(GtkWindow *parent, apple2session *session)
{
    if (!apple2session_fujinet_running(session)) {
        /* Nothing to show; the caller's toast layer reports errors. */
    }
#ifdef HAVE_WEBKIT
    {
        static GtkWindow *win; /* one config window, re-presented */
        if (win) {
            gtk_window_present(win);
            return;
        }
        win = GTK_WINDOW(adw_window_new());
        g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&win);
        gtk_window_set_title(win, "FujiNet Configuration");
        gtk_window_set_default_size(win, 1000, 760);
        gtk_window_set_transient_for(win, parent);

        GtkWidget *view = webkit_web_view_new();
        GtkWidget *tbview = adw_toolbar_view_new();
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tbview),
                                     adw_header_bar_new());
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tbview), view);
        adw_window_set_content(ADW_WINDOW(win), tbview);
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view),
                                 apple2session_fujinet_webui_url(session));
        gtk_window_present(win);
    }
#else
    {
        GtkUriLauncher *launcher =
            gtk_uri_launcher_new(apple2session_fujinet_webui_url(session));
        gtk_uri_launcher_launch(launcher, parent, NULL, NULL, NULL);
        g_object_unref(launcher);
    }
#endif
}

/* ---- console log --------------------------------------------------------- */

typedef struct {
    apple2session *session;
    GtkTextView *view;
    guint timer;
} LogState;

static gboolean log_refresh(gpointer user_data)
{
    LogState *st = user_data;
    static char buf[128 * 1024];
    GtkTextBuffer *tb = gtk_text_view_get_buffer(st->view);
    GtkTextIter end;
    int n = apple2session_fujinet_copy_log(st->session, buf, sizeof(buf));

    gtk_text_buffer_set_text(tb, n > 0 ? buf : "(no FujiNet output yet)", -1);
    gtk_text_buffer_get_end_iter(tb, &end);
    gtk_text_view_scroll_to_iter(st->view, &end, 0, FALSE, 0, 1.0);
    return G_SOURCE_CONTINUE;
}

static void log_destroyed(GtkWidget *win, gpointer user_data)
{
    LogState *st = user_data;
    (void)win;
    g_source_remove(st->timer);
    g_free(st);
}

void apple2_fujinet_log_show(GtkWindow *parent, apple2session *session)
{
    static GtkWindow *win;
    LogState *st;
    GtkWidget *scroll, *view, *tbview;

    if (win) {
        gtk_window_present(win);
        return;
    }
    win = GTK_WINDOW(adw_window_new());
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&win);
    gtk_window_set_title(win, "FujiNet Console Log");
    gtk_window_set_default_size(win, 820, 560);
    gtk_window_set_transient_for(win, parent);

    view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    tbview = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tbview),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tbview), scroll);
    adw_window_set_content(ADW_WINDOW(win), tbview);

    st = g_new0(LogState, 1);
    st->session = session;
    st->view = GTK_TEXT_VIEW(view);
    st->timer = g_timeout_add_seconds(1, log_refresh, st);
    g_signal_connect(win, "destroy", G_CALLBACK(log_destroyed), st);
    log_refresh(st);
    gtk_window_present(win);
}

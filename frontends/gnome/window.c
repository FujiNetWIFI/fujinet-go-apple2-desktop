/*
 * Apple2Window: main window of the GNOME frontend. Header bar + menu over the
 * emulator display; keyboard capture routes everything except F10 (menu) and
 * F11 (fullscreen) to the Apple II. No on-screen input panels are shown
 * unless the user asks for them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "prefs.h"
#include "webview.h"

struct _Apple2Window {
    AdwApplicationWindow parent_instance;

    apple2session *session;
    Apple2Display *display;
    AdwToastOverlay *toasts;
    GtkMenuButton *menu_button;
};

G_DEFINE_FINAL_TYPE(Apple2Window, apple2_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---- helpers ------------------------------------------------------------ */

void apple2_window_toast(Apple2Window *self, const char *message)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(message));
}

void apple2_window_restart_session(Apple2Window *self)
{
    apple2session_start_opts opts;
    apple2session_stop(self->session);
    apple2session_default_opts(self->session, &opts);
    if (apple2session_start(self->session, &opts) != 0)
        apple2_window_toast(self, apple2session_last_error(self->session));
}

static void apply_display_settings(Apple2Window *self)
{
    apple2_display_set_aspect_mode(
        self->display,
        (Apple2AspectMode)apple2session_get_int(self->session, "aspect_mode",
                                                APPLE2_ASPECT_TV_4_3));
    apple2_display_set_smooth(
        self->display,
        apple2session_get_int(self->session, "smooth_scaling", 0));
}

/* ---- keyboard capture ---------------------------------------------------
 * Unlike the ADAM frontend, Alt is NOT passed through to the window manager:
 * the Alt keys are the Apple II's Open Apple and Closed Apple, which games
 * and the boot ROM both use, so they have to reach the machine. F10/F11 keep
 * the menu and fullscreen reachable without them.
 *
 * Both press and release are forwarded: the core tracks Open/Closed Apple as
 * held modifiers, so a missed release would leave one stuck down. */

static gboolean forward_key(Apple2Window *self, guint keyval,
                            GdkModifierType state, int down)
{
    apple2session_key(self->session, down, keyval,
                      gdk_keyval_to_unicode(keyval),
                      (state & GDK_CONTROL_MASK) != 0,
                      (state & GDK_SHIFT_MASK) != 0);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)controller;
    (void)keycode;

    switch (keyval) {
    case GDK_KEY_F10: /* open the primary menu */
        gtk_menu_button_popup(self->menu_button);
        return TRUE;
    case GDK_KEY_F11:
        if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
            gtk_window_unfullscreen(GTK_WINDOW(self));
        else
            gtk_window_fullscreen(GTK_WINDOW(self));
        return TRUE;
    default:
        break;
    }

    return forward_key(self, keyval, state, 1);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)controller;
    (void)keycode;
    if (keyval == GDK_KEY_F10 || keyval == GDK_KEY_F11)
        return;
    forward_key(self, keyval, state, 0);
}

/* ---- actions ------------------------------------------------------------ */

static void action_reset(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    apple2session_reset(self->session, 0);
    apple2_window_toast(self, "Reset");
}

static void action_power_cycle(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    apple2session_reset(self->session, 1);
    apple2_window_toast(self, "Power cycled");
}

static void import_done(GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    char dest[1024];
    g_autofree char *path = NULL;
    g_autofree char *msg = NULL;

    if (!file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    if (apple2session_import_media(self->session, path, dest, sizeof(dest))
        != 0) {
        apple2_window_toast(self, apple2session_last_error(self->session));
        return;
    }
    msg = g_strdup_printf("Copied to FujiNet SD: %s. Mount it from FujiNet "
                          "Configuration.", strrchr(dest, '/') + 1);
    apple2_window_toast(self, msg);
}

static void action_import_media(GSimpleAction *action, GVariant *param,
                                gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    static const char *const exts[] = { "dsk", "do",  "po",  "2mg",
                                        "nib", "woz", "hdv", NULL };
    int i;
    (void)action;
    (void)param;

    gtk_file_filter_set_name(filter, "Apple II disk images");
    for (i = 0; exts[i]; i++)
        gtk_file_filter_add_suffix(filter, exts[i]);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Import Disk Image");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, import_done, self);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void rom_import_done(GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    g_autoptr(GListModel) files =
        gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result,
                                             NULL);
    g_autofree char *msg = NULL;
    guint n, i, ok = 0;

    if (!files)
        return;
    n = g_list_model_get_n_items(files);
    for (i = 0; i < n; i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        g_autofree char *path = g_file_get_path(f);
        char dest[1024];
        if (path && apple2session_import_rom(self->session, path, dest,
                                             sizeof(dest)) == 0)
            ok++;
    }
    if (ok == 0) {
        apple2_window_toast(self, apple2session_last_error(self->session));
        return;
    }
    /* The ROMs are read when the core loads, so this needs a restart. */
    apple2_window_restart_session(self);
    msg = g_strdup_printf("Imported %u ROM file%s (session restarted)",
                          ok, ok == 1 ? "" : "s");
    apple2_window_toast(self, msg);
}

static void action_import_roms(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    (void)action;
    (void)param;

    /* AppleWin looks ROMs up by exact filename (Apple2e_Enhanced.rom,
     * DISK2.rom, ...), so whatever is picked keeps its name. */
    gtk_file_filter_set_name(filter, "Apple II system ROMs");
    gtk_file_filter_add_suffix(filter, "rom");
    gtk_file_filter_add_suffix(filter, "ROM");
    gtk_file_filter_add_suffix(filter, "bin");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Import System ROMs");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(self), NULL,
                                  rom_import_done, self);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void action_fujinet_config(GSimpleAction *action, GVariant *param,
                                  gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    apple2_fujinet_config_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    apple2_fujinet_log_show(GTK_WINDOW(self), self->session);
}

static void action_preferences(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    apple2_prefs_show(self, self->session, apply_display_settings);
}

static void action_about(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    adw_show_about_dialog(
        GTK_WIDGET(self),
        "application-name", "FujiNet Go Apple II",
        "application-icon", apple2_icon_name(),
        "developer-name", "Thomas Cherryhomes",
        "version", APPLE2_VERSION_STRING,
        "license-type", GTK_LICENSE_GPL_3_0,
        "comments", "Self-contained Apple II with built-in FujiNet",
        "website", "https://fujinet.online/",
        NULL);
}

/* ---- construction ------------------------------------------------------- */

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new();
    GMenu *media = g_menu_new();
    GMenu *fujinet = g_menu_new();
    GMenu *tail = g_menu_new();

    g_menu_append(machine, "Reset", "win.reset");
    g_menu_append(machine, "Power Cycle", "win.power-cycle");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));

    g_menu_append(media, "Import Disk Image…", "win.import-media");
    g_menu_append(media, "Import System ROMs…", "win.import-roms");
    g_menu_append_section(menu, "Media", G_MENU_MODEL(media));

    g_menu_append(fujinet, "FujiNet Configuration…", "win.fujinet-config");
    g_menu_append(fujinet, "FujiNet Console Log…", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujinet));

    g_menu_append(tail, "Preferences…", "win.preferences");
    g_menu_append(tail, "About FujiNet Go Apple II", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(machine);
    g_object_unref(media);
    g_object_unref(fujinet);
    g_object_unref(tail);
    return menu;
}

static const GActionEntry win_actions[] = {
    {.name = "reset", .activate = action_reset},
    {.name = "power-cycle", .activate = action_power_cycle},
    {.name = "import-media", .activate = action_import_media},
    {.name = "import-roms", .activate = action_import_roms},
    {.name = "fujinet-config", .activate = action_fujinet_config},
    {.name = "fujinet-log", .activate = action_fujinet_log},
    {.name = "preferences", .activate = action_preferences},
    {.name = "about", .activate = action_about},
};

static void apple2_window_class_init(Apple2WindowClass *klass)
{
    (void)klass;
}

static void apple2_window_init(Apple2Window *self)
{
    (void)self;
}

GtkWidget *apple2_window_new(AdwApplication *app, apple2session *session)
{
    Apple2Window *self = g_object_new(APPLE2_TYPE_WINDOW,
                                      "application", app,
                                      "title", "FujiNet Go Apple II",
                                      "default-width", 1120,
                                      "default-height", 900,
                                      NULL);
    AdwToolbarView *view;
    AdwHeaderBar *header;
    GtkEventController *keys;
    GMenu *menu;

    self->session = session;

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);

    header = ADW_HEADER_BAR(adw_header_bar_new());
    self->menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(self->menu_button, "open-menu-symbolic");
    menu = build_menu();
    gtk_menu_button_set_menu_model(self->menu_button, G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(header, GTK_WIDGET(self->menu_button));

    self->display = APPLE2_DISPLAY(apple2_display_new(session));
    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->display));

    view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(view, GTK_WIDGET(header));
    adw_toolbar_view_set_content(view, GTK_WIDGET(self->toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(view));

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    apply_display_settings(self);
    gtk_widget_grab_focus(GTK_WIDGET(self->display));
    return GTK_WIDGET(self);
}

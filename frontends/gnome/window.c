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
#include "help/help_window.h"

#include <stdlib.h>
#include <string.h>

#include "controllers/controllers_window.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "prefs.h"
#include "webview.h"

struct _Apple2Window {
    AdwApplicationWindow parent_instance;

    apple2session *session;
    Apple2Display *display;
    AdwToastOverlay *toasts;
    GtkMenuButton *menu_button;
    unsigned pad_generation;       /* the pad set the last toast was for */
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

/* A console action from a key, a pad button or the Controllers window, with the
 * same feedback as the Machine menu. UI thread. */
void apple2_window_run_sysaction(Apple2Window *self, int sysact)
{
    switch (sysact) {
    case APPLE2SESSION_SYSACT_RESET:
        apple2session_sysaction_fire(self->session, APPLE2SESSION_SYSACT_RESET);
        apple2_window_toast(self, "Reset");
        break;
    case APPLE2SESSION_SYSACT_POWER:
        apple2session_sysaction_fire(self->session, APPLE2SESSION_SYSACT_POWER);
        apple2_window_toast(self, "Power cycled");
        break;
    case APPLE2SESSION_SYSACT_DEBUG_STOP:
        /* open the debugger first so the stop has somewhere to show */
        apple2_debugger_show(GTK_WINDOW(self), self->session);
        apple2session_sysaction_fire(self->session, APPLE2SESSION_SYSACT_DEBUG_STOP);
        break;
    default:
        break;
    }
}

/* Everything posted since the last look (a bound key or a gamepad button posts
 * it; the gamepad thread cannot call into the toolkit). */
static void drain_sysactions(Apple2Window *self)
{
    unsigned pending = apple2session_sysaction_take(self->session);
    int act;
    for (act = 0; act < APPLE2SESSION_SYSACT_COUNT; act++)
        if (pending & (1u << act))
            apple2_window_run_sysaction(self, act);
}

/* The one key path (see window.h). The keysym names the PHYSICAL key (evdev =
 * GTK keycode - 8), so NumLock and Shift change nothing for the bindings. */
gboolean apple2_window_key(Apple2Window *self, guint keyval, guint keycode,
                           GdkModifierType state, gboolean down)
{
    const uint32_t keysym = apple2session_keysym_from_evdev(keycode >= 8 ? keycode - 8 : 0);
    const int ctrl = (state & GDK_CONTROL_MASK) != 0;

    if (!down) {
        /* A release always reaches the session: it clears exactly what the
         * press asserted, even if Map mode was armed in between. Map only
         * keeps the release of the key it took out of the machine's way. */
        const gboolean released = apple2session_input_key(self->session, keysym, 0, ctrl) != 0;
        if (apple2_controllers_map_key(keyval, keycode, state, FALSE))
            return TRUE;
        if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F10 || keyval == GDK_KEY_F11 ||
            keyval == GDK_KEY_F12)
            return TRUE;
        if (released)
            return TRUE;
        return forward_key(self, keyval, state, 0);
    }

    /* Map capture, then the bindings (Ctrl chords are never bound) */
    if (apple2_controllers_map_key(keyval, keycode, state, TRUE))
        return TRUE;
    if (apple2session_input_key(self->session, keysym, 1, ctrl)) {
        drain_sysactions(self);
        return TRUE;
    }

    /* The window's own keys, deliberately not bindable: they are how you
     * reach the panels that do the binding. */
    switch (keyval) {
    case GDK_KEY_F9:
        apple2_controllers_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    case GDK_KEY_F10: /* open the primary menu */
        gtk_menu_button_popup(self->menu_button);
        return TRUE;
    case GDK_KEY_F11:
        if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
            gtk_window_unfullscreen(GTK_WINDOW(self));
        else
            gtk_window_fullscreen(GTK_WINDOW(self));
        return TRUE;
    case GDK_KEY_F12:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.debugger", NULL);
        return TRUE;
    default:
        break;
    }
    return forward_key(self, keyval, state, 1);
}

/* Lets go of every key the keyboard holds (the mouse's and the pads' holds are
 * their own sources). */
void apple2_window_release_keys(Apple2Window *self)
{
    apple2session_release_all(self->session);
}

/* The Help context the main window shows now: "main", or "prefs/<page>"
 * while Preferences are open. */
const char *apple2_window_help_context(Apple2Window *self)
{
    AdwDialog *d = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self));
    if (d && ADW_IS_PREFERENCES_DIALOG(d)) {
        static char ctx[64];
        const char *page = adw_preferences_dialog_get_visible_page_name(ADW_PREFERENCES_DIALOG(d));
        g_snprintf(ctx, sizeof ctx, "prefs/%s", page ? page : "Machine");
        return ctx;
    }
    return "main";
}

/* app.help: every window's Ctrl+Shift+H, for what it shows. Not a cast of
 * the active window: it can be the debugger or the log. */
static void action_help(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    apple2_help_for(gtk_application_get_active_window(GTK_APPLICATION(user_data)));
}

static const GActionEntry app_actions[] = {
    { "help", action_help, NULL, NULL, NULL, { 0 } },
};

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)controller;

    /* F1 is an APPLE2 function key in the window itself, and Help while
     * Preferences or another dialog is open */
    if (keyval == GDK_KEY_F1 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self)) &&
        !(state & GDK_MODIFIER_MASK & ~GDK_LOCK_MASK)) {
        apple2_help_for(GTK_WINDOW(self));
        return TRUE;
    }
    return apple2_window_key(self, keyval, keycode, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    (void)controller;
    apple2_window_key(APPLE2_WINDOW(user_data), keyval, keycode, state, FALSE);
}

static void on_focus_leave(GtkEventControllerFocus *controller,
                           gpointer user_data)
{
    (void)controller;
    /* a held key never gets its release event once focus is gone */
    apple2_window_release_keys(APPLE2_WINDOW(user_data));
}

/* ---- actions ------------------------------------------------------------ */

static void action_reset(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    apple2_window_run_sysaction(self, APPLE2SESSION_SYSACT_RESET);
}

static void action_power_cycle(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    apple2_window_run_sysaction(self, APPLE2SESSION_SYSACT_POWER);
}

static void action_controllers(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    apple2_controllers_toggle(GTK_WINDOW(self), self->session);
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
    if (apple2session_fujinet_external(self->session)) {
        apple2_window_toast(self, "FujiNet is external: use its own web UI");
        return;
    }
    apple2_fujinet_config_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    if (apple2session_fujinet_external(self->session)) {
        apple2_window_toast(self, "FujiNet is external: see its own console");
        return;
    }
    apple2_fujinet_log_show(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    (void)action;
    (void)param;
    apple2_debugger_show(GTK_WINDOW(self), self->session);
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
    GMenu *view = g_menu_new();
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

    g_menu_append(view, "Controllers (F9)", "win.controllers");
    g_menu_append(view, "Debugger (F12)", "win.debugger");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));

    g_menu_append(tail, "Preferences…", "win.preferences");
    g_menu_append(tail, "FujiNet Go Apple II Help", "app.help");
    g_menu_append(tail, "About FujiNet Go Apple II", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(machine);
    g_object_unref(media);
    g_object_unref(fujinet);
    g_object_unref(view);
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
    {.name = "controllers", .activate = action_controllers},
    {.name = "debugger", .activate = action_debugger},
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

/* Every 50 ms: the console actions the gamepad thread posted (it cannot call
 * into the toolkit), and a toast when a pad comes or goes. */
static gboolean sysact_drain_tick(gpointer user_data)
{
    Apple2Window *self = APPLE2_WINDOW(user_data);
    unsigned gen;

    drain_sysactions(self);
    gen = apple2session_gamepad_generation(self->session);
    if (gen != self->pad_generation) {
        char text[160];
        self->pad_generation = gen;
        if (apple2session_gamepad_last_event(self->session, text, sizeof text) > 0)
            apple2_window_toast(self, text);
    }
    return G_SOURCE_CONTINUE;
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
    if (!g_action_map_lookup_action(G_ACTION_MAP(app), "help"))
        g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions,
                                        G_N_ELEMENTS(app_actions), app);
    {
        static const char *const help_accels[] = { "<Control><Shift>h", NULL };
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.help", help_accels);
    }

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

    {
        GtkEventController *focus = gtk_event_controller_focus_new();
        g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
        gtk_widget_add_controller(GTK_WIDGET(self), focus);
    }

    /* The current pad set is the baseline: no toast for pads already plugged
     * in at launch. */
    self->pad_generation = apple2session_gamepad_generation(session);
    g_timeout_add(50, sysact_drain_tick, self);

    apply_display_settings(self);
    gtk_widget_grab_focus(GTK_WIDGET(self->display));

    /* Developer affordance: open the debugger alongside the main window,
     * which is what you want when the app dies before you can reach a menu. */
    if (getenv("APPLE2_OPEN_CONTROLLERS") || getenv("APPLE2_OPEN_KEYPAD"))
        apple2_controllers_toggle(GTK_WINDOW(self), session);
    if (getenv("APPLE2_OPEN_DEBUGGER"))
        apple2_debugger_show(GTK_WINDOW(self), session);
    if (getenv("APPLE2_OPEN_PREFS"))
        apple2_prefs_show(self, session, apply_display_settings);
    return GTK_WIDGET(self);
}

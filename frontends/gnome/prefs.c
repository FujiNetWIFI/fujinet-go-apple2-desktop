/*
 * Preferences dialog: machine options (applied via session restart on
 * close) and display options (applied live). Values persist through the
 * shared settings store, so the KDE frontend sees the same configuration.
 *
 * The machine and slot options are stored as STRINGS -- they are libretro
 * core-option labels that AppleWin matches literally, so storing an index
 * would silently change meaning if the core ever reorders or inserts a card.
 * The option lists come from the session rather than being repeated here,
 * for the same reason.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "window.h"

static const char *const aspect_names[] = {"TV (4:3)", "Square pixels",
                                           "Integer scale", NULL};

typedef struct {
    Apple2Window *window;
    apple2session *session;
    void (*display_changed)(Apple2Window *window);
    gboolean machine_dirty;
} PrefsState;

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    gboolean is_display;
    /* For string-valued (core option) rows: the label table to index into. */
    const char *const *names;
} RowBinding;

static void mark_changed(RowBinding *b)
{
    if (b->is_display) {
        if (b->state->display_changed)
            b->state->display_changed(b->state->window);
    } else {
        b->state->machine_dirty = TRUE;
    }
}

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (apple2session_get_int(b->state->session, b->key, b->def) == sel)
        return;
    apple2session_set_int(b->state->session, b->key, sel);
    mark_changed(b);
}

static void combo_str_changed(GObject *row, GParamSpec *pspec,
                              gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    const char *cur;
    int n = 0;
    (void)pspec;

    while (b->names[n]) n++;
    if (sel < 0 || sel >= n)
        return;
    cur = apple2session_get_str(b->state->session, b->key, NULL);
    if (cur && g_strcmp0(cur, b->names[sel]) == 0)
        return;
    apple2session_set_str(b->state->session, b->key, b->names[sel]);
    mark_changed(b);
}

static void switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (apple2session_get_int(b->state->session, b->key, b->def) == on)
        return;
    apple2session_set_int(b->state->session, b->key, on);
    mark_changed(b);
}

static RowBinding *binding_new(PrefsState *state, const char *key, int def,
                               gboolean is_display, const char *const *names)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    b->is_display = is_display;
    b->names = names;
    return b;
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *key, int def,
                            const char *const *names, gboolean is_display)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(names);
    RowBinding *b = binding_new(state, key, def, is_display, names);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(
        ADW_COMBO_ROW(row),
        (guint)apple2session_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          b, (GClosureNotify)g_free, 0);
    g_object_unref(model);
    return row;
}

/* A combo over core-option labels, stored by name. names[] must be
 * NUL-terminated and owned by the caller for the dialog's lifetime (the
 * session's static tables are). */
static GtkWidget *combo_row_str(PrefsState *state, const char *title,
                                const char *key, const char *const *names,
                                const char *def)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(names);
    RowBinding *b = binding_new(state, key, 0, FALSE, names);
    const char *cur = apple2session_get_str(state->session, key, def);
    guint sel = 0;
    int i;

    for (i = 0; names[i]; i++) {
        if (g_strcmp0(names[i], cur) == 0) {
            sel = (guint)i;
            break;
        }
    }

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), sel);
    g_signal_connect_data(row, "notify::selected",
                          G_CALLBACK(combo_str_changed), b,
                          (GClosureNotify)g_free, 0);
    g_object_unref(model);
    return row;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *key, int def, gboolean is_display)
{
    GtkWidget *row = adw_switch_row_new();
    RowBinding *b = binding_new(state, key, def, is_display, NULL);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              apple2session_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed),
                          b, (GClosureNotify)g_free, 0);
    return row;
}

static void prefs_closed(GtkWidget *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->machine_dirty) {
        apple2_window_restart_session(state->window);
        apple2_window_toast(state->window,
                            "Machine options applied (session restarted)");
    }
    g_free(state);
}

/* Materialise one of the session's option tables as a NULL-terminated array
 * the GtkStringList constructor accepts. Static storage: the dialog holds the
 * pointers, and there is only ever one dialog. */
#define OPTION_TABLE(fn, storage)                                    \
    static const char *const *storage##_table(void)                  \
    {                                                                \
        static const char *names[32];                                \
        int i;                                                       \
        for (i = 0; i < (int)G_N_ELEMENTS(names) - 1; i++) {         \
            names[i] = fn(i);                                        \
            if (!names[i]) break;                                    \
        }                                                            \
        names[i] = NULL;                                             \
        return names;                                                \
    }

OPTION_TABLE(apple2session_machine_name, machine)
OPTION_TABLE(apple2session_slot3_name, slot3)
OPTION_TABLE(apple2session_slot4_name, slot4)
OPTION_TABLE(apple2session_slot5_name, slot5)
OPTION_TABLE(apple2session_slot7_name, slot7)
OPTION_TABLE(apple2session_video_mode_name, video_mode)

void apple2_prefs_show(Apple2Window *parent, apple2session *session,
                       void (*display_changed)(Apple2Window *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog =
        ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page =
        ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *slots =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *display =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    state->window = parent;
    state->session = session;
    state->display_changed = display_changed;

    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_description(
        machine, "Applied by restarting the session when this dialog closes");
    adw_preferences_group_add(
        machine, combo_row_str(state, "Model", "machine", machine_table(),
                               apple2session_machine_name(0)));
    adw_preferences_group_add(
        machine, combo_row_str(state, "Video", "video_mode",
                               video_mode_table(),
                               apple2session_video_mode_name(0)));

    adw_preferences_group_set_title(slots, "Slots");
    adw_preferences_group_set_description(
        slots, "FujiNet lives in slot 7, the bootable SmartPort slot the "
               "//e scans before the Disk ][ in slot 6");
    adw_preferences_group_add(
        slots, combo_row_str(state, "Slot 3", "slot3", slot3_table(), "Empty"));
    adw_preferences_group_add(
        slots, combo_row_str(state, "Slot 4", "slot4", slot4_table(),
                             "Mockingboard"));
    adw_preferences_group_add(
        slots, combo_row_str(state, "Slot 5", "slot5", slot5_table(), "Empty"));
    adw_preferences_group_add(
        slots, combo_row_str(state, "Slot 7", "slot7", slot7_table(),
                             "FujiNet"));

    adw_preferences_group_set_title(display, "Display");
    adw_preferences_group_add(
        display, combo_row(state, "Aspect ratio", "aspect_mode", 0,
                           aspect_names, TRUE));
    adw_preferences_group_add(
        display,
        switch_row(state, "Smooth scaling", "smooth_scaling", 0, TRUE));

    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, slots);
    adw_preferences_page_add(page, display);
    adw_preferences_dialog_add(dialog, page);
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}

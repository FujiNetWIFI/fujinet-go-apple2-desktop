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

#include "bindings.h"
#include "controllers/controllers_window.h"
#include "window.h"
#include "help/help_window.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PAD_ROWS 8

static const char *const aspect_names[] = {"TV (4:3)", "Square pixels",
                                           "Integer scale", NULL};

typedef struct {
    Apple2Window *window;
    apple2session *session;
    void (*display_changed)(Apple2Window *window);
    gboolean machine_dirty;
    AdwDialog *dialog;
    GtkWidget *pad_group;               /* the Input page's gamepad rows */
    GtkWidget *pad_rows[MAX_PAD_ROWS + 1];
    int pad_row_count;
    unsigned pad_generation;            /* the pad set the rows were built for */
    int pad_mode;                       /* the mode the rows were built for */
    guint pad_timer;
    gboolean syncing;                   /* a refresh, not the user, is moving a row */
} PrefsState;

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    gboolean is_display;
    /* For string-valued (core option) rows: the label table to index into. */
    const char *const *names;
} RowBinding;

static void binding_free(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

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

/* Scanlines is a libretro core option (like machine/slot/video_mode), so it
 * needs a restart -- but smoothing is no longer its own control, it just
 * follows scanlines, and that half applies live like the rest of Display. */
static void scanlines_changed(GObject *row, GParamSpec *pspec,
                              gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (apple2session_get_int(b->state->session, "scanlines", 1) == on)
        return;
    apple2session_set_int(b->state->session, "scanlines", on);
    apple2session_set_int(b->state->session, "smooth_scaling", on);
    b->state->machine_dirty = TRUE;
    if (b->state->display_changed)
        b->state->display_changed(b->state->window);
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
                          b, binding_free, 0);
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
                          binding_free, 0);
    g_object_unref(model);
    return row;
}

static void switch_changed(GObject *row, GParamSpec *pspec,
                           gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (apple2session_get_int(b->state->session, b->key, b->def) == on)
        return;
    apple2session_set_int(b->state->session, b->key, on);
    mark_changed(b);
}

static GtkWidget *scanlines_row(PrefsState *state)
{
    GtkWidget *row = adw_switch_row_new();
    RowBinding *b = binding_new(state, "scanlines", 1, FALSE, NULL);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Scanlines");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(row),
        apple2session_get_int(state->session, "scanlines", 1));
    g_signal_connect_data(row, "notify::active",
                          G_CALLBACK(scanlines_changed), b,
                          binding_free, 0);
    return row;
}

#define EXTERNAL_PLACEHOLDER "0.0.0.0:1985"

/* The address field: valid text (or none, meaning 127.0.0.1:1985) is saved
 * and marks the machine dirty; anything else is flagged and not saved. */
static void addr_changed(GtkEditable *entry, gpointer user_data)
{
    PrefsState *state = user_data;
    const char *text = gtk_editable_get_text(entry);
    char host[64];
    int port;

    if (apple2session_parse_hostport(text, host, sizeof host, &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(apple2session_get_str(state->session,
                                        "fujinet_external_addr", ""),
                  text) == 0)
        return;
    apple2session_set_str(state->session, "fujinet_external_addr", text);
    state->machine_dirty = TRUE;
}

static void external_toggled(GObject *row, GParamSpec *pspec,
                             gpointer user_data)
{
    gtk_widget_set_sensitive(
        GTK_WIDGET(user_data),
        adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    (void)pspec;
}

static GtkWidget *external_row(PrefsState *state)
{
    GtkWidget *row = adw_switch_row_new();
    RowBinding *b = binding_new(state, "fujinet_external", 0, FALSE, NULL);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  "Use external FujiNet");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(row),
        apple2session_get_int(state->session, "fujinet_external", 0));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed), b,
                          binding_free, 0);
    return row;
}

static void prefs_closed(GtkWidget *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->pad_timer)
        g_source_remove(state->pad_timer);
    state->pad_timer = 0;
    if (state->machine_dirty) {
        apple2_window_restart_session(state->window);
        apple2_window_toast(state->window,
                            "Machine options applied (session restarted)");
    }
    g_free(state);
}

/* ---- the Input page ---- */

/* A row with a title and a line under it, nothing to set. */
static GtkWidget *info_row(const char *title, const char *subtitle)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    return row;
}

/* Enable gamepads: a machine option, applied by restarting on close. */
static void gamepad_switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    const int active = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
    (void)pspec;
    if (apple2session_get_int(state->session, "enable_gamepad", 1) == active)
        return;
    apple2session_set_int(state->session, "enable_gamepad", active);
    state->machine_dirty = TRUE;
}

/* A pad row's choice: 0 is Automatic (connection order), then the ports: the
 * joystick, or Paddle 0 and Paddle 1. It is live, not a machine option, so it
 * does not restart the session. */
static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    const int idx = GPOINTER_TO_INT(g_object_get_data(row, "pad-index"));
    const int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (state->syncing)
        return;
    apple2session_gamepad_assign(state->session, idx, sel - 1);
}

/* "Playing the joystick" / "Playing paddle N" / "Playing nothing" for a pad. */
static void pad_subtitle(PrefsState *state, int idx, char *dst, int dstsz)
{
    const int eff = apple2session_gamepad_effective_port(state->session, idx);
    if (eff < 0)
        g_snprintf(dst, dstsz, "Playing nothing");
    else if (apple2session_controller_mode(state->session) == APPLE2_MODE_PADDLES)
        g_snprintf(dst, dstsz, "Playing paddle %d", eff);
    else
        g_snprintf(dst, dstsz, "Playing the joystick");
}

static void rebuild_pad_rows(PrefsState *state)
{
    static const char *const joy_choices[] = { "Automatic", "Joystick", NULL };
    static const char *const pad_choices[] = { "Automatic", "Paddle 0", "Paddle 1", NULL };
    const int mode = apple2session_controller_mode(state->session);
    const char *const *choices = mode == APPLE2_MODE_PADDLES ? pad_choices : joy_choices;
    const int ports = apple2session_controller_ports(state->session);
    const int count = apple2session_gamepad_count(state->session);
    char name[128], sub[64];
    int idx;

    state->syncing = TRUE;
    for (idx = 0; idx < state->pad_row_count; idx++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(state->pad_group),
                                     state->pad_rows[idx]);
    state->pad_row_count = 0;
    state->pad_mode = mode;

    if (count == 0) {
        GtkWidget *row = info_row("No gamepads connected",
                                  "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        state->syncing = FALSE;
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PAD_ROWS; idx++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        const int assign = apple2session_gamepad_assignment(state->session, idx);

        apple2session_gamepad_name(state->session, idx, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        pad_subtitle(state, idx, sub, sizeof sub);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
                                   assign < 0 || assign >= ports ? 0 : (guint)assign + 1);
        g_object_set_data(G_OBJECT(row), "pad-index", GINT_TO_POINTER(idx));
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed), state);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        g_object_unref(model);
    }
    state->syncing = FALSE;
}

/* Once a second: the rows again when a pad came or went or the mode changed,
 * else just the "Playing ..." lines (assigning one pad moves an automatic one). */
static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    const unsigned gen = apple2session_gamepad_generation(state->session);

    if (gen != state->pad_generation ||
        apple2session_controller_mode(state->session) != state->pad_mode) {
        state->pad_generation = gen;
        rebuild_pad_rows(state);
    } else if (apple2session_gamepad_count(state->session) > 0) {
        int idx;
        for (idx = 0; idx < state->pad_row_count; idx++) {
            AdwActionRow *row = ADW_ACTION_ROW(state->pad_rows[idx]);
            char sub[64];
            pad_subtitle(state, idx, sub, sizeof sub);
            if (g_strcmp0(adw_action_row_get_subtitle(row), sub) != 0)
                adw_action_row_set_subtitle(row, sub);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* Joystick or Paddles: live (the mode lets go of everything and centres the
 * machine's inputs). */
static void mode_row_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    const int mode = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (state->syncing || apple2session_controller_mode(state->session) == mode)
        return;
    apple2session_set_controller_mode(state->session, mode);
    rebuild_pad_rows(state);
}

static void model_row_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    const int model = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (state->syncing || apple2session_controller_model(state->session) == model)
        return;
    apple2session_set_controller_model(state->session, model);
}

static void on_open_controllers(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)button;
    apple2_controllers_show(GTK_WINDOW(state->window), state->session);
}

static void on_reset_controls(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)button;
    bindings_reset_defaults(state->session);
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(state->dialog),
                                     adw_toast_new("Controls back to their defaults"));
}

static GtkWidget *combo_live_row(PrefsState *state, const char *title, const char *subtitle,
                                 const char *const *names, int selected, GCallback cb)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(names);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), (guint)selected);
    g_signal_connect(row, "notify::selected", cb, state);
    g_object_unref(model);
    return row;
}

static AdwPreferencesPage *build_input_page(PrefsState *state)
{
    static const char *const mode_names[] = { "Joystick", "Paddles", NULL };
    static const char *const model_names[] = { "Beige (IIe)", "Platinum (IIc)", NULL };
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *row, *open, *reset;

    adw_preferences_page_set_title(page, "Input");
    adw_preferences_page_set_name(page, "Input");
    adw_preferences_page_set_icon_name(page, "input-gaming-symbolic");

    adw_preferences_group_set_title(group, "Game port");
    adw_preferences_group_set_header_suffix(group, apple2_help_button("prefs/Input"));
    adw_preferences_group_set_description(
        group, "What is plugged into the Apple II's one game port. The mode and the "
               "look apply at once; Enable gamepads applies by restarting the session "
               "when this dialog closes.");
    adw_preferences_group_add(
        group, combo_live_row(state, "Controller",
                              "An Apple Joystick, or two Apple Hand Controllers (paddles)",
                              mode_names, apple2session_controller_mode(state->session),
                              G_CALLBACK(mode_row_changed)));
    adw_preferences_group_add(
        group, combo_live_row(state, "Joystick look", "Colours only: the controls are the same",
                              model_names, apple2session_controller_model(state->session),
                              G_CALLBACK(model_row_changed)));
    row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Enable gamepads");
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              apple2session_get_int(state->session, "enable_gamepad", 1) != 0);
    g_signal_connect(row, "notify::active", G_CALLBACK(gamepad_switch_changed), state);
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(page, group);

    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Gamepads");
    adw_preferences_group_set_description(
        group, "A pad plays the joystick, or a paddle in Paddles mode, in connection order "
               "unless chosen here.");
    state->pad_group = GTK_WIDGET(group);
    rebuild_pad_rows(state);
    state->pad_generation = apple2session_gamepad_generation(state->session);
    adw_preferences_page_add(page, group);

    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Controls");
    adw_preferences_group_set_description(
        group, "Keys and gamepad inputs are re-mapped in the Controllers window (F9): "
               "press Map, choose a control, then press the key or gamepad input to give it. "
               "The Alt keys are the Open and Closed Apple keys and cannot be bound.");
    row = info_row("Controllers window", "Map any of the controls, switch to paddles, show the keyboard");
    open = gtk_button_new_with_label("Open Controllers");
    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_controllers), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
    adw_preferences_group_add(group, row);
    row = info_row("Reset Controls", "Put every key and gamepad input back to its default");
    reset = gtk_button_new_with_label("Reset Controls");
    gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_controls), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), reset);
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(page, group);
    return page;
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
OPTION_TABLE(apple2session_slot6_name, slot6)
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
    AdwPreferencesGroup *fujinet =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *display =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *external, *addr;

    state->window = parent;
    state->session = session;
    state->display_changed = display_changed;

    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_header_suffix(machine, apple2_help_button("prefs/Machine"));
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
        slots, combo_row_str(state, "Slot 6", "slot6", slot6_table(),
                             "Disk II"));
    adw_preferences_group_add(
        slots, combo_row_str(state, "Slot 7", "slot7", slot7_table(),
                             "FujiNet"));

    adw_preferences_page_set_name(page, "Machine");
    adw_preferences_group_set_title(fujinet, "FujiNet");
    adw_preferences_group_set_header_suffix(fujinet, apple2_help_button("prefs/FujiNet"));
    adw_preferences_group_set_description(
        fujinet,
        "Skip the built-in FujiNet and wait for another instance to connect "
        "to the slot 7 card. Point that instance's SmartPort-over-SLIP client "
        "at this address. 0.0.0.0 accepts connections from the network; "
        "127.0.0.1 only from this computer.");
    external = external_row(state);
    addr = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr),
                                  "Listen address:port, e.g. "
                                  EXTERNAL_PLACEHOLDER);
    gtk_editable_set_text(GTK_EDITABLE(addr),
                          apple2session_get_str(session,
                                                "fujinet_external_addr", ""));
    g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), state);
    gtk_widget_set_sensitive(addr, adw_switch_row_get_active(
                                       ADW_SWITCH_ROW(external)));
    g_signal_connect(external, "notify::active", G_CALLBACK(external_toggled),
                     addr);
    adw_preferences_group_add(fujinet, external);
    adw_preferences_group_add(fujinet, addr);

    adw_preferences_group_set_title(display, "Display");
    adw_preferences_group_add(
        display, combo_row(state, "Aspect ratio", "aspect_mode", 0,
                           aspect_names, TRUE));
    adw_preferences_group_add(display, scanlines_row(state));

    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_icon_name(page, "applications-games-symbolic");
    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, slots);
    adw_preferences_page_add(page, fujinet);
    adw_preferences_page_add(page, display);
    adw_preferences_dialog_add(dialog, page);
    state->dialog = ADW_DIALOG(dialog);
    adw_preferences_dialog_add(dialog, build_input_page(state));
    /* APPLE2_OPEN_PREFS=Input: open on that page (a developer affordance) */
    {
        const char *open = getenv("APPLE2_OPEN_PREFS");
        if (open && strcmp(open, "Input") == 0)
            adw_preferences_dialog_set_visible_page_name(dialog, open);
    }
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}

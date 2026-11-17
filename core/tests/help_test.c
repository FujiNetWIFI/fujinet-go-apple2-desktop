/*
 * help_test -- the Help covers what the app has: every Preferences setting
 * and choice, every machine, every ROM, every default key and gamepad button,
 * the app's shortcuts (and the Mac's), the two ports, the debugger's tabs
 * and prompt commands, and every context a Help button opens. Every miss is
 * listed before it fails.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apple2help.h"
#include "apple2session.h"

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static const unsigned platforms[] = {
    APPLE2HELP_GNOME, APPLE2HELP_KDE, APPLE2HELP_WINDOWS, APPLE2HELP_MACOS
};
static const char *const platform_names[] = { "GNOME", "KDE", "Windows", "macOS" };

/* What the frontends' Help buttons and menus open. */
static const char *const contexts[] = {
    "main", "fujilog", "controllers", "prefs/Input", "prefs/Machine", "prefs/FujiNet", "prefs/Connection",
    "debugger/console", "debugger/disassembly", "debugger/memory",
    "debugger/breakpoints", "debugger/watches", "debugger/slots", "debugger/trace",
    "debugger/history", "debugger/video", "debugger/fujinet",
};

/* The Preferences labels, as the frontends show them. */
static const char *const labels[] = {
    "Model", "Video", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7", "Use external FujiNet",
    "Listen address:port", "Aspect ratio", "Scanlines", "TV (4:3)", "Square pixels", "Integer scale",
};
/* The Input page and the Controllers window, as the frontends label them. */
static const char *const input_labels[] = {
    "Enable gamepads", "Reset Controls", "Open Controllers", "Joystick", "Paddles",
};
static const char *const controller_words[] = {
    "Plugged in", "Colour", "Reset", "Power Cycle", "Map", "Clear", "Defaults", "Show bindings", "Keyboard",
    "Beige (IIe)", "Platinum (IIc)", "Paddle 0", "Paddle 1", "PDL0", "PDL1", "Reset to CONFIG",
    "Stop in Debugger", "APPLE2_OPEN_CONTROLLERS",
};
/* The keyboard view, the controls and the gamepads each have their own section. */
static const char *const keyboard_words[] = { "Caps Lock", "63 keys", "Open Apple", "Closed Apple", "controllers_keyboard" };
static const char *const control_words[] = {
    "Alt", "Meta", "Super", "F9", "F11", "F12", "Keypad Enter", "Debugger key", "Clear", "Defaults",
};
static const char *const pad_words[] = { "Automatic", "Joystick", "Paddle 0", "Paddle 1", "pad_assign" };
/* Every choice the session's tables offer, in the topic that lists them. */
static const char *const choices[] = {
    "Enhanced Apple //e", "Apple ][ (Original)", "Apple ][+", "Apple ][ J-Plus", "Pravets 82", "Pravets 8M",
    "Pravets 8A", "Base64A", "TK3000 //e", "Color (Composite Monitor)", "Color (Composite Idealized)", "Color TV",
    "B&W TV", "Monochrome (Amber)", "Monochrome (Green)", "Monochrome (White)", "Color (RGB Card/Monitor)",
    "Video HD", "Mockingboard", "Phasor", "SAM/DAC", "CP/M", "Disk II", "Hard Disk",
};

/* The settings keys the pages write. */
static const char *const settings_keys[] = {
    "machine", "video_mode", "slot3", "slot4", "slot5", "slot6", "slot7", "fujinet_external",
    "fujinet_external_addr", "aspect_mode", "scanlines", "smooth_scaling",
};
/* The settings the controls write. */
static const char *const controls_keys[] = {
    "enable_gamepad", "controller_mode", "controller_model", "controllers_show_bindings",
    "controllers_keyboard", "caps_lock", "bindings", "pad_assign",
};

/* The app's shortcuts, as a PC and as a Mac shows them. */
static const struct { const char *pc, *mac; } shortcuts[] = {
    { "Ctrl+Shift+H", "⌘?" }, { "F9", "F9" }, { "F11", "F11" }, { "F12", "F12" },
};

static int misses;

/* Lower case, every run of white space one space. */
static void
fold(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    int space = 0;
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char) *in;
        if (c == ' ' || c == '\n' || c == '\t') {
            space = 1;
            continue;
        }
        if (space && n)
            out[n++] = ' ';
        space = 0;
        out[n++] = (char) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[n] = 0;
}

static char section[1 << 18];
static char raw[1 << 18];

/* The folded text of a context's (or slug's) section on platform p; raw
 * keeps the liapple2. */
static const char *
section_text(int p, const char *context)
{
    int t, b;
    section[0] = 0;
    raw[0] = 0;
    if (apple2help_find(context, &t, &b) == 0) {
        apple2help_plain_text(platforms[p], t, b, raw, sizeof raw);
        fold(raw, section, sizeof section);
    }
    return section;
}

static void
require(int p, const char *context, const char *what, const char *text)
{
    char want[512];
    fold(text, want, sizeof want);
    if (!strstr(section_text(p, context), want)) {
        fprintf(stderr, "MISSING (%s) %s: %s \"%s\"\n", platform_names[p], context, what, text);
        misses++;
    }
}

/* A line of the section (a table row: cells joined by " | ") with every one
 * of these, folded, in that order. */
static void
require_row(int p, const char *context, const char *a, const char *b, const char *c)
{
    char line[1024], fa[256], fb[256], fc[256];
    const char *s, *e;
    int ok = 0;

    section_text(p, context);
    fold(a, fa, sizeof fa);
    fold(b ? b : "", fb, sizeof fb);
    fold(c ? c : "", fc, sizeof fc);
    for (s = raw; *s && !ok; s = *e ? e + 1 : e) {
        char *x, *y;
        size_t n;
        e = strchr(s, '\n');
        if (!e)
            e = s + strlen(s);
        n = (size_t) (e - s) < sizeof line - 1 ? (size_t) (e - s) : sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        fold(line, line, sizeof line);
        x = strstr(line, fa);
        if (!x)
            continue;
        y = b && *fb ? strstr(x + strlen(fa), fb) : x + strlen(fa);
        if (!y)
            continue;
        if (c && *fc && !strstr(y + (b && *fb ? strlen(fb) : 0), fc))
            continue;
        ok = 1;
    }
    if (!ok) {
        fprintf(stderr, "MISSING (%s) %s: a row \"%s\" | \"%s\" | \"%s\"\n", platform_names[p], context,
                a, b ? b : "", c ? c : "");
        misses++;
    }
}

static void
check_prefs(int p)
{
    for (size_t k = 0; k < sizeof labels / sizeof *labels; k++)
        require(p, "preferences", "label", labels[k]);
    for (size_t k = 0; k < sizeof settings_keys / sizeof *settings_keys; k++)
        require(p, "preferences", "setting", settings_keys[k]);
    for (size_t k = 0; k < sizeof choices / sizeof *choices; k++)
        require(p, "machines", "choice", choices[k]);
}

/* The shortcuts the app keeps. */
static void
check_controls(int p)
{
    for (size_t k = 0; k < sizeof shortcuts / sizeof *shortcuts; k++)
        require(p, "controllers-and-keys", "shortcut", platforms[p] == APPLE2HELP_MACOS ? shortcuts[k].mac : shortcuts[k].pc);
}

/* The Controllers window, the keyboard view, the controls and the gamepads. */
static void
check_controllers(int p)
{
    for (size_t k = 0; k < sizeof controller_words / sizeof *controller_words; k++)
        require(p, "the-controllers-window", "controllers window", controller_words[k]);
    for (size_t k = 0; k < sizeof keyboard_words / sizeof *keyboard_words; k++)
        require(p, "keyboard-view", "keyboard view", keyboard_words[k]);
    for (size_t k = 0; k < sizeof control_words / sizeof *control_words; k++)
        require(p, "changing-the-controls", "changing the controls", control_words[k]);
    for (size_t k = 0; k < sizeof pad_words / sizeof *pad_words; k++)
        require(p, "gamepads", "gamepads", pad_words[k]);
    for (size_t k = 0; k < sizeof controls_keys / sizeof *controls_keys; k++)
        require(p, "the-settings-file", "controls setting", controls_keys[k]);
    for (size_t k = 0; k < sizeof input_labels / sizeof *input_labels; k++)
        require(p, "prefs/Input", "input label", input_labels[k]);
    /* the default controls: each row names its keys, and the gamepad column */
    require_row(p, "changing-the-controls", "Joystick left, right, up, down", "Keypad 4, 6, 8, 2", "D-pad");
    require_row(p, "changing-the-controls", "Joystick button 0", "Keypad 0", "South");
    require_row(p, "changing-the-controls", "Joystick button 1", "Keypad Enter", "East");
    require_row(p, "changing-the-controls", "Paddle 0 left, right, button", "Keypad 7, 9, 5", "South");
    require_row(p, "changing-the-controls", "Paddle 1 left, right, button", "Keypad 1, 3", "Keypad .");
    /* the Controllers window is also opened from the main window's topic */
    require(p, "main", "the Controllers menu item", "Controllers");
    if (platforms[p] == APPLE2HELP_WINDOWS)
        require(p, "prefs/Input", "the Windows window", "Input Settings");
    else {
        require(p, "prefs/Input", "joystick colour", "Beige (IIe)");
        require(p, "prefs/Input", "joystick colour", "Platinum (IIc)");
    }
    if (platforms[p] == APPLE2HELP_MACOS)
        require(p, "controllers-and-keys", "the Mac's key", "⌘K");
}

/* The two ports, in the FujiNet topic. */
static void
check_ports(int p)
{
    char port[16];

    snprintf(port, sizeof port, "%d", APPLE2SESSION_SLIP_PORT);
    require(p, "fujinet", "BoIP port", port);
    snprintf(port, sizeof port, "%d", APPLE2SESSION_WEBUI_PORT);
    require(p, "fujinet", "web UI port", port);
    require(p, "an-external-fujinet", "default address", "127.0.0.1:1985");
}

/* The debugger's buttons, its views and each prompt command in a `code` span. */
static void
check_debugger(int p)
{
    static const char *const words[] = { "Run or Stop", "Step Over", "Step Out", "Frame", "Run to Cursor",
                                         "Load Symbols", "Pop Out", "ten tabs" };
    static const char *const commands[] = { "step", "over", "out", "run", "stop", "runto", "scanline", "frame",
                                            "break", "bpr", "bpw", "delete", "enable", "clear", "print", "regs",
                                            "set", "mem", "poke", "video", "io", "map", "fujinet", "label",
                                            "labels", "disasm", "trace", "watch", "unwatch", "find", "save" };
    int t, b;

    for (size_t k = 0; k < sizeof words / sizeof *words; k++)
        require(p, "debugger", "button", words[k]);
    {
        static const char *const vwords[] = { "Screen", "Decoded", "Follow screen", "Break on writes", "Switches", "Colours", "Timing" };
        for (size_t k = 0; k < sizeof vwords / sizeof *vwords; k++)
            require(p, "debugger/video", "view", vwords[k]);
    }
    require(p, "debugger/breakpoints", "limits", "stack");
    require(p, "debugger/breakpoints", "soft switch", "$C030");
    require(p, "debugger/slots", "slots", "FujiNet");
    CHECK(apple2help_find("debugger/console", &t, &b) == 0, "no debugger/console");
    {
        const apple2help_topic *tp = apple2help_topic_at(t);
        int end = apple2help_section_end(t, b);
        for (size_t k = 0; k < sizeof commands / sizeof *commands; k++) {
            const char *cmd = commands[k];
            size_t n = strlen(cmd);
            int found = 0;
            for (int bl = b; bl < end && !found; bl++) {
                const apple2help_block *blk = &tp->blocks[bl];
                if (!(blk->platforms & platforms[p]))
                    continue;
                for (int i = 0; i < blk->nspans && !found; i++) {
                    const char *c = blk->spans[i].text;
                    if (blk->spans[i].kind != APPLE2HELP_CODE)
                        continue;
                    found = !strncmp(c, cmd, n) && (c[n] == 0 || c[n] == ' ' || c[n] == '/');
                }
            }
            if (!found) {
                fprintf(stderr, "MISSING (%s) debugger/console: command `%s`\n", platform_names[p], cmd);
                misses++;
            }
        }
    }
}

int
main(void)
{
    apple2help_hit hits[16];
    int t, b;

    CHECK(apple2help_topic_count() > 0, "no topics");

    for (size_t k = 0; k < sizeof contexts / sizeof *contexts; k++)
        if (apple2help_find(contexts[k], &t, &b) != 0) {
            fprintf(stderr, "MISSING context %s\n", contexts[k]);
            misses++;
        }
    for (int p = 0; p < 4; p++) {
        check_prefs(p);
        check_controls(p);
        check_controllers(p);
        check_ports(p);
        check_debugger(p);
    }

    /* lookup and search */
    CHECK(apple2help_find("#preferences", &t, &b) == 0 && b == -1, "#preferences");
    CHECK(apple2help_find("#the-settings-file", &t, &b) == 0 && b >= 0, "#the-settings-file");
    CHECK(apple2help_search(APPLE2HELP_GNOME, "disk", hits, 16) > 0, "search disk");
    CHECK(apple2help_search(APPLE2HELP_GNOME, "zzqxj", hits, 16) == 0, "search nonsense");
    int n = apple2help_search(APPLE2HELP_KDE, "inspector", hits, 16);
    CHECK(n > 0 && apple2help_find("debugger/video", &t, &b) == 0
          && hits[0].topic == t && hits[0].block == b, "inspector is the first hit");
    for (int i = 1; i < n; i++)
        CHECK(hits[i - 1].score >= hits[i].score, "hits best first");

    /* the Mac sees its own keys */
    {
        char text[4096];
        CHECK(apple2help_find("controllers-and-keys", &t, &b) == 0, "controllers-and-keys");
        apple2help_plain_text(APPLE2HELP_MACOS, t, b, text, sizeof text);
        CHECK(strstr(text, "⌘?") != NULL, "the Mac's Help key");
        apple2help_plain_text(APPLE2HELP_GNOME, t, b, text, sizeof text);
        CHECK(strstr(text, "Ctrl+Shift+H") != NULL && !strstr(text, "⌘?"), "the PC's");
    }

    CHECK(misses == 0, "%d missing from data/help/help.md", misses);
    printf("help ok: %d topics\n", apple2help_topic_count());
    return 0;
}

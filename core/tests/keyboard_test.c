/*
 * keyboard_test -- the on-screen keyboard: the Platinum //e's five rows add up
 * to 15 caps, no cap overlaps another, every printing cap types what it shows
 * (the base character, the shifted one with Shift, the capital with Caps Lock,
 * the base letter with ctrl_down for Control), SHIFT and CONTROL latch and are
 * spent by the next key, CAPS LOCK toggles and starts on, a cap sends a
 * key-down on the press and the same key-up on the release, the Open Apple and
 * Option caps hold a button of the plugged-in controller (and let go with the
 * pointer), the Reset cap posts Reset. What a click would type is recorded
 * through the keyboard's sink; no machine runs.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apple2session.h"
#include "keyboard.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

typedef struct { int down; uint32_t keysym, unicode; int ctrl, shift; } event;
static event g_ev[64];
static int g_nev;

static void sink(apple2session *s, int down, uint32_t keysym, uint32_t unicode, int ctrl, int shift)
{
    (void)s;
    if (g_nev < 64) {
        g_ev[g_nev].down = down;
        g_ev[g_nev].keysym = keysym;
        g_ev[g_nev].unicode = unicode;
        g_ev[g_nev].ctrl = ctrl;
        g_ev[g_nev].shift = shift;
        g_nev++;
    }
}

static const apple2_keycap *g_caps;
static int g_count;

static int cap_named(const char *legend, int nth)
{
    for (int idx = 0; idx < g_count; idx++)
        if (!strcmp(g_caps[idx].legend, legend) && nth-- == 0)
            return idx;
    return -1;
}

static int cap_kind(int kind, int nth)
{
    for (int idx = 0; idx < g_count; idx++)
        if (g_caps[idx].kind == kind && nth-- == 0)
            return idx;
    return -1;
}

static void click(apple2session *s, int idx)
{
    apple2session_keyboard_press(s, idx, 1);
    apple2session_keyboard_press(s, idx, 0);
}

/* one click, as the events it sent: a down then an up of the same key */
static int typed(apple2session *s, int idx, uint32_t keysym, uint32_t unicode, int ctrl, int shift)
{
    g_nev = 0;
    click(s, idx);
    return g_nev == 2 && g_ev[0].down == 1 && g_ev[1].down == 0 &&
           g_ev[0].keysym == keysym && g_ev[1].keysym == keysym &&
           g_ev[0].unicode == unicode && g_ev[1].unicode == unicode &&
           g_ev[0].ctrl == ctrl && g_ev[1].ctrl == ctrl &&
           g_ev[0].shift == shift && g_ev[1].shift == shift;
}

static int overlaps(const apple2_keycap *a, const apple2_keycap *b)
{
    return a->xpos < b->xpos + b->width - 0.001f && b->xpos < a->xpos + a->width - 0.001f &&
           a->ypos < b->ypos + b->height - 0.001f && b->ypos < a->ypos + a->height - 0.001f;
}

int main(void)
{
    char tmpl[512];
    float wide, high;
    test_tmpl(tmpl, sizeof tmpl, "apple2_keyboard_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    apple2session_paths paths = { cfg, data };
    apple2session *s = apple2session_new(&paths);
    if (!s) return 1;
    apple2_keyboard_set_sink(sink);

    g_caps = apple2session_keyboard_layout(&g_count, &wide, &high);
    CHECK(g_caps && g_count == 63, "63 caps: the Reset cap and 62 on the keyboard");
    CHECK(wide > 16.3f && wide < 16.5f && high > 6.0f && high < 6.6f, "the main block is 15 keys wide and five rows high");

    /* geometry: inside, not overlapping, each row is 15 keys wide */
    for (int idx = 0; idx < g_count; idx++) {
        CHECK(g_caps[idx].xpos >= -0.001f && g_caps[idx].ypos >= -0.001f &&
              g_caps[idx].xpos + g_caps[idx].width <= wide + 0.001f &&
              g_caps[idx].ypos + g_caps[idx].height <= high + 0.001f, "every cap is inside the keyboard");
        for (int other = idx + 1; other < g_count; other++)
            CHECK(!overlaps(&g_caps[idx], &g_caps[other]), "no two caps overlap");
    }
    {
        float row_right[5] = { 0 }, row_left[5] = { 99, 99, 99, 99, 99 };
        for (int idx = 0; idx < g_count; idx++) {
            if (g_caps[idx].kind == APPLE2_KEYCAP_RESET) continue;
            const int row = (int)((g_caps[idx].ypos - 0.9f) / 1.1f + 0.5f);
            if (row < 0 || row > 4) { CHECK(0, "a cap is on a row"); continue; }
            if (g_caps[idx].xpos < row_left[row]) row_left[row] = g_caps[idx].xpos;
            if (g_caps[idx].xpos + g_caps[idx].width > row_right[row]) row_right[row] = g_caps[idx].xpos + g_caps[idx].width;
        }
        for (int row = 0; row < 5; row++)
            CHECK(row_left[row] < 0.001f && fabsf(row_right[row] - wide) < 0.001f, "every row spans the same 15 keys");
    }
    {
        const apple2_keycap *reset = &g_caps[cap_kind(APPLE2_KEYCAP_RESET, 0)];
        const apple2_keycap *esc = &g_caps[cap_named("Esc", 0)];
        CHECK(reset->ypos + reset->height <= esc->ypos && reset->xpos < esc->xpos + esc->width, "the Reset cap is above Esc");
    }

    /* the key set */
    {
        int chars = 0, shifts = 0, specials = 0, mods = 0, locks = 0, apples = 0, spaces = 0;
        for (int idx = 0; idx < g_count; idx++) {
            switch (g_caps[idx].kind) {
            case APPLE2_KEYCAP_CHAR: chars++;
                CHECK(g_caps[idx].ch >= 0x20 && g_caps[idx].ch < 0x7f && g_caps[idx].shifted_ch >= 0x20 && g_caps[idx].shifted_ch < 0x7f,
                      "a printing cap has ASCII characters");
                CHECK(g_caps[idx].legend[0] != '\0', "and a legend");
                break;
            case APPLE2_KEYCAP_MODIFIER: mods++; shifts += g_caps[idx].keysym == APPLE2_MOD_SHIFT; break;
            case APPLE2_KEYCAP_SPECIAL: specials++; CHECK(g_caps[idx].keysym != 0, "a named key has its keysym"); break;
            case APPLE2_KEYCAP_LOCK: locks++; break;
            case APPLE2_KEYCAP_APPLE: apples++; break;
            case APPLE2_KEYCAP_SPACE: spaces++; break;
            default: break;
            }
        }
        CHECK(chars == 47, "47 printing keys: 26 letters, the digit row, 11 punctuation keys");
        CHECK(shifts == 2 && mods == 3, "two Shifts and a Control");
        CHECK(specials == 8 && locks == 1 && apples == 2 && spaces == 1, "Esc Tab Delete Return and four arrows; Caps Lock; two Apple keys; the bar");
    }
    {
        /* every character is on exactly one printing cap, and a letter's shifted character is its capital */
        int seen[128] = { 0 };
        for (int idx = 0; idx < g_count; idx++) {
            if (g_caps[idx].kind != APPLE2_KEYCAP_CHAR) continue;
            seen[g_caps[idx].ch]++;
            if (g_caps[idx].ch >= 'a' && g_caps[idx].ch <= 'z')
                CHECK(g_caps[idx].shifted_ch == g_caps[idx].ch - 32 && g_caps[idx].legend[0] == (char)g_caps[idx].shifted_ch &&
                      g_caps[idx].legend[1] == '\0' && g_caps[idx].shifted == NULL, "a letter shows its capital and shifts to it");
            else
                CHECK(g_caps[idx].shifted != NULL, "a symbol shows its shifted legend above");
        }
        for (int chr = 'a'; chr <= 'z'; chr++) CHECK(seen[chr] == 1, "every letter is on one cap");
        for (int chr = '0'; chr <= '9'; chr++) CHECK(seen[chr] == 1, "every digit is on one cap");
        CHECK(seen['-'] == 1 && seen['='] == 1 && seen['['] == 1 && seen[']'] == 1 && seen['\\'] == 1 &&
              seen[';'] == 1 && seen['\''] == 1 && seen[','] == 1 && seen['.'] == 1 && seen['/'] == 1 && seen['`'] == 1,
              "and every punctuation key");
    }

    /* Caps Lock is on at power-up: a letter types the capital, no shift */
    CHECK(apple2session_caps_lock(s) == 1, "Caps Lock starts on, as on the real //e");
    CHECK(apple2session_keyboard_held(s, cap_named("Caps Lock", 0)) & 2, "and its cap shows it");
    CHECK(typed(s, cap_named("Q", 0), 0, 'Q', 0, 0), "Caps Lock on: Q types the capital");
    CHECK(typed(s, cap_named("1", 0), 0, '1', 0, 0), "a digit is unaffected");
    CHECK(typed(s, cap_named("[", 0), 0, '[', 0, 0), "and so is punctuation");
    apple2session_set_caps_lock(s, 0);
    CHECK(typed(s, cap_named("Q", 0), 0, 'q', 0, 0), "Caps Lock off: q is lower case");
    CHECK(!(apple2session_keyboard_held(s, cap_named("Caps Lock", 0)) & 2), "and the cap shows it off");
    {
        const int lock = cap_named("Caps Lock", 0);
        g_nev = 0;
        click(s, lock);
        CHECK(g_nev == 0 && apple2session_caps_lock(s) == 1, "clicking Caps Lock turns it on and types nothing");
        click(s, lock);
        CHECK(apple2session_caps_lock(s) == 0 && !strcmp(apple2session_get_str(s, "caps_lock", ""), "0"), "again: off, and it is a setting");
    }

    /* Shift latches, types the shifted character with shift set, and is spent */
    {
        const int shift = cap_named("Shift", 0), shift2 = cap_named("Shift", 1);
        CHECK(shift >= 0 && shift2 > shift, "two Shift caps");
        g_nev = 0;
        click(s, shift);
        CHECK(g_nev == 0 && (apple2session_keyboard_held(s, shift) & 2), "Shift types nothing; its cap shows it latched");
        CHECK(typed(s, cap_named("Q", 0), 0, 'Q', 0, 1), "the next key is shifted: Q");
        CHECK(!(apple2session_keyboard_held(s, shift) & 2), "and the latch is spent");
        CHECK(typed(s, cap_named("q", 0) < 0 ? cap_named("Q", 0) : cap_named("q", 0), 0, 'q', 0, 0), "so the one after is plain");
        click(s, shift2);
        CHECK(typed(s, cap_named("1", 0), 0, '!', 0, 1), "the other Shift cap: 1 types !");
        click(s, shift);
        click(s, shift);
        CHECK(typed(s, cap_named("2", 0), 0, '2', 0, 0), "a second press lets the latch go");
        click(s, shift);
        CHECK(typed(s, cap_named("'", 0), 0, '"', 0, 1), "Shift + ' is a double quote");
    }

    /* Control latches; the chord sends the base letter with ctrl_down, and Caps Lock does not capitalise it */
    {
        const int ctrl = cap_named("Control", 0);
        apple2session_set_caps_lock(s, 1);
        g_nev = 0;
        click(s, ctrl);
        CHECK(g_nev == 0 && (apple2session_keyboard_held(s, ctrl) & 2), "Control latches");
        CHECK(typed(s, cap_named("C", 0), 0, 'c', 1, 0), "Control + C sends c with ctrl_down (the core resolves it from the keycode)");
        CHECK(!(apple2session_keyboard_held(s, ctrl) & 2), "spent");
        CHECK(typed(s, cap_named("C", 0), 0, 'C', 0, 0), "and the next key is plain (the capital, Caps Lock being on)");
        click(s, ctrl);
        CHECK(typed(s, cap_named("Esc", 0), 0xFF1B, 0, 1, 0), "Control + Esc is a named key with ctrl_down");
        apple2session_set_caps_lock(s, 0);
    }

    /* named keys */
    CHECK(typed(s, cap_named("Return", 0), 0xFF0D, 0, 0, 0), "Return");
    CHECK(typed(s, cap_named("Esc", 0), 0xFF1B, 0, 0, 0), "Esc");
    CHECK(typed(s, cap_named("Tab", 0), 0xFF09, 0, 0, 0), "Tab");
    CHECK(typed(s, cap_named("Delete", 0), 0xFFFF, 0, 0, 0), "Delete is 0xFFFF, not Backspace");
    CHECK(typed(s, cap_named("\xE2\x86\x90", 0), 0xFF51, 0, 0, 0), "the left arrow");
    CHECK(typed(s, cap_named("\xE2\x86\x92", 0), 0xFF53, 0, 0, 0), "the right arrow");
    CHECK(typed(s, cap_named("\xE2\x86\x91", 0), 0xFF52, 0, 0, 0), "the up arrow");
    CHECK(typed(s, cap_named("\xE2\x86\x93", 0), 0xFF54, 0, 0, 0), "the down arrow");
    CHECK(typed(s, cap_kind(APPLE2_KEYCAP_SPACE, 0), 0, ' ', 0, 0), "the space bar types a space");
    {
        const int shift = cap_named("Shift", 0);
        click(s, shift);
        CHECK(typed(s, cap_named("\xE2\x86\x91", 0), 0xFF52, 0, 0, 1), "a named key carries Shift too");
    }

    /* a key stays down while the mouse does, and its release sends the same key-up */
    {
        const int cap = cap_named("A", 0);
        g_nev = 0;
        apple2session_keyboard_press(s, cap, 1);
        CHECK(g_nev == 1 && g_ev[0].down == 1 && (apple2session_keyboard_held(s, cap) & 1), "pressed: a key-down, and the cap is lit");
        apple2session_keyboard_press(s, cap, 0);
        CHECK(g_nev == 2 && g_ev[1].down == 0 && g_ev[1].unicode == g_ev[0].unicode, "released: the matching key-up");
        apple2session_keyboard_press(s, cap, 0);
        CHECK(g_nev == 2, "a release with no press sends nothing");
        CHECK(apple2session_keyboard_held(s, cap) & 1, "the cap flashes after the release");
        usleep(200000);
        CHECK(!(apple2session_keyboard_held(s, cap) & 1), "and goes dark after 120 ms");
        /* the latch state at the press decides the key-up too, whatever happens meanwhile */
        click(s, cap_named("Shift", 0));
        g_nev = 0;
        apple2session_keyboard_press(s, cap, 1);
        click(s, cap_named("Shift", 0));
        apple2session_keyboard_press(s, cap, 0);
        CHECK(g_nev == 2 && g_ev[0].unicode == 'A' && g_ev[1].unicode == 'A' && g_ev[1].shift == 1,
              "the key-up repeats the key-down's character and modifiers");
        click(s, cap_named("Shift", 0));      /* the latch is on again: take it off */
        click(s, cap_named("Shift", 0));
        apple2session_keyboard_release(s);
    }

    /* keyboard_release lets go of what is down and the latches, but not Caps Lock */
    {
        apple2session_set_caps_lock(s, 1);
        click(s, cap_named("Shift", 0));
        g_nev = 0;
        apple2session_keyboard_press(s, cap_named("W", 0), 1);
        apple2session_keyboard_press(s, cap_named("E", 0), 1);
        click(s, cap_named("Control", 0));
        CHECK(g_nev == 2, "two keys down");
        apple2session_keyboard_release(s);
        CHECK(g_nev == 4 && g_ev[2].down == 0 && g_ev[3].down == 0, "release sends both key-ups");
        CHECK(!(apple2session_keyboard_held(s, cap_named("Shift", 0)) & 2) &&
              !(apple2session_keyboard_held(s, cap_named("Control", 0)) & 2), "the latches are cleared");
        CHECK(!(apple2session_keyboard_held(s, cap_named("W", 0)) & 1), "and nothing is lit");
        CHECK(apple2session_caps_lock(s) == 1, "Caps Lock is left alone");
        apple2session_set_caps_lock(s, 0);
        apple2session_keyboard_release(s);
        CHECK(g_nev == 4, "releasing nothing sends nothing");
    }

    /* the Apple keys hold a button of the plugged-in controller */
    {
        const int open = cap_named("Open", 0), option = cap_named("Option", 0);
        CHECK(open >= 0 && option >= 0 && g_caps[open].kind == APPLE2_KEYCAP_APPLE && g_caps[option].kind == APPLE2_KEYCAP_APPLE &&
              g_caps[open].keysym == 0 && g_caps[option].keysym == 1, "Open is button 0 and Option (Closed Apple) button 1");
        CHECK(g_caps[open].xpos > g_caps[cap_kind(APPLE2_KEYCAP_SPACE, 0)].xpos && g_caps[option].xpos < g_caps[cap_kind(APPLE2_KEYCAP_SPACE, 0)].xpos,
              "Option is left of the space bar, Open right of it");
        apple2session_set_controller_mode(s, APPLE2_MODE_JOYSTICK);
        g_nev = 0;
        apple2session_keyboard_press(s, open, 1);
        CHECK(g_nev == 0, "an Apple cap types nothing");
        CHECK(apple2session_target_held(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN0)) &&
              !apple2session_target_held(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN1)), "Open holds the joystick's button 0");
        CHECK(apple2session_keyboard_held(s, open) & 1, "and its cap is lit");
        apple2session_keyboard_press(s, open, 0);
        CHECK(!apple2session_target_held(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN0)), "let go");
        apple2session_keyboard_press(s, option, 1);
        CHECK(apple2session_target_held(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN1)), "Option holds button 1");
        apple2session_keyboard_release(s);
        CHECK(!apple2session_target_held(s, APPLE2_TARGET_JOY(APPLE2_ACT_BTN1)), "keyboard_release lets go of a held Apple cap");
        apple2session_set_controller_mode(s, APPLE2_MODE_PADDLES);
        apple2session_keyboard_press(s, option, 1);
        CHECK(apple2session_target_held(s, APPLE2_TARGET_PADDLE(1, APPLE2_PACT_BUTTON)) &&
              !apple2session_target_held(s, APPLE2_TARGET_PADDLE(0, APPLE2_PACT_BUTTON)), "paddles: Option holds paddle 1's button");
        apple2session_pointer_release(s);
        CHECK(!apple2session_target_held(s, APPLE2_TARGET_PADDLE(1, APPLE2_PACT_BUTTON)), "and the pointer letting go lets go of it");
        apple2session_keyboard_press(s, option, 0);
        /* the Alt keys light the caps too */
        apple2session_key(s, 1, APPLE2SESSION_KEYSYM_LALT, 0, 0, 0);
        CHECK(apple2session_keyboard_held(s, open) & 1, "the Alt key lights the Open cap (the sources OR)");
        apple2session_key(s, 0, APPLE2SESSION_KEYSYM_LALT, 0, 0, 0);
        apple2session_set_controller_mode(s, APPLE2_MODE_JOYSTICK);
    }

    /* the Reset cap posts Reset, on the press */
    {
        const int reset = cap_kind(APPLE2_KEYCAP_RESET, 0);
        (void)apple2session_sysaction_take(s);
        g_nev = 0;
        apple2session_keyboard_press(s, reset, 1);
        CHECK(apple2session_sysaction_take(s) == (1u << APPLE2SESSION_SYSACT_RESET) && g_nev == 0, "the Reset cap posts Reset and types nothing");
        apple2session_keyboard_press(s, reset, 0);
        CHECK(apple2session_sysaction_take(s) == 0, "the release posts nothing");
    }

    /* bad arguments are safe */
    apple2session_keyboard_press(s, -1, 1);
    apple2session_keyboard_press(s, 9999, 1);
    apple2session_keyboard_press(NULL, 0, 1);
    CHECK(apple2session_keyboard_held(s, -1) == 0 && apple2session_keyboard_held(s, 9999) == 0 && apple2session_keyboard_held(NULL, 0) == 0,
          "a bad cap is never lit");

    apple2_keyboard_set_sink(NULL);
    apple2session_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "keyboard_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

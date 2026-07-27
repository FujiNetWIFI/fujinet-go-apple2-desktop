/*
 * Unit tests for the pure input-mapping functions in core/src/input_map.c.
 * Port of the Android app's AppleInputTest.kt + HardwareKeyboardTest.kt.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "apple2session.h"

static int g_fail;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                    \
            fprintf(stderr, "\n");                           \
            g_fail++;                                        \
        }                                                    \
    } while (0)

/* libretro constants, repeated here so the test pins the values the core
 * actually sees rather than reading them from the same header as the code. */
#define RK_BACKSPACE 8
#define RK_TAB       9
#define RK_RETURN    13
#define RK_ESCAPE    27
#define RK_SPACE     32
#define RK_DELETE    127
#define RK_UP        273
#define RK_DOWN      274
#define RK_RIGHT     275
#define RK_LEFT      276
#define RK_RALT      307
#define RK_LALT      308
#define RKMOD_SHIFT  0x01
#define RKMOD_CTRL   0x02

static void expect_key(const char *what, uint32_t keysym, uint32_t uni,
                       int ctrl, int shift, unsigned want_code,
                       uint32_t want_char, uint16_t want_mods)
{
    unsigned code = 0xFFFF;
    uint32_t ch = 0xFFFF;
    uint16_t mods = 0xFFFF;
    int ok = apple2_key_from_event(keysym, uni, ctrl, shift, &code, &ch, &mods);

    CHECK(ok, "%s: expected the key to be forwarded", what);
    CHECK(code == want_code, "%s: keycode %u, want %u", what, code, want_code);
    CHECK(ch == want_char, "%s: character %u, want %u", what, ch, want_char);
    CHECK(mods == want_mods, "%s: mods 0x%x, want 0x%x", what, mods, want_mods);
}

static void test_special_keys(void)
{
    expect_key("Return", 0xFF0D, 0, 0, 0, RK_RETURN, 0, 0);
    expect_key("KP Enter", 0xFF8D, 0, 0, 0, RK_RETURN, 0, 0);
    expect_key("Escape", 0xFF1B, 0, 0, 0, RK_ESCAPE, 0, 0);
    expect_key("Tab", 0xFF09, 0, 0, 0, RK_TAB, 0, 0);
    expect_key("Backspace", 0xFF08, 0, 0, 0, RK_BACKSPACE, 0, 0);
    expect_key("Delete", 0xFFFF, 0, 0, 0, RK_DELETE, 0, 0);
    expect_key("Up", 0xFF52, 0, 0, 0, RK_UP, 0, 0);
    expect_key("Down", 0xFF54, 0, 0, 0, RK_DOWN, 0, 0);
    expect_key("Left", 0xFF51, 0, 0, 0, RK_LEFT, 0, 0);
    expect_key("Right", 0xFF53, 0, 0, 0, RK_RIGHT, 0, 0);
}

static void test_apple_keys(void)
{
    /* Open Apple / Closed Apple: Alt on a PC keyboard, Command on a Mac
     * (which reports Meta/Super) -- both are physically the Apple keys. */
    expect_key("Alt_L -> Open Apple", 0xFFE9, 0, 0, 0, RK_LALT, 0, 0);
    expect_key("Alt_R -> Closed Apple", 0xFFEA, 0, 0, 0, RK_RALT, 0, 0);
    expect_key("Meta_L -> Open Apple", 0xFFE7, 0, 0, 0, RK_LALT, 0, 0);
    expect_key("Super_R -> Closed Apple", 0xFFEC, 0, 0, 0, RK_RALT, 0, 0);
}

static void test_printable(void)
{
    /* The keycode is the UNSHIFTED value; the character is what was typed. */
    expect_key("lowercase a", 'a', 'a', 0, 0, 'a', 'a', 0);
    expect_key("uppercase A", 'A', 'A', 0, 1, 'a', 'A', RKMOD_SHIFT);
    expect_key("digit 1", '1', '1', 0, 0, '1', '1', 0);
    expect_key("shift-1 = !", '!', '!', 0, 1, '1', '!', RKMOD_SHIFT);
    expect_key("shift-; = :", ':', ':', 0, 1, ';', ':', RKMOD_SHIFT);
    expect_key("shift-/ = ?", '?', '?', 0, 1, '/', '?', RKMOD_SHIFT);
    expect_key("space", ' ', ' ', 0, 0, RK_SPACE, ' ', 0);

    /* Ctrl combos: character forced to 0 so the core resolves them from the
     * keycode instead of treating a control byte as typed text. */
    expect_key("Ctrl-C", 'c', 'c', 1, 0, 'c', 0, RKMOD_CTRL);
    expect_key("Ctrl-Shift-C", 'C', 'C', 1, 1, 'c', 0,
               RKMOD_CTRL | RKMOD_SHIFT);

    /* Falls back to the keysym when the frontend gave no translated char. */
    expect_key("no unicode", 'z', 0, 0, 0, 'z', 'z', 0);
}

static void test_not_forwarded(void)
{
    unsigned code;
    uint32_t ch;
    uint16_t mods;
    /* Keys the Apple II has no concept of must be rejected, not passed
     * through as garbage: F5 (0xFFC2), Shift alone (0xFFE1), Home (0xFF50). */
    CHECK(!apple2_key_from_event(0xFFC2, 0, 0, 0, &code, &ch, &mods),
          "F5 should not be forwarded");
    CHECK(!apple2_key_from_event(0xFFE1, 0, 0, 1, &code, &ch, &mods),
          "Shift alone should not be forwarded");
    CHECK(!apple2_key_from_event(0xFF50, 0, 0, 0, &code, &ch, &mods),
          "Home should not be forwarded");
}

static int nearly(float a, float b)
{
    float d = a - b;
    return (d < 0 ? -d : d) < 0.0005f;
}

static void test_paddle_axis(void)
{
    /* Inside the deadzone -> dead centre. */
    CHECK(apple2_paddle_from_axis(0.0f, 0.18f) == 0.0f, "centre stays 0");
    CHECK(apple2_paddle_from_axis(0.1f, 0.18f) == 0.0f, "inside deadzone is 0");
    CHECK(apple2_paddle_from_axis(-0.18f, 0.18f) == 0.0f, "deadzone edge is 0");

    /* Full deflection must still reach the stops despite the deadzone --
     * that is the whole point of rescaling rather than just subtracting. */
    CHECK(nearly(apple2_paddle_from_axis(1.0f, 0.18f), 1.0f),
          "full right reaches +1, got %f", apple2_paddle_from_axis(1.0f, 0.18f));
    CHECK(nearly(apple2_paddle_from_axis(-1.0f, 0.18f), -1.0f),
          "full left reaches -1, got %f", apple2_paddle_from_axis(-1.0f, 0.18f));

    /* Just past the deadzone is near zero, not a jump to full scale. */
    CHECK(apple2_paddle_from_axis(0.19f, 0.18f) > 0.0f &&
          apple2_paddle_from_axis(0.19f, 0.18f) < 0.05f,
          "just past the deadzone is a small value, got %f",
          apple2_paddle_from_axis(0.19f, 0.18f));

    /* Midpoint of the live travel maps to the middle. */
    CHECK(nearly(apple2_paddle_from_axis(0.59f, 0.18f), 0.5f),
          "midpoint maps to 0.5, got %f", apple2_paddle_from_axis(0.59f, 0.18f));

    /* Out-of-range input is clamped rather than amplified. */
    CHECK(nearly(apple2_paddle_from_axis(1.5f, 0.18f), 1.0f), "clamps above +1");
    CHECK(nearly(apple2_paddle_from_axis(-1.5f, 0.18f), -1.0f), "clamps below -1");
}

int main(void)
{
    test_special_keys();
    test_apple_keys();
    test_printable();
    test_not_forwarded();
    test_paddle_axis();

    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("input_test: all checks passed\n");
    return 0;
}

/*
 * keyboard -- the on-screen keyboard: the Platinum Apple //e's caps as they
 * are printed (see DESIGN-apple2.md: the layout read off photographs of the
 * real keyboard, a 15-unit main block of five rows, the small Reset cap above
 * Esc).
 *
 * A cap types what the host keyboard would: the same events apple2session_key
 * takes, a press on the click and a release on the release (the //e's keyboard
 * is edge-based, unlike the ADAM's code stream). A printing key sends its
 * character as `unicode` (the shifted character with Shift, the capital with
 * Caps Lock, the base letter with Control and ctrl_down set: the core resolves
 * a Control chord from the keycode); a named key sends its keysym.
 *
 * SHIFT (both keys) and CONTROL are latches here, as there is no way to hold a
 * cap while clicking another with one mouse: they are spent by the next
 * ordinary key. CAPS LOCK toggles, and is ON at power-up as on the real //e.
 * The Open Apple and Option (Closed Apple) caps are held with the mouse and
 * press button 0 or 1 of the controller that is plugged in (the pointer
 * source, so they OR cleanly with the Alt keys and the pads).
 *
 * Geometry: units are key caps (a key is 1.0 x 1.0, the gap between caps 0.1
 * already in the coordinates, so the pitch is 1.1). The key set and what each
 * key sends are exact; the widths of Caps Lock, Option and the arrow cluster
 * follow the photographs and are approximate.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "apple2_keyboard.h"
#include "session_internal.h"

#define GAP   0.1f
#define PITCH (1.0f + GAP)
#define FLASH_NS 120000000LL
#define ROW0 0.9f                  /* the Reset cap sits above the first row */

#define K_ESC   0xFF1B
#define K_TAB   0xFF09
#define K_DEL   0xFFFF
#define K_RET   0xFF0D
#define K_LEFT  0xFF51
#define K_UP    0xFF52
#define K_RIGHT 0xFF53
#define K_DOWN  0xFF54

#define CAP(lbl, shf, col, row, wid, kind, chr, shchr, sym) \
    { lbl, shf, (col) * PITCH, ROW0 + (row) * PITCH, (wid) * PITCH - GAP, 1.0f, \
      kind, chr, shchr, sym }
/* A character key: the legend, the shifted legend, the two characters. */
#define CH(lbl, shf, col, row, chr, shchr) \
    CAP(lbl, shf, col, row, 1, APPLE2_KEYCAP_CHAR, chr, shchr, 0)
/* A letter: the legend is the capital, the character lower case unshifted. */
#define LET(lbl, chr, col, row) CH(lbl, NULL, col, row, (chr) + 32, (chr))
#define SPECIAL(lbl, col, row, wid, sym) \
    CAP(lbl, NULL, col, row, wid, APPLE2_KEYCAP_SPECIAL, 0, 0, sym)
#define ARROW(glyph, col, row, sym) SPECIAL(glyph, col, row, 1, sym)

static const apple2_keycap k_caps[] = {
    /* the Reset cap, small, above Esc */
    { "Reset", NULL, 0.0f, 0.0f, 1.0f * PITCH - GAP, 0.7f, APPLE2_KEYCAP_RESET, 0, 0, 0 },

    /* row 0 */
    SPECIAL("Esc", 0, 0, 1, K_ESC),
    CH("1", "!", 1, 0, '1', '!'), CH("2", "@", 2, 0, '2', '@'), CH("3", "#", 3, 0, '3', '#'),
    CH("4", "$", 4, 0, '4', '$'), CH("5", "%", 5, 0, '5', '%'), CH("6", "^", 6, 0, '6', '^'),
    CH("7", "&", 7, 0, '7', '&'), CH("8", "*", 8, 0, '8', '*'), CH("9", "(", 9, 0, '9', '('),
    CH("0", ")", 10, 0, '0', ')'), CH("-", "_", 11, 0, '-', '_'), CH("=", "+", 12, 0, '=', '+'),
    SPECIAL("Delete", 13, 0, 2, K_DEL),

    /* row 1 */
    SPECIAL("Tab", 0, 1, 1.5f, K_TAB),
    LET("Q", 'Q', 1.5f, 1), LET("W", 'W', 2.5f, 1), LET("E", 'E', 3.5f, 1), LET("R", 'R', 4.5f, 1),
    LET("T", 'T', 5.5f, 1), LET("Y", 'Y', 6.5f, 1), LET("U", 'U', 7.5f, 1), LET("I", 'I', 8.5f, 1),
    LET("O", 'O', 9.5f, 1), LET("P", 'P', 10.5f, 1),
    CH("[", "{", 11.5f, 1, '[', '{'), CH("]", "}", 12.5f, 1, ']', '}'),
    CAP("\\", "|", 13.5f, 1, 1.5f, APPLE2_KEYCAP_CHAR, '\\', '|', 0),

    /* row 2 */
    CAP("Control", NULL, 0, 2, 1.75f, APPLE2_KEYCAP_MODIFIER, 0, 0, APPLE2_MOD_CONTROL),
    LET("A", 'A', 1.75f, 2), LET("S", 'S', 2.75f, 2), LET("D", 'D', 3.75f, 2), LET("F", 'F', 4.75f, 2),
    LET("G", 'G', 5.75f, 2), LET("H", 'H', 6.75f, 2), LET("J", 'J', 7.75f, 2), LET("K", 'K', 8.75f, 2),
    LET("L", 'L', 9.75f, 2),
    CH(";", ":", 10.75f, 2, ';', ':'), CH("'", "\"", 11.75f, 2, '\'', '"'),
    SPECIAL("Return", 12.75f, 2, 2.25f, K_RET),

    /* row 3 */
    CAP("Shift", NULL, 0, 3, 2.25f, APPLE2_KEYCAP_MODIFIER, 0, 0, APPLE2_MOD_SHIFT),
    LET("Z", 'Z', 2.25f, 3), LET("X", 'X', 3.25f, 3), LET("C", 'C', 4.25f, 3), LET("V", 'V', 5.25f, 3),
    LET("B", 'B', 6.25f, 3), LET("N", 'N', 7.25f, 3), LET("M", 'M', 8.25f, 3),
    CH(",", "<", 9.25f, 3, ',', '<'), CH(".", ">", 10.25f, 3, '.', '>'), CH("/", "?", 11.25f, 3, '/', '?'),
    CAP("Shift", NULL, 12.25f, 3, 2.75f, APPLE2_KEYCAP_MODIFIER, 0, 0, APPLE2_MOD_SHIFT),

    /* row 4 */
    CAP("Caps Lock", NULL, 0, 4, 1.5f, APPLE2_KEYCAP_LOCK, 0, 0, 0),
    CH("`", "~", 1.5f, 4, '`', '~'),
    CAP("Option", NULL, 2.5f, 4, 1.25f, APPLE2_KEYCAP_APPLE, 0, 0, 1),     /* Closed (Solid) Apple: button 1 */
    CAP("", NULL, 3.75f, 4, 6, APPLE2_KEYCAP_SPACE, ' ', ' ', 0),
    CAP("Open", NULL, 9.75f, 4, 1.25f, APPLE2_KEYCAP_APPLE, 0, 0, 0),      /* Open Apple: button 0 */
    ARROW("\xE2\x86\x90", 11, 4, K_LEFT), ARROW("\xE2\x86\x92", 12, 4, K_RIGHT),
    ARROW("\xE2\x86\x93", 13, 4, K_DOWN), ARROW("\xE2\x86\x91", 14, 4, K_UP),
};

#define CAP_COUNT ((int)(sizeof k_caps / sizeof k_caps[0]))
#define KBD_WIDTH  (15 * PITCH - GAP)
#define KBD_HEIGHT (ROW0 + 4 * PITCH + 1.0f)

const apple2_keycap *apple2session_keyboard_layout(int *count, float *width, float *height)
{
    if (count) *count = CAP_COUNT;
    if (width) *width = KBD_WIDTH;
    if (height) *height = KBD_HEIGHT;
    return k_caps;
}

/* ---- state ---------------------------------------------------------------------- */

typedef struct {
    uint32_t keysym, unicode;
    int ctrl, shift;
    uint8_t active;                 /* a key-down went out and its key-up is owed */
} pending_key;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_down[CAP_COUNT];
static int64_t g_flash_ns[CAP_COUNT];
static pending_key g_pending[CAP_COUNT];
static uint8_t g_latch_shift, g_latch_ctrl;

static void default_sink(apple2session *s, int down, uint32_t keysym, uint32_t unicode,
                         int ctrl, int shift)
{
    apple2session_key(s, down, keysym, unicode, ctrl, shift);
}
static apple2_key_sink g_sink = default_sink;

void apple2_keyboard_set_sink(apple2_key_sink sink)
{
    pthread_mutex_lock(&g_mtx);
    g_sink = sink ? sink : default_sink;
    pthread_mutex_unlock(&g_mtx);
}

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int apple2session_caps_lock(apple2session *s)
{
    return apple2session_get_int(s, "caps_lock", 1) != 0;
}

void apple2session_set_caps_lock(apple2session *s, int on)
{
    apple2session_set_int(s, "caps_lock", on ? 1 : 0);
}

/* The target the Open (0) or Option (1) cap holds in the current mode: PB0 or
 * PB1 of whatever is plugged in. */
static int apple_target(apple2session *s, int button)
{
    return apple2session_controller_mode(s) == APPLE2_MODE_PADDLES
               ? APPLE2_TARGET_PADDLE(button, APPLE2_PACT_BUTTON)
               : APPLE2_TARGET_JOY(button == 0 ? APPLE2_ACT_BTN0 : APPLE2_ACT_BTN1);
}

static int is_lower_letter(uint32_t chr)
{
    return chr >= 'a' && chr <= 'z';
}

/* ---- public ------------------------------------------------------------------------ */

void apple2session_keyboard_press(apple2session *s, int index, int down)
{
    const apple2_keycap *cap;
    apple2_key_sink sink;
    pending_key out = { 0, 0, 0, 0, 0 };
    int send = 0;                       /* 1: a key-down, -1: a key-up */
    int apple_down = -1;                /* the Apple cap's hold to apply */
    int post_reset = 0;
    int caps;

    if (!s || index < 0 || index >= CAP_COUNT)
        return;
    cap = &k_caps[index];
    down = down != 0;
    caps = apple2session_caps_lock(s);

    pthread_mutex_lock(&g_mtx);
    sink = g_sink;
    g_down[index] = (uint8_t)down;
    if (down)
        g_flash_ns[index] = now_ns();

    switch (cap->kind) {
    case APPLE2_KEYCAP_MODIFIER:
        if (down) {
            if (cap->keysym == APPLE2_MOD_SHIFT) g_latch_shift ^= 1u;
            else g_latch_ctrl ^= 1u;
        }
        break;
    case APPLE2_KEYCAP_LOCK:
        break;                          /* toggled below, outside the lock */
    case APPLE2_KEYCAP_APPLE:
        apple_down = down;
        break;
    case APPLE2_KEYCAP_RESET:
        post_reset = down;
        break;
    default:                            /* CHAR, SPACE, SPECIAL */
        if (down) {
            const int ctrl = g_latch_ctrl, shift = g_latch_shift;
            uint32_t uni = 0, sym = 0;
            if (cap->kind == APPLE2_KEYCAP_SPECIAL) {
                sym = cap->keysym;
            } else {
                uni = cap->ch;
                if (shift && !ctrl && cap->shifted_ch)
                    uni = cap->shifted_ch;
                else if (caps && !ctrl && is_lower_letter(cap->ch))
                    uni = cap->ch - 32;               /* the capital */
            }
            out.keysym = sym;
            out.unicode = uni;
            out.ctrl = ctrl;
            out.shift = shift;
            out.active = 1;
            g_pending[index] = out;
            g_latch_shift = 0;                        /* spent by the key; Caps Lock stays */
            g_latch_ctrl = 0;
            send = 1;
        } else if (g_pending[index].active) {
            out = g_pending[index];
            g_pending[index].active = 0;
            send = -1;
        }
        break;
    }
    pthread_mutex_unlock(&g_mtx);

    if (send)
        sink(s, send > 0, out.keysym, out.unicode, out.ctrl, out.shift);
    if (cap->kind == APPLE2_KEYCAP_LOCK && down)
        apple2session_set_caps_lock(s, !caps);
    if (apple_down >= 0)
        apple2session_press(s, apple_target(s, (int)cap->keysym), apple_down);
    if (post_reset)
        apple2session_sysaction_post(s, APPLE2SESSION_SYSACT_RESET);
}

int apple2session_keyboard_held(apple2session *s, int index)
{
    const apple2_keycap *cap;
    int held = 0;
    if (!s || index < 0 || index >= CAP_COUNT)
        return 0;
    cap = &k_caps[index];
    pthread_mutex_lock(&g_mtx);
    if (g_down[index] || (g_flash_ns[index] && now_ns() - g_flash_ns[index] < FLASH_NS))
        held |= 1;
    if (cap->kind == APPLE2_KEYCAP_MODIFIER &&
        (cap->keysym == APPLE2_MOD_SHIFT ? g_latch_shift : g_latch_ctrl))
        held |= 2;
    pthread_mutex_unlock(&g_mtx);
    if (cap->kind == APPLE2_KEYCAP_LOCK && apple2session_caps_lock(s))
        held |= 2;
    if (cap->kind == APPLE2_KEYCAP_APPLE &&
        apple2session_target_held(s, apple_target(s, (int)cap->keysym)))
        held |= 1;
    return held;
}

void apple2session_keyboard_release(apple2session *s)
{
    apple2_key_sink sink;
    pending_key owed[CAP_COUNT];
    int apple_down[2] = { 0, 0 };
    if (!s) return;
    pthread_mutex_lock(&g_mtx);
    sink = g_sink;
    memcpy(owed, g_pending, sizeof owed);
    for (int idx = 0; idx < CAP_COUNT; idx++) {
        if (k_caps[idx].kind == APPLE2_KEYCAP_APPLE && g_down[idx])
            apple_down[k_caps[idx].keysym ? 1 : 0] = 1;
    }
    memset(g_pending, 0, sizeof g_pending);
    memset(g_down, 0, sizeof g_down);
    memset(g_flash_ns, 0, sizeof g_flash_ns);
    g_latch_shift = g_latch_ctrl = 0;
    pthread_mutex_unlock(&g_mtx);

    for (int idx = 0; idx < CAP_COUNT; idx++)
        if (owed[idx].active)
            sink(s, 0, owed[idx].keysym, owed[idx].unicode, owed[idx].ctrl, owed[idx].shift);
    for (int btn = 0; btn < 2; btn++)
        if (apple_down[btn])
            apple2session_press(s, apple_target(s, btn), 0);
}

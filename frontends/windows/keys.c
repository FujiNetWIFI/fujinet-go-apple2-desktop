/*
 * Keyboard path shared by the main window and the Controllers window.
 *
 * Everything is driven from WM_KEYDOWN/WM_KEYUP rather than WM_CHAR: the core
 * tracks Open Apple and Closed Apple as HELD modifiers, so every key needs a
 * matching release, and WM_CHAR has none. The printable character is obtained
 * from ToUnicode() instead, which applies the user's layout and shift state
 * exactly as TranslateMessage would have.
 *
 * Every press and release goes to apple2session_input_key first: a key bound to
 * a controller target drives it and is NOT also typed on the Apple II. Alt
 * keys, F9, F11 and F12 are never bound.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keys.h"

/* Win32 virtual key -> X11/xkb keysym, for the keys apple2_key_from_event
 * resolves by keycode rather than by character. */
static uint32_t keysym_for_vk(WPARAM vk, LPARAM lparam)
{
    switch (vk) {
    case VK_RETURN:
        /* Numpad Enter sets the extended-key bit; both are Return here. */
        return 0xFF0D;
    case VK_ESCAPE:   return 0xFF1B;
    case VK_TAB:      return 0xFF09;
    case VK_BACK:     return 0xFF08;
    case VK_DELETE:   return 0xFFFF;
    case VK_LEFT:     return 0xFF51;
    case VK_UP:       return 0xFF52;
    case VK_RIGHT:    return 0xFF53;
    case VK_DOWN:     return 0xFF54;
    /* The Apple keys. Windows reports both Alts as VK_MENU and distinguishes
     * them by the extended-key bit (bit 24 of lParam), which is set for the
     * right-hand one. Left Alt is Open Apple, right Alt is Closed Apple. */
    case VK_MENU:     return (lparam & (1 << 24)) ? 0xFFEA : 0xFFE9;
    case VK_LMENU:    return 0xFFE9;
    case VK_RMENU:    return 0xFFEA;
    /* A Mac keyboard's Command keys land on the Windows keys, and they are
     * physically the Apple keys. */
    case VK_LWIN:     return 0xFFE7;
    case VK_RWIN:     return 0xFFE8;
    default:          return 0;
    }
}

/* The character this key would type, ignoring Ctrl. Ctrl is masked out of the
 * keyboard state because ToUnicode would otherwise fold it into a control
 * code (Ctrl+A -> 0x01), and the mapping table wants the plain character --
 * it forces the character to 0 for Ctrl combos itself, after deriving the
 * keycode from it. */
static uint32_t translated_char(WPARAM vk, LPARAM lparam)
{
    BYTE state[256];
    WCHAR buf[8];
    int n;

    if (!GetKeyboardState(state))
        return 0;
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
    /* Alt is Open/Closed Apple, not a compose modifier. */
    state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = 0;

    /* Flag bit 2 keeps ToUnicode from consuming pending dead-key state, so
     * probing a keystroke here does not disturb the next one. */
    n = ToUnicode((UINT)vk, (UINT)((lparam >> 16) & 0xFF), state, buf,
                  (int)(sizeof(buf) / sizeof(buf[0])), 0x04);
    return n > 0 ? (uint32_t)buf[0] : 0;
}

uint32_t apple2_win_keysym(WPARAM vk, LPARAM lp)
{
    uint32_t keysym;
    /* Windows reports Pause as scan code 0x45 and Num Lock as extended 0x45:
     * the core's table cannot tell them apart, the VK can. */
    if (vk == VK_PAUSE)
        return 0xFF13;
    if (vk == VK_NUMLOCK)
        return 0xFF7F;
    keysym = apple2session_keysym_from_win_scancode((unsigned)((lp >> 16) & 0xFF),
                                                    (int)((lp >> 24) & 1));
    return keysym ? keysym : keysym_for_vk(vk, lp);
}

static void forward_key(apple2session *session, WPARAM vk, LPARAM lparam, int down)
{
    const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    uint32_t unicode = 0;
    uint32_t keysym = keysym_for_vk(vk, lparam);

    if (keysym == 0) {
        unicode = translated_char(vk, lparam);
        keysym = unicode;
    }
    if (keysym == 0)
        return;
    apple2session_key(session, down, keysym, unicode, ctrl, shift);
}

int apple2_win_key_msg(apple2session *session, UINT msg, WPARAM wp, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    uint32_t keysym;

    /* Alt+F4 closes the window, whatever F4 would otherwise do. */
    if (down && wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
        return 0;
    /* Auto-repeat (lParam bit 30 = key was already down): the core latches a
     * key until the machine reads it, so a repeat storm would jam it. Let the
     * Apple II's own repeat handle it. */
    if (down && (lp & (1 << 30)))
        return 1;

    keysym = apple2_win_keysym(wp, lp);
    if (keysym && apple2session_input_key(session, keysym, down, ctrl))
        return 1;
    forward_key(session, wp, lp, down);
    return 1;
}

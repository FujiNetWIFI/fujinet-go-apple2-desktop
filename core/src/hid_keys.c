/*
 * hid_keys -- native key code -> USB HID Keyboard usage -> keysym
 * translation, and the names for those usages.
 *
 * apple2session.h's APPLE2SESSION_KEYSYM_* names only the keys the default
 * bindings care about; a key with no X11 keysym of its own (a media key, an
 * international key, a console-to-USB adapter's keys) is still bindable
 * through the HID band, so a keypad window's Map mode never discards a key.
 * All three tables compile everywhere so the Windows and macOS ones can be
 * checked from Linux CI.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hid_keys.h"

#include <stddef.h>
#include "apple2session.h"
#include "hid_keys.h"

/* ---- Windows: PS/2 scan code set 1 -> HID usage ----------------------------
 * The code in WM_KEYDOWN's lParam bits 16-23. Two tables because the E0
 * prefix (reported as lParam bit 24, the "extended key" flag) reuses the
 * same 8-bit code space for a different key: 0x1C is Enter without it and
 * Keypad Enter with it.
 *
 * Holes are 0 = "no keyboard usage". Most of them are keys on the HID
 * Consumer page rather than the Keyboard page (browser back, volume, launch
 * keys); those legitimately have no keyboard usage, and the caller falls back
 * to APPLE2SESSION_KEYSYM_NATIVE_BASE so they stay bindable anyway. */
static const uint8_t win_set1[128] = {
    /* 0x00 */ 0,    0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
    /* 0x08 */ 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,
    /* 0x10 */ 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C,
    /* 0x18 */ 0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16,
    /* 0x20 */ 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33,
    /* 0x28 */ 0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19,
    /* 0x30 */ 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55,
    /* 0x38 */ 0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    /* 0x40 */ 0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F,
    /* 0x48 */ 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,
    /* 0x50 */ 0x5A, 0x5B, 0x62, 0x63, 0x9A, 0,    0x64, 0x44,
    /* 0x58 */ 0x45, 0x67, 0,    0,    0,    0,    0,    0,
    /* 0x60 */ 0,    0,    0,    0,    0x68, 0x69, 0x6A, 0x6B,
    /* 0x68 */ 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0,
    /* 0x70 */ 0x88, 0,    0,    0x87, 0,    0,    0x73, 0x93,
    /* 0x78 */ 0x92, 0x8A, 0,    0x8B, 0,    0x89, 0x85, 0,
};

/* Note what is NOT here: E0 45. Windows reports the Pause key as scancode
 * 0x45 with the extended bit CLEAR, where the plain table above already
 * (correctly) reads 0x45 as Num Lock -- a genuine Win32 ambiguity no
 * scancode table can resolve. frontends/windows/main.c disambiguates it with
 * the VK, which it has and this function does not. E0 46 is Ctrl+Break, the
 * one extended form of Pause. */
static const uint8_t win_set1_e0[128] = {
    /* 0x00 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x08 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x10 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x18 */ 0,    0,    0,    0,    0x58, 0xE4, 0,    0,
    /* 0x20 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x28 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x30 */ 0,    0,    0,    0,    0,    0x54, 0,    0x46,
    /* 0x38 */ 0xE6, 0,    0,    0,    0,    0,    0,    0,
    /* 0x40 */ 0,    0,    0,    0,    0,    0,    0x48, 0x4A,
    /* 0x48 */ 0x52, 0x4B, 0,    0x50, 0,    0x4F, 0,    0x4D,
    /* 0x50 */ 0x51, 0x4E, 0x49, 0x4C, 0,    0,    0,    0,
    /* 0x58 */ 0,    0,    0,    0xE3, 0xE7, 0x65, 0x66, 0,
    /* 0x60 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x68 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x70 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x78 */ 0,    0,    0,    0,    0,    0,    0,    0,
};

uint32_t apple2session_hid_from_win_scancode(unsigned scancode, int extended)
{
    if (scancode >= 128)
        return 0;
    return extended ? win_set1_e0[scancode] : win_set1[scancode];
}

/* ---- Linux: evdev code -> HID usage ---------------------------------------
 * GTK's gdk_key_event_get_keycode() and Qt's QKeyEvent::nativeScanCode()
 * both report the evdev code biased by 8 (an X11 convention Wayland kept);
 * the caller subtracts that before calling here. Codes past the table are
 * real keys (KEY_F13..KEY_F24 and the media block) but not keyboard-page
 * usages, so they fall to the native band. */
static const uint8_t evdev_low[195] = {
    /* 0   */ 0,    0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
    /* 8   */ 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,
    /* 16  */ 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C,
    /* 24  */ 0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16,
    /* 32  */ 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33,
    /* 40  */ 0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19,
    /* 48  */ 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55,
    /* 56  */ 0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    /* 64  */ 0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F,
    /* 72  */ 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,
    /* 80  */ 0x5A, 0x5B, 0x62, 0x63, 0,    0,    0x64, 0x44,
    /* 88  */ 0x45, 0x87, 0x92, 0x93, 0x8A, 0x88, 0x8B, 0x8C,
    /* 96  */ 0x58, 0xE4, 0x54, 0x46, 0xE6, 0,    0x4A, 0x52,
    /* 104 */ 0x4B, 0x50, 0x4F, 0x4D, 0x51, 0x4E, 0x49, 0x4C,
    /* 112 */ 0,    0x7F, 0x81, 0x80, 0x66, 0x67, 0xD7, 0x48,
    /* 120 */ 0,    0x85, 0x90, 0x91, 0x89, 0xE3, 0xE7, 0x65,
    /* 128 */ 0x78, 0x79, 0x76, 0x7A, 0x77, 0x7C, 0x74, 0x7D,
    /* 136 */ 0x7E, 0x7B, 0x75, 0,    0,    0,    0,    0,
    /* 144 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 152 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 160 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 168 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 176 */ 0,    0,    0,    0,    0,    0,    0,    0x68,
    /* 184 */ 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70,
    /* 192 */ 0x71, 0x72, 0x73,
};

uint32_t apple2session_hid_from_evdev(unsigned code)
{
    if (code >= sizeof(evdev_low) / sizeof(evdev_low[0]))
        return 0;
    return evdev_low[code];
}

/* ---- macOS: NSEvent.keyCode -> HID usage -----------------------------------
 * Apple's virtual key set (the kVK_* constants in Carbon's Events.h), which
 * is its own permutation again -- 0x00 is A, not "no key". */
/* 0x34 is deliberately unmapped: some Apple keyboards report Keypad Enter
 * there as well as at the canonical 0x4C, and two codes sharing one usage
 * would make binding one key silently steal the other. On hardware that only
 * has 0x34 the key still binds, through the native band. */
static const uint8_t macos_vk[128] = {
    /* 0x00 */ 0x04, 0x16, 0x07, 0x09, 0x0B, 0x0A, 0x1D, 0x1B,
    /* 0x08 */ 0x06, 0x19, 0x64, 0x05, 0x14, 0x1A, 0x08, 0x15,
    /* 0x10 */ 0x1C, 0x17, 0x1E, 0x1F, 0x20, 0x21, 0x23, 0x22,
    /* 0x18 */ 0x2E, 0x26, 0x24, 0x2D, 0x25, 0x27, 0x30, 0x12,
    /* 0x20 */ 0x18, 0x2F, 0x0C, 0x13, 0x28, 0x0F, 0x0D, 0x34,
    /* 0x28 */ 0x0E, 0x33, 0x31, 0x36, 0x38, 0x11, 0x10, 0x37,
    /* 0x30 */ 0x2B, 0x2C, 0x35, 0x2A, 0,    0x29, 0xE7, 0xE3,
    /* 0x38 */ 0xE1, 0x39, 0xE2, 0xE0, 0xE5, 0xE6, 0xE4, 0,
    /* 0x40 */ 0x6C, 0x63, 0,    0x55, 0,    0x57, 0,    0x53,
    /* 0x48 */ 0x80, 0x81, 0x7F, 0x54, 0x58, 0,    0x56, 0x6D,
    /* 0x50 */ 0x6E, 0x67, 0x62, 0x59, 0x5A, 0x5B, 0x5C, 0x5D,
    /* 0x58 */ 0x5E, 0x5F, 0x6F, 0x60, 0x61, 0x89, 0x87, 0x85,
    /* 0x60 */ 0x3E, 0x3F, 0x40, 0x3C, 0x41, 0x42, 0x91, 0x44,
    /* 0x68 */ 0x90, 0x68, 0x6B, 0x69, 0,    0x43, 0x65, 0x45,
    /* 0x70 */ 0,    0x6A, 0x75, 0x4A, 0x4B, 0x4C, 0x3D, 0x4D,
    /* 0x78 */ 0x3B, 0x4E, 0x3A, 0x50, 0x4F, 0x51, 0x52, 0,
};

uint32_t apple2session_hid_from_macos_keycode(unsigned keycode)
{
    if (keycode >= 128)
        return 0;
    return macos_vk[keycode];
}

/* ---- HID usage -> X11 keysym ------------------------------------------------
 * The session's keysym space is X11's (a GDK keyval), and a key is named by
 * the physical key -- the unshifted US-layout symbol -- so that '1' means
 * the key that prints 1 whatever Shift is doing (Shift is a binding in its
 * own right on some controllers), and Qt's already-shifted key() cannot
 * kill a whole row of keypad keys. Holes map to the HID band below so any
 * key stays bindable, just without a pretty name. */
static const uint16_t hid_to_x11[APPLE2SESSION_HID_USAGE_MAX + 1] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = APPLE2SESSION_KEYSYM_RETURN, [0x29] = APPLE2SESSION_KEYSYM_ESCAPE,
    [0x2A] = APPLE2SESSION_KEYSYM_BACKSPACE, [0x2B] = APPLE2SESSION_KEYSYM_TAB,
    [0x2C] = APPLE2SESSION_KEYSYM_SPACE,
    [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']', [0x31] = '\\',
    [0x32] = '#', [0x33] = ';', [0x34] = '\'', [0x35] = '`', [0x36] = ',',
    [0x37] = '.', [0x38] = '/', [0x39] = 0xffe5 /* Caps_Lock */,
    [0x3A] = APPLE2SESSION_KEYSYM_F1, [0x3B] = APPLE2SESSION_KEYSYM_F2, [0x3C] = APPLE2SESSION_KEYSYM_F3,
    [0x3D] = APPLE2SESSION_KEYSYM_F4, [0x3E] = APPLE2SESSION_KEYSYM_F5, [0x3F] = APPLE2SESSION_KEYSYM_F6,
    [0x40] = APPLE2SESSION_KEYSYM_F7, [0x41] = APPLE2SESSION_KEYSYM_F8, [0x42] = APPLE2SESSION_KEYSYM_F9,
    [0x43] = APPLE2SESSION_KEYSYM_F10, [0x44] = APPLE2SESSION_KEYSYM_F11, [0x45] = APPLE2SESSION_KEYSYM_F12,
    [0x46] = 0xff61 /* Print */, [0x47] = 0xff14 /* Scroll_Lock */,
    [0x48] = 0xff13 /* Pause */, [0x49] = 0xff63 /* Insert */,
    [0x4A] = 0xff50 /* Home */, [0x4B] = 0xff55 /* Page_Up */,
    [0x4C] = 0xffff /* Delete */, [0x4D] = 0xff57 /* End */,
    [0x4E] = 0xff56 /* Page_Down */,
    [0x4F] = APPLE2SESSION_KEYSYM_RIGHT, [0x50] = APPLE2SESSION_KEYSYM_LEFT,
    [0x51] = APPLE2SESSION_KEYSYM_DOWN, [0x52] = APPLE2SESSION_KEYSYM_UP,
    [0x53] = 0xff7f /* Num_Lock */,
    [0x54] = APPLE2SESSION_KEYSYM_KP_DIVIDE, [0x55] = APPLE2SESSION_KEYSYM_KP_MULTIPLY,
    [0x56] = 0xffad /* KP_Subtract */, [0x57] = 0xffab /* KP_Add */,
    [0x58] = APPLE2SESSION_KEYSYM_KP_ENTER,
    [0x59] = APPLE2SESSION_KEYSYM_KP_1, [0x5A] = APPLE2SESSION_KEYSYM_KP_2, [0x5B] = APPLE2SESSION_KEYSYM_KP_3,
    [0x5C] = APPLE2SESSION_KEYSYM_KP_4, [0x5D] = APPLE2SESSION_KEYSYM_KP_5, [0x5E] = APPLE2SESSION_KEYSYM_KP_6,
    [0x5F] = APPLE2SESSION_KEYSYM_KP_7, [0x60] = APPLE2SESSION_KEYSYM_KP_8, [0x61] = APPLE2SESSION_KEYSYM_KP_9,
    [0x62] = APPLE2SESSION_KEYSYM_KP_0, [0x63] = APPLE2SESSION_KEYSYM_KP_PERIOD,
    [0x64] = '<' /* Non-US backslash, ISO keyboards */,
    [0x65] = 0xff67 /* Menu */,
    [0x67] = 0xffbd /* KP_Equal */,
    [0xE0] = APPLE2SESSION_KEYSYM_LCTRL, [0xE1] = APPLE2SESSION_KEYSYM_LSHIFT,
    [0xE2] = APPLE2SESSION_KEYSYM_LALT, [0xE3] = 0xffeb /* Super_L */,
    [0xE4] = APPLE2SESSION_KEYSYM_RCTRL, [0xE5] = APPLE2SESSION_KEYSYM_RSHIFT,
    [0xE6] = APPLE2SESSION_KEYSYM_RALT, [0xE7] = 0xffec /* Super_R */,
};

uint32_t apple2session_keysym_from_hid(uint32_t usage)
{
    if (usage == 0 || usage > APPLE2SESSION_HID_USAGE_MAX)
        return 0;
    if (hid_to_x11[usage])
        return hid_to_x11[usage];
    return APPLE2SESSION_KEYSYM_HID_BASE + usage;
}

uint32_t apple2session_keysym_from_win_scancode(unsigned scancode, int extended)
{
    return apple2session_keysym_from_hid(apple2session_hid_from_win_scancode(scancode, extended));
}

uint32_t apple2session_keysym_from_evdev(unsigned code)
{
    return apple2session_keysym_from_hid(apple2session_hid_from_evdev(code));
}

uint32_t apple2session_keysym_from_macos_keycode(unsigned keycode)
{
    return apple2session_keysym_from_hid(apple2session_hid_from_macos_keycode(keycode));
}

/* ---- names -----------------------------------------------------------------
 * Mirrors jzIntv's event-table vocabulary (the Intellivision sibling's) where it
 * has one, so a key names the same across the family -- but spelt
 * for a status line ("Keypad Enter", not "KP_ENTER"). Holes are NULL; the
 * caller synthesizes "HID 0x.." for those. */
static const char *const hid_names[APPLE2SESSION_HID_USAGE_MAX + 1] = {
    [0x04] = "A", [0x05] = "B", [0x06] = "C", [0x07] = "D",
    [0x08] = "E", [0x09] = "F", [0x0A] = "G", [0x0B] = "H",
    [0x0C] = "I", [0x0D] = "J", [0x0E] = "K", [0x0F] = "L",
    [0x10] = "M", [0x11] = "N", [0x12] = "O", [0x13] = "P",
    [0x14] = "Q", [0x15] = "R", [0x16] = "S", [0x17] = "T",
    [0x18] = "U", [0x19] = "V", [0x1A] = "W", [0x1B] = "X",
    [0x1C] = "Y", [0x1D] = "Z",
    [0x1E] = "1", [0x1F] = "2", [0x20] = "3", [0x21] = "4",
    [0x22] = "5", [0x23] = "6", [0x24] = "7", [0x25] = "8",
    [0x26] = "9", [0x27] = "0",
    [0x28] = "Return",    [0x29] = "Escape",   [0x2A] = "Backspace",
    [0x2B] = "Tab",       [0x2C] = "Space",    [0x2D] = "-",
    [0x2E] = "=",         [0x2F] = "[",        [0x30] = "]",
    [0x31] = "\\",        [0x32] = "Non-US #", [0x33] = ";",
    [0x34] = "'",         [0x35] = "`",        [0x36] = ",",
    [0x37] = ".",         [0x38] = "/",        [0x39] = "Caps Lock",
    [0x3A] = "F1",  [0x3B] = "F2",  [0x3C] = "F3",  [0x3D] = "F4",
    [0x3E] = "F5",  [0x3F] = "F6",  [0x40] = "F7",  [0x41] = "F8",
    [0x42] = "F9",  [0x43] = "F10", [0x44] = "F11", [0x45] = "F12",
    [0x46] = "Print Screen", [0x47] = "Scroll Lock", [0x48] = "Pause",
    [0x49] = "Insert",       [0x4A] = "Home",        [0x4B] = "Page Up",
    [0x4C] = "Delete",       [0x4D] = "End",         [0x4E] = "Page Down",
    [0x4F] = "Right Arrow",  [0x50] = "Left Arrow",
    [0x51] = "Down Arrow",   [0x52] = "Up Arrow",
    [0x53] = "Num Lock",
    [0x54] = "Keypad /", [0x55] = "Keypad *", [0x56] = "Keypad -",
    [0x57] = "Keypad +", [0x58] = "Keypad Enter",
    [0x59] = "Keypad 1", [0x5A] = "Keypad 2", [0x5B] = "Keypad 3",
    [0x5C] = "Keypad 4", [0x5D] = "Keypad 5", [0x5E] = "Keypad 6",
    [0x5F] = "Keypad 7", [0x60] = "Keypad 8", [0x61] = "Keypad 9",
    [0x62] = "Keypad 0", [0x63] = "Keypad .",
    [0x64] = "Non-US \\", [0x65] = "Application", [0x66] = "Power",
    [0x67] = "Keypad =",
    [0x68] = "F13", [0x69] = "F14", [0x6A] = "F15", [0x6B] = "F16",
    [0x6C] = "F17", [0x6D] = "F18", [0x6E] = "F19", [0x6F] = "F20",
    [0x70] = "F21", [0x71] = "F22", [0x72] = "F23", [0x73] = "F24",
    [0x74] = "Execute", [0x75] = "Help",  [0x76] = "Menu",
    [0x77] = "Select",  [0x78] = "Stop",  [0x79] = "Again",
    [0x7A] = "Undo",    [0x7B] = "Cut",   [0x7C] = "Copy",
    [0x7D] = "Paste",   [0x7E] = "Find",  [0x7F] = "Mute",
    [0x80] = "Volume Up", [0x81] = "Volume Down",
    [0x85] = "Keypad ,", [0x86] = "Keypad = (AS/400)",
    [0x87] = "International 1", [0x88] = "International 2",
    [0x89] = "International 3", [0x8A] = "International 4",
    [0x8B] = "International 5", [0x8C] = "International 6",
    [0x8D] = "International 7", [0x8E] = "International 8",
    [0x8F] = "International 9",
    [0x90] = "Lang 1", [0x91] = "Lang 2", [0x92] = "Lang 3",
    [0x93] = "Lang 4", [0x94] = "Lang 5", [0x95] = "Lang 6",
    [0x96] = "Lang 7", [0x97] = "Lang 8", [0x98] = "Lang 9",
    [0x99] = "Alt Erase", [0x9A] = "SysReq", [0x9B] = "Cancel",
    [0x9C] = "Clear",     [0x9D] = "Prior",  [0x9E] = "Return",
    [0x9F] = "Separator", [0xA0] = "Out",    [0xA1] = "Oper",
    [0xA2] = "Clear/Again", [0xA3] = "CrSel", [0xA4] = "ExSel",
    [0xB0] = "Keypad 00",  [0xB1] = "Keypad 000",
    [0xB2] = "Thousands Separator", [0xB3] = "Decimal Separator",
    [0xB4] = "Currency Unit", [0xB5] = "Currency Sub-unit",
    [0xB6] = "Keypad (", [0xB7] = "Keypad )",
    [0xB8] = "Keypad {", [0xB9] = "Keypad }",
    [0xBA] = "Keypad Tab", [0xBB] = "Keypad Backspace",
    [0xBC] = "Keypad A", [0xBD] = "Keypad B", [0xBE] = "Keypad C",
    [0xBF] = "Keypad D", [0xC0] = "Keypad E", [0xC1] = "Keypad F",
    [0xC2] = "Keypad XOR", [0xC3] = "Keypad ^", [0xC4] = "Keypad %",
    [0xC5] = "Keypad <",   [0xC6] = "Keypad >", [0xC7] = "Keypad &",
    [0xC8] = "Keypad &&",  [0xC9] = "Keypad |", [0xCA] = "Keypad ||",
    [0xCB] = "Keypad :",   [0xCC] = "Keypad #", [0xCD] = "Keypad Space",
    [0xCE] = "Keypad @",   [0xCF] = "Keypad !",
    [0xD0] = "Keypad Mem Store",  [0xD1] = "Keypad Mem Recall",
    [0xD2] = "Keypad Mem Clear",  [0xD3] = "Keypad Mem Add",
    [0xD4] = "Keypad Mem Subtract", [0xD5] = "Keypad Mem Multiply",
    [0xD6] = "Keypad Mem Divide", [0xD7] = "Keypad +/-",
    [0xD8] = "Keypad Clear",      [0xD9] = "Keypad Clear Entry",
    [0xDA] = "Keypad Binary", [0xDB] = "Keypad Octal",
    [0xDC] = "Keypad Decimal", [0xDD] = "Keypad Hexadecimal",
    [0xE0] = "Left Ctrl",  [0xE1] = "Left Shift",
    [0xE2] = "Left Alt",   [0xE3] = "Left GUI",
    [0xE4] = "Right Ctrl", [0xE5] = "Right Shift",
    [0xE6] = "Right Alt",  [0xE7] = "Right GUI",
};

const char *apple2_hid_usage_name(uint32_t usage)
{
    if (usage > APPLE2SESSION_HID_USAGE_MAX)
        return NULL;
    return hid_names[usage];
}

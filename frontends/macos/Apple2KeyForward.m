/*
 * Apple2KeyForward -- see Apple2KeyForward.h. NOT RUN-VERIFIED on macOS: the
 * first real compile is the macOS CI job.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "Apple2KeyForward.h"

#define VK_SHIFT_LEFT    56
#define VK_CAPS_LOCK     57
#define VK_CONTROL_LEFT  59
#define VK_SHIFT_RIGHT   60
#define VK_CONTROL_RIGHT 62

uint32_t Apple2KeysymFromEvent(NSEvent *event)
{
    return apple2session_keysym_from_macos_keycode(event.keyCode);
}

/* macOS unichar (charactersIgnoringModifiers) -> X11 keysym, for the keys
 * apple2_key_from_event resolves by keycode rather than by character. */
static uint32_t keysymForChar(unichar chr)
{
    switch (chr) {
    case NSUpArrowFunctionKey:    return 0xFF52;
    case NSDownArrowFunctionKey:  return 0xFF54;
    case NSLeftArrowFunctionKey:  return 0xFF51;
    case NSRightArrowFunctionKey: return 0xFF53;
    case NSDeleteFunctionKey:     return 0xFFFF; /* forward delete */
    case 0x7F:                    return 0xFF08; /* the key marked Delete */
    case 0x0D: case 0x03:         return 0xFF0D; /* Return, Enter */
    case 0x09:                    return 0xFF09;
    case 0x1B:                    return 0xFF1B;
    default:                      return 0;
    }
}

/* What the machine always did with a key nothing bound: the character it
 * produced, or a special key by its keysym. */
static void typeOnMachine(apple2session *session, NSEvent *event, int down)
{
    NSString *chars = event.charactersIgnoringModifiers;
    const unichar chr = chars.length ? [chars characterAtIndex:0] : 0;
    const BOOL ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const BOOL shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;

    uint32_t keysym = keysymForChar(chr);
    uint32_t unicode = 0;
    if (!keysym) {
        /* charactersIgnoringModifiers already drops Ctrl, so this is the
         * plain character the mapping table wants; the function-key range
         * (0xF700+) is not a character at all. */
        if (chr >= 0x20 && chr < 0xF700)
            unicode = chr;
        keysym = unicode;
    }
    if (!keysym)
        return;
    apple2session_key(session, down, keysym, unicode, ctrl ? 1 : 0, shift ? 1 : 0);
}

BOOL Apple2ForwardKeyEvent(apple2session *session, NSEvent *event, int down)
{
    const uint32_t phys = Apple2KeysymFromEvent(event);
    if (down && phys && Apple2ControllersMapKey(phys))
        return YES;
    /* Command shortcuts are the menus'; AppKit sends no keyUp while Command is
     * down, so forwarding one would leave the machine holding the key. */
    if (down && (event.modifierFlags & NSEventModifierFlagCommand))
        return NO;
    const int ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    if (phys && apple2session_input_key(session, phys, down, ctrl))
        return YES;
    typeOnMachine(session, event, down);
    return YES;
}

void Apple2ForwardModifierEvent(apple2session *session, NSEvent *event)
{
    const NSEventModifierFlags flags = event.modifierFlags;
    BOOL down;
    switch (event.keyCode) {
    case VK_SHIFT_LEFT:
    case VK_SHIFT_RIGHT:     down = (flags & NSEventModifierFlagShift) != 0; break;
    case VK_CONTROL_LEFT:
    case VK_CONTROL_RIGHT:   down = (flags & NSEventModifierFlagControl) != 0; break;
    case VK_CAPS_LOCK:       down = (flags & NSEventModifierFlagCapsLock) != 0; break;
    default:                 return;
    }
    const uint32_t phys = Apple2KeysymFromEvent(event);
    if (!phys)
        return;
    if (down && Apple2ControllersMapKey(phys))
        return;
    apple2session_input_key(session, phys, down, 0);
}

/*
 * DisplayExtras.inc -- the keyboard and the aspect modes, included into the
 * display view's @implementation (see DisplayGlue.h).
 *
 * Keys are forwarded press AND release, because the core tracks Open Apple
 * and Closed Apple as held modifiers -- and on a Mac those arrive through
 * flagsChanged:, not keyDown:.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Virtual key codes for the two Option keys. Spelled out rather than pulled
 * from <Carbon/Carbon.h> (kVK_Option / kVK_RightOption): linking the whole
 * Carbon framework for two integers is not a trade worth making, and these
 * are ANSI keyboard positions fixed since the Apple Extended Keyboard. */
#define VK_OPTION_LEFT  0x3A
#define VK_OPTION_RIGHT 0x3D

- (void)extraInit {}

/* The frame clock follows the window (viewDidMoveToWindow); the app asks for
 * it explicitly once the window is ordered in. */
- (void)start { [self updateVisibility]; }

/* 0: the television's 4:3, 1: square pixels, 2: whole-number scale. */
- (void)setAspectMode:(int)mode
{
    atomic_store(&s_fn_aspect_mode, mode);
    atomic_store(&_redraw, 1);
    [self setNeedsDisplay:YES];
}

/* ---- input --------------------------------------------------------------- */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

/* Keys go through the shared path (Apple2KeyForward.m): Map capture, then the
 * bindings (a bound key drives the joystick or a paddle), then what the
 * Apple II always did -- the character typed, or a special key. */
- (void)forwardKey:(NSEvent *)event down:(int)down
{
    Apple2ForwardKeyEvent(_session, event, down);
}

- (void)keyDown:(NSEvent *)event
{
    if (event.isARepeat) {
        /* The core latches a key until the machine reads it; a repeat storm
         * would jam it. Let the Apple II's own repeat handle it. */
        return;
    }
    [self forwardKey:event down:1];
}

- (void)keyUp:(NSEvent *)event
{
    [self forwardKey:event down:0];
}

/* Open Apple and Closed Apple. On a Mac these are the OPTION keys, not the
 * Command keys: Command is how every menu shortcut on the system is typed,
 * and swallowing it would cost the user Cmd-Q, Cmd-W and the rest. Option
 * sits in the same place on the keyboard and is otherwise unused here.
 *
 * Modifiers never produce keyDown:/keyUp: -- they arrive as flagsChanged:
 * with no press/release of their own, so the state is tracked by hand from
 * the per-key device-dependent bits. */
- (void)flagsChanged:(NSEvent *)event
{
    /* Shift, Control and Caps Lock can be bound to a joystick action. */
    Apple2ForwardModifierEvent(_session, event);

    const NSEventModifierFlags flags = event.modifierFlags;
    BOOL open = _openAppleDown, closed = _closedAppleDown;

    /* NSEventModifierFlagOption is set while EITHER Option is down, so which
     * one changed comes from the key code of this event. */
    switch (event.keyCode) {
    case VK_OPTION_LEFT:
        open = (flags & NSEventModifierFlagOption) != 0;
        break;
    case VK_OPTION_RIGHT:
        closed = (flags & NSEventModifierFlagOption) != 0;
        break;
    default:
        break;
    }

    if (open != _openAppleDown) {
        _openAppleDown = open;
        apple2session_key(_session, open ? 1 : 0, 0xFFE9, 0, 0, 0);
    }
    if (closed != _closedAppleDown) {
        _closedAppleDown = closed;
        apple2session_key(_session, closed ? 1 : 0, 0xFFEA, 0, 0, 0);
    }
}

/* Releases go to the key window, so a chord that loses focus mid-press would
 * leave an Apple key stuck down. Lift whatever is held on the way out. */
- (void)resignFirstResponderCleanup
{
    /* a bound key held when the focus left would keep steering */
    apple2session_release_all(_session);
    if (_openAppleDown) {
        _openAppleDown = NO;
        apple2session_key(_session, 0, 0xFFE9, 0, 0, 0);
    }
    if (_closedAppleDown) {
        _closedAppleDown = NO;
        apple2session_key(_session, 0, 0xFFEA, 0, 0, 0);
    }
}

- (BOOL)resignFirstResponder
{
    [self resignFirstResponderCleanup];
    return YES;
}


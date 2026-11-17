/*
 * The Controllers panel (F9 / Cmd+K / View menu): the Apple Joystick IIe and IIc
 * (a Beige and a Platinum scheme) or the two Apple Hand Controllers, the Reset
 * and Power Cycle buttons, an optional Apple II keyboard, the gamepads with the
 * port each one plays, and Map mode for rebinding. Hidden at start; a
 * fixed-width floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "apple2session.h"

/* The keys the app itself owns (F9, F12): called with the keysym of one typed
 * with the panel key; returns YES when it took it. */
typedef BOOL (^Apple2AppKeyHandler)(uint32_t keysym);

@interface Apple2ControllersWindow : NSObject
- (instancetype)initWithSession:(apple2session *)session appKeys:(Apple2AppKeyHandler)appKeys;
/* Runs a console action the panel's buttons fire (APPLE2SESSION_SYSACT_*); the
 * app sets it so the action also shows its toast. Without one the session
 * runs it. */
@property (nonatomic, copy) void (^sysactionHandler)(int sysact);
/* Show, or hide when showing. */
- (void)toggle;
- (void)show;
- (void)hide;
- (BOOL)isVisible;
/* The settings changed elsewhere (Settings): the mode, the model, the pads and
 * the bindings are read again. */
- (void)refresh;
@end

/*
 * Apple2InputPane -- the Input tab of the macOS Settings window: Enable
 * gamepads, the controller plugged into the game port (Joystick or Paddles) and
 * its colour, each gamepad and the port it plays, Reset Controls and Open
 * Controllers. See InputSettings.m.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "apple2session.h"

@interface Apple2InputPane : NSObject
/* controls runs from the buttons (YES: open the Controllers window, NO: the
 * default controls were restored); changed runs after the mode or colour was
 * set (the Controllers window reads it again); restart runs when a setting
 * that needs the session restarted (Enable gamepads) changed. */
- (instancetype)initWithSession:(apple2session *)session
                       controls:(void (^)(BOOL open))controls
                        changed:(void (^)(void))changed
                        restart:(void (^)(void))restart;
@property (nonatomic, readonly) NSView *view;
/* Stops the hot-plug timer (the window closed). */
- (void)stop;
@end

/*
 * The AppKit debugger window over the 6502 debugger engine (apple2debug.h).
 * NOT COMPILED OR RUN HERE (CI only).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "apple2session.h"

@interface DebuggerWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate>
/* Shows (creating on first use) the debugger for the session; attaching the
 * engine stops the machine, as the other frontends do. */
+ (void)showForSession:(apple2session *)session;
/* F12: shows it, or closes it (detaching, so the machine runs on). */
+ (void)toggleForSession:(apple2session *)session;
@end

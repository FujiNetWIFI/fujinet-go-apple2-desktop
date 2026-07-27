/*
 * AppDelegate: application lifecycle for the macOS frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "apple2session.h"

@interface AppDelegate : NSObject <NSApplicationDelegate>

- (instancetype)initWithSession:(apple2session *)session;

@end

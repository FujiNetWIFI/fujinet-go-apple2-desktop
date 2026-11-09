/*
 * The debugger's Video tab (AppKit): a thin view over core/include/apple2vidtab.h.
 * NOT COMPILED OR RUN HERE (CI only).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "apple2session.h"

@interface Apple2VideoTab : NSObject

/* The tab's content view; this object owns the controller, which is freed with it. */
@property (nonatomic, readonly) NSView *view;

/* Show in Memory: bank (APPLE2_BANK_MAIN or APPLE2_BANK_AUX) and the address in it. */
@property (nonatomic, copy) void (^showInMemory)(int bank, int addr);

- (instancetype)initWithSession:(apple2session *)session;
/* Take the machine's state and redraw what changed (force: everything). Cheap to call often. */
- (void)refreshForce:(BOOL)force;
/* APPLE2_DEBUGGER_VIEW: 0..5 = text1, text2, lores1, lores2, hires1, hires2 (Decoded, Follow off). */
- (void)selectLegacyPage:(int)index;
/* APPLE2_VIDEO_PIN: a point in zoomed picture pixels. */
- (void)pinAtX:(int)x y:(int)y;

@end

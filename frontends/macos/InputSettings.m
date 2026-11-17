/*
 * Apple2InputPane -- see InputSettings.h. Enable gamepads applies when the
 * Settings window closes (the pad thread starts or stops with the session); the
 * controller, its colour and the port a gamepad plays apply at once.
 * NOT RUN-VERIFIED on macOS: the first real compile is the macOS CI job.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "InputSettings.h"

#import "help/HelpWindow.h"

#include "bindings.h"

#define PANE_W 560.0
#define PANE_H 330.0

static NSTextField *label(NSString *text, NSRect frame)
{
    NSTextField *field = [NSTextField labelWithString:text];
    field.frame = frame;
    return field;
}

@implementation Apple2InputPane {
    apple2session *_session;
    void (^_controls)(BOOL);
    void (^_changed)(void);
    void (^_restart)(void);
    NSView *_view;
    NSPopUpButton *_modePopup;
    NSPopUpButton *_modelPopup;
    NSPopUpButton *_padList;
    NSPopUpButton *_padPort;
    NSTimer *_padTimer;
    unsigned _padGeneration;
}

@synthesize view = _view;

- (instancetype)initWithSession:(apple2session *)session
                       controls:(void (^)(BOOL open))controls
                        changed:(void (^)(void))changed
                        restart:(void (^)(void))restart
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _controls = [controls copy];
    _changed = [changed copy];
    _restart = [restart copy];
    [self build];
    [self refreshPadList];
    [self startPadTimer];
    return self;
}

- (void)dealloc
{
    [_padTimer invalidate];
}

- (void)stop
{
    [_padTimer invalidate];
    _padTimer = nil;
}

/* ---- the gamepads ----------------------------------------------------------------- */

/* The ports a gamepad can be assigned to in the current mode. */
- (NSArray<NSString *> *)portTitles
{
    return apple2session_controller_ports(_session) > 1
        ? @[@"Automatic", @"Paddle 0", @"Paddle 1"]
        : @[@"Automatic", @"Joystick"];
}

/* The connected gamepads and the port the chosen one is set to. */
- (void)refreshPadList
{
    const NSInteger keep = _padList.indexOfSelectedItem;
    const int count = apple2session_gamepad_count(_session);
    _padGeneration = apple2session_gamepad_generation(_session);
    [_padList removeAllItems];
    for (int idx = 0; idx < count; idx++) {
        char name[128];
        name[0] = '\0';
        apple2session_gamepad_name(_session, idx, name, (int)sizeof name);
        [[_padList menu] addItemWithTitle:[NSString stringWithUTF8String:name] ?: @"Gamepad" action:nil keyEquivalent:@""];
    }
    if (count == 0)
        [[_padList menu] addItemWithTitle:@"No gamepads connected" action:nil keyEquivalent:@""];
    _padList.enabled = count > 0;
    _padPort.enabled = count > 0;
    [_padList selectItemAtIndex:keep >= 0 && keep < count ? keep : 0];
    [self padSelected:_padList];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    [_padPort removeAllItems];
    [_padPort addItemsWithTitles:[self portTitles]];
    int assign = -1;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < apple2session_gamepad_count(_session))
        assign = apple2session_gamepad_assignment(_session, (int)sel);
    const int ports = apple2session_controller_ports(_session);
    [_padPort selectItemAtIndex:assign < 0 || assign >= ports ? 0 : assign + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < apple2session_gamepad_count(_session))
        apple2session_gamepad_assign(_session, (int)sel, (int)_padPort.indexOfSelectedItem - 1);
}

/* Hot-plugged gamepads, twice a second, while the window is open. */
- (void)startPadTimer
{
    __weak Apple2InputPane *weakSelf = self;
    _padTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *tmr) {
        (void)tmr;
        Apple2InputPane *strong = weakSelf;
        if (strong && apple2session_gamepad_generation(strong->_session) != strong->_padGeneration)
            [strong refreshPadList];
    }];
    _padTimer.tolerance = 0.25;
}

/* ---- the settings ------------------------------------------------------------------- */

/* Enable gamepads: the pad thread starts or stops with the session. */
- (void)padsToggled:(NSButton *)box
{
    const int on = [box state] == NSControlStateValueOn ? 1 : 0;
    if (apple2session_get_int(_session, "enable_gamepad", 1) != on) {
        apple2session_set_int(_session, "enable_gamepad", on);
        if (_restart) _restart();
    }
}

- (void)modeChanged:(NSPopUpButton *)sender
{
    apple2session_keyboard_release(_session);
    apple2session_pointer_release(_session);
    apple2session_set_controller_mode(_session,
        sender.indexOfSelectedItem == 1 ? APPLE2_MODE_PADDLES : APPLE2_MODE_JOYSTICK);
    [self padSelected:_padList];     /* the ports it offers changed */
    if (_changed) _changed();
}

- (void)modelChanged:(NSPopUpButton *)sender
{
    apple2session_set_controller_model(_session,
        sender.indexOfSelectedItem == 1 ? APPLE2_MODEL_IIC : APPLE2_MODEL_IIE);
    if (_changed) _changed();
}

- (void)resetControls:(id)sender
{
    (void)sender;
    bindings_reset_defaults(_session);
    if (_controls) _controls(NO);
}

- (void)openControllers:(id)sender
{
    (void)sender;
    if (_controls) _controls(YES);
}

- (void)build
{
    _view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, PANE_W, PANE_H)];
    const CGFloat top = PANE_H;

    NSButton *enable = [NSButton checkboxWithTitle:@"Enable gamepads" target:self action:@selector(padsToggled:)];
    enable.state = apple2session_get_int(_session, "enable_gamepad", 1) ? NSControlStateValueOn : NSControlStateValueOff;
    enable.frame = NSMakeRect(16, top - 36, 220, 22);
    [_view addSubview:enable];

    [_view addSubview:label(@"Controller", NSMakeRect(16, top - 74, 120, 20))];
    _modePopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(140, top - 78, 150, 26) pullsDown:NO];
    [_modePopup addItemsWithTitles:@[@"Joystick", @"Paddles"]];
    [_modePopup selectItemAtIndex:apple2session_controller_mode(_session) == APPLE2_MODE_PADDLES ? 1 : 0];
    _modePopup.target = self;
    _modePopup.action = @selector(modeChanged:);
    [_view addSubview:_modePopup];
    _modelPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(298, top - 78, 150, 26) pullsDown:NO];
    [_modelPopup addItemsWithTitles:@[@"Beige (IIe)", @"Platinum (IIc)"]];
    [_modelPopup selectItemAtIndex:apple2session_controller_model(_session) == APPLE2_MODEL_IIC ? 1 : 0];
    _modelPopup.target = self;
    _modelPopup.action = @selector(modelChanged:);
    [_view addSubview:_modelPopup];

    [_view addSubview:label(@"Gamepad", NSMakeRect(16, top - 114, 120, 20))];
    _padList = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(140, top - 118, 230, 26) pullsDown:NO];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    [_view addSubview:_padList];
    _padPort = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(378, top - 118, 124, 26) pullsDown:NO];
    _padPort.target = self;
    _padPort.action = @selector(padPortChanged:);
    [_view addSubview:_padPort];

    NSButton *reset = [NSButton buttonWithTitle:@"Reset Controls" target:self action:@selector(resetControls:)];
    reset.frame = NSMakeRect(16, top - 168, 130, 28);
    [_view addSubview:reset];
    NSButton *open = [NSButton buttonWithTitle:@"Open Controllers" target:self action:@selector(openControllers:)];
    open.frame = NSMakeRect(156, top - 168, 150, 28);
    [_view addSubview:open];

    NSTextField *note = [NSTextField wrappingLabelWithString:
        @"The controller and its colour and the port a gamepad plays apply at once. Keys and gamepad "
        @"buttons are re-mapped in the Controllers window (View > Controllers, Cmd+K or F9): click Map, "
        @"choose a control, then press the key or the gamepad button. Reset Controls puts the defaults "
        @"back. Turning gamepads on or off applies when this window is closed."];
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;
    note.frame = NSMakeRect(16, top - 260, PANE_W - 32, 80);
    [_view addSubview:note];

    /* AppKit resizes a tab's view to the tab: everything stays at the top,
     * except the Help button at the bottom */
    for (NSView *sub in [_view subviews])
        [sub setAutoresizingMask:NSViewMinYMargin];
    NSButton *help = Apple2HelpButton(@"prefs/Input");
    [help setFrameOrigin:NSMakePoint(16, 12)];
    [_view addSubview:help];
}
@end

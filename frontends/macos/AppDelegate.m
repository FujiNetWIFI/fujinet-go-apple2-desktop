/*
 * AppDelegate: builds the menu bar and main window, owns the session
 * lifecycle, and hosts the FujiNet configuration (WKWebView) and console log
 * windows. Mirrors the GTK/Qt/Win32 frontends over the same apple2session
 * API, with the native debugger window in debugger/DebuggerWindow.m.
 *
 * The machine and slot settings are the libretro core's own option LABELS
 * (strings), not indices -- so the Settings popups are generated from
 * apple2session_machine_name() and friends, and a card added upstream shows
 * up here with no edit.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <WebKit/WebKit.h>

#import "DisplayView.h"
#import "debugger/DebuggerWindow.h"

#include <stdlib.h>
#include <string.h>

/* The version shown by orderFrontStandardAboutPanel: comes from the bundle's
 * CFBundleShortVersionString, which Info.plist.in fills in from
 * PROJECT_VERSION -- so nothing here repeats it. */

static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t)
            [types addObject:t];
    }
    return types;
}

/* The machine-describing settings, in the order they appear in the Settings
 * window. Each is a libretro core option read when the core loads, so
 * changing one needs the session restarted -- deferred until the window
 * closes, so editing several does not reboot the machine under the user.
 * A popup's tag is its index here. */
typedef struct {
    const char *label;
    const char *key;
    const char *(*name_at)(int);
    const char *def;
} option_row;

static const option_row kOptionRows[] = {
    {"Model",   "machine",    apple2session_machine_name,    NULL},
    {"Video",   "video_mode", apple2session_video_mode_name, NULL},
    {"Slot 3",  "slot3",      apple2session_slot3_name,      "Empty"},
    {"Slot 4",  "slot4",      apple2session_slot4_name,      "Mockingboard"},
    {"Slot 5",  "slot5",      apple2session_slot5_name,      "Empty"},
    {"Slot 6",  "slot6",      apple2session_slot6_name,      "Disk II"},
    {"Slot 7",  "slot7",      apple2session_slot7_name,      "FujiNet"},
};
#define OPTION_ROW_COUNT ((int)(sizeof(kOptionRows) / sizeof(kOptionRows[0])))

@interface AppDelegate () <NSWindowDelegate>
@end

@implementation AppDelegate {
    apple2session *_session;
    NSWindow *_window;
    DisplayView *_display;
    NSWindow *_configWindow;
    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
    NSWindow *_settingsWindow;
    BOOL _machineDirty;
}

- (instancetype)initWithSession:(apple2session *)session
{
    self = [super init];
    if (self)
        _session = session;
    return self;
}

/* ---- session helpers ----------------------------------------------------- */

- (void)restartSession
{
    apple2session_stop(_session);
    apple2session_start_opts opts;
    apple2session_default_opts(_session, &opts);
    if (apple2session_start(_session, &opts) != 0)
        NSLog(@"session restart: %s", apple2session_last_error(_session));
}

- (void)applyDisplaySettings
{
    [_display setAspectMode:apple2session_get_int(_session, "aspect_mode", 0)];
    [_display setSmooth:apple2session_get_int(_session, "smooth_scaling", 0)];
}

/* ---- actions ------------------------------------------------------------- */

- (void)resetMachine:(id)sender
{
    (void)sender;
    apple2session_reset(_session, 0);
}

- (void)powerCycle:(id)sender
{
    (void)sender;
    apple2session_reset(_session, 1);
}

- (void)importDiskImage:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Import Disk Image";
    panel.allowedContentTypes = typesForExtensions(
        @[ @"dsk", @"do", @"po", @"2mg", @"nib", @"woz", @"hdv" ]);
    if ([panel runModal] != NSModalResponseOK || !panel.URL)
        return;

    char dest[1024];
    if (apple2session_import_media(_session, panel.URL.path.UTF8String, dest,
                                   sizeof(dest)) != 0) {
        [self alert:@"Import failed"
               info:@(apple2session_last_error(_session))];
        return;
    }
    [self alert:@"Disk image imported"
           info:@"Copied to the FujiNet SD folder. Mount it from "
                @"FujiNet ▸ Configuration."];
}

- (void)importSystemRoms:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Import System ROMs";
    panel.allowsMultipleSelection = YES;
    panel.allowedContentTypes = typesForExtensions(@[ @"rom", @"bin" ]);
    if ([panel runModal] != NSModalResponseOK)
        return;

    /* AppleWin looks ROMs up by exact filename, so whatever is picked keeps
     * its name. Only a WITH_APPLE_ROMS=OFF build reads them. */
    int ok = 0;
    for (NSURL *url in panel.URLs) {
        char dest[1024];
        if (apple2session_import_rom(_session, url.path.UTF8String, dest,
                                     sizeof(dest)) == 0)
            ok++;
    }
    if (ok == 0) {
        [self alert:@"Import failed"
               info:@(apple2session_last_error(_session))];
        return;
    }
    [self restartSession];
    [self alert:@"System ROMs imported"
           info:[NSString stringWithFormat:@"Imported %d ROM file%s. "
                                           @"The session has restarted.",
                                           ok, ok == 1 ? "" : "s"]];
}

- (void)alert:(NSString *)message info:(NSString *)info
{
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = message;
    if (info)
        alert.informativeText = info;
    [alert runModal];
}

- (void)showFujiNetConfig:(id)sender
{
    (void)sender;
    if (_configWindow) {
        [_configWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 1000, 760);
    /* Miniaturizable so Window ▸ Minimize (and the yellow button) apply to
     * the auxiliary windows too, not just the machine window. */
    _configWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _configWindow.title = @"FujiNet Configuration";
    _configWindow.releasedWhenClosed = NO;
    WKWebView *web = [[WKWebView alloc] initWithFrame:frame];
    web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSString *url = [NSString
        stringWithUTF8String:apple2session_fujinet_webui_url(_session)];
    [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
    _configWindow.contentView = web;
    [_configWindow center];
    [_configWindow makeKeyAndOrderFront:nil];
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 820, 560);
    _logWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _logView = [[NSTextView alloc] initWithFrame:frame];
    _logView.editable = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    scroll.documentView = _logView;
    _logWindow.contentView = scroll;

    _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                repeats:YES
                                                  block:^(NSTimer *timer) {
                                                    (void)timer;
                                                    [self refreshLog];
                                                  }];
    [self refreshLog];
    [_logWindow center];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)refreshLog
{
    static char buf[128 * 1024];
    int n = apple2session_fujinet_copy_log(_session, buf, sizeof(buf));
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf]
                           : @"(no FujiNet output yet)";
    if (text)
        _logView.string = text;
    [_logView scrollToEndOfDocument:nil];
}

- (void)showDebugger:(id)sender
{
    (void)sender;
    [DebuggerWindow showForSession:_session];
}

/* ---- settings ------------------------------------------------------------
 * Rows write straight through to the shared settings store, the way the GNOME
 * preferences window does -- same keys, same defaults, same option order as
 * the GTK, Qt and Win32 frontends, so a machine configured in one shows up
 * the same in the others. */

- (NSTextField *)sectionLabel:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

/* A popup over one core option. Its tag is the kOptionRows index, which is
 * how the action finds the key and the name table again. */
- (NSPopUpButton *)popUpForOptionRow:(int)rowIndex
{
    const option_row *row = &kOptionRows[rowIndex];
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    const char *def = row->def ? row->def : row->name_at(0);
    const char *current = apple2session_get_str(_session, row->key, def);

    for (int i = 0;; i++) {
        const char *name = row->name_at(i);
        if (!name)
            break;
        [popup addItemWithTitle:@(name)];
        if (current && strcmp(name, current) == 0)
            [popup selectItemAtIndex:i];
    }
    popup.tag = rowIndex;
    popup.target = self;
    popup.action = @selector(optionChanged:);
    return popup;
}

- (void)optionChanged:(id)sender
{
    NSPopUpButton *popup = sender;
    const option_row *row = &kOptionRows[popup.tag];
    const char *name = row->name_at((int)popup.indexOfSelectedItem);
    if (!name)
        return;
    if (strcmp(name, apple2session_get_str(_session, row->key, "")) == 0)
        return;
    apple2session_set_str(_session, row->key, name);
    _machineDirty = YES;
}

- (NSPopUpButton *)popUpForIntKey:(const char *)key
                         fallback:(int)def
                            items:(NSArray<NSString *> *)items
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    [popup addItemsWithTitles:items];
    NSInteger current = apple2session_get_int(_session, key, def);
    if (current < 0 || current >= (NSInteger)items.count)
        current = def;
    [popup selectItemAtIndex:current];
    popup.identifier = @(key);
    popup.target = self;
    popup.action = @selector(displaySettingChanged:);
    return popup;
}

- (NSButton *)checkBoxForKey:(const char *)key fallback:(int)def
{
    NSButton *box =
        [NSButton checkboxWithTitle:@""
                             target:self
                             action:@selector(displaySettingChanged:)];
    box.state = apple2session_get_int(_session, key, def)
                    ? NSControlStateValueOn
                    : NSControlStateValueOff;
    box.identifier = @(key);
    return box;
}

/* Scanlines is a libretro core option (like Model/Video/slots above), so it
 * needs a restart -- unlike the rest of Display, which applies live. */
- (NSButton *)checkBoxForScanlines
{
    NSButton *box =
        [NSButton checkboxWithTitle:@""
                             target:self
                             action:@selector(scanlinesChanged:)];
    box.state = apple2session_get_int(_session, "scanlines", 1)
                    ? NSControlStateValueOn
                    : NSControlStateValueOff;
    return box;
}

- (void)scanlinesChanged:(id)sender
{
    NSButton *box = sender;
    int on = box.state == NSControlStateValueOn ? 1 : 0;
    apple2session_set_int(_session, "scanlines", on);
    /* Smoothing is no longer its own control; it just follows scanlines. */
    apple2session_set_int(_session, "smooth_scaling", on);
    _machineDirty = YES;
    [self applyDisplaySettings];
}

/* The display options, unlike the machine ones, apply live. */
- (void)displaySettingChanged:(id)sender
{
    NSControl *control = sender;
    int value;

    if ([control isKindOfClass:[NSPopUpButton class]])
        value = (int)((NSPopUpButton *)control).indexOfSelectedItem;
    else
        value = ((NSButton *)control).state == NSControlStateValueOn ? 1 : 0;

    apple2session_set_int(_session, control.identifier.UTF8String, value);
    [self applyDisplaySettings];
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    [rows addObject:@[
        [self sectionLabel:@"Machine"], [NSTextField labelWithString:@""]
    ]];
    for (int i = 0; i < OPTION_ROW_COUNT; i++) {
        if (i == 2) /* the slots start here */
            [rows addObject:@[
                [self sectionLabel:@"Slots"], [NSTextField labelWithString:@""]
            ]];
        [rows addObject:@[
            [NSTextField labelWithString:@(kOptionRows[i].label)],
            [self popUpForOptionRow:i]
        ]];
    }
    [rows addObject:@[
        [self sectionLabel:@"Display"], [NSTextField labelWithString:@""]
    ]];
    [rows addObject:@[
        [NSTextField labelWithString:@"Aspect ratio"],
        [self popUpForIntKey:"aspect_mode"
                    fallback:0
                       items:@[
                           @"TV (4:3)", @"Square pixels", @"Integer scale"
                       ]]
    ]];
    [rows addObject:@[
        [NSTextField labelWithString:@"Scanlines"], [self checkBoxForScanlines]
    ]];

    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSTextField *note = [NSTextField
        labelWithString:@"FujiNet lives in slot 7, the bootable SmartPort "
                        @"slot the //e scans before the Disk ][ in slot 6.\n"
                        @"Aspect ratio applies immediately. Machine options "
                        @"and Scanlines take effect when this window is "
                        @"closed."];
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;

    NSStackView *root = [NSStackView stackViewWithViews:@[ grid, note ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 12;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 560, 480)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = root;
    [_settingsWindow center];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* Machine options are collected while the window is open and applied in one
 * restart when it closes. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _settingsWindow || !_machineDirty)
        return;
    _machineDirty = NO;
    [self restartSession];
}

/* ---- menus --------------------------------------------------------------- */

- (NSMenuItem *)item:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                 action:sel
                                          keyEquivalent:key];
    item.target = self;
    return item;
}

- (void)buildMenus
{
    NSString *appName = @"FujiNet Go Apple II";
    NSMenu *menubar = [[NSMenu alloc] init];

    /* A menu bar built in code gets nothing for free: the standard items
     * every Mac app is expected to have (Services, Hide/Hide Others/Show
     * All, Minimize/Zoom, the window list) exist only if they are added
     * here -- and Services and the window list stay empty until AppKit is
     * told which menus they belong to (servicesMenu / windowsMenu). */
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:appName]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItem:[self item:@"Settings…"
                         action:@selector(showSettings:)
                            key:@","]];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSMenuItem *servicesItem = [appMenu addItemWithTitle:@"Services"
                                                  action:NULL
                                           keyEquivalent:@""];
    servicesItem.submenu = servicesMenu;
    NSApp.servicesMenu = servicesMenu;
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    NSMenuItem *hideOthers =
        [appMenu addItemWithTitle:@"Hide Others"
                           action:@selector(hideOtherApplications:)
                    keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask =
        NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [menubar addItem:appItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [machine addItem:[self item:@"Reset"
                         action:@selector(resetMachine:)
                            key:@""]];
    [machine addItem:[self item:@"Power Cycle"
                         action:@selector(powerCycle:)
                            key:@""]];
    machineItem.submenu = machine;
    [menubar addItem:machineItem];

    NSMenuItem *mediaItem = [[NSMenuItem alloc] init];
    NSMenu *media = [[NSMenu alloc] initWithTitle:@"Media"];
    [media addItem:[self item:@"Import Disk Image…"
                       action:@selector(importDiskImage:)
                          key:@"i"]];
    [media addItem:[self item:@"Import System ROMs…"
                       action:@selector(importSystemRoms:)
                          key:@""]];
    mediaItem.submenu = media;
    [menubar addItem:mediaItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [fuji addItem:[self item:@"Configuration…"
                      action:@selector(showFujiNetConfig:)
                         key:@""]];
    [fuji addItem:[self item:@"Console Log…"
                      action:@selector(showFujiNetLog:)
                         key:@""]];
    fujiItem.submenu = fuji;
    [menubar addItem:fujiItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *fs = [view addItemWithTitle:@"Toggle Full Screen"
                                     action:@selector(toggleFullScreen:)
                              keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagCommand;
    NSMenuItem *dbg = [self item:@"Debugger"
                          action:@selector(showDebugger:)
                             key:[NSString stringWithFormat:@"%C",
                                                            (unichar)
                                                                NSF12FunctionKey]];
    dbg.keyEquivalentModifierMask = 0;
    [view addItem:dbg];
    viewItem.submenu = view;
    [menubar addItem:viewItem];

    /* Window menu. AppKit appends the list of open windows (the machine
     * window, the debugger, the FujiNet console log) to whichever menu is
     * handed to windowsMenu, and manages their check marks -- that list is
     * what "show or hide windows" means to a Mac user. */
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    /* Close lives here rather than in a File menu: there is no File menu,
     * and the debugger / config / log windows need a keyboard close. */
    [windowMenu addItemWithTitle:@"Close"
                          action:@selector(performClose:)
                   keyEquivalent:@"w"];
    [windowMenu addItemWithTitle:@"Minimize"
                          action:@selector(performMiniaturize:)
                   keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"Zoom"
                          action:@selector(performZoom:)
                   keyEquivalent:@""];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [windowMenu addItemWithTitle:@"Bring All to Front"
                          action:@selector(arrangeInFront:)
                   keyEquivalent:@""];
    windowItem.submenu = windowMenu;
    [menubar addItem:windowItem];
    NSApp.windowsMenu = windowMenu;

    NSApp.mainMenu = menubar;
}

/* ---- lifecycle ----------------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    [self buildMenus];

    NSRect frame = NSMakeRect(0, 0, 1120, 900);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"FujiNet Go Apple II";
    _window.contentMinSize = NSMakeSize(APPLE2SESSION_FB_MAX_WIDTH / 2,
                                        APPLE2SESSION_FB_MAX_HEIGHT / 2);

    _display = [[DisplayView alloc] initWithSession:_session];
    _display.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _display.frame = ((NSView *)_window.contentView).bounds;
    [_window.contentView addSubview:_display];
    [self applyDisplaySettings];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_display];
    [_display start];

    /* Developer affordance shared with the other frontends. */
    if (getenv("APPLE2_OPEN_DEBUGGER"))
        [DebuggerWindow showForSession:_session];
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_logTimer invalidate];
    [_display stop];
    apple2session_stop(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    (void)sender;
    return YES;
}

@end

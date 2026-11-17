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
#import "InputSettings.h"
#import "controllers/ControllersWindow.h"
#import "help/HelpWindow.h"
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

#define TOAST_SECONDS 2.5

@interface AppDelegate () <NSWindowDelegate, WKUIDelegate, NSTabViewDelegate>
@end

@implementation AppDelegate {
    apple2session *_session;
    NSWindow *_window;
    DisplayView *_display;
    id<NSObject> _activity;          /* no App Nap, no timer coalescing while we run */
    NSWindow *_configWindow;
    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
    NSWindow *_settingsWindow;
    BOOL _machineDirty;
    NSTextField *_externalField;
    Apple2ControllersWindow *_controllers;
    Apple2InputPane *_inputPane;     /* the Settings window's Input tab */
    NSTextField *_toast;             /* over the picture, fades */
    unsigned _toastGeneration;       /* the pad generation already toasted */
    NSUInteger _toastSerial;
    NSTimer *_sysactTimer;           /* console actions a gamepad or key posted, pad toasts */
    id _keyMonitor;                  /* F9 in the machine window */
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
    [self runSysaction:APPLE2SESSION_SYSACT_RESET];
}

- (void)powerCycle:(id)sender
{
    (void)sender;
    [self runSysaction:APPLE2SESSION_SYSACT_POWER];
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
    if (apple2session_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Use the web UI of the FujiNet instance "
                                @"that connects to this app.";
        [alert runModal];
        return;
    }
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
    web.UIDelegate = self;
    NSString *url = [NSString
        stringWithUTF8String:apple2session_fujinet_webui_url(_session)];
    [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
    _configWindow.contentView = web;
    [_configWindow center];
    [_configWindow makeKeyAndOrderFront:nil];
}

/* The FujiNet web UI's OneDrive/Google Drive "Authorize" buttons open the
 * provider's consent page via window.open(). WKWebView has no popup window
 * of its own to show it in unless a UIDelegate supplies one, so hand the URL
 * to the system browser instead: Google and Microsoft both reject OAuth
 * flows from an embedded webview's user-agent anyway. */
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures
{
    (void)webView;
    (void)configuration;
    (void)windowFeatures;
    if (navigationAction.request.URL)
        [[NSWorkspace sharedWorkspace] openURL:navigationAction.request.URL];
    return nil;
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (apple2session_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Its console output is in the process "
                                @"that is running it.";
        [alert runModal];
        return;
    }
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

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 36, frame.size.width, frame.size.height - 36)];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _logView = [[NSTextView alloc] initWithFrame:frame];
    _logView.editable = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    scroll.documentView = _logView;
    {
        /* the log over a strip with the Help button */
        NSView *box = [[NSView alloc] initWithFrame:frame];
        NSButton *help = Apple2HelpButton(@"fujilog");
        [help setFrameOrigin:NSMakePoint(10, 6)];
        help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        [box addSubview:scroll];
        [box addSubview:help];
        _logWindow.contentView = box;
    }
    Apple2SetHelpContext(_logWindow, @"fujilog");

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
    NSScrollView *scroll = _logView.enclosingScrollView;
    const NSPoint origin = scroll.contentView.bounds.origin;
    const BOOL atEnd =
        !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                    NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    /* Unchanged: leave the view alone (keeps the scroll position and any
     * selection). Follow the tail only while the reader is at the bottom. */
    if (!text || [text isEqualToString:_logView.string])
        return;
    _logView.string = text;
    if (atEnd) {
        [_logView scrollToEndOfDocument:nil];
    } else {
        [scroll.contentView scrollToPoint:origin];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

- (void)showDebugger:(id)sender
{
    (void)sender;
    [DebuggerWindow toggleForSession:_session];
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

/* The Use external FujiNet switch: the address field is live only with it on. */
- (void)externalChanged:(id)sender
{
    NSButton *box = sender;
    int on = box.state == NSControlStateValueOn ? 1 : 0;
    apple2session_set_int(_session, "fujinet_external", on);
    _externalField.enabled = on != 0;
    _machineDirty = YES;
}

/* The external-FujiNet address field: valid text (or none, meaning
 * 127.0.0.1:1985) is saved; anything else is shown in red and not saved. */
- (void)externalAddrChanged:(id)sender
{
    NSTextField *field = sender;
    NSString *text = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (apple2session_parse_hostport(text.UTF8String, host, sizeof host,
                                     &port) != 0) {
        field.textColor = NSColor.systemRedColor;
        return;
    }
    field.textColor = NSColor.controlTextColor;
    apple2session_set_str(_session, "fujinet_external_addr", text.UTF8String);
    _machineDirty = YES;
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
    _externalField = [NSTextField textFieldWithString:@(
        apple2session_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"0.0.0.0:1985";
    _externalField.preferredMaxLayoutWidth = 240;
    _externalField.enabled =
        apple2session_get_int(_session, "fujinet_external", 0) != 0;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];
    NSButton *externalBox =
        [NSButton checkboxWithTitle:@""
                             target:self
                             action:@selector(externalChanged:)];
    externalBox.state = apple2session_get_int(_session, "fujinet_external", 0)
                            ? NSControlStateValueOn
                            : NSControlStateValueOff;
    [rows addObject:@[
        [self sectionLabel:@"FujiNet"], [NSTextField labelWithString:@""]
    ]];
    [rows addObject:@[
        [NSTextField labelWithString:@"Use external FujiNet"], externalBox
    ]];
    [rows addObject:@[
        [NSTextField labelWithString:@"Listen address:port"], _externalField
    ]];
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
                        @"An external FujiNet connects to the address above "
                        @"(0.0.0.0 accepts the network).\n"
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
        initWithContentRect:NSMakeRect(0, 0, 660, 580)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    {
        /* two tabs: the machine's options over a strip with the Help button,
         * and Input (gamepads and the controller) */
        NSButton *helpBtn = Apple2HelpButton(@"prefs/Machine");
        NSStackView *wrap = [NSStackView stackViewWithViews:@[root, helpBtn]];
        wrap.orientation = NSUserInterfaceLayoutOrientationVertical;
        wrap.alignment = NSLayoutAttributeLeading;
        wrap.edgeInsets = NSEdgeInsetsMake(0, 16, 12, 16);
        const NSSize fit = [wrap fittingSize];
        const CGFloat machineH = fit.height > 330 ? fit.height : 330;
        NSView *machineView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 640, machineH)];
        [wrap setFrame:NSMakeRect(0, machineH - fit.height, fit.width, fit.height)];
        /* AppKit resizes a tab's view to the tab; the options stay at the top */
        [wrap setAutoresizingMask:NSViewMinYMargin];
        [machineView addSubview:wrap];

        __weak AppDelegate *weakSelf = self;
        [_inputPane stop];
        _inputPane = [[Apple2InputPane alloc]
            initWithSession:_session
                   controls:^(BOOL open) {
                       AppDelegate *strong = weakSelf;
                       if (!strong) return;
                       if (open) {
                           [strong->_controllers show];
                       } else {
                           [strong->_controllers refresh];
                           [strong showToast:@"Default controls restored"];
                       }
                   }
                    changed:^{
                        AppDelegate *strong = weakSelf;
                        if (strong) [strong->_controllers refresh];
                    }
                    restart:^{
                        AppDelegate *strong = weakSelf;
                        if (strong) strong->_machineDirty = YES;
                    }];

        NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, 660, 580)];
        tabs.delegate = self;
        NSTabViewItem *machineItem = [[NSTabViewItem alloc] initWithIdentifier:@"machine"];
        machineItem.label = @"Machine";
        machineItem.view = machineView;
        [tabs addTabViewItem:machineItem];
        NSTabViewItem *inputItem = [[NSTabViewItem alloc] initWithIdentifier:@"input"];
        inputItem.label = @"Input";
        inputItem.view = _inputPane.view;
        [tabs addTabViewItem:inputItem];
        _settingsWindow.contentView = tabs;
        Apple2SetHelpContext(_settingsWindow, @"prefs/Machine");
    }
    [_settingsWindow center];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* Command+? and the Help button follow the tab. */
- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    Apple2SetHelpContext(_settingsWindow, [item.identifier isEqual:@"input"] ? @"prefs/Input" : @"prefs/Machine");
}

/* Machine options are collected while the window is open and applied in one
 * restart when it closes. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _settingsWindow) {
        [_settingsWindow makeFirstResponder:nil]; /* commit a field edit */
        [_inputPane stop];
        _inputPane = nil;
    }
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
    [view addItem:[self item:@"Controllers" action:@selector(toggleControllers:) key:@"k"]];
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

    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    [help addItem:[self item:@"FujiNet Go Apple II Help" action:@selector(openHelp:) key:@"?"]];
    helpItem.submenu = help;
    [menubar addItem:helpItem];

    NSApp.mainMenu = menubar;
    NSApp.helpMenu = help;
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[Apple2HelpWindow shared]];
}

/* For the window in front: Settings, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[Apple2HelpWindow shared] showFor:[NSApp keyWindow]];
}

/* ---- lifecycle ----------------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* An emulator is latency-critical work: App Nap must not throttle the
     * machine thread or coalesce its timers when the window is behind
     * another, and Game Mode (Info.plist) asks the system to favour it. */
    _activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                          reason:@"Emulating an Apple II"];
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
    /* layer-backed, so the display's layer and the toast over it composite in
     * the order they were added */
    ((NSView *)_window.contentView).wantsLayer = YES;
    [self buildToast:_window.contentView];
    [self applyDisplaySettings];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_display];
    [_display start];

    __weak AppDelegate *weakSelf = self;
    _controllers = [[Apple2ControllersWindow alloc]
        initWithSession:_session
                appKeys:^BOOL(uint32_t keysym) { return [weakSelf handleAppKey:keysym]; }];
    _controllers.sysactionHandler = ^(int sysact) { [weakSelf runSysaction:sysact]; };
    /* No toast for the pads already attached at launch. */
    _toastGeneration = apple2session_gamepad_generation(_session);

    /* Console actions a gamepad button or a bound key posted (those threads
     * cannot touch AppKit) and pads coming and going: well inside 50 ms. */
    _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.05
        repeats:YES block:^(NSTimer *tmr) { (void)tmr; [weakSelf sysactTick]; }];
    _sysactTimer.tolerance = 0.01;

    /* F9 in the machine window opens the Controllers panel (the panel handles
     * its own F9; the debugger keeps F9 for breakpoints). */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
        handler:^NSEvent *(NSEvent *evt) {
            AppDelegate *strong = weakSelf;
            if (strong && evt.window == strong->_window && !evt.isARepeat &&
                !(evt.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                                       NSEventModifierFlagOption | NSEventModifierFlagShift)) &&
                apple2session_keysym_from_macos_keycode(evt.keyCode) == APPLE2SESSION_KEYSYM_F9) {
                [strong->_controllers toggle];
                return nil;
            }
            return evt;
        }];

    /* Developer affordances shared with the other frontends. */
    if (getenv("APPLE2_OPEN_DEBUGGER"))
        [DebuggerWindow showForSession:_session];
    if (getenv("APPLE2_OPEN_CONTROLLERS"))
        [_controllers toggle];
}

/* ---- Controllers window -------------------------------------------------------- */

- (void)toggleControllers:(id)sender
{
    (void)sender;
    [_controllers toggle];
}

/* The toast: a label over the top of the picture that fades. */
- (void)buildToast:(NSView *)content
{
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.alphaValue = 0.0;
    [_toast setFrame:NSMakeRect(0, content.bounds.size.height - 34, content.bounds.size.width, 26)];
    [_toast setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [content addSubview:_toast];
}

- (void)showToast:(NSString *)text
{
    if (!text) return;
    _toast.stringValue = text;
    _toast.alphaValue = 1.0;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial != self->_toastSerial) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.5;
            self->_toast.animator.alphaValue = 0.0;
        } completionHandler:nil];
    });
}

/* A console action (APPLE2SESSION_SYSACT_*) run on the UI thread, from the
 * panel, a menu, a bound key or a gamepad button. */
- (void)runSysaction:(int)sysact
{
    switch (sysact) {
    case APPLE2SESSION_SYSACT_RESET:
        apple2session_sysaction_fire(_session, APPLE2SESSION_SYSACT_RESET);
        [self showToast:@"Reset"];
        break;
    case APPLE2SESSION_SYSACT_POWER:
        apple2session_sysaction_fire(_session, APPLE2SESSION_SYSACT_POWER);
        [self showToast:@"Power cycle"];
        break;
    case APPLE2SESSION_SYSACT_DEBUG_STOP:
        apple2session_sysaction_fire(_session, APPLE2SESSION_SYSACT_DEBUG_STOP);
        [DebuggerWindow showForSession:_session];
        break;
    default:
        break;
    }
}

/* What the 50 ms timer does: the console actions a gamepad button or a bound
 * key posted, and a toast when a gamepad came or went. */
- (void)sysactTick
{
    const unsigned pending = apple2session_sysaction_take(_session);
    for (int act = 0; act < APPLE2SESSION_SYSACT_COUNT; act++)
        if (pending & (1u << act)) [self runSysaction:act];
    const unsigned gen = apple2session_gamepad_generation(_session);
    if (gen != _toastGeneration) {
        _toastGeneration = gen;
        char msg[160];
        if (apple2session_gamepad_last_event(_session, msg, (int)sizeof msg) > 0)
            [self showToast:@(msg)];
    }
}

/* F9 and F12 are the app's and not bindable; called with a key typed in the
 * Controllers panel. YES when taken. */
- (BOOL)handleAppKey:(uint32_t)keysym
{
    if (keysym == APPLE2SESSION_KEYSYM_F9) {
        [_controllers toggle];
        return YES;
    }
    if (keysym == APPLE2SESSION_KEYSYM_F12) {
        [DebuggerWindow toggleForSession:_session];
        return YES;
    }
    return NO;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_logTimer invalidate];
    [_sysactTimer invalidate];
    if (_keyMonitor) {
        [NSEvent removeMonitor:_keyMonitor];
        _keyMonitor = nil;
    }
    [_display stop];
    if (_activity) {
        [[NSProcessInfo processInfo] endActivity:_activity];
        _activity = nil;
    }
    apple2session_stop(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    (void)sender;
    return YES;
}

@end

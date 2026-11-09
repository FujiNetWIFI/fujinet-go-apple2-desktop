/*
 * The debugger's Video tab (AppKit).
 *
 * Everything that decides what to show lives in the core (apple2vidtab.h): this file is views.
 * The picture comes from the controller's cached 0xFFRRGGBB buffer, already zoomed, and is drawn
 * 1:1 (one picture pixel per point) as a CGImage (kCGImageAlphaNoneSkipFirst |
 * kCGBitmapByteOrder32Little, no interpolation) over a copy of that buffer; the image is rebuilt
 * only when the controller re-rendered. A mouse move redraws only the marks. Only the section
 * showing is filled in.
 * NOT COMPILED OR RUN HERE (CI only).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "VideoTab.h"

#include <stdio.h>
#include <string.h>

#include "apple2vidtab.h"

#define MAX_OV 8
#define SECTIONS_WIDTH 440

/* The icon red (#F44336), the rest of the app's accent. */
static NSColor *accentColor(void)
{
    return [NSColor colorWithSRGBRed:244.0 / 255.0 green:67.0 / 255.0 blue:54.0 / 255.0 alpha:1.0];
}

/* A C string as an NSString, never nil (invalid UTF-8 reads as empty). */
static NSString *str(const char *c)
{
    NSString *s = c ? [NSString stringWithUTF8String:c] : nil;
    return s ? s : @"";
}

/* A 0xFFRRGGBB word as a colour. */
static NSColor *colorFromRGB(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                               green:((rgb >> 8) & 255) / 255.0
                                blue:(rgb & 255) / 255.0
                               alpha:1.0];
}

static NSFont *monoFont11(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

/* A selectable monospace label that wraps at `width` (0: no limit). */
static NSTextField *monoLabel(CGFloat width)
{
    NSTextField *l = [NSTextField wrappingLabelWithString:@""];
    l.font = monoFont11();
    l.selectable = YES;
    if (width > 0) l.preferredMaxLayoutWidth = width;
    return l;
}

/* ---- the picture ------------------------------------------------------------------ */

@interface Apple2VideoPicture : NSView
@property (nonatomic, assign) apple2vid_tab *tab;
@property (nonatomic, copy) void (^onChanged)(void);        /* the inspector changed */
@property (nonatomic, copy) void (^onBreak)(void);          /* a double-click: break on the pixel */
- (void)sync;                                               /* the controller re-rendered */
@end

@implementation Apple2VideoPicture {
    CGImageRef _image;
    NSTrackingArea *_area;
    NSString *_message;
    NSSize _size;
}

- (BOOL)isFlipped { return YES; }

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (NSSize)intrinsicContentSize { return _size.width > 0 ? _size : NSMakeSize(320, 240); }

- (void)sync
{
    int w = 0, h = 0;
    const uint32_t *px = apple2vid_tab_picture(_tab, &w, &h);
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    if (px && w > 0 && h > 0) {
        /* a copy: the controller's buffer lives only until its next refresh */
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)px, (CFIndex)w * h * 4);
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        _image = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                               kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, prov, NULL, false,
                               kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(prov);
        CFRelease(data);
        _message = nil;
        _size = NSMakeSize(w, h);
    } else {
        const char *why = apple2vid_tab_why(_tab);
        _message = str(why && *why ? why : "Nothing to show");
        _size = NSMakeSize(0, 0);
    }
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    [[NSColor colorWithCalibratedRed:0.07 green:0.07 blue:0.09 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_image) {
        NSDictionary *a = @{ NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.6 alpha:1],
                             NSFontAttributeName: [NSFont systemFontOfSize:13] };
        NSString *msg = _message ? _message : @"";
        const NSSize sz = [msg sizeWithAttributes:a];
        [msg drawAtPoint:NSMakePoint((self.bounds.size.width - sz.width) / 2,
                                     (self.bounds.size.height - sz.height) / 2) withAttributes:a];
        return;
    }
    /* the view is flipped: draw the image upright */
    CGContextSaveGState(ctx);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextTranslateCTM(ctx, 0, _size.height);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, _size.width, _size.height), _image);
    CGContextRestoreGState(ctx);
    int hover[4] = { 0, 0, 0, 0 }, pin[4] = { 0, 0, 0, 0 };
    const int marks = apple2vid_tab_marks(_tab, hover, pin);
    if (marks & 1) {
        [[NSColor colorWithCalibratedWhite:1 alpha:0.9] setStroke];
        NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSMakeRect(hover[0] + 0.5, hover[1] + 0.5,
                                                                      hover[2] - 1, hover[3] - 1)];
        p.lineWidth = 1;
        [p stroke];
    }
    if (marks & 2) {
        [[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.1 alpha:1] setStroke];
        NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSMakeRect(pin[0] + 1, pin[1] + 1,
                                                                      pin[2] - 2, pin[3] - 2)];
        p.lineWidth = 2;
        [p stroke];
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (NSPoint)local:(NSEvent *)e { return [self convertPoint:e.locationInWindow fromView:nil]; }

- (void)mouseMoved:(NSEvent *)event
{
    const NSPoint p = [self local:event];
    if (apple2vid_tab_hover(_tab, (int)p.x, (int)p.y)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (apple2vid_tab_hover(_tab, -1, -1)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint p = [self local:event];
    if (event.clickCount >= 2) {
        /* the first click of the double already pinned (or unpinned) the spot: keep that, and break */
        apple2vid_tab_hover(_tab, (int)p.x, (int)p.y);
        if (_onBreak) _onBreak();
    } else {
        apple2vid_tab_pin(_tab, (int)p.x, (int)p.y);
    }
    [self setNeedsDisplay:YES];
    if (_onChanged) _onChanged();
}
@end

/* ---- a colour swatch ---------------------------------------------------------------- */

@interface Apple2SwatchView : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation Apple2SwatchView
- (NSSize)intrinsicContentSize { return NSMakeSize(22, 14); }

- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ? _color : NSColor.blackColor) setFill];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:0.5 alpha:1] setStroke];
    NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)];
    p.lineWidth = 1;
    [p stroke];
}
@end

/* ---- the tab ------------------------------------------------------------------------------ */

@interface Apple2VideoTab () <NSTableViewDataSource, NSTableViewDelegate, NSTabViewDelegate>
@end

@implementation Apple2VideoTab {
    apple2vid_tab *_tab;
    NSView *_root;
    NSPopUpButton *_viewPop, *_zoomPop, *_modePop, *_pagePop;
    NSButton *_follow, *_mixed;
    NSButton *_ov[MAX_OV];
    NSStackView *_decRow;
    NSTextField *_status, *_note, *_pixelText, *_timingText;
    Apple2VideoPicture *_pic;
    NSScrollView *_scroll;
    NSTabView *_sections;
    NSTableView *_switches, *_palette;
    NSProgressIndicator *_beam;
    NSString *_swSig, *_palSig;
    BOOL _updating;
    BOOL _first;
    int _nsw, _npal;
    int _vpW, _vpH;
    id _resizeObserver;
}

- (NSView *)view { return _root; }

- (instancetype)initWithSession:(apple2session *)session
{
    self = [super init];
    if (!self) return nil;
    _tab = apple2vid_tab_new(session);
    _first = YES;
    [self build];
    return self;
}

- (void)dealloc
{
    if (_resizeObserver) [[NSNotificationCenter defaultCenter] removeObserver:_resizeObserver];
    if (_tab) apple2vid_tab_free(_tab);
    _tab = NULL;
}

- (void)build
{
    int n = 0;
    const apple2vid_overlay_info *od = apple2vid_overlays(&n);
    if (n > MAX_OV) n = MAX_OV;
    int nz = 0;
    const int *zooms = apple2vid_zooms(&nz);

    _viewPop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < APPLE2VID_V_COUNT; i++) [_viewPop addItemWithTitle:str(apple2vid_view_name(i))];
    _viewPop.target = self;
    _viewPop.action = @selector(onView:);
    _viewPop.toolTip = @"Screen: the last finished frame, as the monitor showed it.\n"
                        "Decoded: video memory redrawn through the soft switches (or the mode chosen below).";
    [_viewPop setRefusesFirstResponder:YES];
    _zoomPop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < nz; i++)
        [_zoomPop addItemWithTitle:zooms[i] == 0 ? @"Fit" : [NSString stringWithFormat:@"%d×", zooms[i]]];
    _zoomPop.target = self;
    _zoomPop.action = @selector(onZoom:);
    [_zoomPop setRefusesFirstResponder:YES];
    NSStackView *bar = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"View"], _viewPop,
                                                         [NSTextField labelWithString:@"Zoom"], _zoomPop]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    /* Decoded only: Follow screen, the mode, the page, MIXED */
    _follow = [NSButton checkboxWithTitle:@"Follow screen" target:self action:@selector(onFollow:)];
    _follow.toolTip = @"Decoded tracks the mode, page and MIXED the machine is displaying; choosing one below turns this off";
    [_follow setRefusesFirstResponder:YES];
    _modePop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < APPLE2VID_M_COUNT; i++) [_modePop addItemWithTitle:str(apple2vid_mode_name(i))];
    _modePop.target = self;
    _modePop.action = @selector(onMode:);
    [_modePop setRefusesFirstResponder:YES];
    _pagePop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_pagePop addItemsWithTitles:@[@"Page 1", @"Page 2"]];
    _pagePop.target = self;
    _pagePop.action = @selector(onPage:);
    [_pagePop setRefusesFirstResponder:YES];
    _mixed = [NSButton checkboxWithTitle:@"Mixed" target:self action:@selector(onMixed:)];
    _mixed.toolTip = @"Graphics with four lines of text at the bottom";
    [_mixed setRefusesFirstResponder:YES];
    _decRow = [NSStackView stackViewWithViews:@[_follow, _modePop, _pagePop, _mixed]];
    _decRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    _decRow.spacing = 8;

    NSMutableArray<NSView *> *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        _ov[i] = [NSButton checkboxWithTitle:str(od[i].label) target:self action:@selector(onOverlay:)];
        [_ov[i] setRefusesFirstResponder:YES];
        [(i < 3 ? row1 : row2) addObject:_ov[i]];
    }
    NSStackView *ovRow1 = [NSStackView stackViewWithViews:row1], *ovRow2 = [NSStackView stackViewWithViews:row2];
    ovRow1.orientation = ovRow2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    ovRow1.spacing = ovRow2.spacing = 10;

    _status = monoLabel(0);
    _pic = [[Apple2VideoPicture alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    _pic.tab = _tab;
    __weak Apple2VideoTab *weak = self;
    _pic.onChanged = ^{ [weak updatePixel]; };
    _pic.onBreak = ^{ [weak breakOnPixel]; };
    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.documentView = _pic;
    _scroll.drawsBackground = NO;
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _note = [NSTextField labelWithString:@""];

    NSStackView *left = [NSStackView stackViewWithViews:@[bar, _decRow, ovRow1, ovRow2, _status, _scroll, _note]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeLeading;
    left.spacing = 6;
    [_scroll.widthAnchor constraintEqualToAnchor:left.widthAnchor].active = YES;

    /* Fit follows the scroll view's size */
    _scroll.postsFrameChangedNotifications = YES;
    _resizeObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSViewFrameDidChangeNotification
                    object:_scroll
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        [weak viewportChanged];
    }];

    /* ---- the sections, in apple2vid_section_name order */
    _sections = [[NSTabView alloc] init];
    _sections.delegate = self;
    {   /* Pixel */
        _pixelText = monoLabel(SECTIONS_WIDTH - 30);
        NSButton *pbrk = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
        NSButton *show = [NSButton buttonWithTitle:@"Show in Memory" target:self action:@selector(onShowInMemory:)];
        NSButton *unpin = [NSButton buttonWithTitle:@"Unpin" target:self action:@selector(onUnpin:)];
        for (NSButton *b in @[pbrk, show, unpin]) [b setRefusesFirstResponder:YES];
        pbrk.toolTip = @"Add write breakpoints on the inspected pixel's byte (and its aux byte)";
        show.toolTip = @"Jump the Memory tab to the pixel's byte (the Aux RAM bank when only aux applies)";
        NSStackView *btns = [NSStackView stackViewWithViews:@[pbrk, show, unpin]];
        btns.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        btns.spacing = 6;
        NSStackView *v = [NSStackView stackViewWithViews:@[_pixelText, btns]];
        v.orientation = NSUserInterfaceLayoutOrientationVertical;
        v.alignment = NSLayoutAttributeLeading;
        v.spacing = 8;
        [self addSection:@"Pixel" content:v];
    }
    {   /* Switches */
        _switches = [[NSTableView alloc] init];
        [self addSection:@"Switches" content:[self scrollForTable:_switches
                                                          columns:@[@"name", @"state", @"age", @"set", @"clr", @"meaning", @"brk"]
                                                           titles:@[@"Switch", @"State", @"Age", @"Set", @"Clear", @"Meaning", @""]
                                                           widths:@[@72, @38, @34, @46, @46, @84, @104]]];
    }
    {   /* Colours */
        _palette = [[NSTableView alloc] init];
        [self addSection:@"Colours" content:[self scrollForTable:_palette
                                                         columns:@[@"sw", @"name", @"rgb", @"uses", @"mark"]
                                                          titles:@[@"", @"Colour", @"RGB", @"Pixels", @""]
                                                          widths:@[@28, @130, @64, @70, @70]]];
    }
    {   /* Timing */
        _beam = [[NSProgressIndicator alloc] init];
        _beam.indeterminate = NO;
        _beam.minValue = 0;
        _beam.maxValue = 1000;
        _timingText = monoLabel(SECTIONS_WIDTH - 30);
        NSStackView *v = [NSStackView stackViewWithViews:@[_beam, _timingText]];
        v.orientation = NSUserInterfaceLayoutOrientationVertical;
        v.alignment = NSLayoutAttributeLeading;
        v.spacing = 8;
        [_beam.widthAnchor constraintEqualToAnchor:v.widthAnchor].active = YES;
        [self addSection:@"Timing" content:v];
    }
    [_sections.widthAnchor constraintEqualToConstant:SECTIONS_WIDTH].active = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[left, _sections]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 10;
    _root = root;
    [left setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* start from the controller's state (the APPLE2_DEBUGGER_VIDEO_* variables) */
    _updating = YES;
    for (int i = 0; i < nz; i++)
        if (zooms[i] == apple2vid_tab_zoom(_tab)) [_zoomPop selectItemAtIndex:i];
    for (int i = 0; i < n; i++)
        _ov[i].state = (apple2vid_tab_overlays(_tab) & od[i].bit) ? NSControlStateValueOn : NSControlStateValueOff;
    const int sec = apple2vid_tab_env_section(_tab);
    if (sec > 0 && sec < (int)_sections.numberOfTabViewItems) [_sections selectTabViewItemAtIndex:sec];
    _updating = NO;
    [self syncControls];
}

/* Sets a table up and returns the scroll view that holds it. */
- (NSScrollView *)scrollForTable:(NSTableView *)t
                         columns:(NSArray<NSString *> *)ids
                          titles:(NSArray<NSString *> *)titles
                          widths:(NSArray<NSNumber *> *)widths
{
    for (NSUInteger i = 0; i < ids.count; i++) {
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:ids[i]];
        c.title = titles[i];
        c.width = widths[i].doubleValue;
        [t addTableColumn:c];
    }
    t.dataSource = self;
    t.delegate = self;
    t.usesAlternatingRowBackgroundColors = YES;
    NSScrollView *sc = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    sc.hasVerticalScroller = YES;
    sc.hasHorizontalScroller = YES;
    sc.documentView = t;
    return sc;
}

- (void)addSection:(NSString *)title content:(NSView *)content
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    if ([content isKindOfClass:[NSScrollView class]])
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    item.view = holder;
    [_sections addTabViewItem:item];
}

/* The choosers and boxes show what the controller has (it moves them: the set_* calls turn Follow off). */
- (void)syncControls
{
    const BOOL was = _updating;
    _updating = YES;
    const int view = apple2vid_tab_view(_tab);
    if (view >= 0 && view < (int)_viewPop.numberOfItems) [_viewPop selectItemAtIndex:view];
    _decRow.hidden = (view != APPLE2VID_V_DECODED);
    _follow.state = apple2vid_tab_follow(_tab) ? NSControlStateValueOn : NSControlStateValueOff;
    const int mode = apple2vid_tab_mode(_tab);
    if (mode >= 0 && mode < (int)_modePop.numberOfItems) [_modePop selectItemAtIndex:mode];
    [_pagePop selectItemAtIndex:apple2vid_tab_page(_tab) == 2 ? 1 : 0];
    _mixed.state = apple2vid_tab_mixed(_tab) ? NSControlStateValueOn : NSControlStateValueOff;
    _updating = was;
}

/* ---- actions -------------------------------------------------------------------------- */

- (void)onView:(id)sender
{
    (void)sender;
    if (_updating) return;
    apple2vid_tab_set_view(_tab, (int)_viewPop.indexOfSelectedItem);
    [self refreshView:APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES];
}

- (void)onZoom:(id)sender
{
    (void)sender;
    int nz = 0;
    const int *zooms = apple2vid_zooms(&nz);
    const NSInteger i = _zoomPop.indexOfSelectedItem;
    if (_updating || i < 0 || i >= nz) return;
    apple2vid_tab_set_zoom(_tab, zooms[i]);
    [self refreshView:APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES];
}

- (void)onFollow:(id)sender
{
    (void)sender;
    if (_updating) return;
    apple2vid_tab_set_follow(_tab, _follow.state == NSControlStateValueOn ? 1 : 0);
    [self refreshView:APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES];
}

- (void)onMode:(id)sender
{
    (void)sender;
    const NSInteger i = _modePop.indexOfSelectedItem;
    if (_updating || i < 0 || i >= APPLE2VID_M_COUNT) return;
    apple2vid_tab_set_mode(_tab, (int)i);
    [self refreshView:APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES];
}

- (void)onPage:(id)sender
{
    (void)sender;
    if (_updating) return;
    apple2vid_tab_set_page(_tab, _pagePop.indexOfSelectedItem == 1 ? 2 : 1);
    [self refreshView:APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES];
}

- (void)onMixed:(id)sender
{
    (void)sender;
    if (_updating) return;
    apple2vid_tab_set_mixed(_tab, _mixed.state == NSControlStateValueOn ? 1 : 0);
    [self refreshView:APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES];
}

- (void)onOverlay:(id)sender
{
    (void)sender;
    if (_updating) return;
    int n = 0;
    const apple2vid_overlay_info *od = apple2vid_overlays(&n);
    if (n > MAX_OV) n = MAX_OV;
    unsigned m = 0;
    for (int i = 0; i < n; i++)
        if (_ov[i].state == NSControlStateValueOn) m |= od[i].bit;
    apple2vid_tab_set_overlays(_tab, m);
    [self refreshView:APPLE2VID_TAB_PICTURE];
}

/* The pixel's write breakpoints (main, and aux when the mode uses it), with a note of what happened. */
- (void)breakOnPixel
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(_tab);
    const int mainAddr = p ? p->main_addr : -1, auxAddr = p ? p->aux_addr : -1;
    const int ident = apple2vid_tab_break_on_pixel(_tab);
    if (ident < 0) {
        _note.stringValue = @"Nothing to break on";
        return;
    }
    NSMutableString *s = [NSMutableString stringWithString:@"Break on writes to"];
    if (mainAddr >= 0) [s appendFormat:@" $%04X", mainAddr & 0xFFFF];
    if (auxAddr >= 0) [s appendFormat:@"%@ aux $%04X", mainAddr >= 0 ? @" and" : @"", auxAddr & 0xFFFF];
    [s appendString:@" (see Breakpoints)"];
    _note.stringValue = s;
}

- (void)onBreak:(id)sender
{
    (void)sender;
    [self breakOnPixel];
}

/* The main byte in Main RAM, or, when only aux applies, the aux byte in Aux RAM. */
- (void)onShowInMemory:(id)sender
{
    (void)sender;
    const apple2vid_pixel *p = apple2vid_tab_pixel(_tab);
    if (!p) {
        _note.stringValue = @"Pin or hover a pixel first";
        return;
    }
    int bank, addr;
    if (p->main_addr >= 0) {
        bank = APPLE2_BANK_MAIN;
        addr = p->main_addr;
    } else if (p->aux_addr >= 0) {
        bank = APPLE2_BANK_AUX;
        addr = p->aux_addr;
    } else {
        _note.stringValue = @"That pixel has no memory address";
        return;
    }
    _note.stringValue = @"";
    if (_showInMemory) _showInMemory(bank, addr & 0xFFFF);
}

- (void)onSwitchBreak:(id)sender
{
    const int i = (int)((NSButton *)sender).tag;
    const apple2vid_switch *sw = NULL;
    const int n = apple2vid_tab_switches(_tab, &sw);
    const int ident = apple2vid_tab_break_on_switch(_tab, i);
    if (ident >= 0 && sw && i >= 0 && i < n)
        _note.stringValue = [NSString stringWithFormat:@"Break on %s: $%04X and $%04X (see Breakpoints)",
                                                       sw[i].name, sw[i].set_addr & 0xFFFF, sw[i].clr_addr & 0xFFFF];
    else
        _note.stringValue = @"Nothing to break on";
}

- (void)onUnpin:(id)sender
{
    (void)sender;
    apple2vid_tab_unpin(_tab);
    [_pic setNeedsDisplay:YES];
    [self updatePixel];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if (!_updating) [self refreshView:APPLE2VID_TAB_TABLES];
}

/* The picture's room changed: Fit needs a new picture. */
- (void)viewportChanged
{
    const NSSize vp = _scroll.contentSize;
    if ((int)vp.width == _vpW && (int)vp.height == _vpH) return;
    if (apple2vid_tab_zoom(_tab) == 0) {
        [self refreshView:APPLE2VID_TAB_PICTURE];
    } else {
        _vpW = (int)vp.width;
        _vpH = (int)vp.height;
        apple2vid_tab_set_viewport(_tab, _vpW > 2 ? _vpW - 2 : 1, _vpH > 2 ? _vpH - 2 : 1);
    }
}

- (void)selectLegacyPage:(int)index
{
    static const int modes[6] = { APPLE2VID_M_TEXT40, APPLE2VID_M_TEXT40, APPLE2VID_M_LORES, APPLE2VID_M_LORES,
                                  APPLE2VID_M_HIRES, APPLE2VID_M_HIRES };
    if (index < 0 || index > 5) return;
    apple2vid_tab_set_view(_tab, APPLE2VID_V_DECODED);
    apple2vid_tab_set_mode(_tab, modes[index]);
    apple2vid_tab_set_page(_tab, (index & 1) + 1);
    [self refreshView:APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES];
}

- (void)pinAtX:(int)x y:(int)y
{
    [self refreshForce:YES];
    apple2vid_tab_pin(_tab, x, y);
    [_pic setNeedsDisplay:YES];
    [self updatePixel];
}

/* ---- the sections ----------------------------------------------------------------------- */

- (void)updatePixel
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(_tab);
    _pixelText.stringValue = p ? str(p->text)
                               : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                  "Double-click to break on writes to it.";
    /* the inspected pixel's colour is marked in Colours */
    if (_sections.selectedTabViewItem == [_sections tabViewItemAtIndex:APPLE2VID_S_COLOURS]) [self fillColours];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)t
{
    return t == _palette ? _npal : _nsw;
}

- (NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    NSString *cid = col.identifier;
    if (t == _switches) {
        const apple2vid_switch *s = NULL;
        const int n = apple2vid_tab_switches(_tab, &s);
        if (!s || row < 0 || row >= n) return nil;
        const BOOL recent = s[row].age < APPLE2VID_SW_FADE;
        if ([cid isEqualToString:@"brk"]) {
            NSButton *b = [t makeViewWithIdentifier:cid owner:self];
            if (![b isKindOfClass:[NSButton class]]) {
                b = [NSButton buttonWithTitle:@"Break on change" target:self action:@selector(onSwitchBreak:)];
                b.identifier = cid;
                b.controlSize = NSControlSizeSmall;
                b.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
                [b setRefusesFirstResponder:YES];
            }
            b.tag = (NSInteger)row;
            return b;
        }
        NSString *text = @"";
        if ([cid isEqualToString:@"name"]) text = str(s[row].name);
        else if ([cid isEqualToString:@"state"]) text = s[row].on ? @"on" : @"off";
        else if ([cid isEqualToString:@"age"]) text = recent ? [NSString stringWithFormat:@"%d", s[row].age] : @"";
        else if ([cid isEqualToString:@"set"]) text = [NSString stringWithFormat:@"$%04X", s[row].set_addr & 0xFFFF];
        else if ([cid isEqualToString:@"clr"]) text = [NSString stringWithFormat:@"$%04X", s[row].clr_addr & 0xFFFF];
        else text = str(s[row].meaning);
        return [self textCell:t column:col text:text hot:recent dim:!recent];
    }
    const apple2vid_pal_entry *e = NULL;
    const int n = apple2vid_tab_palette(_tab, &e);
    if (!e || row < 0 || row >= n) return nil;
    if ([cid isEqualToString:@"sw"]) {
        Apple2SwatchView *sv = [t makeViewWithIdentifier:cid owner:self];
        if (![sv isKindOfClass:[Apple2SwatchView class]]) {
            sv = [[Apple2SwatchView alloc] initWithFrame:NSMakeRect(0, 0, 22, 14)];
            sv.identifier = cid;
        }
        sv.color = colorFromRGB(e[row].rgb);
        return sv;
    }
    NSString *text = @"";
    if ([cid isEqualToString:@"name"]) text = str(e[row].name);
    else if ([cid isEqualToString:@"rgb"]) text = [NSString stringWithFormat:@"#%06X", e[row].rgb & 0xFFFFFF];
    else if ([cid isEqualToString:@"uses"]) text = [NSString stringWithFormat:@"%ld", e[row].uses];
    else text = e[row].inspected ? @"◀ pixel" : @"";
    return [self textCell:t column:col text:text hot:e[row].inspected != 0 dim:NO];
}

/* A reused text cell: monospace, the accent fill with white text when hot, grey when dim. */
- (NSTextField *)textCell:(NSTableView *)t column:(NSTableColumn *)col text:(NSString *)text hot:(BOOL)hot dim:(BOOL)dim
{
    NSTextField *f = [t makeViewWithIdentifier:col.identifier owner:self];
    if (![f isKindOfClass:[NSTextField class]]) {
        f = [NSTextField labelWithString:@""];
        f.identifier = col.identifier;
        f.font = monoFont11();
    }
    f.stringValue = text;
    f.drawsBackground = hot;
    f.backgroundColor = hot ? accentColor() : NSColor.clearColor;
    f.textColor = hot ? NSColor.whiteColor : (dim ? NSColor.secondaryLabelColor : NSColor.labelColor);
    return f;
}

- (void)fillSwitches
{
    const apple2vid_switch *s = NULL;
    const int n = apple2vid_tab_switches(_tab, &s);
    NSMutableString *sig = [NSMutableString string];
    for (int i = 0; s && i < n; i++)
        [sig appendFormat:@"%d%d,%s;", s[i].on, s[i].age < APPLE2VID_SW_FADE ? s[i].age : -1, s[i].meaning];
    [sig appendFormat:@"%d", n];
    if ([sig isEqualToString:_swSig]) return;
    _swSig = sig;
    _nsw = s ? n : 0;
    [_switches reloadData];
}

- (void)fillColours
{
    const apple2vid_pal_entry *e = NULL;
    const int n = apple2vid_tab_palette(_tab, &e);
    NSMutableString *sig = [NSMutableString string];
    for (int i = 0; e && i < n; i++)
        [sig appendFormat:@"%06X,%ld,%d;", e[i].rgb & 0xFFFFFF, e[i].uses, e[i].inspected];
    [sig appendFormat:@"%d", n];
    if ([sig isEqualToString:_palSig]) return;
    _palSig = sig;
    _npal = e ? n : 0;
    [_palette reloadData];
}

- (void)fillTiming
{
    const char *const *lines = NULL;
    const int n = apple2vid_tab_timing(_tab, &lines);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; lines && i < n; i++) {
        NSString *line = str(lines[i]);
        const NSRange t = [line rangeOfString:@"\t"];
        if (t.location != NSNotFound)
            [s appendFormat:@"%-13s %@\n", [[line substringToIndex:t.location] UTF8String],
                            [line substringFromIndex:t.location + 1]];
        else
            [s appendFormat:@"%@\n", line];
    }
    NSString *status = str(apple2vid_tab_status(_tab));
    if (status.length) [s appendFormat:@"\n%@\n", status];
    _timingText.stringValue = (n || status.length) ? s : @"The machine is off";
    _beam.doubleValue = apple2vid_tab_beam_permille(_tab);
}

/* ---- refresh ------------------------------------------------------------------------------ */

- (void)refreshView:(unsigned)what
{
    [self syncControls];
    if (what & APPLE2VID_TAB_PICTURE) {
        const NSSize vp = _scroll.contentSize;
        _vpW = (int)vp.width;
        _vpH = (int)vp.height;
        if (vp.width > 4 && vp.height > 4) apple2vid_tab_set_viewport(_tab, (int)vp.width - 2, (int)vp.height - 2);
        [_pic sync];
        [_pic setFrameSize:_pic.intrinsicContentSize];
    }
    if (what & APPLE2VID_TAB_TABLES) {
        _status.stringValue = str(apple2vid_tab_status(_tab));
        const NSUInteger sec = [_sections.tabViewItems indexOfObject:_sections.selectedTabViewItem];
        switch (sec) {
        case APPLE2VID_S_SWITCHES: [self fillSwitches]; break;
        case APPLE2VID_S_COLOURS: [self fillColours]; break;
        case APPLE2VID_S_TIMING: [self fillTiming]; break;
        default: break;
        }
        [self updatePixel];
    }
}

- (void)refreshForce:(BOOL)force
{
    if (_first) {       /* nothing has been drawn yet */
        force = YES;
        _first = NO;
    }
    unsigned what = apple2vid_tab_refresh(_tab, force ? 1 : 0);
    if (force) what |= APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES;
    if (what & APPLE2VID_TAB_OFF) what |= APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES;     /* the message */
    [self refreshView:what];
}

@end

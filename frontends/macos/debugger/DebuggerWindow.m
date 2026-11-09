/*
 * Debugger window (AppKit) over this app's 6502 debugger engine, via
 * core/include/apple2debug.h and apple2memtools.h. Mirrors the Astrocade,
 * ADAM, SMS and MSX siblings' macOS debuggers (and the GTK, Qt and Win32
 * debuggers), tab for tab: Console, Disassembly, Memory, Breakpoints, Watches,
 * Slots & I/O, Trace, History, Video, FujiNet. The registers sit in a strip
 * under the tabs; any tab can be popped out into a window of its own (Pop Out
 * / Dock).
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it (F12, the close button, the menu) detaches, and the machine runs on.
 * F12 in the machine's window and the View menu toggle it.
 *
 * A 100 ms NSTimer polls apple2debug_generation() and refreshes only the tab on
 * show, and only when the generation changed (a stop, a resume, a step, a
 * command, an edit). While the machine runs, that tab's live values refresh
 * twice a second (every fifth tick). After a step the window waits a moment
 * (~40 ms) for the stop to settle before it reads anything back; a longer run
 * (Frame, Step Out, Run to Cursor) is picked up by the poll when it stops.
 * apple2debug_set_stop_callback is not used: it fires on the emulator thread.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 stop/run, F7 step, Shift+F7 frame (to the next
 * frame boundary), F8 step over, Shift+F8 step out, F9 toggle the breakpoint
 * on the selected disassembly line, F12 close.
 *
 * APPLE2_DEBUGGER_TAB picks the tab shown first: an index (0 Console,
 * 1 Disassembly, 2 Memory, 3 Breakpoints, 4 Watches, 5 Slots & I/O, 6 Trace,
 * 7 History, 8 Video, 9 FujiNet) or the start of a tab's name ("io" is the
 * Slots & I/O tab). APPLE2_DEBUGGER_VIEW picks the Video tab's page (an index
 * into apple2debug_view_name). For captures: APPLE2_WATCH, APPLE2_WATCH_BREAK,
 * APPLE2_MEM_GOTO, APPLE2_HISTORY=1 and APPLE2_POPOUT=<tab>.
 *
 * Highlights: the PC's line is the accent colour with white text, an enabled
 * breakpoint's marker red and a disabled one's grey, the selected line (Run to
 * Cursor) the system selection colour underlined, labels and comments grey;
 * the Run button is the accent colour while stopped.
 *
 * The Video tab is the video page viewer (apple2debug_render_view): a popup of
 * the pages, shown 2x with nearest-neighbour scaling. The FujiNet tab shows
 * every SmartPort packet between the card and FujiNet, both ways, with hex,
 * beside the card's state (apple2debug_fujinet_*).
 *
 * NOT COMPILED OR RUN HERE (no Cocoa SDK on the Linux dev box): transposed
 * from the Astrocade desktop's macOS debugger, checked on the macOS CI runner.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"
#import "../help/HelpWindow.h"

#import <QuartzCore/QuartzCore.h>

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "apple2debug.h"
#include "apple2memtools.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_REGS 6             /* PC SP A X Y P */
#define NUM_FLAGS 8            /* N V - B D I Z C */
#define MEM_ROWS 32            /* the hex dump: 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define TRACE_MAX 512          /* FujiNet packets shown */
#define TAB_CONSOLE 0          /* the tabs, in order */
#define TAB_DISASM 1
#define TAB_MEMORY 2
#define TAB_BREAKS 3
#define TAB_WATCHES 4
#define TAB_SLOTS 5
#define TAB_TRACE 6
#define TAB_HIST 7
#define TAB_VIDEO 8
#define TAB_FUJINET 9
#define NUM_TABS 10
#define HIST_RING 65536        /* the engine's trace ring */
#define HIST_SHOWN 2000        /* the newest matching lines a tab shows */
#define TRACE_TICKS 4          /* 4 x 100 ms: the Trace tab's live refresh */
#define LIVE_TICKS 5           /* 5 x 100 ms: the live refresh while running */
#define MAX_SLOTS 16
#define MAX_REGIONS 128
#define VIDEO_ZOOM 2           /* the video page is shown at 2x */

static DebuggerWindow *g_debugger;

/* The accent (the icon background, #F44336), white text on it. */
static NSColor *a2Accent(void)
{
    return [NSColor colorWithSRGBRed:244.0/255.0 green:67.0/255.0 blue:54.0/255.0 alpha:1.0];
}

/* Toggled-on look for a push button: accent bezel and a white title. Off
 * restores the plain bezel and title. Call again after setting a new title
 * on a button that is on (a plain title drops the attributed one). */
static void setButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = a2Accent();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName: button.font ?: [NSFont systemFontOfSize:[NSFont systemFontSize]],
                    NSForegroundColorAttributeName: NSColor.whiteColor,
                    NSParagraphStyleAttributeName: para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;       /* drops the attributed title */
    }
}

/* An enabled breakpoint's marker: the same red in every toolkit's debugger. */
static NSColor *breakpointRed(void)
{
    return [NSColor colorWithSRGBRed:0xE0 / 255.0 green:0x1B / 255.0 blue:0x24 / 255.0 alpha:1.0];
}

/* Disassembly text view: a click selects the clicked line (Run to Cursor);
 * a click on its gutter, or a double-click, toggles its breakpoint; the
 * wheel browses by instruction. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScroll)(int rows);
@end

@implementation DasmTextView {
    CGFloat _wheel;     /* the wheel's fraction not yet scrolled */
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    NSUInteger lineStart = 0;
    for (NSUInteger i = 0; i < idx; i++)
        if ([text characterAtIndex:i] == '\n') { line++; lineStart = i + 1; }
    if (self.onClick) self.onClick(line, (int)(idx - lineStart), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    /* a notch is a few rows; a trackpad's pixels are many */
    _wheel += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 8.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_wheel;
    if (rows == 0) return;
    _wheel -= rows;
    if (self.onScroll) self.onScroll(rows);
}
@end

/* XRGB8888 pixels (0x00RRGGBB words, rows `stride` words apart) as an image
 * of w x h. The pixels are copied, so the caller's scratch can be reused for
 * the next picture. On a little-endian host the bytes run B,G,R,X, which
 * kCGBitmapByteOrder32Little + AlphaNoneSkipFirst reads exactly (the same as
 * the main display). */
static NSImage *imageFromXRGB(const uint32_t *px, int w, int h, int stride)
{
    NSData *data = [NSData dataWithBytes:px length:(NSUInteger)stride * (NSUInteger)h * 4];
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGImageRef cg = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)stride * 4, space,
                                  kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, NULL, false,
                                  kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    if (!cg) return nil;
    NSImage *img = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(w, h)];
    CGImageRelease(cg);
    return img;
}

/* The Video tab's picture: reports the mouse in picture coordinates (hover,
 * leave, click with its click count). Not flipped; y is turned over here. */
@interface VideoPicView : NSImageView
@property (nonatomic, copy) void (^onHover)(int x, int y);        /* x < 0: left */
@property (nonatomic, copy) void (^onClick)(int x, int y, NSInteger clicks);
@end

@implementation VideoPicView {
    NSTrackingArea *_area;
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

/* The picture point under the event, or NO outside the view. */
- (BOOL)pictureX:(int *)x y:(int *)y event:(NSEvent *)event
{
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const NSSize sz = self.bounds.size;
    if (sz.width < 1 || sz.height < 1 || p.x < 0 || p.y < 0 || p.x >= sz.width || p.y >= sz.height) return NO;
    *x = (int)(p.x * APPLE2VIEW_WIDTH / sz.width);
    *y = (int)((sz.height - 1 - p.y) * APPLE2VIEW_HEIGHT / sz.height);   /* AppKit's y is up */
    if (*x >= APPLE2VIEW_WIDTH) *x = APPLE2VIEW_WIDTH - 1;
    if (*y >= APPLE2VIEW_HEIGHT) *y = APPLE2VIEW_HEIGHT - 1;
    return *x >= 0 && *y >= 0;
}

- (void)mouseMoved:(NSEvent *)event
{
    int x, y;
    if (!self.onHover) return;
    if ([self pictureX:&x y:&y event:event]) self.onHover(x, y);
    else self.onHover(-1, -1);
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (self.onHover) self.onHover(-1, -1);
}

- (void)mouseDown:(NSEvent *)event
{
    int x, y;
    if (self.onClick && [self pictureX:&x y:&y event:event]) self.onClick(x, y, event.clickCount);
}
@end

/* A 1-pixel outline of a picture rectangle, clipped to the picture. */
static void outlineRect(uint32_t *fb, int rx, int ry, int rw, int rh, uint32_t colour)
{
    for (int i = 0; i < rw; i++)
        for (int e = 0; e < 2; e++) {
            const int x = rx + i, y = e ? ry + rh - 1 : ry;
            if (x >= 0 && x < APPLE2VIEW_WIDTH && y >= 0 && y < APPLE2VIEW_HEIGHT) fb[y * APPLE2VIEW_WIDTH + x] = colour;
        }
    for (int j = 0; j < rh; j++)
        for (int e = 0; e < 2; e++) {
            const int y = ry + j, x = e ? rx + rw - 1 : rx;
            if (x >= 0 && x < APPLE2VIEW_WIDTH && y >= 0 && y < APPLE2VIEW_HEIGHT) fb[y * APPLE2VIEW_WIDTH + x] = colour;
        }
}

@interface DebuggerWindow () <NSTabViewDelegate, NSTableViewDataSource>
- (instancetype)initWithSession:(apple2session *)session;
- (void)show;
- (void)refreshNow;
@end

@implementation DebuggerWindow {
    apple2session *_session;
    apple2debug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _liveTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[NUM_FLAGS];
    NSTextField *_beam;
    NSTextView *_mem;
    NSTextField *_memGoto, *_memWrite;
    int _memBase;               /* the dump's first address */
    BOOL _memHl;                /* a range is highlighted: Go to, a hit, a watch */
    int _memHlLo, _memHlHi;
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry, *_srchNote;

    uint64_t _seenStop;         /* apple2debug_stop_serial() last acted on */
    BOOL _scrolledAway;         /* the wheel moved the disassembly off the PC */
    int _traceTicks;

    NSStackView *_wtList;
    NSString *_wtSig;
    NSTextField *_wtEntry;

    NSTableView *_slotTable, *_mapTable;
    NSArray<NSArray<NSString *> *> *_slotRows, *_mapRows;
    NSTextView *_ioText;
    NSButton *_hRec[2];         /* Trace (0) and History (1) share one ring */
    NSPopUpButton *_hFilter[2];
    NSTextField *_hRange[2], *_hNote[2];
    NSTextView *_hText[2];

    NSTabViewItem *_items[NUM_TABS];    /* each tab's item, in order */
    NSWindow *_popped[NUM_TABS];        /* its own window while popped out */

    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;
    int _selected;              /* the selected line's address, or -1 */

    NSPopUpButton *_viewPick;
    NSTextField *_viewNote;
    VideoPicView *_viewPic;
    uint32_t *_viewFb;          /* APPLE2VIEW_WIDTH x APPLE2VIEW_HEIGHT, XRGB8888: the page as rendered */
    uint32_t *_viewDraw;        /* the same with the hover and pin outlines drawn in */
    NSTextField *_viewHover, *_viewPinned, *_viewStatus;
    apple2debug_pixel _hov, _pin;   /* .valid 0: no hover / no pin */
    int _hovX, _hovY, _pinX, _pinY; /* the picture points they were read at */

    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpErr, *_bpStop;
    NSStackView *_bpList;
    NSString *_bpSig;

    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;             /* what the log shows: -1 forces a rebuild */
}

#define NUM_FILE_KINDS 3
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "mem", "regs" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"Memory (64K)…", @"Registers and switches…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = {
    @"disassembly.asm", @"memory.bin", @"registers.txt"
};

/* APPLE2_DEBUGGER_TAB takes an index or the start of one of these. */
static const char *const kTabNames[NUM_TABS] = {
    "console", "disassembly", "memory", "breakpoints", "watches", "slots & i/o", "trace", "history", "video",
    "fujinet"
};
static NSString *const kTabTitles[NUM_TABS] = {
    @"Console", @"Disassembly", @"Memory", @"Breakpoints", @"Watches", @"Slots & I/O", @"Trace", @"History",
    @"Video", @"FujiNet"
};
/* Help's contexts (apple2help.h), one per tab */
static NSString *const kHelpTabs[NUM_TABS] = {
    @"console", @"disassembly", @"memory", @"breakpoints", @"watches", @"slots", @"trace", @"history", @"video",
    @"fujinet"
};

/* A tab by index or by the start of its name; -1 for none. */
static int tabFromText(const char *tab)
{
    if (!tab || !*tab) return -1;
    if (tab[0] >= '0' && tab[0] <= '9') return atoi(tab);
    if (!strncasecmp(tab, "io", 2) || !strncasecmp(tab, "i/o", 3)) return TAB_SLOTS;
    for (int i = 0; i < NUM_TABS; i++)
        if (!strncasecmp(tab, kTabNames[i], strlen(tab))) return i;
    return -1;
}

/* ---- helpers --------------------------------------------------------------- */

/* The engine's text as a string: control bytes dropped (a console's CRs and
 * escapes), UTF-8 if it is, otherwise Latin-1. */
static NSString *cstr(const char *s)
{
    if (!s) return @"";
    const size_t n = strlen(s);
    char *b = malloc(n + 1);
    if (!b) return @"";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 || c == '\n' || c == '\t') b[o++] = (char)c;
    }
    b[o] = '\0';
    NSString *r = [NSString stringWithUTF8String:b];
    if (!r) r = [[NSString alloc] initWithBytes:b length:o encoding:NSISOLatin1StringEncoding];
    free(b);
    return r ?: @"";
}

/* Pads a column to width (never cuts a longer one). */
static NSString *padTo(NSString *s, NSUInteger width)
{
    return s.length >= width ? s : [s stringByPaddingToLength:width withString:@" " startingAtIndex:0];
}

/* $hex, 0xhex, hexh, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    else if (t.length > 1 && [t.lowercaseString hasSuffix:@"h"]) t = [t substringToIndex:t.length - 1];
    if (t.length == 0) return NO;
    char *end;
    long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* A value as the fields take it: a label; $hex, 0xhex, hexh, #dec or bare hex
 * digits (hex here, though the console's bare digits are decimal); anything
 * else (an expression, a register) through the engine. On failure the reason
 * is in *why. */
- (BOOL)evaluate:(NSString *)text into:(long *)out why:(NSString **)why
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    long v;
    if (t.length == 0) {
        if (why) *why = @"nothing entered";
        return NO;
    }
    const int label = apple2debug_label_address(_dbg, t.UTF8String);
    if (label >= 0) {
        *out = label;
        return YES;
    }
    if (parseNum(t, &v)) {
        *out = v;
        return YES;
    }
    char err[160];
    err[0] = '\0';
    if (apple2debug_eval(_dbg, t.UTF8String, &v, err, (int)sizeof err) == 0) {
        *out = v;
        return YES;
    }
    NSString *e = cstr(err);
    if (why) *why = e.length ? e : @"not a number, label or expression";
    return NO;
}

/* An address: the above, within $0000-$FFFF. -1 when it is not one. */
- (int)resolveAddr:(NSString *)text why:(NSString **)why
{
    long v;
    if (![self evaluate:text into:&v why:why]) return -1;
    if (v < 0 || v > 0xFFFF) {
        if (why) *why = @"out of range ($0000-$FFFF)";
        return -1;
    }
    return (int)v;
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill with white on it: the PC's line, a marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: a2Accent(), NSForegroundColorAttributeName: NSColor.whiteColor };
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same for styled text, always where the reader left it. */
static void setStyledText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:at];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *l = [self label:text];
    l.textColor = NSColor.secondaryLabelColor;
    /* a long hint gives way to a narrow window instead of widening it */
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return l;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = monoFont();
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [p addItemWithTitle:title];
    p.lastItem.tag = -1;
    for (NSUInteger i = 0; i < items.count; i++) {
        [p addItemWithTitle:items[i]];
        p.lastItem.tag = (NSInteger)i;
    }
    p.target = self;
    p.action = sel;
    [p setRefusesFirstResponder:YES];
    return p;
}

/* A picture, scaled up crisp: nearest-neighbour, so the pixels stay square. */
- (NSImageView *)pixelView:(int)w height:(int)h
{
    NSImageView *v = [[NSImageView alloc] init];
    v.imageScaling = NSImageScaleAxesIndependently;
    v.wantsLayer = YES;
    v.layer.magnificationFilter = kCAFilterNearest;
    [v.widthAnchor constraintEqualToConstant:w].active = YES;
    [v.heightAnchor constraintEqualToConstant:h].active = YES;
    return v;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(apple2session *)session
{
    /* no engine until the session runs: nothing to attach to */
    if (!g_debugger && !apple2session_debugger(session)) {
        NSBeep();
        return;
    }
    if (!g_debugger) g_debugger = [[DebuggerWindow alloc] initWithSession:session];
    [g_debugger show];
}

+ (void)toggleForSession:(apple2session *)session
{
    if (g_debugger && [g_debugger->_window isVisible]) {
        /* windowWillClose: detaches */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (void)show
{
    if (_window.isMiniaturized) [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    for (int t = 0; t < NUM_TABS; t++)
        if (_popped[t]) [_popped[t] orderFront:nil];
    const unsigned gen = apple2debug_generation(_dbg);
    apple2debug_attach(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

- (instancetype)initWithSession:(apple2session *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = apple2session_debugger(session);
    _fujiCount = -1;
    _memBase = 0x0400;          /* text page 1 */
    _selected = -1;
    _viewFb = calloc((size_t)APPLE2VIEW_WIDTH * APPLE2VIEW_HEIGHT, sizeof(uint32_t));
    _viewDraw = calloc((size_t)APPLE2VIEW_WIDTH * APPLE2VIEW_HEIGHT, sizeof(uint32_t));
    _history = [NSMutableArray array];
    [self buildWindow];

    /* For captures and tests, like APPLE2_DEBUGGER_TAB */
    {
        const char *watch = getenv("APPLE2_WATCH");
        const char *watchBreak = getenv("APPLE2_WATCH_BREAK");
        const char *gotoText = getenv("APPLE2_MEM_GOTO");
        const char *history = getenv("APPLE2_HISTORY");
        const char *popOut = getenv("APPLE2_POPOUT");
        uint16_t lo, hi;
        if (watch && *watch) apple2watch_add(watch);
        if (watchBreak && *watchBreak && apple2sym_parse_range(_dbg, watchBreak, &lo, &hi))
            [self breakOnChangeLo:lo hi:hi];
        if (gotoText && *gotoText && apple2sym_parse_range(_dbg, gotoText, &lo, &hi))
            [self showMemLo:lo hi:hi];
        if (history && *history) apple2debug_trace_enable(_dbg, 1);
        /* a video page to land on (an index into apple2debug_view_name) */
        const char *view = getenv("APPLE2_DEBUGGER_VIEW");
        if (view && *view) {
            const int idx = atoi(view);
            if (idx >= 0 && idx < _viewPick.numberOfItems) [_viewPick selectItemAtIndex:idx];
        }
        /* a point of the picture to pin, "x,y" in picture coordinates */
        const char *pin = getenv("APPLE2_VIDEO_PIN");
        int px, py;
        if (pin && sscanf(pin, "%d , %d", &px, &py) == 2) {
            [self refreshVideo];    /* the page must exist before it is inspected */
            if (apple2debug_view_inspect(_dbg, (apple2debug_view)_viewPick.indexOfSelectedItem, px, py, &_pin) == 0 &&
                _pin.valid) {
                _pinX = px;
                _pinY = py;
                [self refreshVideo];
            } else {
                _pin.valid = 0;
            }
        }
        if (popOut && *popOut) {
            const int t = tabFromText(popOut);
            if (t >= 0 && t < NUM_TABS) [self popOutTab:t];
        }
    }

    __weak DebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/F9/F12 wherever the focus is in this window, the console's
     * field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        DebuggerWindow *s = weakSelf;
        if (!s || ![s ownsWindow:event.window] || !s->_window.isVisible) return event;
        return [s handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
    free(_viewFb);
    free(_viewDraw);
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    apple2debug_detach(_dbg);
    for (int t = 0; t < NUM_TABS; t++)
        [_popped[t] orderOut:nil];     /* they come back with the window */
}

/* Closing a popped-out tab's window docks the tab again. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    for (int t = 0; t < NUM_TABS; t++)
        if (sender == _popped[t]) {
            [self dockTab:t];
            return NO;
        }
    return YES;
}

/* The debugger's main window or one of its popped-out tabs. */
- (BOOL)ownsWindow:(NSWindow *)window
{
    if (!window) return NO;
    if (window == _window) return YES;
    for (int t = 0; t < NUM_TABS; t++)
        if (_popped[t] == window) return YES;
    return NO;
}

/* The tab the main window's notebook shows, by its place in the full order
 * (-1 if none). */
- (NSInteger)currentTab
{
    NSTabViewItem *selected = _tabs.selectedTabViewItem;
    for (int t = 0; t < NUM_TABS; t++)
        if (_items[t] == selected) return t;
    return -1;
}

/* On screen: the notebook's page, or a window of its own. */
- (BOOL)tabVisible:(int)tab
{
    return _popped[tab] != nil || [self currentTab] == tab;
}

/* The poll: refresh on a stop, a resume or a command (the generation), and
 * the visible tab's live values twice a second while the machine runs. The
 * FujiNet tab also follows the trace twice a second while stopped, as the
 * FujiNet's packets arrive on their own. */
- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = apple2debug_generation(_dbg);
    const BOOL stopped = apple2debug_is_stopped(_dbg) != 0;
    const uint64_t stopSerial = apple2debug_stop_serial(_dbg);
    if (gen != _seenGen || stopped != _wasStopped || stopSerial != _seenStop) {
        if (stopSerial != _seenStop) {
            /* a new stop brings the disassembly back to the PC, even if the
             * wheel moved it away */
            _seenStop = stopSerial;
            if (_scrolledAway) {
                _scrolledAway = NO;
                _followPc.state = NSControlStateValueOn;
            }
        }
        [self refreshNow];
    } else if (++_liveTicks >= LIVE_TICKS) {
        _liveTicks = 0;
        if (!stopped) {
            [self refreshStatus];
            [self refreshTab];
        } else if ([self tabVisible:TAB_FUJINET]) {
            [self refreshFujinet:NO];
        }
    } else if (!stopped && [self tabVisible:TAB_TRACE] && ++_traceTicks >= TRACE_TICKS) {
        _traceTicks = 0;        /* the Trace tab follows the machine */
        [self refreshHist:0];
    }
}

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the machine reads as stopped. */
- (void)settleFrom:(unsigned)gen
{
    for (int i = 0; i < 20; i++) {
        if (apple2debug_generation(_dbg) != gen && apple2debug_is_stopped(_dbg)) break;
        usleep(2000);
    }
}

- (void)updateHelpContext
{
    const NSInteger i = [self currentTab];
    if (i >= 0 && i < NUM_TABS)
        Apple2SetHelpContext(_window, [@"debugger/" stringByAppendingString:kHelpTabs[i]]);
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self updateHelpContext];
    [self refreshTab];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

/* ---- keys -------------------------------------------------------------------- */

- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length != 1) return NO;
    const NSEventModifierFlags mods = event.modifierFlags;
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) return NO;
    const BOOL shift = (mods & NSEventModifierFlagShift) != 0;
    switch ([chars characterAtIndex:0]) {
    case NSF5FunctionKey: [self toggleRun:nil]; return YES;
    case NSF7FunctionKey: if (shift) [self frame:nil]; else [self step:nil]; return YES;
    case NSF8FunctionKey: if (shift) [self stepOut:nil]; else [self stepOver:nil]; return YES;
    case NSF9FunctionKey: [self toggleSelected:nil]; return YES;
    case NSF12FunctionKey: [_window performClose:nil]; return YES;
    default: return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildConsole
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll, YES);
    _conOut.string = @"Debugger console. Type 'help' for every command; Tab completes, Up/Down walk the history.\n";
    _conIn = [self field:@"command (help, step, break $FA62, bpw $0300, regs, mem $0400 32, ...) — Tab completes, ↑↓ history"
                   width:0 action:@selector(runCommand:)];
    _conIn.delegate = self;

    NSStackView *v = vstack(@[outScroll, _conIn]);
    fill(v, outScroll);
    fill(v, _conIn);
    return v;
}

/* The registers, flags and beam, under the tabs on every one of them. */
- (NSView *)buildRegStrip
{
    static NSString *const names[NUM_REGS] = { @"PC", @"SP", @"A", @"X", @"Y", @"P" };
    NSMutableArray *row1 = [NSMutableArray array];
    for (int i = 0; i < NUM_REGS; i++) {
        /* PC and SP are 16-bit (SP as $0100-$01FF), the rest 8-bit */
        const CGFloat width = i < 2 ? 52 : 36;
        [row1 addObject:[self label:names[i]]];
        _reg[i] = [self field:@"" width:width action:@selector(applyRegister:)];
        _reg[i].tag = i;
        _reg[i].toolTip = @"Edit while the machine is stopped (hex, $hex, a label or an expression), then Enter";
        [row1 addObject:_reg[i]];
    }
    static NSString *const fnames[NUM_FLAGS] = { @"N", @"V", @"-", @"B", @"D", @"I", @"Z", @"C" };
    NSMutableArray *flags = [NSMutableArray arrayWithObject:[self label:@"Flags"]];
    for (int i = 0; i < NUM_FLAGS; i++) {
        _flag[i] = [NSButton checkboxWithTitle:fnames[i] target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        if (i == 2) _flag[i].enabled = NO;      /* bit 5 is unused */
        [flags addObject:_flag[i]];
    }
    [row1 addObjectsFromArray:flags];
    _beam = [self hint:@""];
    [row1 addObject:_beam];

    return hstack(row1);
}

- (NSView *)buildMemory
{
    NSScrollView *memScroll;
    _mem = monoView(&memScroll, NO);
    _memGoto = [self field:@"address, label or lo-hi" width:180 action:@selector(gotoMem:)];
    NSButton *go = [self button:@"Go" action:@selector(gotoMem:)];
    NSButton *prev = [self button:@"◀" action:@selector(memPrev:)];
    NSButton *next = [self button:@"▶" action:@selector(memNext:)];
    NSStackView *memHead = hstack(@[[self label:@"Memory (as the CPU sees it, no soft switches touched)"], [self label:@"Go to"], _memGoto, go,
                                    prev, next]);

    _srchKind = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_srchKind addItemsWithTitles:@[@"Hex", @"Text"]];
    _srchKind.toolTip = @"Hex bytes (A9 00) or text, one byte per character";
    [_srchKind setRefusesFirstResponder:YES];
    _srchEntry = [self field:@"hex bytes or text" width:220 action:@selector(searchNext:)];
    _srchNote = [self hint:@""];
    NSStackView *searchRow = hstack(@[[self label:@"Search"], _srchKind, _srchEntry,
                                      [self button:@"Find Next" action:@selector(searchNext:)],
                                      [self button:@"Find Previous" action:@selector(searchPrev:)], _srchNote]);

    _memWrite = [self field:@"address value" width:170 action:@selector(writeMem:)];
    NSStackView *edit = hstack(@[[self label:@"Poke"], _memWrite,
                                 [self hint:@"RAM takes it; ROM ignores it. Several values go to consecutive addresses"]]);

    NSStackView *v = vstack(@[memHead, searchRow, memScroll, edit]);
    fill(v, memScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to select it (Run to Cursor); click its left edge, double-click it "
                                           @"or press F9 for a breakpoint (red dot on, grey ring off); scroll to browse"]]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = monoFont();
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak DebuggerWindow *weakSelf = self;
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        DebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        const BOOL gutter = column < 2;
        s->_selected = s->_lineAddr[line];
        if (gutter ? clicks == 1 : clicks == 2) [s toggleBreakpointAt:s->_lineAddr[line]];
        else [s refreshDisasm];
    };
    _disasm.onScroll = ^(int rows) {
        DebuggerWindow *s = weakSelf;
        if (!s || rows == 0) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_scrolledAway = YES;
        s->_disasmAddr = (uint16_t)apple2debug_row_address(s->_dbg, s->_disasmAddr, rows);
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, row);
    fill(v, scroll);
    return v;
}

/* The video page viewer: a popup of the pages the engine decodes, shown 2x.
 * This is a PAGE, not what the machine is displaying -- it is how you see the
 * buffer a program draws into before it flips to it. One page, which scrolls
 * when the window is smaller than it. */
- (NSView *)buildVideo
{
    _viewPick = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; apple2debug_view_name(i); i++)
        [_viewPick addItemWithTitle:@(apple2debug_view_name(i))];
    _viewPick.target = self;
    _viewPick.action = @selector(viewPickChanged:);
    [_viewPick setRefusesFirstResponder:YES];
    _viewNote = [self hint:@""];
    NSStackView *bar = hstack(@[[self label:@"Page"], _viewPick, _viewNote]);

    _viewPic = [[VideoPicView alloc] init];
    _viewPic.imageScaling = NSImageScaleAxesIndependently;
    _viewPic.wantsLayer = YES;
    _viewPic.layer.magnificationFilter = kCAFilterNearest;
    [_viewPic.widthAnchor constraintEqualToConstant:APPLE2VIEW_WIDTH * VIDEO_ZOOM].active = YES;
    [_viewPic.heightAnchor constraintEqualToConstant:APPLE2VIEW_HEIGHT * VIDEO_ZOOM].active = YES;
    _viewPic.layer.backgroundColor = NSColor.blackColor.CGColor;
    __weak DebuggerWindow *weakSelf = self;
    _viewPic.onHover = ^(int x, int y) { [weakSelf videoHoverX:x y:y]; };
    _viewPic.onClick = ^(int x, int y, NSInteger clicks) { [weakSelf videoClickX:x y:y clicks:clicks]; };

    /* beside the picture: what is under the mouse, the pinned byte, buttons */
    _viewHover = [self label:@"Hover the picture to inspect it"];
    _viewHover.font = monoFont();
    _viewHover.textColor = NSColor.secondaryLabelColor;
    _viewPinned = [self label:@""];
    _viewPinned.font = monoFont();
    _viewStatus = [self hint:@"Click pins a byte, double-click breaks on writes to it"];
    NSStackView *buttons = hstack(@[ [self button:@"Show in Memory" action:@selector(videoShowInMemory:)],
                                     [self button:@"Unpin" action:@selector(videoUnpin:)] ]);
    NSStackView *side = vstack(@[ _viewHover, _viewPinned, buttons, _viewStatus ]);
    side.spacing = 10;
    [side.widthAnchor constraintEqualToConstant:340].active = YES;
    NSStackView *row = hstack(@[ _viewPic, side ]);
    row.alignment = NSLayoutAttributeTop;
    row.spacing = 12;

    /* the page is the scroll view's document, pinned to its top left; its
     * own size is what its views ask for */
    NSStackView *page = vstack(@[ bar, row ]);
    page.spacing = 12;
    page.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *outer = [[NSScrollView alloc] init];
    outer.hasVerticalScroller = YES;
    outer.hasHorizontalScroller = YES;
    outer.drawsBackground = NO;
    outer.documentView = page;
    NSClipView *clip = outer.contentView;
    [page.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [page.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    return outer;
}

/* One breakpoint-table cell: a fixed-width monospace label, or (width 0) the
 * flexible one that takes what is left. */
static NSTextField *bpCell(NSString *text, CGFloat width, BOOL bold)
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = bold ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold] : monoFont();
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [l.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [l setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return l;
}

- (NSView *)buildBreaks
{
    /* the table's rows scroll: a session can collect more breakpoints than
     * the tab is tall. The stack is the scroll view's document, pinned to
     * the top and to the clip view's width so rows keep the tab's width. */
    _bpStop = [self label:@""];
    _bpStop.textColor = NSColor.secondaryLabelColor;
    NSStackView *header = hstack(@[bpCell(@"On  #", 70, YES), bpCell(@"Breakpoint", 0, YES), bpCell(@"Hits", 60, YES),
                                   bpCell(@"", 70, YES)]);
    header.distribution = NSStackViewDistributionFill;
    _bpList = vstack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;

    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Read/Write"]];
    _bpStart = [self field:@"$FA62 or a label" width:190 action:@selector(addBreakpoint:)];
    _bpEnd = [self field:@"end (optional)" width:120 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): a == 5" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clearHits = [self button:@"Clear Hits" action:@selector(clearHits:)];
    NSButton *clear = [self button:@"Clear All" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, [self label:@"Start"], _bpStart, [self label:@"End"], _bpEnd,
                                [self label:@"Condition"], _bpCond, add, clearHits, clear]);
    row.distribution = NSStackViewDistributionFill;
    _bpErr = [self label:@""];
    _bpErr.textColor = NSColor.systemRedColor;
    NSTextField *hint = [self hint:@"Untick to disable. A condition is an expression, e.g. a == 5 or, on a read or write "
                                   @"break, addr == $0300 && value > 3 (old is the byte before a write). Ranges include "
                                   @"the soft switches ($C000-$C0FF); the end defaults to the start."];

    NSStackView *v = vstack(@[_bpStop, header, bpScroll, hint, row, _bpErr]);
    fill(v, header);
    fill(v, bpScroll);
    fill(v, row);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* Watches: an entry, then a row for each with its value and three buttons. */
- (NSView *)buildWatches
{
    _wtEntry = [self field:@"address, label, expression or lo-hi" width:0 action:@selector(addWatch:)];
    NSStackView *row = hstack(@[[self label:@"Watch"], _wtEntry, [self button:@"Add" action:@selector(addWatch:)]]);
    row.distribution = NSStackViewDistributionFill;

    _wtList = vstack(@[]);
    _wtList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = _wtList;
    NSClipView *clip = scroll.contentView;
    [_wtList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_wtList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_wtList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;

    NSTextField *hint = [self hint:@"Values show while the machine is stopped. Memory shows the watch in the Memory tab; "
                                   @"Break on change stops when the machine writes a different value there."];
    NSStackView *v = vstack(@[row, scroll, hint]);
    fill(v, row);
    fill(v, scroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* A table in a scroll view (the caller picks it up through documentView). */
- (NSScrollView *)tableColumns:(NSArray<NSString *> *)titles widths:(NSArray<NSNumber *> *)widths
{
    NSTableView *table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    for (NSUInteger i = 0; i < titles.count; i++) {
        NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:[NSString stringWithFormat:@"c%lu", (unsigned long)i]];
        col.title = titles[i];
        col.width = widths[i].doubleValue;
        col.minWidth = 40;
        ((NSCell *)col.dataCell).font = monoFont();
        [table addTableColumn:col];
    }
    table.dataSource = self;
    table.allowsColumnSelection = NO;
    table.allowsEmptySelection = YES;
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.documentView = table;
    return scroll;
}

/* The slots, the CPU's memory map as it stands, and the soft switches. */
- (NSView *)buildSlots
{
    NSScrollView *slotScroll = [self tableColumns:@[@"Slot", @"Card", @"FujiNet"] widths:@[@50, @220, @70]];
    NSScrollView *mapScroll = [self tableColumns:@[@"Range", @"R/W", @"Region"] widths:@[@110, @50, @300]];
    _slotTable = (NSTableView *)slotScroll.documentView;
    _mapTable = (NSTableView *)mapScroll.documentView;
    [slotScroll.widthAnchor constraintEqualToConstant:380].active = YES;
    NSStackView *tables = hstack(@[slotScroll, mapScroll]);
    tables.distribution = NSStackViewDistributionFill;
    [tables.heightAnchor constraintEqualToConstant:230].active = YES;
    [slotScroll.heightAnchor constraintEqualToAnchor:tables.heightAnchor].active = YES;
    [mapScroll.heightAnchor constraintEqualToAnchor:tables.heightAnchor].active = YES;

    NSScrollView *ioScroll;
    _ioText = monoView(&ioScroll, NO);
    NSStackView *v = vstack(@[tables, [self label:@"Soft switches and slots"], ioScroll]);
    fill(v, tables);
    fill(v, ioScroll);
    return v;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView
{
    return (NSInteger)(tableView == _slotTable ? _slotRows.count : _mapRows.count);
}

- (id)tableView:(NSTableView *)tableView objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    NSArray<NSArray<NSString *> *> *rows = tableView == _slotTable ? _slotRows : _mapRows;
    const NSInteger col = (NSInteger)[tableView.tableColumns indexOfObject:column];
    if (row < 0 || row >= (NSInteger)rows.count || col < 0 || col >= (NSInteger)rows[(NSUInteger)row].count) return @"";
    return rows[(NSUInteger)row][(NSUInteger)col];
}

/* Trace (idx 0) and History (idx 1): two views of the engine's one ring. */
- (NSView *)buildHist:(int)idx
{
    _hRec[idx] = [self button:@"Record" action:@selector(histRec:)];
    [_hRec[idx] setButtonType:NSButtonTypePushOnPushOff];
    _hRec[idx].toolTip = @"Record every instruction the CPU runs (the machine runs a little slower)";
    _hFilter[idx] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_hFilter[idx] addItemsWithTitles:@[@"All instructions", @"Branches only", @"In range"]];
    _hFilter[idx].target = self;
    _hFilter[idx].action = @selector(histChanged:);
    [_hFilter[idx] setRefusesFirstResponder:YES];
    _hRange[idx] = [self field:@"range: lo-hi or a label" width:180 action:@selector(histChanged:)];
    _hNote[idx] = [self hint:@""];
    NSStackView *bar = hstack(@[_hRec[idx], _hFilter[idx], _hRange[idx], [self button:@"Clear" action:@selector(histClear:)],
                                _hNote[idx]]);
    NSScrollView *scroll;
    _hText[idx] = monoView(&scroll, NO);
    NSStackView *v = vstack(@[bar, scroll]);
    fill(v, bar);
    fill(v, scroll);
    return v;
}

/* The packets left (a note and Clear over them), the link right. */
- (NSView *)buildFujinet
{
    _fujiNote = [self label:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_fujiNote, [self button:@"Clear" action:@selector(fujiClear:)]]);

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSTextField *title = [self label:@"SmartPort packets (SLIP-decoded)"];
    title.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *left = vstack(@[title, row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSTextField *stateTitle = [self label:@"SmartPort card"];
    stateTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *right = vstack(@[stateTitle, stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:320].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 820)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Apple II Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. The keys are handled by the key monitor, not as the
     * buttons' key equivalents, so they work wherever the focus is. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    NSButton *toCursor = [self button:@"Run to Cursor" action:@selector(runToCursor:)];
    NSButton *scan = [self button:@"Scanline" action:@selector(scanline:)];
    NSButton *frame = [self button:@"Frame (⇧F7)" action:@selector(frame:)];
    NSButton *symbols = [self button:@"Load Symbols…" action:@selector(loadSymbols:)];
    NSButton *popBtn = [self button:@"Pop Out" action:@selector(popOutCurrent:)];
    popBtn.toolTip = @"Move the tab that shows into a window of its own; Dock puts it back";
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int i = 0; i < NUM_FILE_KINDS; i++) [saveTitles addObject:kFileTitles[i]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSButton *helpBtn = Apple2HelpButton(nil);
    [helpBtn setRefusesFirstResponder:YES];
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, toCursor, scan, frame, symbols, save, popBtn, _status, helpBtn]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ [self buildConsole], [self buildDisasm], [self buildMemory], [self buildBreaks],
                        [self buildWatches], [self buildSlots], [self buildHist:0], [self buildHist:1],
                        [self buildVideo], [self buildFujinet] ];
    for (int i = 0; i < NUM_TABS; i++) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:kTabTitles[i]];
        item.label = kTabTitles[i];
        NSView *content = pages[(NSUInteger)i];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        _items[i] = item;
        [_tabs addTabViewItem:item];
    }
    const int first = tabFromText(getenv("APPLE2_DEBUGGER_TAB"));
    if (first >= 0 && first < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:first];
    _tabs.delegate = self;
    [self updateHelpContext];

    /* the registers sit under every tab */
    NSView *strip = [self buildRegStrip];
    NSStackView *root = vstack(@[toolbar, _tabs, strip]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [strip.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

/* A line for the status label (and the console's log, when asked). */
- (void)message:(NSString *)text console:(BOOL)console
{
    _status.stringValue = text;
    if (console) [self appendConsole:[text stringByAppendingString:@"\n"]];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    const unsigned gen = apple2debug_generation(_dbg);
    if (apple2debug_is_stopped(_dbg)) apple2debug_resume(_dbg); else apple2debug_stop(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
}

/* An action that moves the machine: the stop settles (or the poll catches
 * it, for a long run), then everything shown is read back. */
- (void)moved:(unsigned)gen
{
    [self settleFrom:gen];
    [self refreshNow];
}

- (void)step:(id)sender { (void)sender; const unsigned g = apple2debug_generation(_dbg); apple2debug_step(_dbg); [self moved:g]; }
- (void)stepOver:(id)sender { (void)sender; const unsigned g = apple2debug_generation(_dbg); apple2debug_step_over(_dbg); [self moved:g]; }
- (void)stepOut:(id)sender { (void)sender; const unsigned g = apple2debug_generation(_dbg); apple2debug_step_out(_dbg); [self moved:g]; }
- (void)scanline:(id)sender { (void)sender; const unsigned g = apple2debug_generation(_dbg); apple2debug_scanline(_dbg, 1); [self moved:g]; }
- (void)frame:(id)sender { (void)sender; const unsigned g = apple2debug_generation(_dbg); apple2debug_frame(_dbg, 1); [self moved:g]; }

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to run to" console:NO];
        return;
    }
    const unsigned g = apple2debug_generation(_dbg);
    apple2debug_run_to(_dbg, (uint16_t)_selected);
    [self moved:g];
}

- (void)toggleBreakpointAt:(uint16_t)addr
{
    apple2debug_breakpoint_toggle(_dbg, addr);
    [self refreshDisasm];
}

/* F9 */
- (void)toggleSelected:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to toggle its breakpoint" console:NO];
        return;
    }
    [self toggleBreakpointAt:(uint16_t)_selected];
    if ([self tabVisible:TAB_BREAKS]) [self refreshBps];
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    apple2debug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)appendConsole:(NSString *)text
{
    const BOOL follow = atEnd(_conOut);     /* a reader scrolled back keeps their place */
    [_conOut.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (follow) [_conOut scrollRangeToVisible:NSMakeRange(_conOut.string.length, 0)];
}

- (void)runCommand:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_conIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self appendConsole:[NSString stringWithFormat:@"> %@\n", cmd]];
    out[0] = '\0';
    const unsigned gen = apple2debug_generation(_dbg);
    apple2debug_command(_dbg, cmd.UTF8String, out, (int)sizeof out);
    NSString *text = cstr(out);
    if (text.length) [self appendConsole:[text hasSuffix:@"\n"] ? text : [text stringByAppendingString:@"\n"]];
    _conIn.stringValue = @"";
    [self settleFrom:gen];
    [self refreshNow];
}

/* In the console, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn) return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger n = (NSInteger)_history.count;
        if (n == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > n) _historyPos = n;
        _conIn.stringValue = _historyPos < n ? _history[(NSUInteger)_historyPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    /* the engine takes the whole line and completes its last word */
    NSString *text = _conIn.stringValue;
    const NSRange sp = [text rangeOfString:@" " options:NSBackwardsSearch];
    char comps[4096];
    comps[0] = '\0';
    const int n = apple2debug_completions(_dbg, text.UTF8String, comps, (int)sizeof comps);
    NSString *list = cstr(comps);
    if (n == 1) {
        NSString *c = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = sp.location == NSNotFound ? @"" : [text substringToIndex:sp.location + 1];
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, c];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (n > 1) {
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendConsole:list];
    }
    return YES;
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    char msg[512];
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = @"Load Symbols (\"HHHH NAME\" per line, or a ca65 .lbl / .sym)";
    if ([p runModal] != NSModalResponseOK) return;
    msg[0] = '\0';
    apple2debug_load_symbols(_dbg, [[p URL] fileSystemRepresentation], msg, (int)sizeof msg);
    [self message:cstr(msg) console:YES];
    [self refreshNow];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [p setNameFieldStringValue:kFileNames[kind]];
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    msg[0] = '\0';
    apple2debug_save(_dbg, kFileKinds[kind], [[p URL] fileSystemRepresentation], msg, (int)sizeof msg);
    [self message:cstr(msg) console:YES];
}

- (void)applyRegister:(NSTextField *)sender
{
    static const int regIds[NUM_REGS] = {
        APPLE2_REG_PC, APPLE2_REG_SP, APPLE2_REG_A, APPLE2_REG_X, APPLE2_REG_Y, APPLE2_REG_P
    };
    long v;
    NSString *why = nil;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
    if (!apple2debug_is_stopped(_dbg)) {
        [self message:@"Stop the machine (F5) to edit registers" console:NO];
    } else if (sender.tag < 0 || sender.tag >= NUM_REGS) {
        /* not one of ours */
    } else if ([self evaluate:sender.stringValue into:&v why:&why]) {
        /* SP is shown as $0100-$01FF; a bare stack offset means that page */
        if (sender.tag == 1 && v >= 0 && v <= 0xFF) v |= 0x100;
        apple2debug_cpu_set(_dbg, regIds[sender.tag], (int)v);
    } else {
        [self message:[NSString stringWithFormat:@"Register: %@", why] console:NO];
    }
    [self refreshCpu];
    if ([self tabVisible:TAB_DISASM]) [self refreshDisasm];
}

- (void)flagClicked:(NSButton *)sender
{
    /* N V - B D I Z C: bit 5 has no flag to set */
    static const int flagRegs[NUM_FLAGS] = {
        APPLE2_FLAG_N, APPLE2_FLAG_V, -1, APPLE2_FLAG_B, APPLE2_FLAG_D, APPLE2_FLAG_I, APPLE2_FLAG_Z, APPLE2_FLAG_C
    };
    if (sender.tag >= 0 && sender.tag < NUM_FLAGS && flagRegs[sender.tag] >= 0 && apple2debug_is_stopped(_dbg)) {
        apple2debug_cpu_set(_dbg, flagRegs[sender.tag], sender.state == NSControlStateValueOn ? 1 : 0);
    } else {
        [self message:@"Stop the machine (F5) to edit flags" console:NO];
    }
    [self refreshCpu];
}

/* Brings a range into the dump (a couple of rows down from the top) and
 * highlights it. */
- (void)showMemLo:(int)lo hi:(int)hi
{
    _memBase = (lo & ~0xF) - 2 * 16;
    _memHl = YES;
    _memHlLo = lo;
    _memHlHi = hi;
    [self refreshMem];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    uint16_t lo, hi;
    if (!apple2sym_parse_range(_dbg, _memGoto.stringValue.UTF8String, &lo, &hi)) {
        [self message:[NSString stringWithFormat:@"Go to %@: not an address, label or range", _memGoto.stringValue]
              console:NO];
        return;
    }
    [self showMemLo:lo hi:hi];
}

- (void)memPrev:(id)sender { (void)sender; _memBase -= MEM_BYTES; [self refreshMem]; }
- (void)memNext:(id)sender { (void)sender; _memBase += MEM_BYTES; [self refreshMem]; }

/* "address value [value...]": the values land at consecutive addresses. */
- (void)writeMem:(id)sender
{
    (void)sender;
    NSArray<NSString *> *words = [_memWrite.stringValue componentsSeparatedByCharactersInSet:
        [NSCharacterSet whitespaceCharacterSet]];
    NSMutableArray<NSString *> *tok = [NSMutableArray array];
    for (NSString *w in words) if (w.length) [tok addObject:w];
    NSString *why = nil;
    const int a = tok.count > 0 ? [self resolveAddr:tok[0] why:&why] : -1;
    if (tok.count < 2 || a < 0) {
        [self message:@"Poke: address value [value...]" console:NO];
        return;
    }
    for (NSUInteger i = 1; i < tok.count; i++) {
        long v;
        if (![self evaluate:tok[i] into:&v why:&why] || v < 0 || v > 0xFF || a + (int)i - 1 > 0xFFFF) {
            [self message:[NSString stringWithFormat:@"Not a byte: %@", tok[i]] console:NO];
            return;
        }
        apple2debug_write(_dbg, (uint16_t)(a + (int)i - 1), (uint8_t)v);
    }
    _memWrite.stringValue = @"";
    if (a < _memBase || a >= _memBase + MEM_BYTES) [self showMemLo:a hi:a];
    else [self refreshMem];
}

- (void)followToggled:(id)sender
{
    (void)sender;
    _scrolledAway = NO;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_jump.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Jump to %@: %@", _jump.stringValue, why] console:NO];
        return;
    }
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)addr;
    _selected = addr;
    [self refreshDisasm];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[4] = { APPLE2DEBUG_BP_EXEC, APPLE2DEBUG_BP_READ, APPLE2DEBUG_BP_WRITE,
                                  APPLE2DEBUG_BP_READ | APPLE2DEBUG_BP_WRITE };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 4 ? sel : 0];
    NSString *what = @"address";
    NSString *startText = [_bpStart.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *endText = [_bpEnd.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (startText.length == 0) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Give a %@", what];
        return;
    }
    NSString *why = nil;
    const int start = [self resolveAddr:startText why:&why];
    const int end = endText.length ? [self resolveAddr:endText why:&why] : start;
    if (start < 0 || end < 0 || end < start) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Bad %@ range: %@%@%@%@%@", what, startText,
                              endText.length ? @" - " : @"", endText, why.length ? @" (" : @"",
                              why.length ? [why stringByAppendingString:@")"] : @""];
        return;
    }
    char err[256];
    err[0] = '\0';
    const int bpId = apple2debug_breakpoint_add(_dbg, type, (uint16_t)start, (uint16_t)end,
                                               _bpCond.stringValue.UTF8String, err, (int)sizeof err);
    if (bpId < 0) {
        NSString *e = cstr(err);
        _bpErr.stringValue = e.length ? e : @"The breakpoint was refused";
        return;
    }
    _bpErr.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshNow];
}

- (void)bpEnabled:(NSButton *)sender
{
    apple2debug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshNow];
}

- (void)bpRemove:(NSButton *)sender
{
    apple2debug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshNow];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    apple2debug_breakpoint_clear(_dbg);
    [self refreshNow];
}

- (void)clearHits:(id)sender
{
    (void)sender;
    apple2debug_breakpoint_clear_hits(_dbg);
    _bpSig = nil;       /* the hit counts redraw */
    [self refreshNow];
}

/* ---- watches, search, trace, pop out ---------------------------------------------- */

- (void)addWatch:(id)sender
{
    (void)sender;
    NSString *text = [_wtEntry.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (text.length == 0 || apple2watch_add(text.UTF8String) < 0) {
        [self message:@"Type an address, label, expression or lo-hi range to watch" console:NO];
        return;
    }
    _wtEntry.stringValue = @"";
    [self refreshWatches];
}

/* A Write breakpoint over lo..hi that fires when the value changes. */
- (void)breakOnChangeLo:(uint16_t)lo hi:(uint16_t)hi
{
    char err[256];
    err[0] = '\0';
    if (apple2debug_breakpoint_add(_dbg, APPLE2DEBUG_BP_WRITE, lo, hi, "value != old", err, (int)sizeof err) < 0) {
        NSString *e = cstr(err);
        [self message:e.length ? e : @"The breakpoint was refused" console:NO];
        return;
    }
    [self message:@"Breaking when it changes" console:NO];
    [self refreshNow];
}

/* Selects a tab, or raises the window it was popped out into. */
- (void)showTab:(int)tab
{
    if (_popped[tab]) {
        [_popped[tab] makeKeyAndOrderFront:nil];
        return;
    }
    [_tabs selectTabViewItem:_items[tab]];
}

- (void)wtMemory:(NSButton *)sender
{
    uint16_t lo, hi;
    if (!apple2watch_range(_dbg, (int)sender.tag, &lo, &hi)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self showTab:TAB_MEMORY];
    [self showMemLo:lo hi:hi];
}

- (void)wtBreak:(NSButton *)sender
{
    uint16_t lo, hi;
    if (!apple2watch_range(_dbg, (int)sender.tag, &lo, &hi)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self breakOnChangeLo:lo hi:hi];
}

- (void)wtRemove:(NSButton *)sender
{
    apple2watch_remove((int)sender.tag);
    [self refreshWatches];
}

- (void)searchBackward:(BOOL)backward
{
    uint8_t pat[APPLE2SEARCH_MAX], mask = 0xFF;
    int n = 0, flags = 0;
    if (!apple2search_parse((int)_srchKind.indexOfSelectedItem, _srchEntry.stringValue.UTF8String, pat, &n, &mask,
                           &flags) || n <= 0) {
        _srchNote.stringValue = @"Type hex bytes (A9 00) or some text.";
        return;
    }
    if (!apple2debug_is_stopped(_dbg)) {
        _srchNote.stringValue = @"Stop the machine first.";
        return;
    }
    const uint16_t from = _memHl ? (backward ? (uint16_t)_memHlLo : (uint16_t)(_memHlLo + 1)) : (uint16_t)_memBase;
    uint16_t found = 0;
    const int r = apple2debug_search_next(_dbg, pat, n, mask, flags, from, backward ? 1 : 0, &found);
    if (r < 0) {
        _srchNote.stringValue = @"Stop the machine first.";
    } else if (r == 0) {
        _srchNote.stringValue = @"Not found.";
    } else {
        _srchNote.stringValue = [NSString stringWithFormat:@"Found at $%04X", found];
        [self showMemLo:found hi:found + n - 1];
    }
}

- (void)searchNext:(id)sender { (void)sender; [self searchBackward:NO]; }
- (void)searchPrev:(id)sender { (void)sender; [self searchBackward:YES]; }

/* Both Record buttons follow the engine's one recorder. */
- (void)syncRecord:(BOOL)on
{
    for (int k = 0; k < 2; k++) {
        const NSInteger want = on ? NSControlStateValueOn : NSControlStateValueOff;
        if (_hRec[k].state != want) {
            _hRec[k].state = want;
            setButtonAccent(_hRec[k], on);
        }
    }
}

- (void)histRec:(NSButton *)sender
{
    apple2debug_trace_enable(_dbg, sender.state == NSControlStateValueOn);
    [self syncRecord:apple2debug_trace_enabled(_dbg) != 0];
    [self refreshTab];
}

- (void)histChanged:(id)sender
{
    (void)sender;
    [self refreshTab];
}

- (void)histClear:(id)sender
{
    (void)sender;
    apple2debug_trace_clear(_dbg);
    [self refreshTab];
}

- (void)popOutCurrent:(id)sender
{
    (void)sender;
    const NSInteger t = [self currentTab];
    if (t >= 0) [self popOutTab:(int)t];
}

/* Moves a tab into a window of its own. One tab always stays. */
- (void)popOutTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS || _popped[tab] || _tabs.numberOfTabViewItems <= 1) return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    [_tabs removeTabViewItem:item];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 580)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = [@"Apple II Debugger - " stringByAppendingString:kTabTitles[tab]];
    win.releasedWhenClosed = NO;
    win.delegate = self;
    NSButton *dockBtn = [self button:@"Dock" action:@selector(dockButton:)];
    dockBtn.tag = tab;
    dockBtn.toolTip = @"Put this tab back in the debugger";
    NSView *root = [[NSView alloc] init];
    dockBtn.translatesAutoresizingMaskIntoConstraints = NO;
    view.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:dockBtn];
    [root addSubview:view];
    [dockBtn.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [dockBtn.topAnchor constraintEqualToAnchor:root.topAnchor constant:8].active = YES;
    [view.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [view.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8].active = YES;
    [view.topAnchor constraintEqualToAnchor:dockBtn.bottomAnchor constant:6].active = YES;
    [view.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8].active = YES;
    win.contentView = root;
    Apple2SetHelpContext(win, [@"debugger/" stringByAppendingString:kHelpTabs[tab]]);
    _popped[tab] = win;
    [win center];
    [win makeKeyAndOrderFront:nil];
    [self updateHelpContext];
    [self refreshTab];
}

- (void)dockButton:(id)sender
{
    [self dockTab:(int)((NSButton *)sender).tag];
}

/* Puts a popped-out tab back where it was in the order. */
- (void)dockTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS) return;
    NSWindow *win = _popped[tab];
    if (!win) return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    int pos = 0;
    _popped[tab] = nil;
    win.delegate = nil;
    [view removeFromSuperview];
    view.translatesAutoresizingMaskIntoConstraints = YES;     /* the tab view sizes it again */
    item.view = view;
    for (int t = 0; t < tab; t++)
        if (!_popped[t]) pos++;
    [_tabs insertTabViewItem:item atIndex:pos];
    [_tabs selectTabViewItem:item];
    [win orderOut:nil];
    [self updateHelpContext];
    [self refreshTab];
}

/* ---- refreshers ------------------------------------------------------------------ */

/* Everything the window shows, read back now: the status and the visible
 * tab. The poll compares against what was seen here. */
- (void)refreshNow
{
    _seenGen = apple2debug_generation(_dbg);
    _wasStopped = apple2debug_is_stopped(_dbg) != 0;
    _liveTicks = 0;
    [self refreshStatus];
    [self refreshTab];
}

/* The registers, and each tab on show (the notebook's page and any popped-out
 * window): the others refresh when they are selected. */
- (void)refreshTab
{
    [self refreshCpu];
    for (int t = 0; t < NUM_TABS; t++)
        if ([self tabVisible:t]) [self refreshTabAt:t];
}

- (void)refreshTabAt:(int)tab
{
    switch (tab) {
    case TAB_DISASM: [self refreshDisasm]; break;
    case TAB_MEMORY: [self refreshMem]; break;
    case TAB_BREAKS: [self refreshBps]; break;
    case TAB_WATCHES: [self refreshWatches]; break;
    case TAB_SLOTS: [self refreshSlots]; break;
    case TAB_TRACE: [self refreshHist:0]; break;
    case TAB_HIST: [self refreshHist:1]; break;
    case TAB_VIDEO: [self refreshVideo]; break;
    case TAB_FUJINET: [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshStatus
{
    char reason[160];
    int addr = -1;
    const BOOL stopped = apple2debug_is_stopped(_dbg) != 0;
    reason[0] = '\0';
    apple2debug_stop_reason(_dbg, reason, (int)sizeof reason, &addr);
    NSString *why = cstr(reason);
    NSMutableString *text = [NSMutableString stringWithString:stopped ? @"Stopped" : @"Running"];
    if (stopped) {
        /* "stopped" says nothing the word does not; a reason does */
        if (why.length && ![why isEqualToString:@"stopped"]) [text appendFormat:@": %@", why];
        if (addr >= 0) {
            uint16_t off = 0;
            const char *sym = apple2debug_symbol_at(_dbg, (uint16_t)addr, &off);
            [text appendFormat:@"   PC $%04X", addr & 0xFFFF];
            if (sym && off < 0x100) [text appendFormat:@"  %@%@", cstr(sym), off ? [NSString stringWithFormat:@"+%X", off] : @""];
        }
    }
    _status.stringValue = text;
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    setButtonAccent(_runBtn, stopped);    /* the accent while stopped */
}

- (void)refreshCpu
{
    apple2debug_cpu c;
    apple2debug_cpu_get(_dbg, &c);
    const int vals[NUM_REGS] = { c.pc, c.sp, c.a, c.x, c.y, c.p };
    for (int i = 0; i < NUM_REGS; i++)
        if (!_reg[i].currentEditor)         /* never over a field being edited */
            _reg[i].stringValue = [NSString stringWithFormat:(i < 2 ? @"%04X" : @"%02X"), vals[i] & (i < 2 ? 0xFFFF : 0xFF)];
    /* N V - B D I Z C: bit 5 is always set, shown ticked and greyed */
    const int flags[NUM_FLAGS] = { c.nf, c.vf, 1, c.bf, c.df, c.if_, c.zf, c.cf };
    for (int i = 0; i < NUM_FLAGS; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _beam.stringValue = [NSString stringWithFormat:@"%@   line %d   horz %d   frame %u   cycles %llu%@",
                         c.is_65c02 ? @"65C02" : @"6502", c.line, c.horz, c.frame, (unsigned long long)c.cycles,
                         c.jammed ? @"   JAMMED" : @""];
}

- (void)refreshMem
{
    uint8_t data[MEM_BYTES];
    if (_memBase > 0x10000 - MEM_BYTES) _memBase = 0x10000 - MEM_BYTES;
    if (_memBase < 0) _memBase = 0;
    memset(data, 0, sizeof data);
    apple2debug_read(_dbg, (uint16_t)_memBase, data, MEM_BYTES);
    NSMutableString *text = [NSMutableString stringWithString:@"      0  1  2  3  4  5  6  7   8  9  A  B  C  D  E  F\n"];
    NSMutableArray<NSValue *> *marks = [NSMutableArray array];     /* the highlighted bytes' cells */
    for (int row = 0; row < MEM_ROWS; row++) {
        const int base = _memBase + row * 16;
        [text appendFormat:@"%04X  ", base];
        for (int col = 0; col < 16; col++) {
            const int addr = base + col;
            if (col == 8) [text appendString:@" "];
            if (_memHl && addr >= _memHlLo && addr <= _memHlHi)
                [marks addObject:[NSValue valueWithRange:NSMakeRange(text.length, 2)]];
            [text appendFormat:@"%02X ", data[row * 16 + col]];
        }
        [text appendString:@" "];
        for (int col = 0; col < 16; col++) {
            const int addr = base + col;
            const uint8_t c = data[row * 16 + col] & 0x7F;      /* Apple II text carries the high bit */
            if (_memHl && addr >= _memHlLo && addr <= _memHlHi)
                [marks addObject:[NSValue valueWithRange:NSMakeRange(text.length, 1)]];
            [text appendFormat:@"%c", (c >= 0x20 && c < 0x7F) ? (char)c : '.'];
        }
        [text appendString:@"\n"];
    }
    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (NSValue *v in marks) [as addAttributes:accentAttrs() range:v.rangeValue];
    setStyledText(_mem, as);
}

- (void)refreshDisasm
{
    static apple2debug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        apple2debug_cpu c;
        apple2debug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)apple2debug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int n = apple2debug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    if (n < 0) n = 0;
    if (n > DISASM_WINDOW) n = DISASM_WINDOW;
    _lineCount = n;

    /* "●> 8033  CD 50 80     LABEL            CALL $8050   ; note": the
     * ranges are the line, its label and its comment, for the colours */
    NSMutableString *text = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW], labels[DISASM_WINDOW], notes[DISASM_WINDOW];
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        /* a red dot for a breakpoint, a grey ring for a disabled one */
        NSString *gutter = !lines[i].has_breakpoint ? @" " : (lines[i].breakpoint_enabled ? @"●" : @"○");
        NSString *label = cstr(lines[i].label), *comment = cstr(lines[i].comment);
        NSMutableString *line = [NSMutableString stringWithFormat:@"%@%c %04X  ", gutter,
                                 lines[i].is_pc ? '>' : ' ', lines[i].address];
        [line appendString:padTo(cstr(lines[i].bytes), 11)];
        [line appendString:@"  "];
        labels[i] = NSMakeRange(text.length + line.length, label.length);
        [line appendString:padTo(label, 16)];
        [line appendString:@" "];
        [line appendString:padTo(cstr(lines[i].disasm), 24)];
        notes[i] = NSMakeRange(NSNotFound, 0);
        if (comment.length) {
            notes[i] = NSMakeRange(text.length + line.length, comment.length + 3);
            [line appendString:@" ; "];
            [line appendString:comment];
        }
        ranges[i] = NSMakeRange(text.length, line.length);
        [text appendString:line];
        [text appendString:@"\n"];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];

    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (int i = 0; i < n; i++) {
        /* labels and comments grey */
        if (labels[i].length)
            [as addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor range:labels[i]];
        if (notes[i].location != NSNotFound)
            [as addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor range:notes[i]];
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[i].has_breakpoint)
            [as addAttribute:NSForegroundColorAttributeName
                       value:lines[i].breakpoint_enabled ? breakpointRed() : NSColor.systemGrayColor
                       range:NSMakeRange(ranges[i].location, 1)];
        /* the selected line underlined on a selection fill */
        if ((int)lines[i].address == _selected)
            [as addAttributes:@{ NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle),
                                 NSBackgroundColorAttributeName: NSColor.selectedTextBackgroundColor }
                        range:ranges[i]];
    }
    /* the PC's line in the accent colour with white text, last so its
     * colours win */
    for (int i = 0; i < n; i++) {
        if (!lines[i].is_pc) continue;
        [as addAttributes:accentAttrs() range:ranges[i]];
        if ((int)lines[i].address == _selected)
            [as addAttribute:NSUnderlineStyleAttributeName value:@(NSUnderlineStyleSingle) range:ranges[i]];
    }
    [_disasm.textStorage setAttributedString:as];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

/* Decodes whichever page the popup names into the picture. */
- (void)viewPickChanged:(id)sender
{
    (void)sender;
    _pin.valid = 0;     /* another page: the pinned point means something else */
    _hov.valid = 0;
    [self refreshVideo];
}

- (void)refreshVideo
{
    const NSInteger sel = _viewPick.indexOfSelectedItem;
    if (!_viewFb || !_viewPic || sel < 0) return;
    if (apple2debug_render_view(_dbg, (apple2debug_view)sel, _viewFb) != 0) {
        _viewNote.stringValue = @"(no such page)";
        return;
    }
    _viewNote.stringValue = [NSString stringWithFormat:@"decoded from $%04X, as the CPU sees memory now",
                             apple2debug_view_base((apple2debug_view)sel)];
    /* the pin and the hover are read again: their bytes change as the machine runs */
    if (_pin.valid &&
        (apple2debug_view_inspect(_dbg, (apple2debug_view)sel, _pinX, _pinY, &_pin) != 0 || !_pin.valid))
        _pin.valid = 0;
    if (_hov.valid &&
        (apple2debug_view_inspect(_dbg, (apple2debug_view)sel, _hovX, _hovY, &_hov) != 0 || !_hov.valid))
        _hov.valid = 0;
    [self redrawVideo];
}

/* The cached page with the outlines drawn in (pin yellow, hover white), as
 * the picture, and the two labels. No re-render: a mouse move calls this. */
- (void)redrawVideo
{
    if (!_viewFb || !_viewDraw || !_viewPic) return;
    memcpy(_viewDraw, _viewFb, (size_t)APPLE2VIEW_WIDTH * APPLE2VIEW_HEIGHT * sizeof(uint32_t));
    if (_pin.valid) outlineRect(_viewDraw, _pin.rx, _pin.ry, _pin.rw, _pin.rh, 0x00FFE000);
    if (_hov.valid) outlineRect(_viewDraw, _hov.rx, _hov.ry, _hov.rw, _hov.rh, 0x00FFFFFF);
    NSImage *img = imageFromXRGB(_viewDraw, APPLE2VIEW_WIDTH, APPLE2VIEW_HEIGHT, APPLE2VIEW_WIDTH);
    if (img) _viewPic.image = img;
    _viewHover.stringValue = _hov.valid ? cstr(_hov.text)
                                        : @"Hover the picture to inspect it";
    _viewPinned.stringValue = _pin.valid
        ? [NSString stringWithFormat:@"Pinned:\n%@", cstr(_pin.text)]
        : @"";
}

- (void)videoHoverX:(int)x y:(int)y
{
    const NSInteger sel = _viewPick.indexOfSelectedItem;
    if (x < 0 || sel < 0 ||
        apple2debug_view_inspect(_dbg, (apple2debug_view)sel, x, y, &_hov) != 0 || !_hov.valid) {
        if (!_hov.valid && x < 0) return;       /* nothing changes */
        _hov.valid = 0;
    } else {
        _hovX = x;
        _hovY = y;
    }
    [self redrawVideo];
}

- (void)videoClickX:(int)x y:(int)y clicks:(NSInteger)clicks
{
    const NSInteger sel = _viewPick.indexOfSelectedItem;
    apple2debug_pixel px;
    if (sel < 0 || apple2debug_view_inspect(_dbg, (apple2debug_view)sel, x, y, &px) != 0 || !px.valid) return;
    const BOOL same = _pin.valid && px.rx == _pin.rx && px.ry == _pin.ry && px.rw == _pin.rw && px.rh == _pin.rh;
    _hov = px;
    _hovX = x;
    _hovY = y;
    if (clicks >= 2) {
        /* the first click of a double already toggled the pin: keep it pinned */
        if (!same) {
            _pin = px;
            _pinX = x;
            _pinY = y;
        }
        [self videoBreakOnWrites:px.addr];
    } else if (same) {
        _pin.valid = 0;
    } else {
        _pin = px;
        _pinX = x;
        _pinY = y;
    }
    [self redrawVideo];
}

/* A write breakpoint on one byte, unless an enabled unconditional one is there. */
- (void)videoBreakOnWrites:(uint16_t)addr
{
    static apple2debug_breakpoint bps[MAX_BPS];
    int n = apple2debug_breakpoint_list(_dbg, bps, MAX_BPS);
    if (n < 0) n = 0;
    if (n > MAX_BPS) n = MAX_BPS;
    BOOL have = NO;
    for (int i = 0; i < n; i++)
        if (bps[i].enabled && bps[i].type == APPLE2DEBUG_BP_WRITE && bps[i].start == addr && bps[i].end == addr &&
            bps[i].condition[0] == '\0')
            have = YES;
    if (!have) {
        char err[256];
        err[0] = '\0';
        if (apple2debug_breakpoint_add(_dbg, APPLE2DEBUG_BP_WRITE, addr, addr, "", err, (int)sizeof err) < 0) {
            NSString *e = cstr(err);
            _viewStatus.stringValue = e.length ? e : @"The breakpoint was refused";
            return;
        }
    }
    _viewStatus.stringValue = [NSString stringWithFormat:@"Break on writes to $%04X", addr];
}

- (void)videoShowInMemory:(id)sender
{
    (void)sender;
    const apple2debug_pixel *px = _pin.valid ? &_pin : (_hov.valid ? &_hov : NULL);
    if (!px) {
        _viewStatus.stringValue = @"Pin or hover a byte first";
        return;
    }
    const int addr = px->addr;
    [self showTab:TAB_MEMORY];
    [self showMemLo:addr hi:addr];
}

- (void)videoUnpin:(id)sender
{
    (void)sender;
    _pin.valid = 0;
    [self redrawVideo];
}

- (void)refreshBps
{
    static apple2debug_breakpoint bps[MAX_BPS];
    int n = apple2debug_breakpoint_list(_dbg, bps, MAX_BPS);
    if (n < 0) n = 0;
    if (n > MAX_BPS) n = MAX_BPS;

    /* the first line: why it stopped, or that it runs */
    {
        char reason[160];
        int where = -1;
        reason[0] = '\0';
        apple2debug_stop_reason(_dbg, reason, (int)sizeof reason, &where);
        NSString *why = cstr(reason);
        _bpStop.stringValue = apple2debug_is_stopped(_dbg)
            ? [NSString stringWithFormat:@"Stopped: %@", why.length ? why : @"stopped"]
            : @"Running.";
    }

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click) */
    NSMutableString *sig = [NSMutableString string];
    NSMutableArray<NSString *> *descs = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        char desc[256];
        desc[0] = '\0';
        apple2bp_describe(_dbg, &bps[i], desc, (int)sizeof desc);
        [descs addObject:cstr(desc)];
        [sig appendFormat:@"%d %d %u %@\n", bps[i].id, bps[i].enabled, bps[i].hits, descs[(NSUInteger)i]];
    }
    if ([sig isEqualToString:_bpSig]) return;
    _bpSig = sig;

    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int i = 0; i < n; i++) {
        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[i].id]
                                             target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = monoFont();
        [box.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [remove.widthAnchor constraintEqualToConstant:70].active = YES;
        NSStackView *row = hstack(@[box, bpCell(descs[(NSUInteger)i], 0, NO),
                                    bpCell([NSString stringWithFormat:@"%u", bps[i].hits], 60, NO), remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshWatches
{
    const int n = apple2watch_count();
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSMutableString *sig = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        char buf[256];
        buf[0] = '\0';
        apple2watch_format(_dbg, i, buf, (int)sizeof buf);
        [lines addObject:cstr(buf)];
        [sig appendFormat:@"%@\n", lines[(NSUInteger)i]];
    }
    [sig appendFormat:@"%u", apple2watch_generation()];
    if ([sig isEqualToString:_wtSig]) return;      /* a refresh must not eat a click */
    _wtSig = sig;

    for (NSView *v in [_wtList.arrangedSubviews copy]) [_wtList removeView:v];
    if (n == 0) [_wtList addArrangedSubview:[self label:@"No watches"]];
    for (int i = 0; i < n; i++) {
        NSButton *mem = [self button:@"Memory" action:@selector(wtMemory:)];
        mem.tag = i;
        NSButton *brk = [self button:@"Break on change" action:@selector(wtBreak:)];
        brk.tag = i;
        NSButton *remove = [self button:@"Remove" action:@selector(wtRemove:)];
        remove.tag = i;
        NSStackView *row = hstack(@[bpCell(lines[(NSUInteger)i], 0, NO), mem, brk, remove]);
        row.distribution = NSStackViewDistributionFill;
        [_wtList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_wtList.widthAnchor].active = YES;
    }
}

/* The slots, the memory map and the soft switches. */
- (void)refreshSlots
{
    static char io[8192];
    static apple2debug_slot slots[MAX_SLOTS];
    static apple2debug_region regions[MAX_REGIONS];

    int ns = apple2debug_slots(_dbg, slots, MAX_SLOTS);
    if (ns < 0) ns = 0;
    if (ns > MAX_SLOTS) ns = MAX_SLOTS;
    NSMutableArray<NSArray<NSString *> *> *slotRows = [NSMutableArray array];
    for (int i = 0; i < ns; i++) {
        NSString *card = cstr(slots[i].card);
        [slotRows addObject:@[slots[i].slot == 8 ? @"Aux" : [NSString stringWithFormat:@"%d", slots[i].slot],
                              card.length ? card : @"(empty)", slots[i].fujinet ? @"FujiNet" : @""]];
    }
    int nr = apple2debug_memory_map(_dbg, regions, MAX_REGIONS);
    if (nr < 0) nr = 0;
    if (nr > MAX_REGIONS) nr = MAX_REGIONS;
    NSMutableArray<NSArray<NSString *> *> *mapRows = [NSMutableArray array];
    for (int i = 0; i < nr; i++)
        [mapRows addObject:@[[NSString stringWithFormat:@"%04X-%04X", regions[i].lo, regions[i].hi],
                             [NSString stringWithFormat:@"%c%c", regions[i].readable ? 'R' : '-',
                              regions[i].writable ? 'W' : '-'],
                             cstr(regions[i].name)]];
    if (![slotRows isEqualToArray:_slotRows]) {
        _slotRows = slotRows;
        [_slotTable reloadData];
    }
    if (![mapRows isEqualToArray:_mapRows]) {
        _mapRows = mapRows;
        [_mapTable reloadData];
    }

    io[0] = '\0';
    apple2debug_io_state(_dbg, io, (int)sizeof(io));
    setText(_ioText, cstr(io), NO);
}

/* Trace (idx 0) and History (idx 1): the newest HIST_SHOWN lines of the
 * engine's ring that pass the filter, oldest first. */
- (void)refreshHist:(int)idx
{
    static apple2debug_trace ring[HIST_RING];
    static int hit[HIST_SHOWN];
    const BOOL on = apple2debug_trace_enabled(_dbg) != 0;
    [self syncRecord:on];

    uint16_t lo = 0, hi = 0xFFFF;
    int filter = APPLE2DEBUG_TRACE_ALL;
    BOOL badRange = NO;
    switch (_hFilter[idx].indexOfSelectedItem) {
    case 1: filter = APPLE2DEBUG_TRACE_BRANCHES; break;
    case 2:
        if (apple2sym_parse_range(_dbg, _hRange[idx].stringValue.UTF8String, &lo, &hi)) filter = APPLE2DEBUG_TRACE_RANGE;
        else badRange = YES;
        break;
    default: break;
    }
    const int total = apple2debug_trace_count(_dbg);
    /* newest first: unfiltered, only the lines shown are needed */
    int n = apple2debug_trace_read(_dbg, ring, filter == APPLE2DEBUG_TRACE_ALL ? HIST_SHOWN : HIST_RING);
    if (n < 0) n = 0;
    int shown = 0;
    for (int i = 0; i < n && shown < HIST_SHOWN; i++)
        if (apple2debug_trace_match(&ring[i], filter, lo, hi)) hit[shown++] = i;

    NSMutableString *t = [NSMutableString string];
    for (int k = shown - 1; k >= 0; k--) {
        char line[256];
        line[0] = '\0';
        apple2debug_trace_format(&ring[hit[k]], line, (int)sizeof line);
        [t appendString:cstr(line)];
        [t appendString:@"\n"];
    }
    NSString *note;
    if (!on && total == 0)
        note = @"recording off";
    else
        note = [NSString stringWithFormat:@"%@, %d instructions%@", on ? @"recording" : @"recording off", total,
                shown < total ? [NSString stringWithFormat:@" (showing %d)", shown] : @""];
    if (badRange) note = [note stringByAppendingString:@" (range not understood: showing all)"];
    _hNote[idx].stringValue = note;
    setText(_hText[idx], t, YES);
}

/* The log is rebuilt only when it changed: a new packet, or a repeat folded
 * into the newest. */
- (void)refreshFujinet:(BOOL)force
{
    static apple2_fuji_packet p[TRACE_MAX];
    static char state[4096];
    int n = apple2debug_fujinet_trace(_dbg, p, TRACE_MAX);
    if (n < 0) n = 0;

    const int slot = apple2session_fujinet_slot(_session);
    if (!slot)
        _fujiNote.stringValue = @"FujiNet is off (Preferences ▸ Slots).";
    else if (apple2session_fujinet_external(_session))
        _fujiNote.stringValue = [NSString stringWithFormat:@"External FujiNet on the slot %d SmartPort card, listening at %s, %@.",
                                 slot, apple2session_slip_endpoint(_session),
                                 apple2session_fujinet_link(_session) ? @"connected" : @"not connected"];
    else if (!apple2session_fujinet_running(_session))
        _fujiNote.stringValue = [NSString stringWithFormat:@"FujiNet's card is in slot %d, but FujiNet is not running.", slot];
    else
        _fujiNote.stringValue = [NSString stringWithFormat:@"FujiNet is on the slot %d SmartPort card, %@.", slot,
                                 apple2session_fujinet_link(_session) ? @"connected" : @"not connected"];
    state[0] = '\0';
    apple2debug_fujinet_state(_dbg, state, (int)sizeof(state));
    setText(_fujiState, cstr(state), NO);

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast))
        return;
    const BOOL follow = force || _fujiCount < 0 || atEnd(_fujiText);
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        char hex[sizeof(p[i].data) * 3 + 8];
        int len = 0;
        hex[0] = '\0';
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof(p[i].data); k++)
            len += snprintf(hex + len, sizeof(hex) - (size_t)len, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof(p[i].data))
            snprintf(hex + len, sizeof(hex) - (size_t)len, "\xe2\x80\xa6");
        if (i) [text appendString:@"\n"];
        [text appendString:cstr(p[i].text)];
        [text appendString:@"\n     "];
        [text appendString:cstr(hex)];
    }
    if (n == 0) [text appendString:@"(no SmartPort packets since the trace was cleared)"];
    setText(_fujiText, text, follow);
}

@end

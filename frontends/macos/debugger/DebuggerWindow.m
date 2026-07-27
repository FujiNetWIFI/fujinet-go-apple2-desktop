/*
 * Debugger window (AppKit): run control, 6502 registers, disassembly with
 * click-to-toggle breakpoints, and a memory view -- the same shape as the
 * GNOME, KDE and Win32 debuggers, over the same shared engine
 * (core/debugger).
 *
 * The engine's rule is that only the emulator thread touches AppleWin, and
 * its stop callback therefore FIRES ON THE EMULATOR THREAD. It is marshalled
 * onto the main queue with dispatch_async before any view is touched.
 *
 * Keys match the rest of the family: F5 pause/continue, F7 step into,
 * F8 step over, Shift+F8 step out.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

#include "apple2debug.h"

#define DISASM_LINES 24
#define MEM_COLS 16
#define MEM_ROWS 24

static DebuggerWindow *g_debugger;

/* Disassembly text view: a click toggles the breakpoint on the clicked line.
 * Each row is "%c%c $%04X  ..." -- the breakpoint marker, the PC marker, a
 * space, then "$" and four hex digits starting at index 4. */
@interface DasmTextView : NSTextView
@property(nonatomic, copy) void (^onToggleAddr)(uint16_t addr);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length)
        idx = text.length;
    NSUInteger start = [text lineRangeForRange:NSMakeRange(idx, 0)].location;
    NSString *line = [text substringFromIndex:start];
    if (line.length >= 8) {
        unsigned addr = 0;
        NSScanner *scan = [NSScanner
            scannerWithString:[line substringWithRange:NSMakeRange(4, 4)]];
        if ([scan scanHexInt:&addr] && addr <= 0xFFFF && self.onToggleAddr)
            self.onToggleAddr((uint16_t)addr);
    }
}
@end

@interface DebuggerWindow () <NSWindowDelegate>
- (instancetype)initWithSession:(apple2session *)session;
- (void)onStopped:(apple2debug_stop_reason)reason pc:(uint16_t)pc;
@end

@implementation DebuggerWindow {
    apple2session *_session;
    apple2debug *_dbg;
    NSWindow *_window;

    NSButton *_pauseBtn;
    NSTextField *_status;
    DasmTextView *_disasm;
    uint16_t _disasmBase;
    BOOL _followPc;

    NSTextView *_regs;
    NSTextField *_memAddr;
    NSTextView *_memView;
    uint16_t _memBase;

    NSTimer *_tick;
    id _keyMonitor;
}

/* ---- helpers ------------------------------------------------------------- */

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

/* A read-only monospace text view inside a scroll view. `cls` is NSTextView
 * or the clickable DasmTextView subclass. The caller picks the text view back
 * up through scroll.documentView -- rather than an out-parameter, which ARC
 * will not accept the address of a __strong ivar for. */
static NSScrollView *monoScroll(Class cls, NSRect frame)
{
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;

    NSTextView *view = [[cls alloc] initWithFrame:scroll.contentView.bounds];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    view.minSize = NSMakeSize(0, 0);
    view.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.verticallyResizable = YES;
    view.horizontallyResizable = NO;
    view.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = view;

    return scroll;
}

/* EMULATOR THREAD. Does nothing but hand the event to the main queue. */
static void stop_trampoline(apple2debug_stop_reason reason, uint16_t pc,
                            void *user)
{
    DebuggerWindow *window = (__bridge DebuggerWindow *)user;
    dispatch_async(dispatch_get_main_queue(), ^{
      [window onStopped:reason pc:pc];
    });
}

+ (void)showForSession:(apple2session *)session
{
    if (!g_debugger)
        g_debugger = [[DebuggerWindow alloc] initWithSession:session];
    [g_debugger present];
}

- (void)present
{
    [_window makeKeyAndOrderFront:nil];
}

/* ---- construction -------------------------------------------------------- */

- (NSButton *)button:(NSString *)title action:(SEL)sel frame:(NSRect)frame
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    b.frame = frame;
    b.bezelStyle = NSBezelStyleRounded;
    b.autoresizingMask = NSViewMinYMargin;
    return b;
}

- (instancetype)initWithSession:(apple2session *)session
{
    self = [super init];
    if (!self)
        return nil;
    _session = session;
    _dbg = apple2session_debugger(session);
    _followPc = YES;
    _memBase = 0x0400; /* text page 1, as in the other frontends */

    const CGFloat W = 1240, H = 760;
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, W, H)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    NSView *root = _window.contentView;
    const CGFloat toolbarY = H - 36;

    /* Toolbar. AppKit's origin is bottom-left, so the "top" row sits at a
     * high y and pins itself there with NSViewMinYMargin. */
    _pauseBtn = [self button:@"Pause"
                      action:@selector(toggleRun:)
                       frame:NSMakeRect(12, toolbarY, 100, 26)];
    [root addSubview:_pauseBtn];
    [root addSubview:[self button:@"Into"
                           action:@selector(stepInto:)
                            frame:NSMakeRect(118, toolbarY, 80, 26)]];
    [root addSubview:[self button:@"Over"
                           action:@selector(stepOver:)
                            frame:NSMakeRect(204, toolbarY, 80, 26)]];
    [root addSubview:[self button:@"Out"
                           action:@selector(stepOut:)
                            frame:NSMakeRect(290, toolbarY, 80, 26)]];

    _status = [NSTextField labelWithString:@"Running"];
    _status.frame = NSMakeRect(384, toolbarY + 4, W - 396, 18);
    _status.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [root addSubview:_status];

    /* Disassembly on the left, registers and memory stacked on the right --
     * the same split the other frontends use. */
    const CGFloat bodyTop = toolbarY - 12;   /* y of the top of the body */
    const CGFloat bodyH = bodyTop - 12;
    const CGFloat leftW = 520;

    NSScrollView *dasmScroll =
        monoScroll([DasmTextView class], NSMakeRect(12, 12, leftW, bodyH));
    dasmScroll.autoresizingMask = NSViewHeightSizable;
    _disasm = (DasmTextView *)dasmScroll.documentView;
    __weak DebuggerWindow *weakSelf = self;
    _disasm.onToggleAddr = ^(uint16_t addr) {
      [weakSelf toggleBreakpoint:addr];
    };
    [root addSubview:dasmScroll];

    const CGFloat rx = 12 + leftW + 12;
    const CGFloat rw = W - rx - 12;
    const CGFloat regsH = 88;

    NSTextField *regLabel = [NSTextField labelWithString:@"Registers"];
    regLabel.frame = NSMakeRect(rx, bodyTop - 18, rw, 18);
    regLabel.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [root addSubview:regLabel];

    NSScrollView *regScroll =
        monoScroll([NSTextView class],
                   NSMakeRect(rx, bodyTop - 22 - regsH, rw, regsH));
    regScroll.hasVerticalScroller = NO;
    regScroll.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    _regs = regScroll.documentView;
    [root addSubview:regScroll];

    const CGFloat memLabelY = bodyTop - 22 - regsH - 26;
    NSTextField *memLabel =
        [NSTextField labelWithString:@"Memory (address or symbol, Enter)"];
    memLabel.frame = NSMakeRect(rx, memLabelY, rw, 18);
    memLabel.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [root addSubview:memLabel];

    const CGFloat memAddrY = memLabelY - 26;
    _memAddr = [[NSTextField alloc]
        initWithFrame:NSMakeRect(rx, memAddrY, rw, 22)];
    _memAddr.target = self;
    _memAddr.action = @selector(memAddrEntered:);
    _memAddr.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [root addSubview:_memAddr];

    NSScrollView *memScroll = monoScroll(
        [NSTextView class], NSMakeRect(rx, 12, rw, memAddrY - 12 - 8));
    memScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _memView = memScroll.documentView;
    [root addSubview:memScroll];

    [_window center];

    /* F5/F7/F8/Shift+F8 wherever the focus is inside this window. A local
     * monitor rather than keyDown: on a view, so the keys work with the
     * memory field focused too. */
    _keyMonitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                     handler:^NSEvent *(NSEvent *event) {
                                       DebuggerWindow *s = weakSelf;
                                       if (!s || event.window != s->_window)
                                           return event;
                                       return [s handleKeyEvent:event] ? nil
                                                                       : event;
                                     }];

    if (_dbg) {
        apple2debug_set_stop_callback(_dbg, stop_trampoline,
                                      (__bridge void *)self);
        /* Open paused on purpose: a debugger that opens on a running machine
         * shows a disassembly that is stale before it is drawn. */
        apple2debug_pause(_dbg);
    } else {
        _status.stringValue = @"No debugger (the session is not running)";
    }

    /* While the machine runs, keep the registers live but leave the
     * disassembly and memory alone -- they would be stale before they were
     * drawn, and re-rendering them fights with scrolling. */
    _tick = [NSTimer scheduledTimerWithTimeInterval:0.1
                                            repeats:YES
                                              block:^(NSTimer *timer) {
                                                (void)timer;
                                                [weakSelf onTick];
                                              }];
    [self refreshAll];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor)
        [NSEvent removeMonitor:_keyMonitor];
    if (_dbg)
        apple2debug_set_stop_callback(_dbg, NULL, NULL);
}

/* ---- rendering ----------------------------------------------------------- */

- (void)renderRegs
{
    if (!_dbg)
        return;
    /* Bit 7 is N, bit 0 is C; a set flag shows in upper case and a clear one
     * as a dot, the way every 6502 monitor has always done it. */
    static const char kFlag[] = "NV-BDIZC";
    apple2debug_regs r;
    char flags[9];
    char text[256];

    apple2debug_get_regs(_dbg, &r);
    for (int i = 0; i < 8; i++)
        flags[i] = (r.ps & (0x80 >> i)) ? kFlag[i] : '.';
    flags[8] = '\0';

    /* SP is a full address because that is what AppleWin stores: the 6502's
     * 8-bit stack pointer plus the $0100 page base. */
    snprintf(text, sizeof(text),
             "PC  $%04X\n"
             "A   $%02X      X   $%02X\n"
             "Y   $%02X      SP  $%04X\n"
             "P   $%02X   %s%s",
             r.pc, r.a, r.x, r.y, r.sp, r.ps, flags,
             r.jammed ? "\n\nCPU JAMMED" : "");
    _regs.string = @(text);
}

- (void)renderDisasm
{
    if (!_dbg)
        return;
    apple2dasm_line lines[DISASM_LINES];
    apple2debug_regs r;
    uint16_t next = 0;

    apple2debug_get_regs(_dbg, &r);
    if (_followPc) {
        _disasmBase = r.pc;
        _followPc = NO;
    }

    const int n =
        apple2debug_disassemble(_dbg, _disasmBase, DISASM_LINES, lines, &next);

    NSMutableString *out = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        char bytes[16];
        char text[160];

        bytes[0] = '\0';
        for (int b = 0; b < lines[i].len; b++)
            snprintf(bytes + b * 3, sizeof(bytes) - (size_t)b * 3, "%02X ",
                     lines[i].bytes[b]);

        snprintf(text, sizeof(text), "%c%c $%04X  %-9s %-16s%s%s\n",
                 apple2debug_bp_is_set(_dbg, lines[i].addr) ? '*' : ' ',
                 lines[i].addr == r.pc ? '>' : ' ', lines[i].addr, bytes,
                 lines[i].text, lines[i].symbol ? "  ; " : "",
                 lines[i].symbol ? lines[i].symbol : "");
        [out appendString:@(text)];
    }
    _disasm.string = out;
}

- (void)renderMem
{
    if (!_dbg)
        return;
    NSMutableString *out = [NSMutableString string];
    uint8_t row[MEM_COLS];

    for (int r = 0; r < MEM_ROWS; r++) {
        const uint16_t addr = (uint16_t)(_memBase + r * MEM_COLS);
        char line[32 + MEM_COLS * 4];
        size_t len = 0;

        apple2debug_read_mem(_dbg, addr, row, MEM_COLS);
        len += (size_t)snprintf(line + len, sizeof(line) - len, "$%04X ", addr);
        for (int c = 0; c < MEM_COLS; c++)
            len += (size_t)snprintf(line + len, sizeof(line) - len, "%02X ",
                                    row[c]);
        len += (size_t)snprintf(line + len, sizeof(line) - len, " ");
        for (int c = 0; c < MEM_COLS; c++) {
            /* Apple II text carries the high bit; mask it before deciding
             * whether a byte is printable, or nothing ever looks like text. */
            const uint8_t ch = row[c] & 0x7F;
            len += (size_t)snprintf(line + len, sizeof(line) - len, "%c",
                                    (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.');
        }
        [out appendFormat:@"%s\n", line];
    }
    _memView.string = out;
}

- (void)refreshAll
{
    if (!_dbg)
        return;
    _pauseBtn.title = apple2debug_is_paused(_dbg) ? @"Continue" : @"Pause";
    [self renderRegs];
    [self renderDisasm];
    [self renderMem];
}

- (void)onTick
{
    if (_dbg && !apple2debug_is_paused(_dbg))
        [self renderRegs];
}

/* ---- actions ------------------------------------------------------------- */

- (void)toggleRun:(id)sender
{
    (void)sender;
    if (!_dbg)
        return;
    if (apple2debug_is_paused(_dbg)) {
        apple2debug_resume(_dbg);
        _status.stringValue = @"Running";
        [self refreshAll];
    } else {
        apple2debug_pause(_dbg);
    }
}

- (void)stepInto:(id)sender
{
    (void)sender;
    if (_dbg)
        apple2debug_step_into(_dbg);
}

- (void)stepOver:(id)sender
{
    (void)sender;
    if (_dbg)
        apple2debug_step_over(_dbg);
}

- (void)stepOut:(id)sender
{
    (void)sender;
    if (_dbg)
        apple2debug_step_out(_dbg);
}

- (void)toggleBreakpoint:(uint16_t)addr
{
    if (!_dbg)
        return;
    apple2debug_bp_toggle(_dbg, addr);
    [self renderDisasm];
}

- (void)memAddrEntered:(id)sender
{
    (void)sender;
    if (!_dbg)
        return;

    NSString *raw = _memAddr.stringValue;
    NSString *text = [raw stringByTrimmingCharactersInSet:
                              NSCharacterSet.whitespaceCharacterSet];
    while ([text hasPrefix:@"$"])
        text = [text substringFromIndex:1];
    if (!text.length)
        return;

    uint16_t sym = 0;
    if (apple2debug_symbol_find(_dbg, raw.UTF8String, &sym)) {
        _memBase = sym;
    } else {
        unsigned value = 0;
        NSScanner *scan = [NSScanner scannerWithString:text];
        if (![scan scanHexInt:&value] || !scan.isAtEnd || value > 0xFFFF)
            return;
        _memBase = (uint16_t)value;
    }
    [self renderMem];
}

- (void)onStopped:(apple2debug_stop_reason)reason pc:(uint16_t)pc
{
    static NSString *const kReason[] = {@"Paused", @"Breakpoint", @"Stepped",
                                        @"Ran to address", @"CPU JAMMED"};
    NSString *what = reason <= APPLE2DBG_STOP_JAMMED ? kReason[reason]
                                                     : @"Stopped";
    const char *sym = apple2debug_symbol_at(_dbg, pc);
    _status.stringValue =
        sym ? [NSString stringWithFormat:@"%@ at $%04X  %s", what, pc, sym]
            : [NSString stringWithFormat:@"%@ at $%04X", what, pc];
    _followPc = YES;
    [self refreshAll];
}

- (BOOL)handleKeyEvent:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    if (!chars.length)
        return NO;
    const unichar c = [chars characterAtIndex:0];
    const BOOL shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;

    if (c == NSF5FunctionKey) {
        [self toggleRun:nil];
        return YES;
    }
    if (c == NSF7FunctionKey) {
        [self stepInto:nil];
        return YES;
    }
    if (c == NSF8FunctionKey) {
        if (shift)
            [self stepOut:nil];
        else
            [self stepOver:nil];
        return YES;
    }
    return NO;
}

/* ---- lifecycle ----------------------------------------------------------- */

/* Closing leaves the machine running: hiding a window is not a reason to
 * strand the Apple II paused. The window itself is kept
 * (releasedWhenClosed is NO), so reopening it restores the same state. */
- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    if (_dbg)
        apple2debug_resume(_dbg);
}

@end

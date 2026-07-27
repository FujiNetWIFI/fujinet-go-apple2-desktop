/*
 * DisplayView: paints the emulator's latest frame with CoreGraphics,
 * letterboxed to the chosen aspect. A CVDisplayLink provides the vsync ticks
 * that feed the session's phase-lock (the macOS analog of the GTK frame clock
 * / Qt frameSwapped loop) and schedules main-thread repaints.
 *
 * Two things differ from the ADAM frontend this is modelled on:
 *
 *  - the frame needs no conversion. The session hands out XRGB8888, and a
 *    CGImage asked for kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst
 *    reads exactly that, so the session's buffer is handed to CoreGraphics as
 *    it stands. (ADAM's core produces RGB565 and its view expands every pixel
 *    by hand.)
 *  - the geometry is not fixed. A VidHD card in slot 3 switches the core from
 *    560x384 to 640x400 at runtime, so the frame's own dimensions drive both
 *    the CGImage and the letterbox.
 *
 * Keys are forwarded press AND release, because the core tracks Open Apple
 * and Closed Apple as held modifiers -- and on a Mac those arrive through
 * flagsChanged:, not keyDown:.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

#include <time.h>

/* Virtual key codes for the two Option keys. Spelled out rather than pulled
 * from <Carbon/Carbon.h> (kVK_Option / kVK_RightOption): linking the whole
 * Carbon framework for two integers is not a trade worth making, and these
 * are ANSI keyboard positions fixed since the Apple Extended Keyboard. */
#define VK_OPTION_LEFT  0x3A
#define VK_OPTION_RIGHT 0x3D

@implementation DisplayView {
    apple2session *_session;
    CVDisplayLinkRef _link;
    uint32_t *_fb;
    uint64_t _serial;
    CGImageRef _image;
    int _fbW, _fbH;
    int _aspectMode;
    BOOL _smooth;
    /* Which Apple keys flagsChanged: currently believes are held, so a
     * release is only sent for one that was actually pressed. */
    BOOL _openAppleDown, _closedAppleDown;
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static CVReturn link_cb(CVDisplayLinkRef link, const CVTimeStamp *now,
                        const CVTimeStamp *output, CVOptionFlags flagsIn,
                        CVOptionFlags *flagsOut, void *ctx)
{
    DisplayView *view = (__bridge DisplayView *)ctx;
    (void)link; (void)now; (void)output; (void)flagsIn; (void)flagsOut;
    [view onVsync];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(apple2session *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0,
                                           APPLE2SESSION_FB_MAX_WIDTH * 2,
                                           APPLE2SESSION_FB_MAX_HEIGHT * 2)];
    if (!self)
        return nil;
    _session = session;
    _fb = calloc((size_t)APPLE2SESSION_FB_MAX_WIDTH *
                     APPLE2SESSION_FB_MAX_HEIGHT,
                 sizeof(uint32_t));
    return self;
}

- (void)dealloc
{
    [self stop];
    if (_image)
        CGImageRelease(_image);
    free(_fb);
}

/* CVDisplayLink is deprecated in macOS 15 in favor of NSView.displayLink
 * (macOS 14+), but the retro community runs plenty of older Intel Macs that
 * never see macOS 14 -- so the older API stays, deliberately. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (void)start
{
    if (_link)
        return;
    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, link_cb, (__bridge void *)self);
    CVDisplayLinkStart(_link);
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}
#pragma clang diagnostic pop

- (void)setAspectMode:(int)mode
{
    _aspectMode = mode;
    [self setNeedsDisplay:YES];
}

- (void)setSmooth:(BOOL)smooth
{
    _smooth = smooth;
    [self setNeedsDisplay:YES];
}

/* Display-link thread: feed the phase-lock, then hop to the main thread to
 * pull and paint whatever frame is latest. */
- (void)onVsync
{
    apple2session_notify_vsync(_session, monotonic_ns());
    dispatch_async(dispatch_get_main_queue(), ^{
      [self pullFrame];
    });
}

- (void)pullFrame
{
    int w = 0, h = 0;
    if (!apple2session_copy_frame(_session, _fb, &_serial, &w, &h))
        return;
    _fbW = w;
    _fbH = h;
    if (_fbW <= 0 || _fbH <= 0)
        return;

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    /* No copy: the provider reads _fb in place. Safe because this method and
     * drawRect: both run on the main thread, so no draw can be reading the
     * buffer at the moment copy_frame overwrites it. */
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, _fb, (size_t)_fbW * _fbH * 4, NULL);
    /* XRGB8888 as a little-endian 32-bit word is X,R,G,B from the high byte
     * down -- which is exactly "skip the first component, byte order 32
     * little". Nothing is converted. */
    CGImageRef image = CGImageCreate(
        (size_t)_fbW, (size_t)_fbH, 8, 32, (size_t)_fbW * 4, space,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
        NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);

    if (_image)
        CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}

- (NSRect)destRect
{
    const CGFloat w = self.bounds.size.width;
    const CGFloat h = self.bounds.size.height;
    const CGFloat fbW = _fbW > 0 ? _fbW : APPLE2SESSION_FB_MAX_WIDTH;
    const CGFloat fbH = _fbH > 0 ? _fbH : APPLE2SESSION_FB_MAX_HEIGHT;
    CGFloat dw, dh;

    if (_aspectMode == 2) { /* integer scale */
        int scale = (int)MIN(w / fbW, h / fbH);
        if (scale < 1)
            scale = 1;
        dw = scale * fbW;
        dh = scale * fbH;
    } else {
        const CGFloat aspect = _aspectMode == 0 ? 4.0 / 3.0 : fbW / fbH;
        if (w / h > aspect) {
            dh = h;
            dw = h * aspect;
        } else {
            dw = w;
            dh = w / aspect;
        }
    }
    return NSMakeRect((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [[NSColor blackColor] setFill];
    NSRectFill(self.bounds);
    if (!_image)
        return;

    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSetInterpolationQuality(
        ctx, _smooth ? kCGInterpolationLow : kCGInterpolationNone);
    CGContextDrawImage(ctx, NSRectToCGRect([self destRect]), _image);
}

/* ---- input --------------------------------------------------------------- */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

/* macOS unichar (charactersIgnoringModifiers) -> X11/xkb keysym, for the keys
 * apple2_key_from_event resolves by keycode rather than by character. */
static uint32_t keysymForChar(unichar c)
{
    switch (c) {
    case NSUpArrowFunctionKey:    return 0xFF52;
    case NSDownArrowFunctionKey:  return 0xFF54;
    case NSLeftArrowFunctionKey:  return 0xFF51;
    case NSRightArrowFunctionKey: return 0xFF53;
    case NSDeleteFunctionKey:     return 0xFFFF; /* forward delete */
    case 0x7F:                    return 0xFF08; /* the key marked Delete */
    case 0x0D: case 0x03:         return 0xFF0D; /* Return, Enter */
    case 0x09:                    return 0xFF09;
    case 0x1B:                    return 0xFF1B;
    default:                      return 0;
    }
}

- (void)forwardKey:(NSEvent *)event down:(int)down
{
    NSString *chars = event.charactersIgnoringModifiers;
    unichar c = chars.length ? [chars characterAtIndex:0] : 0;
    const BOOL ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const BOOL shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;

    uint32_t keysym = keysymForChar(c);
    uint32_t unicode = 0;
    if (!keysym) {
        /* charactersIgnoringModifiers already drops Ctrl, so this is the
         * plain character the mapping table wants; the function-key range
         * (0xF700+) is not a character at all. */
        if (c >= 0x20 && c < 0xF700)
            unicode = c;
        keysym = unicode;
    }
    if (!keysym)
        return;
    apple2session_key(_session, down, keysym, unicode, ctrl ? 1 : 0,
                      shift ? 1 : 0);
}

- (void)keyDown:(NSEvent *)event
{
    if (event.isARepeat) {
        /* The core latches a key until the machine reads it; a repeat storm
         * would jam it. Let the Apple II's own repeat handle it. */
        return;
    }
    [self forwardKey:event down:1];
}

- (void)keyUp:(NSEvent *)event
{
    [self forwardKey:event down:0];
}

/* Open Apple and Closed Apple. On a Mac these are the OPTION keys, not the
 * Command keys: Command is how every menu shortcut on the system is typed,
 * and swallowing it would cost the user Cmd-Q, Cmd-W and the rest. Option
 * sits in the same place on the keyboard and is otherwise unused here.
 *
 * Modifiers never produce keyDown:/keyUp: -- they arrive as flagsChanged:
 * with no press/release of their own, so the state is tracked by hand from
 * the per-key device-dependent bits. */
- (void)flagsChanged:(NSEvent *)event
{
    const NSEventModifierFlags flags = event.modifierFlags;
    BOOL open = _openAppleDown, closed = _closedAppleDown;

    /* NSEventModifierFlagOption is set while EITHER Option is down, so which
     * one changed comes from the key code of this event. */
    switch (event.keyCode) {
    case VK_OPTION_LEFT:
        open = (flags & NSEventModifierFlagOption) != 0;
        break;
    case VK_OPTION_RIGHT:
        closed = (flags & NSEventModifierFlagOption) != 0;
        break;
    default:
        break;
    }

    if (open != _openAppleDown) {
        _openAppleDown = open;
        apple2session_key(_session, open ? 1 : 0, 0xFFE9, 0, 0, 0);
    }
    if (closed != _closedAppleDown) {
        _closedAppleDown = closed;
        apple2session_key(_session, closed ? 1 : 0, 0xFFEA, 0, 0, 0);
    }
}

/* Releases go to the key window, so a chord that loses focus mid-press would
 * leave an Apple key stuck down. Lift whatever is held on the way out. */
- (void)resignFirstResponderCleanup
{
    if (_openAppleDown) {
        _openAppleDown = NO;
        apple2session_key(_session, 0, 0xFFE9, 0, 0, 0);
    }
    if (_closedAppleDown) {
        _closedAppleDown = NO;
        apple2session_key(_session, 0, 0xFFEA, 0, 0, 0);
    }
}

- (BOOL)resignFirstResponder
{
    [self resignFirstResponderCleanup];
    return YES;
}

@end

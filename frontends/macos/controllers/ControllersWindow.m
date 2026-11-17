/*
 * Apple2ControllersWindow -- the AppKit Controllers panel: the Apple Joystick
 * IIe and IIc, the two Apple Hand Controllers (paddles), the console's Reset
 * and Power Cycle buttons and an Apple II keyboard, drawn from the session's
 * layout tables (apple2session_controls_layout, apple2session_keyboard_layout)
 * with NSBezierPath, so every frontend shows the same machine; the gamepads and
 * the port each one plays; and Map mode.
 *
 * The joystick is the A2M2002 of the user's photograph: a square case, a raised
 * block with a recessed pyramid well, a stick that tilts in it and two square
 * orange buttons (the left one is button 0 = Open Apple, the top one button 1 =
 * Closed Apple). A Colour control switches its scheme between Beige (the IIe's)
 * and Platinum (the IIc's); the shape is the same. The paddles are the ribbed
 * wedges of the Hand Controllers II: a ribbed knob you turn through 270 degrees
 * and an orange button at the front. A Mode control picks which one the machine
 * has plugged in (one game port: the joystick, or Paddle 0 and Paddle 1).
 *
 * Gestures: press a button and it is held until it is released or dragged off;
 * press in the joystick's well and drag to tilt the stick (proportionally: the
 * position is analog, not eight ways); drag on a knob to turn it (it holds where
 * it is left), the wheel nudges it. The pointer is its own input source, so the
 * panel lets go of everything the mouse holds when it is hidden or loses key
 * status. What the machine sees held -- host keyboard, gamepads and the mouse --
 * is painted in the accent colour, so the panel doubles as an input tester for a
 * new gamepad.
 *
 * The keyboard is the Platinum //e's, five rows of a 15-unit block with the
 * small Reset cap above Esc. A click types the cap; Shift and Control latch
 * until the next key, Caps Lock toggles (it starts on, as on the real //e), and
 * the Open Apple and Option (Closed Apple) caps are held with the mouse and
 * drive buttons 0 and 1. Off by default (the Keyboard check box).
 *
 * Keys typed with the panel key go through the same path as the machine window
 * (Apple2KeyForward): a bound key drives the controller, anything else is typed;
 * the Option keys are Open and Closed Apple. No view here takes the keyboard, so
 * the panel itself is the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents; the
 * Keyboard check box is what changes its height. Hidden at start, ordered out
 * rather than closed, so its position survives. The 30 Hz poll runs only while
 * the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture (the
 * stick's four directions and a knob's two turn keys and its axis show as small
 * labels) or choose one from the list, then press a key or a gamepad button (for
 * a knob axis, move a gamepad axis); the next one is bound and whatever held it
 * is told so in the status line. Esc cancels the pick. Rebinding steals the key
 * from its old target. F9, F11, F12 and the Apple keys are the app's and never
 * bound.
 *
 * Written to the family's AppKit pattern (see the ADAM and Astrocade panels);
 * built and verified on CI only (no Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../Apple2KeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#include "bindings.h"

#define UNIT         34.0     /* points per layout unit (controllers and keyboard) */
#define WIN_W        590.0    /* wide enough for the keyboard's 16.4 units */
#define MARGIN       16.0
#define STAGE_PAD    10.0
#define CONSOLE_GAP  8.0
#define CONSOLE_PAD  10.0
#define KBD_PAD      8.0
#define TITLE_H      18.0
#define ROW_H        24.0
#define EFF_W        130.0
#define POPUP_W      110.0
#define BTN_H        28.0
#define MIN_PAD_ROWS 2       /* rows kept for pads, more when more are connected */
#define MAX_PAD_ROWS 4
#define SLOW_TICKS   8       /* the 30 Hz tick's 250 ms part */
#define MAX_CAPS     96
#define PILL_W       76.0
#define PILL_H       18.0
#define OPTION_LEFT  58      /* macOS virtual key codes of the two Option keys */
#define OPTION_RIGHT 61

/* ---- colours (DESIGN-apple2.md: [user]/[photo] where a photograph gave them,
 * else inferred) ------------------------------------------------------------- */

#define COL_STAGE_TOP    0x45464A
#define COL_STAGE_BOT    0x2E2F33
#define COL_STAGE_EDGE   0x1B1B1D
#define COL_CAPTION      0xC8C8CC
#define COL_STRIP_TOP    0xE2DFD4     /* the console strip's buttons: platinum plastic */
#define COL_STRIP_BOT    0xD0CDC2
#define COL_STRIP_HOT    0xB9B6AB
#define COL_STRIP_INK    0x3A3A3C
#define COL_STRIP_EDGE   0x8E8B80
#define COL_KB_CASE      0xCFCDC4     /* the keyboard */
#define COL_KB_CASE_EDGE 0x9C9A90
#define COL_KB_ALPHA     0xE4E2DA
#define COL_KB_MOD       0xCCCABF
#define COL_KB_RESET     0xF4F3EE
#define COL_KB_EDGE      0x8E8C82
#define COL_KB_INK       0x2B2B2D
#define COL_KB_BAR       0x6E6C62
#define COL_PILL         0x1A1A1C

typedef struct {
    uint32_t caseTop, caseShade, wallTB, wallLR, floorCol, stick, shaft;
    uint32_t btn, btnPressed, btnFrame, logo;
    uint32_t padBody, padShade, knob, knobRim, knobIndex, padBtn, padBtnPressed;
} Scheme;

static const Scheme kBeige = {
    .caseTop = 0xE6DBBF, .caseShade = 0xCFC3A4, .wallTB = 0x4A4A4E, .wallLR = 0x5C5C60,
    .floorCol = 0x2A2A2E, .stick = 0x1D1D26, .shaft = 0x2B2B34,
    .btn = 0xF26A1F, .btnPressed = 0xC9500F, .btnFrame = 0xB9AC8B, .logo = 0xD6CAAC,
    .padBody = 0xC9C3A8, .padShade = 0xB3AD92, .knob = 0x6F6C54, .knobRim = 0x5C5A44,
    .knobIndex = 0xE9E5D0, .padBtn = 0xEB6A25, .padBtnPressed = 0xC24E10,
};

static const Scheme kPlatinum = {
    .caseTop = 0xEBE9E1, .caseShade = 0xCFCDC4, .wallTB = 0xBDBBB2, .wallLR = 0xCBC9C0,
    .floorCol = 0xA9A79E, .stick = 0xB4B2A7, .shaft = 0xA6A499,
    .btn = 0xF0641C, .btnPressed = 0xC54B0D, .btnFrame = 0xB6B4AB, .logo = 0xD7D5CC,
    .padBody = 0xD5D3CA, .padShade = 0xBFBDB4, .knob = 0x8A8878, .knobRim = 0x76746A,
    .knobIndex = 0xE9E5D0, .padBtn = 0xEB6A25, .padBtnPressed = 0xC24E10,
};

static const Scheme *schemeFor(int model)
{
    return model == APPLE2_MODEL_IIC ? &kPlatinum : &kBeige;
}

/* ---- helpers -------------------------------------------------------------------- */

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) apple2session_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSString *padInputName(int ident)
{
    char name[64];
    name[0] = '\0';
    if (ident >= 0) apple2session_pad_input_name(ident, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

static NSColor *rgba(uint32_t value, CGFloat alpha)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:alpha];
}

/* The colour a fraction of the way from a to b. */
static uint32_t mixRgb(uint32_t from, uint32_t to, double frac)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double ca = (double)((from >> shift) & 0xff), cb = (double)((to >> shift) & 0xff);
        out |= (uint32_t)lround(ca + (cb - ca) * frac) << shift;
    }
    return out;
}

/* The accent: the debugger's red, with white text on it. */
static NSColor *accent(void) { return rgb(APPLE2SESSION_ACCENT_RGB); }

/* Toggled-on look for a push button: accent bezel, white title. */
static void setButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = accent();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.whiteColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

static BOOL isKnobTarget(int target)
{
    return target >= APPLE2_TARGET_PADDLES &&
           (target - APPLE2_TARGET_PADDLES) % APPLE2_PADDLE_ACTS == APPLE2_PACT_KNOB;
}

/* A linear gradient clipped to a path, from a to b, with alpha at each end. */
static void fillLinear(NSBezierPath *path, NSPoint from, NSPoint to, uint32_t rgbA, CGFloat alphaA,
                       uint32_t rgbB, CGFloat alphaB)
{
    const CGFloat comps[8] = {
        ((rgbA >> 16) & 0xff) / 255.0, ((rgbA >> 8) & 0xff) / 255.0, (rgbA & 0xff) / 255.0, alphaA,
        ((rgbB >> 16) & 0xff) / 255.0, ((rgbB >> 8) & 0xff) / 255.0, (rgbB & 0xff) / 255.0, alphaB };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(from.x, from.y), CGPointMake(to.x, to.y),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A vertical gradient clipped to a path (the views here are flipped, so "top"
 * is the smaller y). */
static void fillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    fillLinear(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)), NSMakePoint(NSMinX(bnd), NSMaxY(bnd)),
               topRgb, 1.0, botRgb, 1.0);
}

/* Lines of text centred in rect, the font shrunk (to 5 pt) until they fit. */
static void drawFitted(NSString *text, NSRect rect, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat count = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat widest = 0;
        lineHeight = 0;
        for (NSString *line in lines) {
            const NSSize extent = [line sizeWithAttributes:attrs];
            if (extent.width > widest) widest = extent.width;
            if (extent.height > lineHeight) lineHeight = extent.height;
        }
        if ((widest <= rect.size.width && lineHeight * count <= rect.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    CGFloat ypos = rect.origin.y + floor((rect.size.height - lineHeight * count) / 2);
    for (NSString *line in lines) {
        [line drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, lineHeight) withAttributes:attrs];
        ypos += lineHeight;
    }
}

/* The Apple logo glyph (U+F8FF, in the system fonts): filled, or outlined for
 * the Open Apple. */
static void drawApple(NSRect rect, NSColor *ink, BOOL outline)
{
    NSString *glyph = @"";
    NSMutableDictionary *attrs = [NSMutableDictionary dictionary];
    attrs[NSFontAttributeName] = [NSFont systemFontOfSize:rect.size.height];
    if (outline) {
        attrs[NSStrokeWidthAttributeName] = @(3.0);   /* positive: the outline only */
        attrs[NSStrokeColorAttributeName] = ink;
        attrs[NSForegroundColorAttributeName] = ink;
    } else {
        attrs[NSForegroundColorAttributeName] = ink;
    }
    const NSSize ext = [glyph sizeWithAttributes:attrs];
    [glyph drawAtPoint:NSMakePoint(NSMidX(rect) - ext.width / 2, NSMidY(rect) - ext.height / 2)
        withAttributes:attrs];
}

static void strokeLine(NSPoint from, NSPoint to, CGFloat width, NSColor *colour)
{
    NSBezierPath *line = [NSBezierPath bezierPath];
    [line moveToPoint:from];
    [line lineToPoint:to];
    line.lineWidth = width;
    line.lineCapStyle = NSLineCapStyleRound;
    [colour setStroke];
    [line stroke];
}

/* A label on a dark pill, filled with the accent when picked. */
static void drawPill(NSRect rect, NSString *text, BOOL picked)
{
    NSBezierPath *pill = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:rect.size.height / 2
                                                         yRadius:rect.size.height / 2];
    [(picked ? accent() : rgba(COL_PILL, 0.8)) setFill];
    [pill fill];
    drawFitted(text, NSInsetRect(rect, 6, 1), 10, YES, NSColor.whiteColor);
}

/* ---- views ---------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface Apple2FlippedView : NSView
@end

@implementation Apple2FlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class Apple2ControllersWindow;
@interface Apple2ControllersPanel : NSPanel
@property (nonatomic, weak) Apple2ControllersWindow *owner;
@end

@interface Apple2ControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)runSysaction:(int)sysact;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
- (BOOL)mapKeyFromApp:(uint32_t)keysym;
@end

/* One label the stage shows over a control while Map is armed or Show bindings
 * is on: the stick's four directions, a knob's two turns and its axis. */
typedef struct {
    NSRect rect;
    int target;
    char text[64];
} Pill;

/* The stage: the joystick or the two paddles on a dark backdrop, and the two
 * console buttons under it. Everything is drawn here and hit-tested by the core
 * (the dial and the stick's drag maths are the core's too). */
@interface Apple2StageView : Apple2FlippedView
@property (nonatomic) apple2session *session;
@property (nonatomic, weak) Apple2ControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic) int mode;              /* APPLE2_MODE_* */
@property (nonatomic) int model;             /* APPLE2_MODEL_* */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Reads what the machine sees held; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation Apple2StageView {
    const apple2_control *_lists[APPLE2_VIEW_COUNT];
    int _counts[APPLE2_VIEW_COUNT];
    float _dimW[APPLE2_VIEW_COUNT], _dimH[APPLE2_VIEW_COUNT];
    NSSize _size;
    float _ax, _ay;              /* the stick's deflection shown, -1..1 */
    float _knob[APPLE2SESSION_PADDLES];
    BOOL _btnHeld[2];            /* the joystick's two buttons */
    BOOL _pbtnHeld[APPLE2SESSION_PADDLES];
    BOOL _dragging[APPLE2SESSION_PADDLES];

    /* the mouse */
    int _downView, _downIdx;     /* the control pressed, or -1 */
    int _downTarget;             /* the target the pointer holds with a press, or -1 */
    BOOL _stickDrag;
    BOOL _dialDrag;
    BOOL _sysDown;               /* the pressed control is a console button */
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downView = _downIdx = -1;
    _downTarget = -1;
    _mapTarget = -1;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }

- (void)configure
{
    for (int view = 0; view < APPLE2_VIEW_COUNT; view++) {
        _lists[view] = apple2session_controls_layout(view, &_counts[view], &_dimW[view], &_dimH[view]);
        if (!_lists[view]) _counts[view] = 0;
    }
    _size = NSMakeSize(WIN_W, STAGE_PAD * 2 + ceil(_dimH[APPLE2_VIEW_JOYSTICK] * UNIT) + CONSOLE_GAP +
                              ceil(_dimH[APPLE2_VIEW_CONSOLE] * UNIT) + CONSOLE_PAD);
    [self setNeedsDisplay:YES];
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

- (void)setMode:(int)mode
{
    if (mode == _mode) return;
    _mode = mode;
    [self setNeedsDisplay:YES];
}

- (void)setModel:(int)model
{
    if (model == _model) return;
    _model = model;
    [self setNeedsDisplay:YES];
}

/* ---- geometry ------------------------------------------------------------------ */

- (int)activeView
{
    return _mode == APPLE2_MODE_PADDLES ? APPLE2_VIEW_PADDLES : APPLE2_VIEW_JOYSTICK;
}

/* Where a view's layout origin is, in view points. */
- (NSPoint)originOfView:(int)view
{
    const CGFloat joyH = _dimH[APPLE2_VIEW_JOYSTICK] * UNIT;
    switch (view) {
    case APPLE2_VIEW_JOYSTICK:
        return NSMakePoint(floor((WIN_W - _dimW[view] * UNIT) / 2), STAGE_PAD);
    case APPLE2_VIEW_PADDLES:
        return NSMakePoint(floor((WIN_W - _dimW[view] * UNIT) / 2),
                           STAGE_PAD + floor((joyH - _dimH[view] * UNIT) / 2));
    default:
        return NSMakePoint(floor((WIN_W - _dimW[view] * UNIT) / 2), STAGE_PAD * 2 + ceil(joyH) + CONSOLE_GAP);
    }
}

- (NSRect)rectOf:(const apple2_control *)ctl view:(int)view
{
    const NSPoint org = [self originOfView:view];
    return NSMakeRect(org.x + ctl->xpos * UNIT, org.y + ctl->ypos * UNIT, ctl->width * UNIT, ctl->height * UNIT);
}

/* A layout point (units) in view points. */
- (NSPoint)pointX:(CGFloat)xpos y:(CGFloat)ypos view:(int)view
{
    const NSPoint org = [self originOfView:view];
    return NSMakePoint(org.x + xpos * UNIT, org.y + ypos * UNIT);
}

- (const apple2_control *)ctlOfView:(int)view index:(int)idx
{
    return idx >= 0 && idx < _counts[view] ? &_lists[view][idx] : NULL;
}

/* The control under a view point: the console strip first, then the active
 * view. */
- (BOOL)locate:(NSPoint)pnt view:(int *)view index:(int *)idx
{
    const int views[2] = { APPLE2_VIEW_CONSOLE, [self activeView] };
    for (int slot = 0; slot < 2; slot++) {
        const NSPoint org = [self originOfView:views[slot]];
        const int hit = apple2session_controls_hit(views[slot], (float)((pnt.x - org.x) / UNIT),
                                                   (float)((pnt.y - org.y) / UNIT));
        if (hit >= 0 && hit < _counts[views[slot]]) {
            *view = views[slot];
            *idx = hit;
            return YES;
        }
    }
    return NO;
}

/* The labels shown over the controls (Map armed, or Show bindings on). */
- (int)pills:(Pill *)out max:(int)max
{
    int num = 0;
    if (!(self.mapping || self.showKeys)) return 0;
    NSString *(^legend)(int) = ^NSString *(int target) { return [self.owner legendForTarget:target]; };
#define ADD_PILL(cx, cy, tgt, prefix) do { \
        if (num < max) { \
            Pill *pill = &out[num++]; \
            pill->rect = NSMakeRect((cx) - PILL_W / 2, (cy) - PILL_H / 2, PILL_W, PILL_H); \
            pill->target = (tgt); \
            NSString *name = legend(tgt); \
            NSString *text = [NSString stringWithFormat:@"%@ %@", prefix, name.length ? name : @"-"]; \
            strncpy(pill->text, text.UTF8String, sizeof pill->text - 1); \
            pill->text[sizeof pill->text - 1] = '\0'; \
        } } while (0)
    if (_mode == APPLE2_MODE_JOYSTICK) {
        const apple2_control *stick = NULL;
        for (int idx = 0; idx < _counts[APPLE2_VIEW_JOYSTICK]; idx++)
            if (_lists[APPLE2_VIEW_JOYSTICK][idx].kind == APPLE2_CTL_STICK) stick = &_lists[APPLE2_VIEW_JOYSTICK][idx];
        if (stick) {
            const NSPoint ctr = [self pointX:stick->xpos + stick->width / 2
                                           y:stick->ypos + stick->height / 2 view:APPLE2_VIEW_JOYSTICK];
            ADD_PILL(ctr.x - 1.75 * UNIT, ctr.y, APPLE2_TARGET_JOY(APPLE2_ACT_LEFT), @"◀");
            ADD_PILL(ctr.x + 1.75 * UNIT, ctr.y, APPLE2_TARGET_JOY(APPLE2_ACT_RIGHT), @"▶");
            ADD_PILL(ctr.x, ctr.y - 1.5 * UNIT, APPLE2_TARGET_JOY(APPLE2_ACT_UP), @"▲");
            ADD_PILL(ctr.x, ctr.y + 1.5 * UNIT, APPLE2_TARGET_JOY(APPLE2_ACT_DOWN), @"▼");
        }
    } else {
        for (int idx = 0; idx < _counts[APPLE2_VIEW_PADDLES]; idx++) {
            const apple2_control *ctl = &_lists[APPLE2_VIEW_PADDLES][idx];
            if (ctl->kind != APPLE2_CTL_KNOB) continue;
            const NSPoint ctr = [self pointX:ctl->cx y:ctl->cy view:APPLE2_VIEW_PADDLES];
            const CGFloat xpos = ctr.x + 2.2 * UNIT;
            ADD_PILL(xpos, ctr.y - 0.45 * UNIT, APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_CCW), @"↺");
            ADD_PILL(xpos, ctr.y + 0.35 * UNIT, APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_CW), @"↻");
            ADD_PILL(xpos, ctr.y + 1.15 * UNIT, APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_KNOB), @"axis");
        }
    }
#undef ADD_PILL
    return num;
}

/* The target Map would pick at a point (view points), or -1: a label, or a
 * button, a console button or a knob's hub. */
- (int)mapTargetAt:(NSPoint)pnt
{
    Pill pills[16];
    const int num = [self pills:pills max:16];
    for (int idx = 0; idx < num; idx++)
        if (NSPointInRect(pnt, pills[idx].rect)) return pills[idx].target;
    int view = 0, idx = 0;
    if (![self locate:pnt view:&view index:&idx]) return -1;
    const apple2_control *ctl = [self ctlOfView:view index:idx];
    if (!ctl) return -1;
    switch (ctl->kind) {
    case APPLE2_CTL_BUTTON:
    case APPLE2_CTL_SYSBUTTON:
        return ctl->target;
    case APPLE2_CTL_KNOB:
        return APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_KNOB);
    default:
        return -1;
    }
}

/* ---- polling ------------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    float axisX = 0, axisY = 0;
    if (_mode == APPLE2_MODE_JOYSTICK && !self.mapping)
        apple2session_axes_get(_session, &axisX, &axisY);
    if (axisX != _ax || axisY != _ay) {
        _ax = axisX;
        _ay = axisY;
        dirty = YES;
    }
    for (int btn = 0; btn < 2; btn++) {
        const BOOL held = _mode == APPLE2_MODE_JOYSTICK && !self.mapping &&
                          apple2session_target_held(_session, APPLE2_TARGET_JOY(APPLE2_ACT_BTN0 + btn)) != 0;
        if (held != _btnHeld[btn]) { _btnHeld[btn] = held; dirty = YES; }
    }
    for (int pad = 0; pad < APPLE2SESSION_PADDLES; pad++) {
        const float knob = apple2session_knob_get(_session, pad);
        if (knob != _knob[pad]) { _knob[pad] = knob; dirty = YES; }
        const BOOL held = _mode == APPLE2_MODE_PADDLES && !self.mapping &&
                          apple2session_target_held(_session, APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_BUTTON)) != 0;
        if (held != _pbtnHeld[pad]) { _pbtnHeld[pad] = held; dirty = YES; }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------- */

- (NSString *)legendOfTarget:(int)target
{
    if (!(self.mapping || self.showKeys) || target < 0) return nil;
    return [self.owner legendForTarget:target];
}

- (BOOL)picked:(int)target
{
    return self.mapping && target >= 0 && target == self.mapTarget;
}

/* A held part: the accent outline. */
- (void)strokeHeld:(NSBezierPath *)path
{
    [accent() setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* A square button: a frame, the face pushed in 0.1 unit when pressed. */
- (void)drawButton:(const apple2_control *)ctl view:(int)view held:(BOOL)held
            colour:(uint32_t)face pressed:(uint32_t)facePressed frame:(uint32_t)frame
{
    const NSRect rect = [self rectOf:ctl view:view];
    const CGFloat rad = 0.12 * UNIT;
    NSBezierPath *outer = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -0.08 * UNIT, -0.08 * UNIT)
                                                          xRadius:rad + 2 yRadius:rad + 2];
    [rgb(frame) setFill];
    [outer fill];
    NSRect top = rect;
    if (held) top.origin.y += 0.1 * UNIT;
    NSBezierPath *face_ = [NSBezierPath bezierPathWithRoundedRect:top xRadius:rad yRadius:rad];
    fillGradient(face_, held ? facePressed : mixRgb(face, 0xFFFFFF, 0.12), held ? mixRgb(facePressed, 0x000000, 0.1) : face);
    [rgba(0x000000, 0.25) setStroke];
    face_.lineWidth = 0.75;
    [face_ stroke];
    if (held) [self strokeHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -0.08 * UNIT - 2, -0.08 * UNIT - 2)
                                                               xRadius:rad + 4 yRadius:rad + 4]];
    if ([self picked:ctl->target]) {
        NSBezierPath *ring = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -6, -6) xRadius:rad + 6
                                                             yRadius:rad + 6];
        ring.lineWidth = 2.5;
        [accent() setStroke];
        [ring stroke];
    }
    NSString *legend = [self legendOfTarget:ctl->target];
    if (legend.length) drawFitted(legend, NSInsetRect(top, 2, 2), 10, YES, NSColor.whiteColor);
}

- (void)drawJoystickPart:(const apple2_control *)ctl scheme:(const Scheme *)sch
{
    const int view = APPLE2_VIEW_JOYSTICK;
    const NSRect rect = [self rectOf:ctl view:view];
    switch (ctl->part) {
    case APPLE2_PART_JOY_BASE: {
        NSBezierPath *base = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.35 * UNIT yRadius:0.35 * UNIT];
        /* a soft shadow on the stage under the case */
        NSBezierPath *shadow = [NSBezierPath bezierPathWithRoundedRect:NSOffsetRect(rect, 2, 4)
                                                               xRadius:0.35 * UNIT yRadius:0.35 * UNIT];
        [rgba(0x000000, 0.35) setFill];
        [shadow fill];
        fillGradient(base, mixRgb(sch->caseTop, 0xFFFFFF, 0.1), sch->caseTop);
        [rgb(sch->caseShade) setStroke];
        base.lineWidth = 1;
        [base stroke];
        /* the seam of the lower shell */
        strokeLine(NSMakePoint(NSMinX(rect) + 0.2 * UNIT, NSMinY(rect) + 7.7 * UNIT),
                   NSMakePoint(NSMaxX(rect) - 0.2 * UNIT, NSMinY(rect) + 7.7 * UNIT), 1, rgb(sch->caseShade));
        break;
    }
    case APPLE2_PART_JOY_BLOCK: {
        /* the raised block: a shadow on its left, a lighter top edge */
        NSBezierPath *block = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.3 * UNIT yRadius:0.3 * UNIT];
        NSBezierPath *shadow = [NSBezierPath bezierPathWithRect:NSMakeRect(NSMinX(rect) - 0.35 * UNIT, NSMinY(rect),
                                                                           0.35 * UNIT, NSHeight(rect))];
        fillLinear(shadow, NSMakePoint(NSMinX(rect) - 0.35 * UNIT, 0), NSMakePoint(NSMinX(rect), 0),
                   sch->caseShade, 0.0, sch->caseShade, 0.9);
        fillGradient(block, mixRgb(sch->caseTop, 0xFFFFFF, 0.18), sch->caseTop);
        [rgb(sch->caseShade) setStroke];
        block.lineWidth = 1;
        [block stroke];
        strokeLine(NSMakePoint(NSMinX(rect) + 0.3 * UNIT, NSMinY(rect) + 1), NSMakePoint(NSMaxX(rect) - 0.3 * UNIT, NSMinY(rect) + 1),
                   1.5, rgba(0xFFFFFF, 0.6));
        break;
    }
    case APPLE2_PART_JOY_WELL: {
        /* the pyramid: four walls sloping to the floor square */
        NSRect floor_ = NSMakeRect(NSMidX(rect) - 0.5 * UNIT, NSMidY(rect) - 0.5 * UNIT, UNIT, UNIT);
        for (int fidx = 0; fidx < _counts[view]; fidx++)
            if (_lists[view][fidx].part == APPLE2_PART_JOY_FLOOR) floor_ = [self rectOf:&_lists[view][fidx] view:view];
        const NSPoint outerTL = NSMakePoint(NSMinX(rect), NSMinY(rect)), outerTR = NSMakePoint(NSMaxX(rect), NSMinY(rect));
        const NSPoint outerBL = NSMakePoint(NSMinX(rect), NSMaxY(rect)), outerBR = NSMakePoint(NSMaxX(rect), NSMaxY(rect));
        const NSPoint innerTL = NSMakePoint(NSMinX(floor_), NSMinY(floor_)), innerTR = NSMakePoint(NSMaxX(floor_), NSMinY(floor_));
        const NSPoint innerBL = NSMakePoint(NSMinX(floor_), NSMaxY(floor_)), innerBR = NSMakePoint(NSMaxX(floor_), NSMaxY(floor_));
        const NSPoint walls[4][4] = {
            { outerTL, outerTR, innerTR, innerTL },    /* top */
            { outerBL, outerBR, innerBR, innerBL },    /* bottom */
            { outerTL, innerTL, innerBL, outerBL },    /* left */
            { outerTR, innerTR, innerBR, outerBR },    /* right */
        };
        for (int wall = 0; wall < 4; wall++) {
            NSBezierPath *path = [NSBezierPath bezierPath];
            [path moveToPoint:walls[wall][0]];
            for (int vtx = 1; vtx < 4; vtx++) [path lineToPoint:walls[wall][vtx]];
            [path closePath];
            [rgb(wall < 2 ? sch->wallTB : sch->wallLR) setFill];
            [path fill];
            [rgba(0x000000, 0.3) setStroke];
            path.lineWidth = 0.75;
            [path stroke];
        }
        break;
    }
    case APPLE2_PART_JOY_FLOOR: {
        [rgb(sch->floorCol) setFill];
        NSRectFill(rect);
        break;
    }
    case APPLE2_PART_JOY_LOGO: {
        NSBezierPath *logo = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.15 * UNIT yRadius:0.15 * UNIT];
        [rgb(sch->logo) setFill];
        [logo fill];
        [rgb(mixRgb(sch->logo, 0x000000, 0.16)) setStroke];
        logo.lineWidth = 1;
        [logo stroke];
        drawApple(NSInsetRect(rect, 0.15 * UNIT, 0.2 * UNIT), rgb(mixRgb(sch->logo, 0x000000, 0.35)), NO);
        break;
    }
    default:
        break;
    }
}

/* The stick: the shaft from its socket to the cap, which follows the deflection
 * from where it rests. */
- (void)drawStick:(const apple2_control *)ctl scheme:(const Scheme *)sch
{
    const int view = APPLE2_VIEW_JOYSTICK;
    const NSPoint socket = [self pointX:ctl->cx y:ctl->cy view:view];
    const NSPoint cap = [self pointX:ctl->capx + _ax * ctl->reach y:ctl->capy + _ay * ctl->reach view:view];
    CGFloat dirx = cap.x - socket.x, diry = cap.y - socket.y;
    const CGFloat len = hypot(dirx, diry);
    if (len < 0.001) { dirx = 0; diry = -1; } else { dirx /= len; diry /= len; }
    strokeLine(socket, cap, 0.24 * UNIT, rgb(sch->shaft));
    /* the cap: a capsule along the shaft */
    const NSPoint tail = NSMakePoint(cap.x - dirx * 0.3 * UNIT, cap.y - diry * 0.3 * UNIT);
    const NSPoint head = NSMakePoint(cap.x + dirx * 0.3 * UNIT, cap.y + diry * 0.3 * UNIT);
    strokeLine(tail, head, 0.55 * UNIT, rgb(sch->stick));
    strokeLine(NSMakePoint(tail.x - 0.05 * UNIT, tail.y - 0.06 * UNIT), NSMakePoint(head.x - 0.05 * UNIT, head.y - 0.06 * UNIT),
               0.18 * UNIT, rgba(0xFFFFFF, 0.22));
    if (fabs(_ax) > 0.05 || fabs(_ay) > 0.05)
        [self strokeHeld:[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cap.x - 0.5 * UNIT, cap.y - 0.5 * UNIT,
                                                                           UNIT, UNIT)]];
}

- (void)drawPaddleBody:(const apple2_control *)ctl scheme:(const Scheme *)sch
{
    const int view = APPLE2_VIEW_PADDLES;
    const CGFloat ox = ctl->xpos;
    static const float pts[6][2] = { {0, 0.6f}, {5.2f, 0}, {6.0f, 0.9f}, {6.0f, 2.4f}, {5.0f, 3.0f}, {0, 2.6f} };
    NSBezierPath *body = [NSBezierPath bezierPath];
    for (int idx = 0; idx < 6; idx++) {
        const NSPoint pnt = [self pointX:ox + pts[idx][0] y:pts[idx][1] view:view];
        if (idx == 0) [body moveToPoint:pnt]; else [body lineToPoint:pnt];
    }
    [body closePath];
    body.lineJoinStyle = NSLineJoinStyleRound;
    NSBezierPath *shadow = [body copy];
    NSAffineTransform *move = [NSAffineTransform transform];
    [move translateXBy:2 yBy:4];
    [shadow transformUsingAffineTransform:move];
    [rgba(0x000000, 0.35) setFill];
    [shadow fill];
    fillGradient(body, mixRgb(sch->padBody, 0xFFFFFF, 0.14), sch->padBody);
    [rgb(sch->padShade) setStroke];
    body.lineWidth = 1.5;
    [body stroke];
    /* the ribs along the lower side */
    for (int rib = 0; rib < 11; rib++) {
        const CGFloat xpos = 0.7 + rib * 0.4;
        const CGFloat bottom = 2.6 + 0.4 * (xpos / 5.0);
        strokeLine([self pointX:ox + xpos y:bottom - 0.5 view:view], [self pointX:ox + xpos y:bottom - 0.1 view:view],
                   1.5, rgb(sch->padShade));
    }
}

- (void)drawKnob:(const apple2_control *)ctl scheme:(const Scheme *)sch
{
    const int view = APPLE2_VIEW_PADDLES;
    const NSPoint ctr = [self pointX:ctl->cx y:ctl->cy view:view];
    const CGFloat rad = ctl->reach * UNIT;
    NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(ctr.x - rad, ctr.y - rad, 2 * rad, 2 * rad)];
    fillGradient(disc, mixRgb(sch->knob, 0xFFFFFF, 0.1), mixRgb(sch->knob, 0x000000, 0.12));
    /* the ribbed rim */
    for (int rib = 0; rib < 24; rib++) {
        const double ang = rib * M_PI / 12.0;
        strokeLine(NSMakePoint(ctr.x + rad * 0.84 * cos(ang), ctr.y + rad * 0.84 * sin(ang)),
                   NSMakePoint(ctr.x + rad * 0.98 * cos(ang), ctr.y + rad * 0.98 * sin(ang)), 1.6, rgb(sch->knobRim));
    }
    NSBezierPath *top = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(ctr.x - rad * 0.72, ctr.y - rad * 0.72,
                                                                          rad * 1.44, rad * 1.44)];
    fillGradient(top, mixRgb(sch->knob, 0xFFFFFF, 0.2), sch->knob);
    /* the index mark: the knob's value, 270 degrees of travel */
    const double ang = apple2session_knob_angle(_knob[ctl->port]) * M_PI / 180.0;
    const NSPoint dir = NSMakePoint(sin(ang), -cos(ang));
    strokeLine(NSMakePoint(ctr.x + dir.x * rad * 0.2, ctr.y + dir.y * rad * 0.2),
               NSMakePoint(ctr.x + dir.x * rad * 0.62, ctr.y + dir.y * rad * 0.62), 2.5, rgb(sch->knobIndex));
    if (_dragging[ctl->port] || (self.mapping && [self picked:APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_KNOB)]))
        [self strokeHeld:[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(ctr.x - rad - 2, ctr.y - rad - 2,
                                                                           2 * rad + 4, 2 * rad + 4)]];
}

- (void)drawSys:(const apple2_control *)ctl
{
    const NSRect rect = [self rectOf:ctl view:APPLE2_VIEW_CONSOLE];
    const BOOL pressed = _sysDown && _downView == APPLE2_VIEW_CONSOLE && _downIdx >= 0 &&
                         &_lists[APPLE2_VIEW_CONSOLE][_downIdx] == ctl;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:6 yRadius:6];
    fillGradient(path, pressed ? COL_STRIP_HOT : COL_STRIP_TOP, pressed ? COL_STRIP_HOT : COL_STRIP_BOT);
    [rgb(COL_STRIP_EDGE) setStroke];
    path.lineWidth = 1;
    [path stroke];
    NSString *legend = [self legendOfTarget:ctl->target];
    NSString *text = legend.length ? [NSString stringWithFormat:@"%@\n%@", nsstr(ctl->label), legend] : nsstr(ctl->label);
    drawFitted(text, NSInsetRect(rect, 4, 2), 12, YES, rgb(COL_STRIP_INK));
    if ([self picked:ctl->target]) {
        NSBezierPath *ring = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -3, -3) xRadius:8 yRadius:8];
        ring.lineWidth = 2.5;
        [accent() setStroke];
        [ring stroke];
    }
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const Scheme *sch = schemeFor(_model);
    const CGFloat stageH = STAGE_PAD * 2 + ceil(_dimH[APPLE2_VIEW_JOYSTICK] * UNIT);

    NSBezierPath *stage = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, 0, WIN_W, stageH) xRadius:10 yRadius:10];
    fillGradient(stage, COL_STAGE_TOP, COL_STAGE_BOT);
    [rgb(COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];

    const int view = [self activeView];
    for (int idx = 0; idx < _counts[view]; idx++) {
        const apple2_control *ctl = &_lists[view][idx];
        switch (ctl->kind) {
        case APPLE2_CTL_PART:
            if (view == APPLE2_VIEW_JOYSTICK) [self drawJoystickPart:ctl scheme:sch];
            else if (ctl->part == APPLE2_PART_PADDLE_BODY) [self drawPaddleBody:ctl scheme:sch];
            break;
        case APPLE2_CTL_STICK:
            [self drawStick:ctl scheme:sch];
            break;
        case APPLE2_CTL_BUTTON:
            if (view == APPLE2_VIEW_JOYSTICK) {
                const BOOL held = ctl->target == APPLE2_TARGET_JOY(APPLE2_ACT_BTN0) ? _btnHeld[0] : _btnHeld[1];
                [self drawButton:ctl view:view held:held colour:sch->btn pressed:sch->btnPressed frame:sch->btnFrame];
            } else {
                [self drawButton:ctl view:view held:_pbtnHeld[ctl->port] colour:sch->padBtn pressed:sch->padBtnPressed
                           frame:sch->padShade];
            }
            break;
        case APPLE2_CTL_KNOB:
            [self drawKnob:ctl scheme:sch];
            break;
        case APPLE2_CTL_CAPTION:
            drawFitted(nsstr(ctl->label), [self rectOf:ctl view:view], 11, YES, rgb(COL_CAPTION));
            break;
        default:
            break;
        }
    }
    for (int idx = 0; idx < _counts[APPLE2_VIEW_CONSOLE]; idx++)
        [self drawSys:&_lists[APPLE2_VIEW_CONSOLE][idx]];

    /* the stage's labels: the console strip's name, and the Map / Show bindings pills */
    {
        const NSPoint org = [self originOfView:APPLE2_VIEW_CONSOLE];
        NSDictionary *attrs = @{ NSFontAttributeName : [NSFont systemFontOfSize:11],
                                 NSForegroundColorAttributeName : [NSColor secondaryLabelColor] };
        [@"Apple II" drawAtPoint:NSMakePoint(MARGIN, org.y + 8) withAttributes:attrs];
    }
    Pill pills[16];
    const int num = [self pills:pills max:16];
    for (int idx = 0; idx < num; idx++)
        drawPill(pills[idx].rect, nsstr(pills[idx].text), [self picked:pills[idx].target]);
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)letGo
{
    if (_downTarget >= 0) apple2session_press(_session, _downTarget, 0);
    apple2session_pointer_release(_session);
    _downView = _downIdx = -1;
    _downTarget = -1;
    _stickDrag = NO;
    _dialDrag = NO;
    _sysDown = NO;
    for (int pad = 0; pad < APPLE2SESSION_PADDLES; pad++) _dragging[pad] = NO;
    [self setNeedsDisplay:YES];
}

/* The stick follows the pointer, proportionally, from where its cap rests. */
- (void)dragStickAt:(NSPoint)pnt
{
    const NSPoint org = [self originOfView:APPLE2_VIEW_JOYSTICK];
    float xval = 0, yval = 0;
    apple2session_joystick_from_point((float)((pnt.x - org.x) / UNIT), (float)((pnt.y - org.y) / UNIT), &xval, &yval);
    apple2session_pointer_set(_session, xval, yval);
}

/* The dial: the knob goes where the pointer's angle puts it. */
- (void)dragDialAt:(NSPoint)pnt
{
    const apple2_control *ctl = [self ctlOfView:_downView index:_downIdx];
    if (!ctl) return;
    const NSPoint ctr = [self pointX:ctl->cx y:ctl->cy view:_downView];
    float value = 0;
    if (apple2session_knob_from_point(pnt.x - ctr.x, pnt.y - ctr.y, ctl->reach * UNIT, &value))
        apple2session_knob_set(_session, ctl->port, value);
    [self poll];
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];

    if (self.mapping) {
        /* Map: a label, a button, a console button or a knob's hub is picked */
        const int target = [self mapTargetAt:pnt];
        if (target >= 0) [self.owner mapPick:target];
        return;
    }

    int view = 0, idx = 0;
    if (![self locate:pnt view:&view index:&idx]) return;
    const apple2_control *ctl = [self ctlOfView:view index:idx];
    if (!ctl) return;
    switch (ctl->kind) {
    case APPLE2_CTL_SYSBUTTON:
        _downView = view;
        _downIdx = idx;
        _sysDown = YES;      /* fires on release over the button */
        break;
    case APPLE2_CTL_BUTTON:
        _downView = view;
        _downIdx = idx;
        _downTarget = ctl->target;
        apple2session_press(_session, ctl->target, 1);
        break;
    case APPLE2_CTL_STICK:
        _downView = view;
        _downIdx = idx;
        _stickDrag = YES;
        [self dragStickAt:pnt];
        break;
    case APPLE2_CTL_KNOB:
        _downView = view;
        _downIdx = idx;
        _dialDrag = YES;
        _dragging[ctl->port] = YES;
        [self dragDialAt:pnt];
        break;
    default:
        break;
    }
    [self setNeedsDisplay:YES];
    [self poll];
}

- (void)dragTo:(NSPoint)pnt
{
    if (_downIdx < 0 || _sysDown) return;
    if (_dialDrag) {
        [self dragDialAt:pnt];
        return;
    }
    if (_stickDrag) {
        [self dragStickAt:pnt];
        [self poll];
        return;
    }
    /* a button: dragging off lets go */
    const apple2_control *ctl = [self ctlOfView:_downView index:_downIdx];
    if (ctl && !NSPointInRect(pnt, [self rectOf:ctl view:_downView])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseDragged:(NSEvent *)evt
{
    [self dragTo:[self convertPoint:[evt locationInWindow] fromView:nil]];
}

- (void)mouseUp:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    if (_sysDown) {
        /* a console button acts on release over it, like a real one */
        const apple2_control *ctl = [self ctlOfView:_downView index:_downIdx];
        const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
        const BOOL inside = ctl && NSPointInRect(pnt, [self rectOf:ctl view:_downView]);
        const int target = ctl ? ctl->target : -1;
        [self letGo];
        if (inside && target >= APPLE2_TARGET_SYSACT_BASE && target < APPLE2_TARGET_PADDLES)
            [self.owner runSysaction:target - APPLE2_TARGET_SYSACT_BASE];
        return;
    }
    [self letGo];
    [self poll];
}

/* The wheel over a dial nudges the knob (up turns it right). */
- (void)scrollWheel:(NSEvent *)evt
{
    if (self.mapping) return;
    int view = 0, idx = 0;
    if (![self locate:[self convertPoint:[evt locationInWindow] fromView:nil] view:&view index:&idx]) return;
    const apple2_control *ctl = [self ctlOfView:view index:idx];
    if (!ctl || ctl->kind != APPLE2_CTL_KNOB) return;
    const CGFloat step = [evt hasPreciseScrollingDeltas] ? [evt scrollingDeltaY] * 0.01
                                                          : [evt scrollingDeltaY] * 0.05;
    apple2session_knob_nudge(_session, ctl->port, (float)step);
    [self poll];
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    int cview = 0, idx = 0;
    int target = -1;
    if ([self locate:pnt view:&cview index:&idx]) {
        const apple2_control *ctl = [self ctlOfView:cview index:idx];
        if (ctl && ctl->kind == APPLE2_CTL_KNOB)
            target = APPLE2_TARGET_PADDLE(ctl->port, APPLE2_PACT_KNOB);
        else if (ctl)
            target = ctl->target;
    }
    if (target < 0) return nil;
    NSString *key = bindings_target_keysym(target) ? keyName(bindings_target_keysym(target)) : @"none";
    const int pad = bindings_target_pad(target);
    NSString *padText = pad >= 0 ? padInputName(pad) : @"none";
    if (isKnobTarget(target))
        return [NSString stringWithFormat:@"%@\nGamepad axis: %@", nsstr(bindings_target_label(target)), padText];
    return [NSString stringWithFormat:@"%@\nKey: %@    Gamepad: %@", nsstr(bindings_target_label(target)),
            key, padText];
}

/* One tooltip rect over the whole stage; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

/* ---- the keyboard ------------------------------------------------------------------- */

/* The Apple II keyboard drawn from apple2session_keyboard_layout. A click types
 * the cap; the core owns the latches, the flash and the Apple caps. */
@interface Apple2KeyboardView : Apple2FlippedView
@property (nonatomic) apple2session *session;
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
- (void)poll;
- (void)letGo;
@end

@implementation Apple2KeyboardView {
    const apple2_keycap *_caps;
    int _num;
    float _layW, _layH;
    NSSize _size;
    int _held[MAX_CAPS];
    int _down;                   /* the cap the pointer is on, or -1 */
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _down = -1;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (NSSize)contentSize { return _size; }

- (void)configure
{
    _caps = apple2session_keyboard_layout(&_num, &_layW, &_layH);
    if (!_caps) _num = 0;
    if (_num > MAX_CAPS) _num = MAX_CAPS;
    _size = NSMakeSize(WIN_W, ceil(_layH * UNIT) + 2 * KBD_PAD + 6);
    memset(_held, 0, sizeof _held);
    [self setNeedsDisplay:YES];
}

- (NSPoint)origin
{
    return NSMakePoint(floor((WIN_W - _layW * UNIT) / 2), KBD_PAD + 3);
}

- (NSRect)rectOfCap:(int)idx
{
    const NSPoint org = [self origin];
    const apple2_keycap *cap = &_caps[idx];
    return NSMakeRect(org.x + cap->xpos * UNIT, org.y + cap->ypos * UNIT, cap->width * UNIT, cap->height * UNIT);
}

- (int)capAt:(NSPoint)pnt
{
    for (int idx = 0; idx < _num; idx++)
        if (NSPointInRect(pnt, [self rectOfCap:idx])) return idx;
    return -1;
}

- (void)poll
{
    BOOL dirty = NO;
    for (int idx = 0; idx < _num; idx++) {
        const int held = apple2session_keyboard_held(_session, idx);
        if (held != _held[idx]) {
            _held[idx] = held;
            dirty = YES;
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

- (void)letGo
{
    apple2session_keyboard_release(_session);
    _down = -1;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect bnd = [self bounds];
    NSBezierPath *plate = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(bnd, 4, 0) xRadius:8 yRadius:8];
    fillGradient(plate, mixRgb(COL_KB_CASE, 0xFFFFFF, 0.2), COL_KB_CASE);
    [rgb(COL_KB_CASE_EDGE) setStroke];
    plate.lineWidth = 1;
    [plate stroke];

    for (int idx = 0; idx < _num; idx++) {
        const apple2_keycap *cap = &_caps[idx];
        const NSRect rect = [self rectOfCap:idx];
        const BOOL flash = (_held[idx] & 1) != 0, latch = (_held[idx] & 2) != 0;
        uint32_t face = COL_KB_ALPHA;
        if (cap->kind == APPLE2_KEYCAP_RESET) face = COL_KB_RESET;
        else if (cap->kind != APPLE2_KEYCAP_CHAR && cap->kind != APPLE2_KEYCAP_SPACE) face = COL_KB_MOD;
        const NSRect inner = NSInsetRect(rect, 1.5, 1.5);
        NSBezierPath *shadow = [NSBezierPath bezierPathWithRoundedRect:NSOffsetRect(inner, 0, 1.5) xRadius:4 yRadius:4];
        [rgb(COL_KB_EDGE) setFill];
        [shadow fill];
        NSBezierPath *key = [NSBezierPath bezierPathWithRoundedRect:inner xRadius:4 yRadius:4];
        if (flash) {
            [accent() setFill];
            [key fill];
        } else {
            fillGradient(key, mixRgb(face, 0xFFFFFF, 0.35), face);
        }
        [rgba(0x000000, 0.28) setStroke];
        key.lineWidth = 0.75;
        [key stroke];
        NSColor *ink = flash ? NSColor.whiteColor : rgb(COL_KB_INK);
        NSString *legend = nsstr(cap->legend);
        NSString *shifted = nsstr(cap->shifted);

        if (cap->kind == APPLE2_KEYCAP_APPLE) {
            /* the Apple on the key, with its name under it */
            NSRect glyph = NSMakeRect(NSMinX(inner) + 2, NSMinY(inner) + 1, NSWidth(inner) - 4, NSHeight(inner) * 0.55);
            drawApple(glyph, ink, cap->keysym == 0);
            drawFitted(cap->keysym == 0 ? @"Open" : @"Option",
                       NSMakeRect(NSMinX(inner), NSMinY(inner) + NSHeight(inner) * 0.56, NSWidth(inner),
                                  NSHeight(inner) * 0.4), 8, NO, ink);
        } else if (shifted.length) {
            drawFitted(shifted, NSMakeRect(NSMinX(inner), NSMinY(inner) + 1, NSWidth(inner), NSHeight(inner) * 0.46), 10, NO,
                       ink);
            drawFitted(legend, NSMakeRect(NSMinX(inner), NSMinY(inner) + NSHeight(inner) * 0.46, NSWidth(inner),
                                          NSHeight(inner) * 0.52), 12, YES, ink);
        } else if (cap->kind == APPLE2_KEYCAP_CHAR || cap->kind == APPLE2_KEYCAP_SPACE) {
            drawFitted(legend, inner, 14, YES, ink);
        } else {
            drawFitted(legend, NSInsetRect(inner, 1, 1), cap->kind == APPLE2_KEYCAP_RESET ? 7 : 9, YES, ink);
        }
        if (latch) {
            /* a lit latch: a dot in the corner and an accent border */
            NSBezierPath *ring = [NSBezierPath bezierPathWithRoundedRect:inner xRadius:4 yRadius:4];
            ring.lineWidth = 2;
            [accent() setStroke];
            [ring stroke];
            [accent() setFill];
            [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(NSMaxX(inner) - 9, NSMinY(inner) + 3, 6, 6)] fill];
        }
    }
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_down >= 0) return;
    const int idx = [self capAt:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (idx < 0) return;
    _down = idx;
    apple2session_keyboard_press(_session, idx, 1);
    [self poll];
    [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)evt
{
    if (_down < 0) return;
    /* dragging off the cap lets go of it */
    if (!NSPointInRect([self convertPoint:[evt locationInWindow] fromView:nil], [self rectOfCap:_down])) {
        apple2session_keyboard_press(_session, _down, 0);
        _down = -1;
        [self poll];
    }
}

- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    if (_down < 0) return;
    apple2session_keyboard_press(_session, _down, 0);
    _down = -1;
    [self poll];
}
@end

@implementation Apple2ControllersPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
/* AppKit would beep for Esc with no responder for it. */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

static __weak Apple2ControllersWindow *g_window;

@implementation Apple2ControllersWindow {
    apple2session *_session;
    Apple2AppKeyHandler _appKeys;
    Apple2ControllersPanel *_panel;
    Apple2FlippedView *_content;
    Apple2StageView *_stage;
    Apple2KeyboardView *_keyboard;
    NSSegmentedControl *_modeSeg;
    NSSegmentedControl *_modelSeg;
    NSButton *_keyboardBox;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "plays the joystick" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;                           /* the rows showing a pad */
    int _reservedRows;                      /* the rows the panel has room for */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSTextField *_pickLabel;
    NSPopUpButton *_pickPopup;              /* every target of the mode, for Map */
    NSMutableArray<NSNumber *> *_pickTargets;
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button (an axis, for a knob) to bind to that target */
    int _mapState;
    BOOL _placed;
    BOOL _optionLeft, _optionRight;         /* Open and Closed Apple held with the panel key */
}

- (instancetype)initWithSession:(apple2session *)session appKeys:(Apple2AppKeyHandler)appKeys
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _appKeys = [appKeys copy];
    _mapState = -2;
    _padGeneration = (unsigned)-1;
    _reservedRows = MIN_PAD_ROWS;
    _pickTargets = [NSMutableArray array];
    g_window = self;

    _panel = [[Apple2ControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, WIN_W, 700)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];
    Apple2SetHelpContext(_panel, @"controllers");

    _content = [[Apple2FlippedView alloc] initWithFrame:NSMakeRect(0, 0, WIN_W, 700)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _stage = [[Apple2StageView alloc] initWithFrame:NSZeroRect];
    _stage.owner = self;
    _stage.session = _session;
    _stage.mapTarget = -1;
    _stage.showKeys = apple2session_get_int(_session, "controllers_show_bindings", 0) > 0;
    _stage.mode = apple2session_controller_mode(_session);
    _stage.model = apple2session_controller_model(_session);
    [_content addSubview:_stage];

    _modeSeg = [NSSegmentedControl segmentedControlWithLabels:@[@"Joystick", @"Paddles"]
                                                 trackingMode:NSSegmentSwitchTrackingSelectOne
                                                       target:self
                                                       action:@selector(modeChanged:)];
    [_modeSeg setRefusesFirstResponder:YES];
    [_content addSubview:_modeSeg];
    _modelSeg = [NSSegmentedControl segmentedControlWithLabels:@[@"Beige", @"Platinum"]
                                                  trackingMode:NSSegmentSwitchTrackingSelectOne
                                                        target:self
                                                        action:@selector(modelChanged:)];
    [_modelSeg setRefusesFirstResponder:YES];
    [_content addSubview:_modelSeg];
    _keyboardBox = [NSButton checkboxWithTitle:@"Keyboard" target:self action:@selector(toggleKeyboard:)];
    [_keyboardBox setState:apple2session_get_int(_session, "controllers_keyboard", 0) ? NSControlStateValueOn
                                                                                       : NSControlStateValueOff];
    [_keyboardBox setRefusesFirstResponder:YES];
    [_content addSubview:_keyboardBox];

    _keyboard = [[Apple2KeyboardView alloc] initWithFrame:NSZeroRect];
    _keyboard.session = _session;
    [_content addSubview:_keyboard];

    /* Gamepads: one row per pad, its name, the port it plays and the one it is
     * set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_stage.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _pickLabel = [NSTextField labelWithString:@"Control"];
    [_content addSubview:_pickLabel];
    _pickPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _pickPopup.target = self;
    _pickPopup.action = @selector(pickChanged:);
    [_pickPopup setRefusesFirstResponder:YES];
    [_content addSubview:_pickPopup];
    _helpButton = Apple2HelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_stage configure];
    [_keyboard configure];
    [self syncControls];
    [self rebuildPickList];
    [self setMapState:-2];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- layout ------------------------------------------------------------------------- */

/* The Mode, Colour and Keyboard controls as the session has them. */
- (void)syncControls
{
    const int mode = apple2session_controller_mode(_session);
    const int model = apple2session_controller_model(_session);
    _stage.mode = mode;
    _stage.model = model;
    [_modeSeg setSelectedSegment:mode == APPLE2_MODE_PADDLES ? 1 : 0];
    [_modelSeg setSelectedSegment:model == APPLE2_MODEL_IIC ? 1 : 0];
}

- (BOOL)keyboardShown
{
    return [_keyboardBox state] == NSControlStateValueOn;
}

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize ssize = _stage.contentSize;
    const CGFloat inner = WIN_W - 2 * MARGIN;
    CGFloat ypos = ssize.height + 6;
    const CGFloat ctlY = ypos;
    ypos += 24 + 8;
    const BOOL showKbd = [self keyboardShown];
    const CGFloat kbdY = ypos;
    if (showKbd) ypos += _keyboard.contentSize.height + 8;
    const CGFloat padsY = ypos;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + _reservedRows * ROW_H + 6;
    const CGFloat pickY = mapY + BTN_H + 6;
    const CGFloat hintY = pickY + 26 + 4;
    const CGFloat total = hintY + 30 + MARGIN;

    [self fitPanel:NSMakeSize(WIN_W, total)];
    [_stage setFrame:NSMakeRect(0, 0, ssize.width, ssize.height)];
    [_modeSeg setFrame:NSMakeRect(MARGIN, ctlY, 170, 24)];
    [_modelSeg setFrame:NSMakeRect(MARGIN + 182, ctlY, 160, 24)];
    [_keyboardBox setFrame:NSMakeRect(MARGIN + 356, ctlY + 1, 110, 22)];
    _keyboard.hidden = !showKbd;
    [_keyboard setFrame:NSMakeRect(0, kbdY, WIN_W, _keyboard.contentSize.height)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const CGFloat rowY = rowsY + row * ROW_H;
        [_padName[row] setFrame:NSMakeRect(MARGIN, rowY + 4, inner - POPUP_W - EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 6, rowY + 4, EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, rowY - 1, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 56, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 62, mapY, 56, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 124, mapY, 72, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 206, mapY + 3, 122, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(WIN_W - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_pickLabel setFrame:NSMakeRect(MARGIN, pickY + 4, 50, 18)];
    [_pickPopup setFrame:NSMakeRect(MARGIN + 54, pickY, inner - 54, 25)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 30)];
    [_stage setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_stage letGo];
    [_keyboard letGo];
}

/* ---- Mode, Colour, Keyboard ---------------------------------------------------------- */

- (void)modeChanged:(NSSegmentedControl *)sender
{
    const int mode = sender.selectedSegment == 1 ? APPLE2_MODE_PADDLES : APPLE2_MODE_JOYSTICK;
    if (mode == apple2session_controller_mode(_session)) return;
    [self setMapState:-2];
    [self letGo];
    apple2session_set_controller_mode(_session, mode);
    [self applyMode];
}

/* The mode changed (here or in Settings): everything that depends on it. */
- (void)applyMode
{
    _stage.mode = apple2session_controller_mode(_session);
    [_modeSeg setSelectedSegment:_stage.mode == APPLE2_MODE_PADDLES ? 1 : 0];
    [self rebuildPickList];
    [self refreshPads];
    [self refreshLabels];
}

- (void)modelChanged:(NSSegmentedControl *)sender
{
    const int model = sender.selectedSegment == 1 ? APPLE2_MODEL_IIC : APPLE2_MODEL_IIE;
    apple2session_set_controller_model(_session, model);
    _stage.model = model;
}

- (void)toggleKeyboard:(id)sender
{
    (void)sender;
    const BOOL show = [self keyboardShown];
    apple2session_set_int(_session, "controllers_keyboard", show ? 1 : 0);
    if (!show) [_keyboard letGo];
    [self relayout];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (void)rebuildPickList
{
    const int mode = apple2session_controller_mode(_session);
    [_pickTargets removeAllObjects];
    [_pickPopup removeAllItems];
    for (int target = 0; target < APPLE2_TARGET_COUNT; target++) {
        if (!bindings_target_in_mode(target, mode)) continue;
        [_pickTargets addObject:@(target)];
        [[_pickPopup menu] addItemWithTitle:nsstr(bindings_target_label(target)) action:nil keyEquivalent:@""];
    }
}

- (NSString *)legendForTarget:(int)target
{
    /* a knob follows a gamepad axis; every other target has a key */
    if (isKnobTarget(target)) return padInputName(bindings_target_pad(target));
    return keyName(bindings_target_keysym(target));
}

/* What is bound to a target, for the status line. */
- (NSString *)bindingText:(int)target
{
    if (isKnobTarget(target)) {
        const int pad = bindings_target_pad(target);
        return pad >= 0 ? [NSString stringWithFormat:@"axis %@", padInputName(pad)] : @"no axis";
    }
    NSString *key = keyName(bindings_target_keysym(target));
    NSString *text = key.length ? key : @"no key";
    const int pad = bindings_target_pad(target);
    if (pad >= 0) text = [NSString stringWithFormat:@"%@, pad %@", text, padInputName(pad)];
    return text;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _stage.mapping = _mapState != -2;
    _stage.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_stage setNeedsDisplay:YES];
    [_stage poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        if (isKnobTarget(state)) {
            apple2session_gamepad_capture_begin_axis(_session);
            _hint.stringValue = [NSString stringWithFormat:
                @"Move a gamepad stick or trigger for %@ (now: %@). The knob takes an axis, not a key. "
                @"Esc cancels.", nsstr(bindings_target_label(state)), [self bindingText:state]];
        } else {
            apple2session_gamepad_capture_begin(_session);
            _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc cancels.",
                                 nsstr(bindings_target_label(state)), [self bindingText:state]];
        }
    } else {
        apple2session_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a control or a label, or choose one from the list, to remap it."
            : @"Hold a button, drag the stick, turn a knob (drag it, or scroll). "
              @"Keys and gamepads work here too.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    setButtonAccent(_mapButton, state != -2);
    _clearButton.enabled = state >= 0;
    _pickPopup.enabled = state != -2;
    if (state >= 0) {
        const NSUInteger at = [_pickTargets indexOfObject:@(state)];
        if (at != NSNotFound) [_pickPopup selectItemAtIndex:(NSInteger)at];
    }
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2 && bindings_target_in_mode(target, apple2session_controller_mode(_session)))
        [self setMapState:target];
}

- (void)pickChanged:(NSPopUpButton *)sender
{
    if (_mapState == -2) return;
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel >= 0 && sel < (NSInteger)_pickTargets.count) [self setMapState:[_pickTargets[(NSUInteger)sel] intValue]];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(int)stolen
{
    NSString *what = nsstr(bindings_target_label(target));
    _hint.stringValue = stolen >= 0 && stolen < APPLE2_TARGET_COUNT
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, nsstr(bindings_target_label(stolen))]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)keysym
{
    const int target = _mapState;
    if (isKnobTarget(target)) {
        _hint.stringValue = @"The knob follows a gamepad axis: move a stick or trigger. Esc cancels.";
        return;
    }
    int stolen = -1;
    bindings_set_keysym(_session, target, keysym, &stolen);
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int ident = APPLE2_PAD_BTN_NONE;
    if (_mapState < 0 || !apple2session_gamepad_capture_poll(_session, &ident)) return;
    int stolen = -1;
    const int target = _mapState;
    bindings_set_pad(_session, target, ident, &stolen);
    [self setMapState:-1];
    [self showBound:padInputName(ident) target:target stolen:stolen];
}

/* A key typed while Map waits for one, from any window (Apple2KeyForward's
 * hook): Esc backs out of the pick; F9, F11, F12 and the Apple keys are the
 * app's and the machine's. */
- (BOOL)mapKeyFromApp:(uint32_t)keysym
{
    if (_mapState < 0) return NO;
    if (keysym == APPLE2SESSION_KEYSYM_F9 || keysym == APPLE2SESSION_KEYSYM_F11 ||
        keysym == APPLE2SESSION_KEYSYM_F12 || keysym == APPLE2SESSION_KEYSYM_LALT ||
        keysym == APPLE2SESSION_KEYSYM_RALT || keysym == APPLE2SESSION_KEYSYM_LMETA ||
        keysym == APPLE2SESSION_KEYSYM_RMETA || keysym == APPLE2SESSION_KEYSYM_LSUPER ||
        keysym == APPLE2SESSION_KEYSYM_RSUPER)
        return NO;
    if (keysym == APPLE2SESSION_KEYSYM_ESCAPE) [self setMapState:-1];
    else [self bindKey:keysym];
    return YES;
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    bindings_clear(_session, target);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", nsstr(bindings_target_label(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    bindings_reset_defaults(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    apple2session_set_int(_session, "controllers_show_bindings", show ? 1 : 0);
    _stage.showKeys = show;
    [_stage setNeedsDisplay:YES];
}

/* A console button on the panel was released over itself. */
- (void)runSysaction:(int)sysact
{
    if (self.sysactionHandler) self.sysactionHandler(sysact);
    else apple2session_sysaction_fire(_session, (apple2session_sysaction)sysact);
}

/* ---- gamepads ----------------------------------------------------------------------- */

/* The rows a pad can be assigned to in this mode: Automatic, then the joystick
 * or Paddle 0 and Paddle 1. */
- (NSArray<NSString *> *)assignTitles
{
    return apple2session_controller_ports(_session) > 1
        ? @[@"Automatic", @"Paddle 0", @"Paddle 1"]
        : @[@"Automatic", @"Joystick"];
}

- (void)refreshPadPorts
{
    const BOOL paddles = apple2session_controller_ports(_session) > 1;
    for (int row = 0; row < _padRows; row++) {
        const int port = apple2session_gamepad_effective_port(_session, row);
        NSString *text;
        if (port < 0) text = @"plays nothing";
        else text = paddles ? [NSString stringWithFormat:@"plays paddle %d", port] : @"plays the joystick";
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

- (void)refreshPads
{
    const int count = apple2session_gamepad_count(_session);
    const int ports = apple2session_controller_ports(_session);
    _padGeneration = apple2session_gamepad_generation(_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more
     * pads are connected); the panel changes size only then */
    const int rooms = _padRows > MIN_PAD_ROWS ? _padRows : MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", MAX_PAD_ROWS, count]
        : @"Gamepads";
    NSArray<NSString *> *titles = [self assignTitles];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        apple2session_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = nsstr(name);
        _padName[row].toolTip = nsstr(name);
        [_padAssign[row] removeAllItems];
        [_padAssign[row] addItemsWithTitles:titles];
        const int assign = apple2session_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= ports ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    apple2session_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads and a mode or model changed in Settings. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (apple2session_controller_mode(_session) != _stage.mode) [self applyMode];
        else if (apple2session_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
        if (apple2session_controller_model(_session) != _stage.model) [self syncControls];
    }
    [_stage poll];
    if (![self keyboardShown]) return;
    [_keyboard poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak Apple2ControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)evt
{
    /* Command shortcuts are the menus'; AppKit sends no keyUp while Command
     * is down, so forwarding one would leave the machine holding the key. */
    if ([evt modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t keysym = Apple2KeysymFromEvent(evt);
    if (keysym == APPLE2SESSION_KEYSYM_F9 || keysym == APPLE2SESSION_KEYSYM_F12) {
        if (![evt isARepeat] && _appKeys) _appKeys(keysym);
        return;
    }
    if (keysym == APPLE2SESSION_KEYSYM_F11) return;
    if (_mapState >= 0) {
        if ([evt isARepeat]) return;
        /* the next key is the binding; Esc backs out of the pick */
        [self mapKeyFromApp:keysym];
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    /* the main window's path: a bound key drives its controller (or posts its
     * console action), anything else is typed; the main window does not repeat
     * a key either */
    if ([evt isARepeat]) return;
    Apple2ForwardKeyEvent(_session, evt, 1);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    if (!Apple2KeysymFromEvent(evt)) return;
    Apple2ForwardKeyEvent(_session, evt, 0);
}

/* Shift, Control and Caps Lock bind too; the Option keys are Open and Closed
 * Apple, as in the machine window. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)evt
{
    if (_mapState == -1) return;
    const NSEventModifierFlags flags = [evt modifierFlags];
    if ([evt keyCode] == OPTION_LEFT || [evt keyCode] == OPTION_RIGHT) {
        if (_mapState >= 0) return;     /* the Apple keys are never bound */
        const BOOL down = (flags & NSEventModifierFlagOption) != 0;
        if ([evt keyCode] == OPTION_LEFT) {
            if (down != _optionLeft) {
                _optionLeft = down;
                apple2session_key(_session, down ? 1 : 0, APPLE2SESSION_KEYSYM_LALT, 0, 0, 0);
            }
        } else if (down != _optionRight) {
            _optionRight = down;
            apple2session_key(_session, down ? 1 : 0, APPLE2SESSION_KEYSYM_RALT, 0, 0, 0);
        }
        return;
    }
    Apple2ForwardModifierEvent(_session, evt);
}

/* ---- the window --------------------------------------------------------------------- */

/* Lets go of an Apple key the panel holds. */
- (void)releaseOptionKeys
{
    if (_optionLeft) {
        _optionLeft = NO;
        apple2session_key(_session, 0, APPLE2SESSION_KEYSYM_LALT, 0, 0, 0);
    }
    if (_optionRight) {
        _optionRight = NO;
        apple2session_key(_session, 0, APPLE2SESSION_KEYSYM_RALT, 0, 0, 0);
    }
}

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    [self releaseOptionKeys];
    apple2session_release_all(_session);
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [self syncControls];
    [self rebuildPickList];
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    [self syncControls];
    [self rebuildPickList];
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    [self releaseOptionKeys];
    apple2session_release_all(_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end

BOOL Apple2ControllersMapKey(uint32_t keysym)
{
    Apple2ControllersWindow *win = g_window;
    return win ? [win mapKeyFromApp:keysym] : NO;
}

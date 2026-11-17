/*
 * The emulator display (FN_VIEW, named in DisplayGlue.h) -- the framebuffer, drawn the way a game draws: a Metal
 * layer fed from a CVDisplayLink.
 *
 * CVDisplayLink is this platform's frame clock -- the equivalent of
 * GdkFrameClock and DwmFlush -- and feeding it to the session is what lets
 * the emulator phase-lock to the panel instead of beating against it.
 *
 * What makes this a game's display path rather than a window's:
 *
 *   - The picture is a MTLTexture, scaled by the GPU in one triangle (nearest
 *     or linear), presented on a CAMetalLayer with display sync. Nothing is
 *     rasterised on the CPU and AppKit allocates no backing store.
 *   - The display link's own thread uploads, draws and presents. There is no
 *     per-frame hop to the main thread, so a menu, a debugger window or a
 *     resize drag cannot make a frame late.
 *   - The link follows the window to whichever panel it is on, and a link
 *     whose panel runs at a multiple of the machine's rate (120 Hz ProMotion
 *     under a 60 Hz machine) hands the session every Nth tick, so the
 *     machine still phase-locks instead of falling back to the wall clock.
 *   - The link runs ONLY while the window can be seen: on screen, not
 *     occluded, not miniaturized, the app not hidden. A window nobody sees
 *     costs no ticks; the machine keeps running (netplay, audio) and paces on
 *     the wall clock until the link returns.
 *   - A tick draws only when the session published a new frame or the size,
 *     shape or filter changed: a static CONFIG screen costs nothing.
 *   - Three textures take turns, with a semaphore, so a frame is never
 *     rewritten while the GPU may still read it.
 *
 * With no Metal device (a headless VM) the view falls back to the original
 * CoreGraphics drawRect: path, and the link's tick hops to the main thread.
 *
 * This file is the same in every Go desktop; what differs per machine is in
 * DisplayGlue.h (the session's functions, the frame size, the picture shape)
 * and, where a machine has a pointer device, the extras it includes
 * (FN_EXTRA_IVARS, FN_EXTRA_METHODS with an extraInit method) and, for a
 * whole-number scale mode, FN_SNAP_SIZE (fn_snap_size in the glue).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"
#include "DisplayGlue.h"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

/* CoreVideo hands out host time in mach_absolute_time units, which are NOT
 * nanoseconds on every machine -- the timebase ratio converts them. */
#include <mach/mach_time.h>

#include <math.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TEX_COUNT 3

/* Machines whose frames vary in WIDTH too (the Atari 8-bit's 80-column cards)
 * define FN_VARIABLE_WIDTH and give the glue fn_pull_frame_w and
 * fn_picture_aspect_w; the rest have one width, FN_FB_W. */
#ifdef FN_VARIABLE_WIDTH
#define FN_ASPECT(w, l, tv) fn_picture_aspect_w(_session, (w), (l), (tv))
#else
#define FN_ASPECT(w, l, tv) fn_picture_aspect(_session, (l), (tv))
#endif

/* One triangle covers the viewport; the vertex shader maps it onto the
 * visible part of the texture (a machine may make fewer lines than the
 * buffer holds). Alpha in the frames is 0 ("skip"), so the shader writes 1. */
static NSString *const kShaderSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 pos [[position]]; float2 uv; };\n"
     "vertex VOut vmain(uint vid [[vertex_id]], constant float4 &u [[buffer(0)]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "  VOut o;\n"
     "  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
     "  o.uv = float2(p.x, 1.0 - p.y) * u.xy;\n"
     "  return o;\n"
     "}\n"
     "fragment float4 fmain(VOut in [[stage_in]], constant float4 &u [[buffer(0)]],\n"
     "                      texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "  float2 uv = min(in.uv, u.zw);\n"
     "  return float4(t.sample(s, uv).rgb, 1.0);\n"
     "}\n";

@implementation FN_VIEW {
    FN_SESSION *_session;
    CVDisplayLinkRef _link;
    BOOL _linkRunning;               /* main thread */
    uint32_t *_fb;                   /* staging: the newest frame's pixels (under _fbLock) */
    os_unfair_lock _fbLock;
    int _width, _height;             /* its visible size */
    _Atomic uint64_t _frameCount;    /* frames pulled since it was made */
    uint64_t _serial;                /* the link thread's: last frame copied */
    uint64_t _tickCount;
    _Atomic int _tv;
    _Atomic int _smooth;
    _Atomic int _redraw;             /* size/shape/filter changed, or link (re)started */

    /* Metal path (all nil when there is no device) */
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLSamplerState> _nearest, _linear;
    id<MTLTexture> _tex[TEX_COUNT];
    int _texNext;
    int _texLast;                    /* the texture holding the newest frame, -1 none */
    dispatch_semaphore_t _inflight;

    /* Fallback path */
    uint32_t *_shown;                /* what drawRect: draws (a snapshot of _fb) */
    CGColorSpaceRef _cs;
#ifdef FN_EXTRA_IVARS
    FN_EXTRA_IVARS                   /* per-machine state (DisplayGlue.h) */
#endif
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)out; (void)flagsIn; (void)flagsOut;
    FN_VIEW *self = (__bridge FN_VIEW *)ctx;
    [self tick:now->hostTime];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(FN_SESSION *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, FN_INITIAL_W, FN_INITIAL_H)];
    if (!self) return nil;
    _session = session;
    atomic_init(&_tv, 1);
    atomic_init(&_smooth, 0);
    atomic_init(&_redraw, 1);
    atomic_init(&_frameCount, 0);
    _fbLock = OS_UNFAIR_LOCK_INIT;
    _width = FN_FB_W;
    _texLast = -1;

    const size_t n = (size_t)FN_FB_W * FN_FB_H;
    _fb = calloc(n, sizeof *_fb);

    if ([self setUpMetal]) {
        [self setWantsLayer:YES];          /* makeBackingLayer gives the CAMetalLayer */
    } else {
        _cs = CGColorSpaceCreateDeviceRGB();
        _shown = calloc(n, sizeof *_shown);
    }
#ifdef FN_EXTRA_METHODS
    [self extraInit];
#endif
    return self;
}

/* ---- Metal ------------------------------------------------------------------ */

- (BOOL)setUpMetal
{
    if ([self buildMetal]) return YES;
    _device = nil;
    _queue = nil;
    _pipeline = nil;
    _nearest = _linear = nil;
    for (int i = 0; i < TEX_COUNT; i++) _tex[i] = nil;
    return NO;
}

- (BOOL)buildMetal
{
    _device = MTLCreateSystemDefaultDevice();
    if (!_device) return NO;
    _queue = [_device newCommandQueue];
    if (!_queue) return NO;

    NSError *err = nil;
    id<MTLLibrary> lib = [_device newLibraryWithSource:kShaderSource options:nil error:&err];
    if (!lib) return NO;

    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!_pipeline) return NO;

    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _nearest = [_device newSamplerStateWithDescriptor:sd];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    _linear = [_device newSamplerStateWithDescriptor:sd];
    if (!_nearest || !_linear) return NO;

    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:FN_FB_W
                                    height:FN_FB_H
                                 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = [_device hasUnifiedMemory] ? MTLStorageModeShared : MTLStorageModeManaged;
    for (int i = 0; i < TEX_COUNT; i++) {
        _tex[i] = [_device newTextureWithDescriptor:td];
        if (!_tex[i]) return NO;
    }
    _inflight = dispatch_semaphore_create(TEX_COUNT);
    return YES;
}

/* The root layer: Metal, opaque, synced to the display. */
- (CALayer *)makeBackingLayer
{
    if (!_device) return [super makeBackingLayer];
    CAMetalLayer *ml = [CAMetalLayer layer];
    ml.device = _device;
    ml.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ml.framebufferOnly = YES;
    ml.opaque = YES;
    ml.displaySyncEnabled = YES;
    ml.maximumDrawableCount = 3;
    ml.presentsWithTransaction = NO;
    ml.contentsGravity = kCAGravityResize;
    return ml;
}

/* The layer is Metal and the link thread owns its contents: AppKit must
 * neither draw into it nor replace it. */
- (BOOL)wantsUpdateLayer { return _device != nil; }
- (void)updateLayer {}
- (BOOL)isOpaque { return YES; }
#ifdef FN_ACCEPTS_FIRST_RESPONDER
- (BOOL)acceptsFirstResponder { return YES; }
#endif

/* Keep the drawable the size of the view in PIXELS. */
- (void)updateDrawableSize
{
    if (!_device) return;
    CAMetalLayer *ml = (CAMetalLayer *)[self layer];
    if (![ml isKindOfClass:[CAMetalLayer class]]) return;
    CGFloat scale = [[self window] backingScaleFactor];
    if (scale < 1.0) scale = [[NSScreen mainScreen] backingScaleFactor];
    if (scale < 1.0) scale = 1.0;
    ml.contentsScale = scale;
    const NSSize b = [self bounds].size;
    ml.drawableSize = CGSizeMake(floor(b.width * scale), floor(b.height * scale));
    atomic_store(&_redraw, 1);
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

/* ---- the frame clock: only while the window can be seen --------------------- */

- (BOOL)shouldRun
{
    NSWindow *w = [self window];
    if (!w || ![w isVisible] || [w isMiniaturized]) return NO;
    if (!([w occlusionState] & NSWindowOcclusionStateVisible)) return NO;
    if ([NSApp isHidden]) return NO;
    return YES;
}

/* The panel the window is on: the link follows it, so its ticks come at that
 * panel's refresh. */
- (void)retargetLink
{
    NSScreen *screen = [[self window] screen];
    NSNumber *num = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
    if (num && _link) CVDisplayLinkSetCurrentCGDisplay(_link, (CGDirectDisplayID)[num unsignedIntValue]);
}

- (void)startLink
{
    if (!_link) {
        if (CVDisplayLinkCreateWithActiveCGDisplays(&_link) != kCVReturnSuccess) {
            _link = NULL;
            return;
        }
        CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
    }
    [self retargetLink];
    if (!_linkRunning) {
        atomic_store(&_redraw, 1);     /* the first tick paints what is there */
        CVDisplayLinkStart(_link);
        _linkRunning = YES;
    }
}

- (void)stopLink
{
    if (_link && _linkRunning) CVDisplayLinkStop(_link);
    _linkRunning = NO;
}

- (void)updateVisibility
{
    if ([self shouldRun]) [self startLink];
    else [self stopLink];
}

- (void)visibilityChanged:(NSNotification *)note
{
    /* a closing window is still "visible" while the notice is delivered */
    if ([[note name] isEqualToString:NSWindowWillCloseNotification]) {
        [self stopLink];
        return;
    }
    [self updateVisibility];
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc removeObserver:self];
    NSWindow *w = [self window];
    if (w) {
        NSArray<NSString *> *names = @[
            NSWindowDidChangeOcclusionStateNotification,
            NSWindowDidMiniaturizeNotification,
            NSWindowDidDeminiaturizeNotification,
            NSWindowDidChangeScreenNotification,
            NSWindowWillCloseNotification,
        ];
        for (NSString *name in names)
            [nc addObserver:self selector:@selector(visibilityChanged:) name:name object:w];
        /* Cmd+H hides every window without minimising them */
        [nc addObserver:self selector:@selector(visibilityChanged:)
                   name:NSApplicationDidHideNotification object:nil];
        [nc addObserver:self selector:@selector(visibilityChanged:)
                   name:NSApplicationDidUnhideNotification object:nil];
    }
    [self updateDrawableSize];
    [self updateVisibility];
}

/* Final teardown: the link and the observers. */
- (void)stop
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self stopLink];
    if (_link) {
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}

- (void)dealloc
{
    [self stop];
    if (_cs) CGColorSpaceRelease(_cs);
    free(_fb);
    free(_shown);
}

/* ---- the link's thread -------------------------------------------------------- */

/* How many display ticks make one machine frame: 1 at 60 Hz, 2 at 120 Hz. A
 * panel that is no whole multiple (a 60 Hz panel under a PAL machine) gets 1
 * and the session's own tolerance refuses the lock, as before. */
- (unsigned)ticksPerMachineFrame
{
    const double disp = _link ? CVDisplayLinkGetActualOutputVideoRefreshPeriod(_link) : 0.0;
    const int rate = fn_machine_hz(_session);
    if (disp <= 0.0 || rate <= 0) return 1;
    const double ratio = (1.0 / (double)rate) / disp;
    const long n = lround(ratio);
    if (n < 2 || fabs(ratio - (double)n) > 0.08 * (double)n) return 1;
    return (unsigned)n;
}

- (void)tick:(uint64_t)hostTime
{
    @autoreleasepool {
        static double toNs = 0.0;
        if (toNs == 0.0) {
            mach_timebase_info_data_t tb;
            mach_timebase_info(&tb);
            toNs = (double)tb.numer / (double)tb.denom;
        }
        const unsigned div = [self ticksPerMachineFrame];
        if (div == 1 || (_tickCount++ % div) == 0)
            fn_notify_vsync(_session, (int64_t)((double)hostTime * toNs));

        int w = FN_FB_W, h = 0;
        os_unfair_lock_lock(&_fbLock);
#ifdef FN_VARIABLE_WIDTH
        const BOOL fresh = fn_pull_frame_w(_session, _fb, &w, &h, &_serial) != 0;
#else
        const BOOL fresh = fn_pull_frame(_session, _fb, &h, &_serial) != 0;
#endif
        if (fresh) {
            _width = w;
            _height = h;
            atomic_fetch_add(&_frameCount, 1);
        }
        if (_device)
            [self drawMetalNewFrame:fresh];          /* uploads _fb: still under the lock */
        os_unfair_lock_unlock(&_fbLock);

        if (!_device && fresh) {
            /* AppKit is main-thread only; the display link's callback is not. */
            dispatch_async(dispatch_get_main_queue(), ^{ [self setNeedsDisplay:YES]; });
        }
    }
}

/* Where the picture sits in the view, in points, letterboxed: for hit-testing
 * the pointer (a light gun, an analog stick). Main thread. */
- (NSRect)pictureRect
{
    const NSRect b = [self bounds];
    if (_height <= 0 || b.size.width <= 0 || b.size.height <= 0) return NSZeroRect;
    const double want = FN_ASPECT(_width, _height, atomic_load(&_tv) != 0);
    const double w = b.size.width, h = b.size.height;
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
#ifdef FN_SNAP_SIZE
    { CGSize snapped; if (fn_snap_size(b.size, _width, _height, &snapped)) { sw = snapped.width; sh = snapped.height; } }
#endif
    return NSMakeRect((w - sw) / 2, (h - sh) / 2, sw, sh);
}

/* The letterboxed rectangle, in the drawable's pixels. */
- (CGRect)pictureRectIn:(CGSize)ds width:(int)width lines:(int)lines
{
    const double want = FN_ASPECT(width, lines, atomic_load(&_tv) != 0);
    double sw, sh;
    if (ds.width / ds.height > want) { sh = ds.height; sw = sh * want; }
    else                             { sw = ds.width;  sh = sw / want; }
#ifdef FN_SNAP_SIZE
    { CGSize snapped; if (fn_snap_size(ds, width, lines, &snapped)) { sw = snapped.width; sh = snapped.height; } }
#endif
    return CGRectMake(floor((ds.width - sw) / 2), floor((ds.height - sh) / 2),
                      floor(sw), floor(sh));
}

- (void)drawMetalNewFrame:(BOOL)fresh
{
    const BOOL redraw = atomic_exchange(&_redraw, 0) != 0;
    if (!fresh && !(redraw && _texLast >= 0)) {
        if (redraw) atomic_store(&_redraw, 1);   /* nothing to show yet */
        return;
    }
    CAMetalLayer *ml = (CAMetalLayer *)[self layer];
    if (![ml isKindOfClass:[CAMetalLayer class]]) return;
    const CGSize ds = ml.drawableSize;
    if (ds.width < 1 || ds.height < 1 || _height <= 0) return;

    if (fresh) {
        /* A texture the GPU is not reading: the semaphore counts the ones in
         * flight. A GPU that is stuck costs this frame, not the thread. */
        if (dispatch_semaphore_wait(_inflight, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) != 0) {
            atomic_store(&_redraw, 1);
            return;
        }
        const int idx = _texNext;
        _texNext = (_texNext + 1) % TEX_COUNT;
        [_tex[idx] replaceRegion:MTLRegionMake2D(0, 0, _width, _height)
                     mipmapLevel:0
                       withBytes:_fb
                     bytesPerRow:(NSUInteger)_width * 4];
        _texLast = idx;
    } else if (dispatch_semaphore_wait(_inflight, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) != 0) {
        atomic_store(&_redraw, 1);
        return;
    }

    id<CAMetalDrawable> drawable = [ml nextDrawable];
    if (!drawable) {
        dispatch_semaphore_signal(_inflight);
        atomic_store(&_redraw, 1);
        return;
    }

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    const CGRect r = [self pictureRectIn:ds width:_width lines:_height];
    MTLViewport vp = { r.origin.x, r.origin.y, r.size.width, r.size.height, 0.0, 1.0 };
    const float u[4] = {
        (float)_width / (float)FN_FB_W, (float)_height / (float)FN_FB_H,
        ((float)_width - 0.5f) / (float)FN_FB_W,
        ((float)_height - 0.5f) / (float)FN_FB_H,
    };
    [enc setViewport:vp];
    [enc setRenderPipelineState:_pipeline];
    [enc setVertexBytes:u length:sizeof u atIndex:0];
    [enc setFragmentBytes:u length:sizeof u atIndex:0];
    [enc setFragmentTexture:_tex[_texLast] atIndex:0];
    [enc setFragmentSamplerState:(atomic_load(&_smooth) ? _linear : _nearest) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];

    dispatch_semaphore_t sem = _inflight;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        dispatch_semaphore_signal(sem);
    }];
    [cb presentDrawable:drawable];
    [cb commit];
}

/* ---- settings (main thread) --------------------------------------------------- */

- (void)setTvAspect:(BOOL)tv
{
    atomic_store(&_tv, tv ? 1 : 0);
    atomic_store(&_redraw, 1);
    [self setNeedsDisplay:YES];
}

- (void)setSmooth:(BOOL)smooth
{
    atomic_store(&_smooth, smooth ? 1 : 0);
    atomic_store(&_redraw, 1);
    [self setNeedsDisplay:YES];
}

/* ---- the fallback: CoreGraphics ----------------------------------------------- */

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if (_device) return;                 /* the layer is Metal; nothing to draw */
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];

    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, b);

    /* The link's thread rewrites _fb every frame: draw a snapshot taken
     * under the lock, or the picture tears. */
    os_unfair_lock_lock(&_fbLock);
    const int fw = _width, fh = _height;
    if (fw > 0 && fh > 0) memcpy(_shown, _fb, (size_t)fw * (size_t)fh * sizeof *_shown);
    os_unfair_lock_unlock(&_fbLock);
    if (fw <= 0 || fh <= 0) return;

    /* The session's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit image with the alpha byte skipped reads them as they are. */
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, _shown, (size_t)fw * (size_t)fh * 4, NULL);
    if (!dp) return;
    CGImageRef img = CGImageCreate((size_t)fw, (size_t)fh, 8, 32, (size_t)fw * 4, _cs,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    if (!img) return;

    const double want = FN_ASPECT(fw, fh, atomic_load(&_tv) != 0);
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
#ifdef FN_SNAP_SIZE
    { CGSize snapped; if (fn_snap_size(b.size, fw, fh, &snapped)) { sw = snapped.width; sh = snapped.height; } }
#endif

    CGContextSetInterpolationQuality(dc, atomic_load(&_smooth) ? kCGInterpolationHigh : kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake((w - sw) / 2, (h - sh) / 2, sw, sh), img);
    CGImageRelease(img);
}

#ifdef FN_EXTRA_METHODS
#include FN_EXTRA_METHODS
#endif
@end

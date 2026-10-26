/*
 * The emulator picture, drawn the way a game draws: Direct3D 11 into a DXGI
 * flip-model swap chain, presented on vblank from a render thread of its own.
 *
 *   The render thread runs under MMCSS "Games" priority. Each pass waits on
 *   the swap chain's frame-latency object (maximum latency 1, so a frame is
 *   never queued behind another), feeds that tick to the core's vsync phase
 *   lock, takes the newest frame the core has published, uploads it to a
 *   dynamic texture, draws one triangle that covers the picture's viewport,
 *   and presents with sync interval 1. The display shows the latest frame at
 *   every refresh, whatever the monitor's rate.
 *
 *   The pixel shader is sharp bilinear by default: nearest-neighbour inside
 *   each console pixel, a one-screen-pixel blend at its edges, so a
 *   non-integer scale shows even pixels with no shimmer and no blur. Smooth
 *   Scaling swaps it for plain bilinear.
 *
 *   The shaders are compiled at start by d3dcompiler_47.dll, which every
 *   Windows 10 and 11 carries; it is loaded, not imported, so the exe still
 *   imports only system DLLs that are always present.
 *
 * Falls back, in order: a hardware device, WARP (the software rasterizer),
 * then GDI -- StretchDIBits on a present thread paced by DwmFlush(), with
 * only the letterbox bars filled so nothing flashes. APPLE2_RENDERER=gdi forces
 * the GDI path; APPLE2_FPS=1 prints the renderer and its frame rates to stderr
 * once a second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define COBJMACROS
#include <initguid.h>   /* defines the IIDs below, in this one file */

#include "display.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISPLAY_CLASS "Apple2Display"
#define FB_W APPLE2SESSION_FB_MAX_WIDTH   /* the texture; the frame is smaller */
#define FB_H APPLE2SESSION_FB_MAX_HEIGHT

static const char k_hlsl[] =
    "cbuffer C : register(b0) {\n"
    "    float4 sizes;   /* frame wh, viewport wh */\n"
    "    float4 tex_wh;  /* texture wh, smooth, 0 */\n"
    "};\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID)\n"
    "{\n"
    "    V o;\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.uv = uv;\n"
    "    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float4 ps(V i) : SV_Target\n"
    "{\n"
    "    float2 texel = i.uv * sizes.xy;\n"
    "    float2 p = texel;\n"
    "    if (tex_wh.z < 0.5) {\n"
    "        float2 scale = max(floor(sizes.zw / sizes.xy), 1.0);\n"
    "        float2 d = frac(texel) - 0.5;\n"
    "        float2 range = 0.5 - 0.5 / scale;\n"
    "        p = floor(texel) + (d - clamp(d, -range, range)) * scale + 0.5;\n"
    "    }\n"
    "    /* never blend in a row below the frame: the texture is taller */\n"
    "    p = clamp(p, 0.5, sizes.xy - 0.5);\n"
    "    return float4(tex.Sample(smp, p / tex_wh.xy).rgb, 1.0);\n"
    "}\n";

typedef enum { MODE_NONE, MODE_D3D, MODE_GDI } display_mode;

static apple2session *g_session;
static HWND g_hwnd;
static HANDLE g_thread;
static volatile LONG g_running;
static volatile LONG g_width, g_height;       /* the client, from WM_SIZE */
static volatile LONG g_aspect, g_smooth;   /* aspect: 0 TV 4:3, 1 square, 2 integer */
static display_mode g_mode;
static int g_warp;

/* ---- Direct3D 11 state (the render thread's alone) ---- */
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain1 *g_swap;
static ID3D11RenderTargetView *g_rtv;
static ID3D11Texture2D *g_tex;
static ID3D11ShaderResourceView *g_srv;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11SamplerState *g_smp;
static ID3D11Buffer *g_cb;
static HANDLE g_waitable;
static UINT g_swap_flags;
static int g_bb_w, g_bb_h;

/* ---- GDI fallback ---- */
static uint32_t g_gdi_fb[FB_W * FB_H];
static int g_gdi_w, g_gdi_h;
static CRITICAL_SECTION g_gdi_lock;

/* The picture inside a client of cw x ch, centred: the frame (fbw x fbh,
 * which a VidHD card changes at runtime) at 4:3 like a television, with
 * square pixels, or at the largest whole-number scale that fits. */
static RECT picture_rect(int cw, int ch, int fbw, int fbh)
{
    RECT r;
    double dw, dh;
    if (cw <= 0 || ch <= 0 || fbw <= 0 || fbh <= 0) { SetRect(&r, 0, 0, 0, 0); return r; }
    if (g_aspect == 2) {
        int scale = cw / fbw < ch / fbh ? cw / fbw : ch / fbh;
        if (scale < 1) scale = 1;
        dw = (double)scale * fbw;
        dh = (double)scale * fbh;
    } else {
        double want = g_aspect == 0 ? 4.0 / 3.0 : (double)fbw / fbh;
        if ((double)cw / ch > want) { dh = ch; dw = dh * want; }
        else                        { dw = cw; dh = dw / want; }
    }
    r.left = (int)((cw - dw) / 2);
    r.top = (int)((ch - dh) / 2);
    r.right = r.left + (int)dw;
    r.bottom = r.top + (int)dh;
    return r;
}

/* MMCSS "Games": the scheduler's class for a thread that must hit vblank. */
static void join_mmcss(void)
{
    typedef HANDLE (WINAPI *av_fn)(LPCWSTR, LPDWORD);
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    av_fn fn = avrt ? (av_fn)(void (*)(void))GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
    DWORD index = 0;
    if (!fn || !fn(L"Games", &index))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

/* The core's vsync phase lock: one tick per display refresh. */
static void tick(void)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    apple2session_notify_vsync(g_session, (int64_t)(t.QuadPart * 1000000000LL / f.QuadPart));
}

/* ---- Direct3D 11 ---- */

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static void d3d_release(void)
{
    if (g_ctx) ID3D11DeviceContext_ClearState(g_ctx);
    RELEASE(g_rtv);
    RELEASE(g_srv);
    RELEASE(g_tex);
    RELEASE(g_vs);
    RELEASE(g_ps);
    RELEASE(g_smp);
    RELEASE(g_cb);
    RELEASE(g_swap);
    RELEASE(g_ctx);
    RELEASE(g_dev);
    if (g_waitable) { CloseHandle(g_waitable); g_waitable = NULL; }
}

static int compile(const char *entry, const char *target, ID3DBlob **out)
{
    static pD3DCompile fn;
    static int looked;
    ID3DBlob *err = NULL;
    if (!looked) {
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        fn = dll ? (pD3DCompile)(void (*)(void))GetProcAddress(dll, "D3DCompile") : NULL;
        looked = 1;
    }
    if (!fn)
        return -1;
    if (FAILED(fn(k_hlsl, sizeof k_hlsl - 1, "apple2_display", NULL, NULL, entry, target,
                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err))) {
        if (err) {
            fprintf(stderr, "display: %s: %s\n", entry, (const char *)ID3D10Blob_GetBufferPointer(err));
            ID3D10Blob_Release(err);
        }
        return -1;
    }
    RELEASE(err);
    return 0;
}

static int make_target(void)
{
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return -1;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb, NULL, &g_rtv);
    ID3D11Texture2D_Release(bb);
    return SUCCEEDED(hr) ? 0 : -1;
}

/* The swap chain: flip-discard (Windows 10), flip-sequential (8), or the
 * old blit model, whichever the system takes. */
static int make_swap_chain(IDXGIFactory2 *factory, int w, int h)
{
    static const struct { DXGI_SWAP_EFFECT effect; DXGI_SCALING scaling; UINT flags; } tries[] = {
        { DXGI_SWAP_EFFECT_FLIP_DISCARD,    DXGI_SCALING_NONE,    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT },
        { DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SCALING_NONE,    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT },
        { DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SCALING_STRETCH, 0 },
        { DXGI_SWAP_EFFECT_DISCARD,         DXGI_SCALING_STRETCH, 0 },
    };
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        DXGI_SWAP_CHAIN_DESC1 sd;
        memset(&sd, 0, sizeof sd);
        sd.Width = (UINT)w;
        sd.Height = (UINT)h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = tries[i].effect == DXGI_SWAP_EFFECT_DISCARD ? 1 : 2;
        sd.Scaling = tries[i].scaling;
        sd.SwapEffect = tries[i].effect;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = tries[i].flags;
        if (SUCCEEDED(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)g_dev, g_hwnd, &sd,
                                                           NULL, NULL, &g_swap))) {
            g_swap_flags = sd.Flags;
            return 0;
        }
    }
    return -1;
}

static int d3d_init(int warp)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    IDXGIDevice1 *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    ID3DBlob *vsb = NULL, *psb = NULL;
    int w = g_width > 0 ? g_width : 1, h = g_height > 0 ? g_height : 1;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
                           &g_dev, NULL, &g_ctx);
    if (hr == E_INVALIDARG)     /* a pre-11.1 runtime refuses the 11_1 entry */
        hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3, D3D11_SDK_VERSION,
                               &g_dev, NULL, &g_ctx);
    if (FAILED(hr))
        goto fail;
    if (FAILED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice1, (void **)&dxgi)) ||
        FAILED(IDXGIDevice1_GetAdapter(dxgi, &adapter)) ||
        FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory)))
        goto fail;
    if (make_swap_chain(factory, w, h) != 0)
        goto fail;
    IDXGIFactory2_MakeWindowAssociation(factory, g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    /* never more than one frame between the core and the glass */
    if (g_swap_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        IDXGISwapChain2 *sc2 = NULL;
        if (SUCCEEDED(IDXGISwapChain1_QueryInterface(g_swap, &IID_IDXGISwapChain2, (void **)&sc2))) {
            IDXGISwapChain2_SetMaximumFrameLatency(sc2, 1);
            g_waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
            IDXGISwapChain2_Release(sc2);
        }
    } else {
        IDXGIDevice1_SetMaximumFrameLatency(dxgi, 1);
    }
    g_bb_w = w;
    g_bb_h = h;
    if (make_target() != 0)
        goto fail;

    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = FB_W;
        td.Height = FB_H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;     /* the core's 0x00RRGGBB */
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_tex, NULL, &g_srv)))
            goto fail;
    }
    {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_smp)))
            goto fail;
    }
    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb)))
            goto fail;
    }
    if (compile("vs", "vs_4_0", &vsb) != 0 || compile("ps", "ps_4_0", &psb) != 0 ||
        FAILED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb),
                                               ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb),
                                              ID3D10Blob_GetBufferSize(psb), NULL, &g_ps)))
        goto fail;

    RELEASE(vsb);
    RELEASE(psb);
    RELEASE(factory);
    RELEASE(adapter);
    RELEASE(dxgi);
    g_warp = warp;
    return 0;

fail:
    RELEASE(vsb);
    RELEASE(psb);
    RELEASE(factory);
    RELEASE(adapter);
    RELEASE(dxgi);
    d3d_release();
    return -1;
}

/* Follows the client size: the swap chain's buffers are the window's
 * pixels, so the picture is drawn at the glass's resolution. */
static int d3d_resize(void)
{
    int w = g_width, h = g_height;
    if (w <= 0 || h <= 0 || (w == g_bb_w && h == g_bb_h))
        return 0;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    RELEASE(g_rtv);
    if (FAILED(IDXGISwapChain1_ResizeBuffers(g_swap, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, g_swap_flags)))
        return -1;
    g_bb_w = w;
    g_bb_h = h;
    return make_target();
}

static void d3d_upload(const uint32_t *fb, int fbw, int lines)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    for (int y = 0; y < lines; y++)
        memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, fb + (size_t)y * fbw, (size_t)fbw * 4);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_tex, 0);
}

static HRESULT d3d_draw(int fbw, int lines)
{
    static const float black[4] = { 0, 0, 0, 1 };
    const RECT pic = picture_rect(g_bb_w, g_bb_h, fbw, lines);
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE m;

    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, black);
    if (pic.right > pic.left && pic.bottom > pic.top) {
        vp.TopLeftX = (float)pic.left;
        vp.TopLeftY = (float)pic.top;
        vp.Width = (float)(pic.right - pic.left);
        vp.Height = (float)(pic.bottom - pic.top);
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float *c = m.pData;
            c[0] = (float)fbw;
            c[1] = (float)lines;
            c[2] = vp.Width;
            c[3] = vp.Height;
            c[4] = FB_W;
            c[5] = FB_H;
            c[6] = g_smooth ? 1.0f : 0.0f;
            c[7] = 0;
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
        }
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srv);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_smp);
        ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    }
    return IDXGISwapChain1_Present(g_swap, 1, 0);
}

/* ---- the render thread ---- */

static void report(unsigned *presents, unsigned *frames, DWORD *since)
{
    const DWORD now = GetTickCount();
    if (now - *since < 1000)
        return;
    fprintf(stderr, "display: %s, %u presents/s, %u new frames/s\n", apple2_display_renderer(),
            (unsigned)(*presents * 1000u / (now - *since)), (unsigned)(*frames * 1000u / (now - *since)));
    *presents = *frames = 0;
    *since = now;
}

static void gdi_loop(void);

static DWORD WINAPI render_thread(LPVOID arg)
{
    static uint32_t fb[FB_W * FB_H];
    uint64_t serial = 0;
    int lines = 0, fbw = 0;
    unsigned presents = 0, frames = 0;
    DWORD since = GetTickCount();
    const int fps = getenv("APPLE2_FPS") != NULL;
    (void)arg;

    join_mmcss();
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        HRESULT hr;
        int w, h;
        if (g_waitable)
            WaitForSingleObjectEx(g_waitable, 100, TRUE);
        if (!IsWindowVisible(g_hwnd) || IsIconic(GetAncestor(g_hwnd, GA_ROOT))) {
            Sleep(30);          /* minimized */
            continue;
        }
        tick();
        if (d3d_resize() != 0)
            goto lost;
        if (apple2session_copy_frame(g_session, fb, &serial, &w, &h)) {
            fbw = w > FB_W ? FB_W : w;
            lines = h > FB_H ? FB_H : h;
            d3d_upload(fb, fbw, lines);
            frames++;
        }
        if (lines > 0) {
            hr = d3d_draw(fbw, lines);
            presents++;
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
                goto lost;
            if (hr == DXGI_STATUS_OCCLUDED)
                Sleep(30);
            if (!g_waitable && FAILED(hr))
                Sleep(8);
        } else {
            Sleep(8);           /* no frame yet */
        }
        if (fps)
            report(&presents, &frames, &since);
        continue;
lost:
        /* a driver update or a TDR: build it all again, or fall back */
        d3d_release();
        if (d3d_init(g_warp) != 0 && d3d_init(1) != 0) {
            g_mode = MODE_GDI;
            InvalidateRect(g_hwnd, NULL, FALSE);
            gdi_loop();
            return 0;
        }
        serial = 0;
    }
    d3d_release();
    return 0;
}

/* ---- GDI ---- */

/* Blocks until the compositor's next vblank; falls through at once if the
 * DWM is off, and a short sleep paces it instead. A DwmFlush that returns at
 * once (Wine's) is paced the same way, so the loop never spins. */
static void gdi_loop(void)
{
    LARGE_INTEGER freq, last, now;
    uint64_t serial = 0;
    unsigned presents = 0, frames = 0;
    DWORD since = GetTickCount();
    const int fps = getenv("APPLE2_FPS") != NULL;
    static uint32_t fb[FB_W * FB_H];

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        int w = 0, h = 0;
        if (FAILED(DwmFlush())) Sleep(8);
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - last.QuadPart) * 1000 < freq.QuadPart * 4) {
            Sleep(8);
            QueryPerformanceCounter(&now);
        }
        last = now;
        tick();
        if (apple2session_copy_frame(g_session, fb, &serial, &w, &h)) {
            if (w > FB_W) w = FB_W;
            if (h > FB_H) h = FB_H;
            EnterCriticalSection(&g_gdi_lock);
            memcpy(g_gdi_fb, fb, (size_t)h * w * sizeof *fb);
            g_gdi_w = w;
            g_gdi_h = h;
            LeaveCriticalSection(&g_gdi_lock);
            if (IsWindowVisible(g_hwnd)) {
                RECT c, pic;
                GetClientRect(g_hwnd, &c);
                pic = picture_rect(c.right, c.bottom, w, h);
                InvalidateRect(g_hwnd, &pic, FALSE);
            }
            frames++;
            presents++;
        }
        if (fps)
            report(&presents, &frames, &since);
    }
}

static DWORD WINAPI gdi_thread(LPVOID arg)
{
    (void)arg;
    join_mmcss();
    gdi_loop();
    return 0;
}

/* Only the bars around the picture are filled: painting black under the
 * picture first is what made it flash. */
static void gdi_paint(HDC dc)
{
    RECT c, pic, r;
    BITMAPINFO bmi;
    int w, h;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);

    GetClientRect(g_hwnd, &c);
    EnterCriticalSection(&g_gdi_lock);
    w = g_gdi_w;
    h = g_gdi_h;
    pic = picture_rect(c.right, c.bottom, w, h);
    if (h <= 0) {
        LeaveCriticalSection(&g_gdi_lock);
        FillRect(dc, &c, black);
        return;
    }
    SetRect(&r, 0, 0, c.right, pic.top);           FillRect(dc, &r, black);
    SetRect(&r, 0, pic.bottom, c.right, c.bottom); FillRect(dc, &r, black);
    SetRect(&r, 0, pic.top, pic.left, pic.bottom); FillRect(dc, &r, black);
    SetRect(&r, pic.right, pic.top, c.right, pic.bottom); FillRect(dc, &r, black);

    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;        /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc, pic.left, pic.top, pic.right - pic.left, pic.bottom - pic.top,
                  0, 0, w, h, g_gdi_fb, &bmi, DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_gdi_lock);
}

/* ---- the window ---- */

static LRESULT CALLBACK display_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        InterlockedExchange(&g_width, LOWORD(lp));
        InterlockedExchange(&g_height, HIWORD(lp));
        if (g_mode != MODE_D3D)
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_mode != MODE_D3D)
            gdi_paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;   /* clicks and focus stay with the main window */
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

const char *apple2_display_renderer(void)
{
    if (g_mode == MODE_D3D)
        return g_warp ? "Direct3D 11 (WARP)" : "Direct3D 11";
    return "GDI";
}

void apple2_display_set_options(int aspect, int smooth)
{
    InterlockedExchange(&g_aspect, aspect);
    InterlockedExchange(&g_smooth, smooth != 0);
    if (g_hwnd && g_mode != MODE_D3D)
        InvalidateRect(g_hwnd, NULL, FALSE);
}

HWND apple2_display_create(HWND parent, apple2session *session)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    const char *force = getenv("APPLE2_RENDERER");
    RECT c;

    g_session = session;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = display_proc;
        wc.hInstance = inst;
        wc.lpszClassName = DISPLAY_CLASS;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        registered = RegisterClassA(&wc) != 0;
        InitializeCriticalSection(&g_gdi_lock);
    }
    GetClientRect(parent, &c);
    g_hwnd = CreateWindowExA(0, DISPLAY_CLASS, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                             0, 0, c.right, c.bottom, parent, NULL, inst, NULL);
    if (!g_hwnd)
        return NULL;
    GetClientRect(g_hwnd, &c);
    g_width = c.right;
    g_height = c.bottom;

    /* the device and swap chain are made here, used on the render thread
     * (D3D11 devices are free-threaded; the immediate context is the
     * render thread's alone from now on) */
    g_mode = MODE_GDI;
    if (!(force && _stricmp(force, "gdi") == 0) && (d3d_init(0) == 0 || d3d_init(1) == 0))
        g_mode = MODE_D3D;
    g_running = 1;
    g_thread = CreateThread(NULL, 0, g_mode == MODE_D3D ? render_thread : gdi_thread, NULL, 0, NULL);
    return g_hwnd;
}

void apple2_display_destroy(void)
{
    if (!g_thread)
        return;
    InterlockedExchange(&g_running, 0);
    /* The render thread may be inside Present, which can send this window's
     * thread a message: keep pumping sent messages while waiting for it. */
    for (DWORD start = GetTickCount(); GetTickCount() - start < 3000;) {
        DWORD r = MsgWaitForMultipleObjects(1, &g_thread, FALSE, 100, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0)
            break;
        if (r == WAIT_OBJECT_0 + 1) {
            MSG m;
            PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
    }
    CloseHandle(g_thread);
    g_thread = NULL;
}

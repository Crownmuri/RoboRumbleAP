/* Windowed mode for the software renderer (rr_soft.exe).

   The software renderer draws every frame into its own 640x480 16-bit buffer, copies it into
   a DirectDraw back buffer with Lock/Unlock and calls Flip on a fullscreen exclusive primary
   surface. Here DirectDrawCreate hands out a small fake DirectDraw whose surfaces are plain
   memory; Flip (and Unlock of the primary) stretch the frame into the game's window with GDI.

   The window is turned into a normal (or borderless) window, and the mouse is translated
   between window pixels and the 640x480 coordinates the game expects. */
#include "common.h"
#include <ddraw.h>
#include <stdlib.h>
#include <stdio.h>

#define GAME_W 640
#define GAME_H 480

static HWND  g_hwnd;
static RECT  g_dst = { 0, 0, GAME_W, GAME_H };   /* where the frame goes in the client area */
static int   g_active;

static MMRESULT (WINAPI *p_timeBeginPeriod)(UINT);

/* ------------------------------------------------------------------ surfaces */
typedef struct FakeSurf FakeSurf;
struct FakeSurf {
    void   **vtbl;
    LONG     ref;
    int      w, h, pitch;
    BYTE    *pixels;
    DWORD    caps;
    FakeSurf *back;       /* attached back buffer of the primary */
    HDC      dc;          /* GetDC/ReleaseDC */
    HBITMAP  dib, old_bmp;
    void    *dib_bits;
};

static FakeSurf *g_primary;

static const DDPIXELFORMAT k_pf565 = {
    sizeof(DDPIXELFORMAT), DDPF_RGB, 0, { 16 }, { 0xF800 }, { 0x07E0 }, { 0x001F }, { 0 }
};

static FakeSurf *surf_new(int w, int h, DWORD caps);

static void fill_desc(FakeSurf *s, DDSURFACEDESC *d, int with_pointer)
{
    DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
    if (size > sizeof(DDSURFACEDESC)) size = sizeof(DDSURFACEDESC);
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = s->w;
    d->dwHeight = s->h;
    d->lPitch = s->pitch;
    d->ddpfPixelFormat = k_pf565;
    d->ddsCaps.dwCaps = s->caps;
    if (s->back) { d->dwFlags |= DDSD_BACKBUFFERCOUNT; d->dwBackBufferCount = 1; }
    if (with_pointer) { d->dwFlags |= DDSD_LPSURFACE; d->lpSurface = s->pixels; }
}

/* ---- presenting ---- */
static void update_dst(void)
{
    RECT c;
    if (!g_hwnd || !GetClientRect(g_hwnd, &c)) return;
    int cw = c.right, ch = c.bottom;
    if (cw <= 0 || ch <= 0) return;
    int w = cw, h = ch;
    if (cw * GAME_H > ch * GAME_W) w = ch * GAME_W / GAME_H;   /* pillarbox */
    else h = cw * GAME_H / GAME_W;                              /* letterbox */
    SetRect(&g_dst, (cw - w) / 2, (ch - h) / 2, (cw - w) / 2 + w, (ch - h) / 2 + h);
}

static void present(FakeSurf *s)
{
    if (!g_hwnd || !s || !s->pixels) return;
    /* The video player never looks at the message queue; in a window that gets it flagged
       "Not Responding" after 5 s. Peeking (without removing anything) is enough for Windows. */
    MSG m;
    PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE);
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bmi = { 0 };
    bmi.h.biSize = sizeof bmi.h;
    bmi.h.biWidth = s->pitch / 2;
    bmi.h.biHeight = -s->h;
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = 16;
    bmi.h.biCompression = BI_BITFIELDS;
    bmi.masks[0] = 0xF800; bmi.masks[1] = 0x07E0; bmi.masks[2] = 0x001F;
    HDC dc = GetDC(g_hwnd);
    if (!dc) return;
    RECT c;
    GetClientRect(g_hwnd, &c);
    /* black bars around the 4:3 picture */
    if (g_dst.left > 0) { PatBlt(dc, 0, 0, g_dst.left, c.bottom, BLACKNESS); PatBlt(dc, g_dst.right, 0, c.right - g_dst.right, c.bottom, BLACKNESS); }
    if (g_dst.top > 0) { PatBlt(dc, 0, 0, c.right, g_dst.top, BLACKNESS); PatBlt(dc, 0, g_dst.bottom, c.right, c.bottom - g_dst.bottom, BLACKNESS); }
    if (g_cfg.smooth) { SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL); }
    else SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, g_dst.left, g_dst.top, g_dst.right - g_dst.left, g_dst.bottom - g_dst.top,
                  0, 0, s->w, s->h, s->pixels, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_hwnd, dc);
}

static void frame_limit(void)
{
    static LARGE_INTEGER freq, last;
    if (g_cfg.fps_limit <= 0) return;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG frame = freq.QuadPart / g_cfg.fps_limit;
    if (last.QuadPart) {
        LONGLONG next = last.QuadPart + frame;
        while (now.QuadPart < next) {
            LONGLONG ms = (next - now.QuadPart) * 1000 / freq.QuadPart;
            Sleep(ms > 1 ? (DWORD)(ms - 1) : 0);
            QueryPerformanceCounter(&now);
        }
        /* fell far behind (loading screen, debugger): don't try to catch up */
        last.QuadPart = (now.QuadPart - next > frame) ? now.QuadPart : next;
    } else last = now;
}

/* ---- IDirectDrawSurface ---- */
#define SURF(p) ((FakeSurf *)(p))

static HRESULT WINAPI s_QueryInterface(void *t, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectDrawSurface)) {
        *out = t; SURF(t)->ref++; return S_OK;
    }
    rr_log("ddraw: surface QI for unsupported interface %08lx", riid->Data1);
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI s_AddRef(void *t) { return ++SURF(t)->ref; }
static ULONG WINAPI s_Release(void *t)
{
    FakeSurf *s = SURF(t);
    if (--s->ref > 0) return s->ref;
    if (s->back) { s_Release(s->back); s->back = NULL; }
    if (s == g_primary) g_primary = NULL;
    if (s->dib) { SelectObject(s->dc, s->old_bmp); DeleteObject(s->dib); DeleteDC(s->dc); }
    _aligned_free(s->pixels);
    free(s);
    return 0;
}
static HRESULT WINAPI s_ok(void *t) { (void)t; return DD_OK; }
static HRESULT WINAPI s_ok1(void *t, void *a) { (void)t; (void)a; return DD_OK; }
static HRESULT WINAPI s_ok2(void *t, void *a, void *b) { (void)t; (void)a; (void)b; return DD_OK; }
static HRESULT WINAPI s_unsup1(void *t, void *a) { (void)t; (void)a; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_unsup2(void *t, void *a, void *b) { (void)t; (void)a; (void)b; return DDERR_UNSUPPORTED; }

static void clip_rect(RECT *r, const RECT *in, int w, int h)
{
    if (in) *r = *in; else SetRect(r, 0, 0, w, h);
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > w) r->right = w;
    if (r->bottom > h) r->bottom = h;
}

static HRESULT WINAPI s_Blt(void *t, RECT *dst_rect, void *src_surf, RECT *src_rect, DWORD flags, DDBLTFX *fx)
{
    FakeSurf *d = SURF(t), *s = SURF(src_surf);
    RECT dr;
    clip_rect(&dr, dst_rect, d->w, d->h);
    if (dr.right <= dr.left || dr.bottom <= dr.top) return DD_OK;
    if (flags & DDBLT_COLORFILL) {
        WORD c = fx ? (WORD)fx->dwFillColor : 0;
        for (int y = dr.top; y < dr.bottom; y++) {
            WORD *row = (WORD *)(d->pixels + y * d->pitch);
            for (int x = dr.left; x < dr.right; x++) row[x] = c;
        }
    } else if (s) {
        RECT sr;
        clip_rect(&sr, src_rect, s->w, s->h);
        int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
        int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
        if (sw <= 0 || sh <= 0) return DD_OK;
        for (int y = 0; y < dh; y++) {
            WORD *drow = (WORD *)(d->pixels + (dr.top + y) * d->pitch) + dr.left;
            const WORD *srow = (const WORD *)(s->pixels + (sr.top + y * sh / dh) * s->pitch) + sr.left;
            if (sw == dw) memmove(drow, srow, dw * 2);
            else for (int x = 0; x < dw; x++) drow[x] = srow[x * sw / dw];
        }
    }
    if (d == g_primary) present(d);
    return DD_OK;
}

static HRESULT WINAPI s_BltFast(void *t, DWORD x, DWORD y, void *src, RECT *src_rect, DWORD trans)
{
    (void)trans;
    FakeSurf *s = SURF(src);
    if (!s) return DDERR_INVALIDPARAMS;
    RECT sr, dr;
    clip_rect(&sr, src_rect, s->w, s->h);
    SetRect(&dr, x, y, x + (sr.right - sr.left), y + (sr.bottom - sr.top));
    return s_Blt(t, &dr, src, &sr, 0, NULL);
}

static HRESULT WINAPI s_EnumAttachedSurfaces(void *t, void *ctx, LPDDENUMSURFACESCALLBACK cb)
{
    FakeSurf *s = SURF(t);
    if (s->back && cb) {
        DDSURFACEDESC d = { sizeof d };
        fill_desc(s->back, &d, 0);
        s->back->ref++;
        cb((LPDIRECTDRAWSURFACE)s->back, &d, ctx);
    }
    return DD_OK;
}

static HRESULT WINAPI s_Flip(void *t, void *target, DWORD flags)
{
    (void)target; (void)flags;
    FakeSurf *s = SURF(t);
    if (s->back) {
        BYTE *p = s->pixels;
        s->pixels = s->back->pixels;
        s->back->pixels = p;
    }
    present(s);
    frame_limit();
    return DD_OK;
}

static HRESULT WINAPI s_GetAttachedSurface(void *t, DDSCAPS *caps, void **out)
{
    FakeSurf *s = SURF(t);
    if (s->back && (!caps || (caps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP)))) {
        s->back->ref++;
        *out = s->back;
        return DD_OK;
    }
    *out = NULL;
    return DDERR_NOTFOUND;
}

static HRESULT WINAPI s_GetCaps(void *t, DDSCAPS *caps) { if (caps) caps->dwCaps = SURF(t)->caps; return DD_OK; }

static HRESULT WINAPI s_GetDC(void *t, HDC *out)
{
    FakeSurf *s = SURF(t);
    if (!s->dib) {
        struct { BITMAPINFOHEADER h; DWORD masks[3]; } bmi = { 0 };
        bmi.h.biSize = sizeof bmi.h;
        bmi.h.biWidth = s->pitch / 2;
        bmi.h.biHeight = -s->h;
        bmi.h.biPlanes = 1;
        bmi.h.biBitCount = 16;
        bmi.h.biCompression = BI_BITFIELDS;
        bmi.masks[0] = 0xF800; bmi.masks[1] = 0x07E0; bmi.masks[2] = 0x001F;
        s->dc = CreateCompatibleDC(NULL);
        s->dib = CreateDIBSection(s->dc, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, &s->dib_bits, NULL, 0);
        if (!s->dib) { DeleteDC(s->dc); s->dc = NULL; return DDERR_GENERIC; }
        s->old_bmp = SelectObject(s->dc, s->dib);
    }
    GdiFlush();
    memcpy(s->dib_bits, s->pixels, s->pitch * s->h);
    *out = s->dc;
    return DD_OK;
}

static HRESULT WINAPI s_ReleaseDC(void *t, HDC dc)
{
    (void)dc;
    FakeSurf *s = SURF(t);
    if (!s->dib) return DDERR_GENERIC;
    GdiFlush();
    memcpy(s->pixels, s->dib_bits, s->pitch * s->h);
    if (s == g_primary) present(s);
    return DD_OK;
}

static HRESULT WINAPI s_GetPixelFormat(void *t, DDPIXELFORMAT *pf) { (void)t; if (pf) *pf = k_pf565; return DD_OK; }
static HRESULT WINAPI s_GetSurfaceDesc(void *t, DDSURFACEDESC *d) { if (!d) return DDERR_INVALIDPARAMS; fill_desc(SURF(t), d, 1); return DD_OK; }

static HRESULT WINAPI s_Lock(void *t, RECT *r, DDSURFACEDESC *d, DWORD flags, HANDLE h)
{
    (void)flags; (void)h;
    FakeSurf *s = SURF(t);
    if (!d) return DDERR_INVALIDPARAMS;
    fill_desc(s, d, 1);
    if (r) d->lpSurface = s->pixels + r->top * s->pitch + r->left * 2;
    return DD_OK;
}

static HRESULT WINAPI s_Unlock(void *t, void *r)
{
    (void)r;
    if (SURF(t) == g_primary) present(SURF(t));
    return DD_OK;
}

static HRESULT WINAPI s_GetClipper(void *t, void *a) { (void)t; (void)a; return DDERR_NOCLIPPERATTACHED; }
static HRESULT WINAPI s_GetColorKey(void *t, DWORD f, void *a) { (void)t; (void)f; (void)a; return DDERR_NOCOLORKEY; }
static HRESULT WINAPI s_GetPalette(void *t, void *a) { (void)t; (void)a; return DDERR_NOPALETTEATTACHED; }
static HRESULT WINAPI s_SetColorKey(void *t, DWORD f, void *a) { (void)t; (void)f; (void)a; return DD_OK; }
static HRESULT WINAPI s_GetBltStatus(void *t, DWORD f) { (void)t; (void)f; return DD_OK; }
static HRESULT WINAPI s_GetOverlayPosition(void *t, LONG *x, LONG *y) { (void)t; (void)x; (void)y; return DDERR_NOTAOVERLAYSURFACE; }
static HRESULT WINAPI s_SetOverlayPosition(void *t, LONG x, LONG y) { (void)t; (void)x; (void)y; return DDERR_NOTAOVERLAYSURFACE; }
static HRESULT WINAPI s_BltBatch(void *t, void *a, DWORD n, DWORD f) { (void)t; (void)a; (void)n; (void)f; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_UpdateOverlay(void *t, void *a, void *b, void *c, DWORD f, void *fx) { (void)t; (void)a; (void)b; (void)c; (void)f; (void)fx; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_UpdateOverlayZOrder(void *t, DWORD f, void *a) { (void)t; (void)f; (void)a; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_EnumOverlayZOrders(void *t, DWORD f, void *c, void *cb) { (void)t; (void)f; (void)c; (void)cb; return DD_OK; }
static HRESULT WINAPI s_UpdateOverlayDisplay(void *t, DWORD f) { (void)t; (void)f; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_GetFlipStatus(void *t, DWORD f) { (void)t; (void)f; return DD_OK; }

/* IDirectDrawSurface v1 vtable order */
static void *g_surf_vtbl[] = {
    s_QueryInterface, s_AddRef, s_Release,
    s_unsup1,              /* AddAttachedSurface */
    s_ok1,                 /* AddOverlayDirtyRect */
    s_Blt, s_BltBatch, s_BltFast,
    s_ok2,                 /* DeleteAttachedSurface(flags, surf) */
    s_EnumAttachedSurfaces, s_EnumOverlayZOrders, s_Flip, s_GetAttachedSurface,
    s_GetBltStatus, s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC, s_GetFlipStatus,
    s_GetOverlayPosition, s_GetPalette, s_GetPixelFormat, s_GetSurfaceDesc,
    s_unsup2,              /* Initialize(dd, desc) */
    s_ok,                  /* IsLost */
    s_Lock, s_ReleaseDC,
    s_ok,                  /* Restore */
    s_ok1,                 /* SetClipper */
    s_SetColorKey, s_SetOverlayPosition,
    s_ok1,                 /* SetPalette */
    s_Unlock,
    s_UpdateOverlay, s_UpdateOverlayDisplay, s_UpdateOverlayZOrder,
};

static FakeSurf *surf_new(int w, int h, DWORD caps)
{
    FakeSurf *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->vtbl = g_surf_vtbl;
    s->ref = 1;
    s->w = w;
    s->h = h;
    s->pitch = ((w * 2) + 3) & ~3;
    s->caps = caps | DDSCAPS_SYSTEMMEMORY;
    s->pixels = _aligned_malloc((size_t)s->pitch * h, 32);
    if (!s->pixels) { free(s); return NULL; }
    memset(s->pixels, 0, (size_t)s->pitch * h);
    return s;
}

/* ------------------------------------------------------------------ DirectDraw */
typedef struct { void **vtbl; } FakeDD;
static FakeDD g_dd1, g_dd2;
static LONG g_dd_ref;

static HRESULT WINAPI d_QueryInterface(void *t, REFIID riid, void **out)
{
    (void)t;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectDraw)) { *out = &g_dd1; g_dd_ref++; return S_OK; }
    if (IsEqualGUID(riid, &IID_IDirectDraw2)) { *out = &g_dd2; g_dd_ref++; return S_OK; }
    rr_log("ddraw: QI for unsupported interface %08lx", riid->Data1);
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI d_AddRef(void *t) { (void)t; return ++g_dd_ref; }
static ULONG WINAPI d_Release(void *t) { (void)t; return g_dd_ref > 0 ? --g_dd_ref : 0; }
static HRESULT WINAPI d_ok(void *t) { (void)t; return DD_OK; }

static HRESULT WINAPI d_CreateSurface(void *t, DDSURFACEDESC *d, void **out, IUnknown *outer)
{
    (void)t; (void)outer;
    if (!d || !out) return DDERR_INVALIDPARAMS;
    DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
    FakeSurf *s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s = surf_new(GAME_W, GAME_H, caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE);
        if (!s) return DDERR_OUTOFMEMORY;
        if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount > 0)
            s->back = surf_new(GAME_W, GAME_H, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
        g_primary = s;
        rr_log("ddraw: primary surface%s", s->back ? " + back buffer" : "");
    } else {
        int w = (d->dwFlags & DDSD_WIDTH) ? (int)d->dwWidth : GAME_W;
        int h = (d->dwFlags & DDSD_HEIGHT) ? (int)d->dwHeight : GAME_H;
        s = surf_new(w, h, caps ? caps : DDSCAPS_OFFSCREENPLAIN);
        if (!s) return DDERR_OUTOFMEMORY;
        rr_log("ddraw: surface %dx%d caps %08lx", w, h, caps);
    }
    *out = s;
    return DD_OK;
}

static HRESULT WINAPI d_EnumDisplayModes(void *t, DWORD flags, DDSURFACEDESC *d, void *ctx, LPDDENUMMODESCALLBACK cb)
{
    (void)t; (void)flags; (void)d;
    if (!cb) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC m = { sizeof m };
    m.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    m.dwWidth = GAME_W; m.dwHeight = GAME_H; m.lPitch = GAME_W * 2;
    m.ddpfPixelFormat = k_pf565;
    cb(&m, ctx);
    return DD_OK;
}

static HRESULT WINAPI d_GetCaps(void *t, DDCAPS *drv, DDCAPS *hel)
{
    (void)t;
    /* No hardware features at all: the game then renders at full size without stretching. */
    if (drv) { DWORD n = drv->dwSize; if (n > sizeof(DDCAPS)) n = sizeof(DDCAPS); memset(drv, 0, n); drv->dwSize = n; }
    if (hel) { DWORD n = hel->dwSize; if (n > sizeof(DDCAPS)) n = sizeof(DDCAPS); memset(hel, 0, n); hel->dwSize = n; }
    return DD_OK;
}

static HRESULT WINAPI d_GetDisplayMode(void *t, DDSURFACEDESC *d)
{
    (void)t;
    if (!d) return DDERR_INVALIDPARAMS;
    DWORD size = d->dwSize ? d->dwSize : sizeof *d;
    if (size > sizeof *d) size = sizeof *d;
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d->dwWidth = GAME_W; d->dwHeight = GAME_H; d->lPitch = GAME_W * 2;
    d->dwRefreshRate = 60;
    d->ddpfPixelFormat = k_pf565;
    return DD_OK;
}

static HRESULT WINAPI d_GetFourCCCodes(void *t, DWORD *n, DWORD *codes) { (void)t; (void)codes; if (n) *n = 0; return DD_OK; }
static HRESULT WINAPI d_GetGDISurface(void *t, void **out) { (void)t; if (!g_primary) { *out = NULL; return DDERR_NOTFOUND; } g_primary->ref++; *out = g_primary; return DD_OK; }
static HRESULT WINAPI d_GetMonitorFrequency(void *t, DWORD *f) { (void)t; if (f) *f = 60; return DD_OK; }
static HRESULT WINAPI d_GetScanLine(void *t, DWORD *l) { (void)t; if (l) *l = 0; return DD_OK; }
static HRESULT WINAPI d_GetVerticalBlankStatus(void *t, BOOL *b) { (void)t; if (b) *b = TRUE; return DD_OK; }
static HRESULT WINAPI d_Initialize(void *t, GUID *g) { (void)t; (void)g; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI d_unsup2(void *t, void *a, void *b) { (void)t; (void)a; (void)b; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI d_unsup3(void *t, void *a, void *b, void *c) { (void)t; (void)a; (void)b; (void)c; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI d_unsup4(void *t, void *a, void *b, void *c, void *e) { (void)t; (void)a; (void)b; (void)c; (void)e; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI d_EnumSurfaces(void *t, DWORD f, void *d, void *c, void *cb) { (void)t; (void)f; (void)d; (void)c; (void)cb; return DD_OK; }
static HRESULT WINAPI d_WaitForVerticalBlank(void *t, DWORD f, HANDLE h) { (void)t; (void)f; (void)h; return DD_OK; }

static HRESULT WINAPI d_SetCooperativeLevel(void *t, HWND hwnd, DWORD flags)
{
    (void)t;
    rr_log("ddraw: SetCooperativeLevel %p %08lx (ignored)", hwnd, flags);
    if (hwnd && !g_hwnd) g_hwnd = hwnd;
    update_dst();
    return DD_OK;
}
static HRESULT WINAPI d_SetDisplayMode1(void *t, DWORD w, DWORD h, DWORD bpp)
{
    (void)t; rr_log("ddraw: SetDisplayMode %lux%lux%lu (ignored)", w, h, bpp); return DD_OK;
}
static HRESULT WINAPI d_SetDisplayMode2(void *t, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD flags)
{
    (void)t; (void)rate; (void)flags; rr_log("ddraw: SetDisplayMode %lux%lux%lu (ignored)", w, h, bpp); return DD_OK;
}
static HRESULT WINAPI d_GetAvailableVidMem(void *t, DDSCAPS *caps, DWORD *total, DWORD *free_)
{
    (void)t; (void)caps;
    if (total) *total = 64u << 20;
    if (free_) *free_ = 64u << 20;
    return DD_OK;
}

#define DD_COMMON \
    d_QueryInterface, d_AddRef, d_Release, \
    d_ok,              /* Compact */ \
    d_unsup3,          /* CreateClipper */ \
    d_unsup4,          /* CreatePalette */ \
    d_CreateSurface, \
    d_unsup2,          /* DuplicateSurface */ \
    d_EnumDisplayModes, d_EnumSurfaces, \
    d_ok,              /* FlipToGDISurface */ \
    d_GetCaps, d_GetDisplayMode, d_GetFourCCCodes, d_GetGDISurface, d_GetMonitorFrequency, \
    d_GetScanLine, d_GetVerticalBlankStatus, d_Initialize, \
    d_ok,              /* RestoreDisplayMode */ \
    d_SetCooperativeLevel

static void *g_dd1_vtbl[] = { DD_COMMON, d_SetDisplayMode1, d_WaitForVerticalBlank };
static void *g_dd2_vtbl[] = { DD_COMMON, d_SetDisplayMode2, d_WaitForVerticalBlank, d_GetAvailableVidMem };

static HRESULT WINAPI fake_DirectDrawCreate(GUID *guid, void **out, IUnknown *outer)
{
    (void)guid; (void)outer;
    if (!out) return DDERR_INVALIDPARAMS;
    g_dd1.vtbl = g_dd1_vtbl;
    g_dd2.vtbl = g_dd2_vtbl;
    g_dd_ref++;
    *out = &g_dd1;
    if (p_timeBeginPeriod) p_timeBeginPeriod(1);
    rr_log("ddraw: DirectDrawCreate -> windowed fake");
    return DD_OK;
}

/* ------------------------------------------------------------------ window + mouse */
static HWND (WINAPI *real_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static BOOL (WINAPI *real_ShowWindow)(HWND, int);
static BOOL (WINAPI *real_GetCursorPos)(LPPOINT);
static BOOL (WINAPI *real_SetCursorPos)(int, int);
static SHORT (WINAPI *real_GetAsyncKeyState)(int);
static WNDPROC g_game_wndproc;
static int g_owner_draws;   /* 1 = our fake DirectDraw paints the window (software renderer) */
static int g_translate_only;/* 1 = another wrapper owns the window (nGlide): only map the mouse */

static void to_game(int cx, int cy, int *gx, int *gy)
{
    int w = g_dst.right - g_dst.left, h = g_dst.bottom - g_dst.top;
    if (w <= 0 || h <= 0) { *gx = cx; *gy = cy; return; }
    int x = (cx - g_dst.left) * GAME_W / w;
    int y = (cy - g_dst.top) * GAME_H / h;
    *gx = x < 0 ? 0 : x >= GAME_W ? GAME_W - 1 : x;
    *gy = y < 0 ? 0 : y >= GAME_H ? GAME_H - 1 : y;
}

static int g_sizing;   /* the window is being moved or resized by the user */

/* Edge scrolling needs the cursor kept in the game during missions. In menus and the briefing it stays free, so the
   title bar can be reached to move the window ([window] clip_cursor=2 confines it everywhere, like before). */
static int clip_wanted(void)
{
    return g_cfg.clip_cursor >= 2 || g_gui_mode == RR_MODE_GAMEPLAY;
}

/* Confines the cursor to the game while it is in front, but only once the cursor is over the game: dragging the
   title bar or a frame edge must not pull it in. */
static void update_clip(void)
{
    if (!g_cfg.clip_cursor || !g_hwnd || g_translate_only) return;
    RECT r = g_dst;
    MapWindowPoints(g_hwnd, NULL, (POINT *)&r, 2);
    POINT cur;
    if (!g_sizing && clip_wanted() && g_active && GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd) &&
        GetCursorPos(&cur) && PtInRect(&r, cur))
        ClipCursor(&r);
    else ClipCursor(NULL);
}

/* Mouse wheel zoom. The game zooms while Insert (in) or Page Up (out) is held, read from its
   own key table, which its message loop fills from WM_KEYDOWN/WM_KEYUP as they come out of
   the queue (before dispatch), so the keys are posted. A notch holds the key for wheel_ms. */
#define WHEEL_TIMER 0x5257
static int g_wheel_vk;
static DWORD g_wheel_until;

static void wheel_release(HWND hwnd)
{
    if (!g_wheel_vk) return;
    PostMessageA(hwnd, WM_KEYUP, g_wheel_vk, 0xC0000001);
    g_wheel_vk = 0;
    KillTimer(hwnd, WHEEL_TIMER);
}

static void wheel(HWND hwnd, int delta)
{
    int ms = GetPrivateProfileIntA("window", "wheel_ms", 120, g_ini_path);
    int vk = delta > 0 ? VK_INSERT : VK_PRIOR;
    int notches = (delta < 0 ? -delta : delta) / WHEEL_DELTA;
    if (notches < 1) notches = 1;
    if (g_wheel_vk && g_wheel_vk != vk) wheel_release(hwnd);
    DWORD now = GetTickCount();
    if (!g_wheel_vk) {
        g_wheel_vk = vk;
        g_wheel_until = now;
        PostMessageA(hwnd, WM_KEYDOWN, vk, 1);
        SetTimer(hwnd, WHEEL_TIMER, 15, NULL);
    }
    g_wheel_until += notches * ms;
    if ((int)(g_wheel_until - now) > 1000) g_wheel_until = now + 1000;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEWHEEL:
        if (GetPrivateProfileIntA("window", "wheel_zoom", 1, g_ini_path)) {
            wheel(hwnd, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == WHEEL_TIMER) {
            if ((int)(GetTickCount() - g_wheel_until) >= 0) wheel_release(hwnd);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        /* With the mouse captured (the game/wrapper does that while focused) a click on the title bar or frame
           arrives as a client click outside the client area. It goes to Windows as the title-bar click it is. */
        POINT sp = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT cr;
        GetClientRect(hwnd, &cr);
        if (!PtInRect(&cr, sp)) {
            ClientToScreen(hwnd, &sp);
            LRESULT hit = DefWindowProcA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(sp.x, sp.y));
            if (hit != HTCLIENT && hit != HTNOWHERE && hit != HTERROR) {
                ReleaseCapture();
                ClipCursor(NULL);
                if (hit == HTCLOSE) { rr_log("window: close button, exiting"); ExitProcess(0); }
                if (hit == HTCAPTION) return DefWindowProcA(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, MAKELPARAM(sp.x, sp.y));
                if (hit >= HTLEFT && hit <= HTBOTTOMRIGHT)
                    return DefWindowProcA(hwnd, WM_SYSCOMMAND, SC_SIZE | (hit - HTLEFT + WMSZ_LEFT), MAKELPARAM(sp.x, sp.y));
                return DefWindowProcA(hwnd, msg == WM_LBUTTONDOWN ? WM_NCLBUTTONDOWN : WM_NCLBUTTONDBLCLK, hit,
                                      MAKELPARAM(sp.x, sp.y));
            }
        }
    }
    /* fall through */
    case WM_MOUSEMOVE: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: {
        int gx, gy;
        to_game((short)LOWORD(lp), (short)HIWORD(lp), &gx, &gy);
        lp = MAKELPARAM(gx, gy);
        break;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }  /* the game draws its own */
        return DefWindowProcA(hwnd, msg, wp, lp);
    case WM_ERASEBKGND:
        if (g_owner_draws) return 1;
        break;
    case WM_PAINT:
        if (g_owner_draws) {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (g_primary) present(g_primary);
            return 0;
        }
        break;
    case WM_SIZE:
        update_dst();
        update_clip();
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_ENTERSIZEMOVE:
        g_sizing = 1;
        ClipCursor(NULL);
        break;
    case WM_EXITSIZEMOVE:
        g_sizing = 0;
        break;
    case WM_MOVE:
        update_clip();
        break;
    case WM_ACTIVATE:
        g_active = LOWORD(wp) != WA_INACTIVE;
        update_clip();
        break;
    case WM_ACTIVATEAPP:
        /* The game pauses (and used to minimize) when it is deactivated. */
        if (!wp && !g_translate_only && GetPrivateProfileIntA("window", "keep_running_in_background", 1, g_ini_path)) {
            g_active = 0;
            ClipCursor(NULL);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        wheel_release(hwnd);
        g_active = 0;
        update_clip();
        break;
    case WM_SYSKEYDOWN:
        /* Alt+Enter: toggle borderless (nGlide uses Alt+Enter itself) */
        if (!g_translate_only && wp == VK_RETURN && (lp & (1 << 29))) {
            g_cfg.borderless = !g_cfg.borderless;
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof mi };
            GetMonitorInfoA(mon, &mi);
            if (g_cfg.borderless) {
                SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            } else {
                RECT r = { 0, 0, GAME_W * g_cfg.scale, GAME_H * g_cfg.scale };
                AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
                int w = r.right - r.left, h = r.bottom - r.top;
                SetWindowLongA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                SetWindowPos(hwnd, HWND_NOTOPMOST, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                             mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: case WM_NCLBUTTONDOWN: case WM_NCLBUTTONUP: case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN: case WM_NCRBUTTONUP:
        /* title bar and frame: the game's window procedure swallows these, so the close button, dragging and
           resizing never started */
        if (msg == WM_NCLBUTTONDOWN && wp == HTCLOSE) {
            /* Windows doesn't turn this into a close here (the game's loop is in the way), so close at once */
            rr_log("window: close button, exiting");
            ClipCursor(NULL);
            ExitProcess(0);
        }
        /* Dragging the title bar or a frame edge: Windows would send itself SC_MOVE / SC_SIZE through the whole
           window procedure chain, where a Glide wrapper's procedure can swallow it, so the loop starts here. */
        if (msg == WM_NCLBUTTONDOWN && wp == HTCAPTION) {
            ClipCursor(NULL);
            return DefWindowProcA(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, lp);
        }
        if (msg == WM_NCLBUTTONDOWN && wp >= HTLEFT && wp <= HTBOTTOMRIGHT) {
            ClipCursor(NULL);
            return DefWindowProcA(hwnd, WM_SYSCOMMAND, SC_SIZE | (wp - HTLEFT + WMSZ_LEFT), lp);
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    case WM_CLOSE:
        /* Alt+F4 / the close button: the game only quits from its own menu and would keep running without a
           window, so end the process here. */
        rr_log("window: closed by the user, exiting");
        ClipCursor(NULL);
        ExitProcess(0);
    case WM_SYSCOMMAND:
        /* the game swallows most system commands; let window management through */
        switch (wp & 0xFFF0) {
        case SC_MOVE: case SC_SIZE: case SC_MAXIMIZE: case SC_RESTORE: case SC_MINIMIZE: case SC_CLOSE:
            return DefWindowProcA(hwnd, msg, wp, lp);
        }
        break;
    }
    return CallWindowProcA(g_game_wndproc, hwnd, msg, wp, lp);
}

static HWND WINAPI hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    if (g_hwnd || !cls || !HIWORD((DWORD_PTR)cls) || lstrcmpiA(cls, "RoboRumble") != 0)
        return real_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (g_translate_only) {
        HWND hwnd = real_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
        if (hwnd) {
            g_hwnd = hwnd;
            g_game_wndproc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)window_proc);
            update_dst();
            rr_log("window: mouse translation for the game window (the Glide wrapper owns the window)");
        }
        return hwnd;
    }

    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoA(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    if (g_cfg.borderless) {
        style = WS_POPUP;
        x = mi.rcMonitor.left; y = mi.rcMonitor.top;
        w = mi.rcMonitor.right - mi.rcMonitor.left; h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    } else {
        style = WS_OVERLAPPEDWINDOW;
        int scale = g_cfg.scale;
        RECT r;
        for (;;) {
            SetRect(&r, 0, 0, GAME_W * scale, GAME_H * scale);
            AdjustWindowRectEx(&r, style, FALSE, WS_EX_APPWINDOW);
            if (scale <= 1 || (r.right - r.left <= mi.rcWork.right - mi.rcWork.left &&
                               r.bottom - r.top <= mi.rcWork.bottom - mi.rcWork.top)) break;
            scale--;
        }
        w = r.right - r.left; h = r.bottom - r.top;
        x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    }
    HWND hwnd = real_CreateWindowExA(WS_EX_APPWINDOW, cls, title, style, x, y, w, h, NULL, menu, inst, param);
    if (hwnd) {
        g_hwnd = hwnd;
        g_game_wndproc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)window_proc);
        update_dst();
        rr_log("window: %s %dx%d at %d,%d", g_cfg.borderless ? "borderless" : "windowed", w, h, x, y);
    }
    return hwnd;
}

static BOOL WINAPI hook_ShowWindow(HWND hwnd, int cmd)
{
    /* Fullscreen-era habit: the game minimizes itself when it loses focus. In a window that
       makes it vanish (and from a stream). The title bar's minimize button still works. */
    if (hwnd == g_hwnd && !g_translate_only &&
        (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE)) {
        rr_log("window: game tried to minimize itself, ignored");
        return TRUE;
    }
    if (hwnd == g_hwnd && !g_translate_only && (cmd == SW_MAXIMIZE || cmd == SW_SHOWMAXIMIZED)) cmd = SW_SHOW;
    return real_ShowWindow(hwnd, cmd);
}

static BOOL WINAPI hook_GetCursorPos(LPPOINT pt)
{
    /* Confine the cursor while the game is in front and free it the moment anything else is
       (Alt+Tab, the Windows key's Start menu, ...). Activation messages can't be relied on
       because a wrapper may swallow them, so this follows the foreground window. */
    /* The game stops asking while it is in the background and Windows drops the confinement
       when another window takes over, so on every call: in front but not confined -> confine. */
    if (g_hwnd && g_cfg.clip_cursor && !g_translate_only) {
        HWND fg = GetForegroundWindow();
        g_active = fg == g_hwnd;
        RECT want = g_dst, have;
        MapWindowPoints(g_hwnd, NULL, (POINT *)&want, 2);
        GetClipCursor(&have);
        if (g_active && !IsIconic(g_hwnd) && clip_wanted()) {
            /* only once the cursor is over the game: the title bar and its close button stay usable */
            POINT cur;
            if (!g_sizing && !EqualRect(&want, &have) && real_GetCursorPos(&cur) && PtInRect(&want, cur))
                ClipCursor(&want);
        } else if (EqualRect(&want, &have)) {
            ClipCursor(NULL);
        }
    }
    if (!real_GetCursorPos(pt)) return FALSE;
    if (g_hwnd) {
        ScreenToClient(g_hwnd, pt);
        int gx, gy;
        to_game(pt->x, pt->y, &gx, &gy);
        pt->x = gx;
        pt->y = gy;
    }
    return TRUE;
}

static BOOL WINAPI hook_SetCursorPos(int x, int y)
{
    if (!g_hwnd) return TRUE;                       /* before the window exists: nothing to aim at */
    if (GetForegroundWindow() != g_hwnd) return TRUE;  /* never grab the mouse from other windows */
    POINT p = { g_dst.left + x * (g_dst.right - g_dst.left) / GAME_W,
                g_dst.top + y * (g_dst.bottom - g_dst.top) / GAME_H };
    ClientToScreen(g_hwnd, &p);
    return real_SetCursorPos(p.x, p.y);
}

static SHORT WINAPI hook_GetAsyncKeyState(int vk)
{
    if (g_hwnd && GetForegroundWindow() != g_hwnd) return 0;   /* no input while in the background */
    return real_GetAsyncKeyState(vk);
}

/* dgVoodoo2 (Glide + DirectX wrapper) set to windowed mode draws into the game's window;
   the window and mouse handling below is then all that is missing. */
HWND rr_game_hwnd(void) { return g_hwnd; }

int rr_nglide_managed(void)
{
    return strcmp(g_exe_name, "rr_glide.exe") == 0 && !rr_have_dgvoodoo() &&
           GetPrivateProfileIntA("window", "glide_no_mode_switch", 1, g_ini_path);
}

int rr_have_dgvoodoo(void)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%sdgVoodoo.conf", g_game_dir);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

void ddwin_install(void)
{
    if (!g_cfg.window_enabled) { rr_log("window: disabled in rrpatch.ini"); return; }
    HMODULE exe = GetModuleHandleA(NULL);
    if (strcmp(g_exe_name, "rr_soft.exe") == 0) {
        p_timeBeginPeriod = (void *)rr_real("timeBeginPeriod");
        if (!rr_hook_iat(exe, "DDRAW.dll", "DirectDrawCreate", (void *)fake_DirectDrawCreate)) {
            rr_log("window: DirectDrawCreate import not found");
            return;
        }
        g_owner_draws = 1;
    } else if (rr_have_dgvoodoo()) {
        rr_log("window: dgVoodoo.conf found, window + mouse handling for %s", g_exe_name);
    } else if (rr_nglide_managed()) {
        rr_log("window: nGlide kept windowed, window + mouse handling for %s", g_exe_name);
    } else if (strcmp(g_exe_name, "rr_glide.exe") == 0) {
        /* nGlide running windowed after Alt+Enter: only the mouse needs translating */
        g_translate_only = 1;
    } else {
        rr_log("window: %s draws fullscreen itself (software renderer or dgVoodoo needed)", g_exe_name);
        return;
    }
    real_CreateWindowExA = rr_hook_iat(exe, "USER32.dll", "CreateWindowExA", (void *)hook_CreateWindowExA);
    real_ShowWindow = rr_hook_iat(exe, "USER32.dll", "ShowWindow", (void *)hook_ShowWindow);
    real_GetCursorPos = rr_hook_iat(exe, "USER32.dll", "GetCursorPos", (void *)hook_GetCursorPos);
    real_SetCursorPos = rr_hook_iat(exe, "USER32.dll", "SetCursorPos", (void *)hook_SetCursorPos);
    real_GetAsyncKeyState = rr_hook_iat(exe, "USER32.dll", "GetAsyncKeyState", (void *)hook_GetAsyncKeyState);
    if (!real_CreateWindowExA || !real_ShowWindow || !real_GetCursorPos || !real_SetCursorPos || !real_GetAsyncKeyState)
        rr_log("window: some user32 imports were not hooked");
    rr_log(g_owner_draws ? "window: fake DirectDraw installed" : "window: window hooks installed");
}

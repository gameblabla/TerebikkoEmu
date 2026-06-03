/* tb_win32_video.c - flicker-free GDI back-buffer compositor.
 *
 * The video frame, the Mania push transition and all overlays are drawn into a
 * single offscreen DIB and blitted to the window once per frame, so there is no
 * tearing or flashing between the picture, the overlays and the child controls.
 *
 * The optional "D3D11" / "GDI" menu choice only selects the scaling quality
 * (smooth HALFTONE vs. fast COLORONCOLOR); both go through this compositor.
 * TB_WIN32_GDI_ONLY builds remove that menu and force COLORONCOLOR/GDI.
 * Hardware video *decode* (NVDEC / D3D11VA) is independent and handled in the
 * player, and is disabled for GDI-only Win32 builds.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "tb_win32_video.h"

#define PUSH_MS 300.0

typedef struct tb_win32_video {
    HWND hwnd;
    int backend;
    char last_error[512];
    int client_w, client_h;

    /* offscreen back-buffer */
    HDC      bb_dc;
    HBITMAP  bb_bmp, bb_old;
    int      bb_w, bb_h;

    /* last shown frame (copied), used as the outgoing image for a push */
    uint8_t *last_rgba; int last_w, last_h, last_stride; RECT last_dst;

    /* active Mania push */
    bool     push_active;
    uint8_t *push_rgba; int push_w, push_h, push_stride; RECT push_dst;
    int      push_dir;
    LARGE_INTEGER push_start, qpc_freq;
} tb_win32_video;

const char *tb_win32_video_last_error(const tb_win32_video *v) { return v && v->last_error[0] ? v->last_error : "No video error has been recorded."; }
const char *tb_win32_video_backend_name(int b) {
#ifdef TB_WIN32_GDI_ONLY
    (void)b;
    return "GDI";
#else
    return b == TB_WIN32_VIDEO_D3D11 ? "D3D11" : "GDI";
#endif
}

static double now_sec(tb_win32_video *v) {
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - v->push_start.QuadPart) / (double)v->qpc_freq.QuadPart;
}

static void ensure_backbuffer(tb_win32_video *v) {
    RECT rc; GetClientRect(v->hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top; if (w < 1) w = 1; if (h < 1) h = 1;
    v->client_w = w; v->client_h = h;
    if (v->bb_dc && v->bb_w == w && v->bb_h == h) return;
    if (v->bb_dc) { SelectObject(v->bb_dc, v->bb_old); DeleteObject(v->bb_bmp); DeleteDC(v->bb_dc); v->bb_dc = NULL; }
    HDC wdc = GetDC(v->hwnd);
    v->bb_dc = CreateCompatibleDC(wdc);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    v->bb_bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    v->bb_old = (HBITMAP)SelectObject(v->bb_dc, v->bb_bmp);
    v->bb_w = w; v->bb_h = h;
    ReleaseDC(v->hwnd, wdc);
}

/* Blit an RGBA frame (red byte first) into the back-buffer at dst. A V4 header
 * with RGBA bit masks tells GDI the byte order, so red and blue are not swapped
 * (the classic "blue tint" from feeding RGBA to a default BI_RGB BGRA DIB). */
static void blit_frame(tb_win32_video *v, const RECT *dst, const uint8_t *rgba, int w, int h, int stride, bool smooth) {
    if (!rgba || w <= 0 || h <= 0) return;
    BITMAPV4HEADER bh; memset(&bh, 0, sizeof(bh));
    bh.bV4Size = sizeof(BITMAPV4HEADER);
    bh.bV4Width = stride / 4; bh.bV4Height = -h; bh.bV4Planes = 1; bh.bV4BitCount = 32;
    bh.bV4V4Compression = BI_BITFIELDS;
    bh.bV4RedMask = 0x000000FF; bh.bV4GreenMask = 0x0000FF00; bh.bV4BlueMask = 0x00FF0000; bh.bV4AlphaMask = 0xFF000000;
    SetStretchBltMode(v->bb_dc, smooth ? HALFTONE : COLORONCOLOR); SetBrushOrgEx(v->bb_dc, 0, 0, NULL);
    StretchDIBits(v->bb_dc, dst->left, dst->top, dst->right - dst->left, dst->bottom - dst->top,
                  0, 0, w, h, rgba, (BITMAPINFO*)&bh, DIB_RGB_COLORS, SRCCOPY);
}

static void store_last(tb_win32_video *v, const RECT *dst, const tb_frame *f) {
    if (!f || !f->valid) { v->last_w = v->last_h = 0; return; }
    size_t need = (size_t)f->stride * f->height;
    uint8_t *p = (uint8_t*)realloc(v->last_rgba, need);
    if (!p) return;
    memcpy(p, f->rgba, need);
    v->last_rgba = p; v->last_w = f->width; v->last_h = f->height; v->last_stride = f->stride; v->last_dst = *dst;
}

tb_win32_video *tb_win32_video_create(HWND hwnd) {
    tb_win32_video *v = (tb_win32_video*)calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->hwnd = hwnd;
#ifdef TB_WIN32_GDI_ONLY
    v->backend = TB_WIN32_VIDEO_GDI;
#else
    v->backend = TB_WIN32_VIDEO_D3D11;
#endif
    QueryPerformanceFrequency(&v->qpc_freq);
    return v;
}
void tb_win32_video_destroy(tb_win32_video *v) {
    if (!v) return;
    if (v->bb_dc) { SelectObject(v->bb_dc, v->bb_old); DeleteObject(v->bb_bmp); DeleteDC(v->bb_dc); }
    free(v->last_rgba); free(v->push_rgba); free(v);
}
void tb_win32_video_set_backend(tb_win32_video *v, int backend) {
    if (!v) return;
#ifdef TB_WIN32_GDI_ONLY
    (void)backend;
    v->backend = TB_WIN32_VIDEO_GDI;
#else
    v->backend = backend == TB_WIN32_VIDEO_GDI ? TB_WIN32_VIDEO_GDI : TB_WIN32_VIDEO_D3D11;
#endif
}
int tb_win32_video_get_backend(const tb_win32_video *v) { return v ? v->backend : TB_WIN32_VIDEO_GDI; }
void tb_win32_video_resize(tb_win32_video *v, int w, int h) { (void)w; (void)h; if (v) ensure_backbuffer(v); }
void tb_win32_video_clear(tb_win32_video *v, const RECT *dst) { (void)dst; if (!v) return; ensure_backbuffer(v); RECT rc = {0,0,v->bb_w,v->bb_h}; FillRect(v->bb_dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH)); HDC wdc = GetDC(v->hwnd); BitBlt(wdc, 0, 0, v->bb_w, v->bb_h, v->bb_dc, 0, 0, SRCCOPY); ReleaseDC(v->hwnd, wdc); }

void tb_win32_video_begin_push(tb_win32_video *v, int dir) {
    if (!v || v->last_w <= 0 || !v->last_rgba) return;
    size_t need = (size_t)v->last_stride * v->last_h;
    uint8_t *p = (uint8_t*)realloc(v->push_rgba, need);
    if (!p) return;
    memcpy(p, v->last_rgba, need);
    v->push_rgba = p; v->push_w = v->last_w; v->push_h = v->last_h; v->push_stride = v->last_stride; v->push_dst = v->last_dst;
    v->push_dir = dir; v->push_active = true;
    QueryPerformanceCounter(&v->push_start);
}

bool tb_win32_video_present(tb_win32_video *v, const RECT *dst, const tb_frame *frame, bool smooth) {
    if (!v || !dst) return false;
    (void)smooth;
    ensure_backbuffer(v);
#ifdef TB_WIN32_GDI_ONLY
    bool sm = false;
#else
    bool sm = (v->backend != TB_WIN32_VIDEO_GDI);
#endif

    RECT full = {0, 0, v->bb_w, v->bb_h};
    FillRect(v->bb_dc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

    double te = v->push_active ? now_sec(v) / (PUSH_MS / 1000.0) : 2.0;
    if (v->push_active && te < 1.0 && v->push_rgba) {
        double prog = te * te * (3.0 - 2.0 * te);   /* smoothstep, matches Qt */
        int W = v->bb_w, H = v->bb_h;
        int dxN = 0, dyN = 0, dxO = 0, dyO = 0;
        switch (v->push_dir) {
            case 0: dyN = -(int)(H * (1 - prog)); dyO =  (int)(H * prog); break; /* from top */
            case 1: dxN = -(int)(W * (1 - prog)); dxO =  (int)(W * prog); break; /* from left */
            case 2: dxN =  (int)(W * (1 - prog)); dxO = -(int)(W * prog); break; /* from right */
            default:dyN =  (int)(H * (1 - prog)); dyO = -(int)(H * prog); break; /* from bottom */
        }
        RECT od = v->push_dst; OffsetRect(&od, dxO, dyO);
        blit_frame(v, &od, v->push_rgba, v->push_w, v->push_h, v->push_stride, sm);
        if (frame && frame->valid) { RECT nd = *dst; OffsetRect(&nd, dxN, dyN); blit_frame(v, &nd, frame->rgba, frame->width, frame->height, frame->stride, sm); }
    } else {
        if (v->push_active && te >= 1.0) v->push_active = false;
        if (frame && frame->valid) blit_frame(v, dst, frame->rgba, frame->width, frame->height, frame->stride, sm);
    }
    store_last(v, dst, frame);
    return true;
}

HDC tb_win32_video_overlay_dc(tb_win32_video *v) { if (!v) return NULL; ensure_backbuffer(v); return v->bb_dc; }

void tb_win32_video_finish(tb_win32_video *v, HDC dst_dc, const RECT *region) {
    if (!v || !v->bb_dc || !dst_dc) return;
    RECT r = region ? *region : (RECT){0, 0, v->bb_w, v->bb_h};
    if (r.left < 0) r.left = 0; if (r.top < 0) r.top = 0;
    if (r.right > v->bb_w) r.right = v->bb_w; if (r.bottom > v->bb_h) r.bottom = v->bb_h;
    BitBlt(dst_dc, r.left, r.top, r.right - r.left, r.bottom - r.top, v->bb_dc, r.left, r.top, SRCCOPY);
}

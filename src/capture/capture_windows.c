/**
 * @file capture_windows.c
 * @brief Windows capture backend: GDI+ (legacy) and DXGI Desktop Duplication (modern).
 *
 * Strategy selection at runtime:
 *   - Windows 7/8   → GDI BitBlt path (always available)
 *   - Windows 10/11 → DXGI Desktop Duplication (lower latency, handles DWM)
 *     Falls back to GDI if DXGI adapter acquisition fails.
 *
 * All captured images are converted to RGBA byte order for SnapxImage.
 */

#ifdef SNAPX_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <wingdi.h>
#include <dwmapi.h>

#ifdef SNAPX_HAVE_DXGI
#  include <d3d11.h>
#  include <dxgi1_2.h>
#endif

#include "capture.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Private state ──────────────────────────────────────────────────────── */

typedef struct {
    int use_dxgi;  /**< Non-zero if DXGI path is active */
} WindowsState;

/* ─── Monitor enumeration ────────────────────────────────────────────────── */

typedef struct {
    SnapxMonitorInfo *out;
    int               max;
    int               count;
} MonitorEnumCtx;

static BOOL CALLBACK monitor_enum_proc(HMONITOR hmon, HDC hdc, LPRECT rect, LPARAM lp)
{
    MonitorEnumCtx *ctx = (MonitorEnumCtx *)lp;
    if (ctx->count >= ctx->max) return FALSE;

    MONITORINFOEXW mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(hmon, (MONITORINFO *)&mi);

    SnapxMonitorInfo *m = &ctx->out[ctx->count];
    m->index    = ctx->count;
    m->x        = mi.rcMonitor.left;
    m->y        = mi.rcMonitor.top;
    m->width    = mi.rcMonitor.right  - mi.rcMonitor.left;
    m->height   = mi.rcMonitor.bottom - mi.rcMonitor.top;
    m->scale    = 1;
    m->is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1,
                        m->name, (int)sizeof(m->name), NULL, NULL);
    ctx->count++;
    (void)hdc; (void)rect;
    return TRUE;
}

static int windows_get_monitors(SnapxCaptureBackend *backend,
                                 SnapxMonitorInfo *out, int max)
{
    (void)backend;
    MonitorEnumCtx ctx = { out, max, 0 };
    EnumDisplayMonitors(NULL, NULL, monitor_enum_proc, (LPARAM)&ctx);
    return ctx.count;
}

/* ─── GDI BitBlt capture ─────────────────────────────────────────────────── */

/**
 * @brief Capture a rectangle from the virtual desktop using GDI BitBlt.
 */
static SnapxImage *gdi_capture_rect(int x, int y, int w, int h)
{
    HDC screen_dc = GetDC(NULL);
    if (!screen_dc) return NULL;

    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) { ReleaseDC(NULL, screen_dc); return NULL; }

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;  /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        DeleteDC(mem_dc); ReleaseDC(NULL, screen_dc); return NULL;
    }

    HGDIOBJ old = SelectObject(mem_dc, bmp);
    if (!BitBlt(mem_dc, 0, 0, w, h, screen_dc, x, y, SRCCOPY | CAPTUREBLT)) {
        fprintf(stderr, "[windows] BitBlt failed: %lu\n", GetLastError());
        SelectObject(mem_dc, old);
        DeleteObject(bmp); DeleteDC(mem_dc); ReleaseDC(NULL, screen_dc);
        return NULL;
    }
    GdiFlush();

    /* bits is BGRA (Windows DIB); convert to RGBA */
    SnapxImage *img = snapx_image_alloc(w, h);
    if (!img) {
        SelectObject(mem_dc, old);
        DeleteObject(bmp); DeleteDC(mem_dc); ReleaseDC(NULL, screen_dc);
        return NULL;
    }

    const uint8_t *src = (const uint8_t *)bits;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            const uint8_t *s = src  + (row * w + col) * 4;
            uint8_t       *d = img->data + row * img->stride + col * 4;
            d[0] = s[2]; /* R ← B */
            d[1] = s[1]; /* G */
            d[2] = s[0]; /* B ← R */
            d[3] = 0xFF;
        }
    }

    SelectObject(mem_dc, old);
    DeleteObject(bmp);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);
    return img;
}

/* ─── DXGI Desktop Duplication capture ──────────────────────────────────── */

#ifdef SNAPX_HAVE_DXGI

/**
 * @brief Capture a single monitor via DXGI Desktop Duplication API.
 * @param adapter_index  0-based DXGI adapter (GPU) index.
 * @param output_index   0-based output (monitor) index on that adapter.
 * @return Newly allocated SnapxImage, or NULL on failure.
 */
static SnapxImage *dxgi_capture_output(UINT adapter_index, UINT output_index)
{
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) {
        fprintf(stderr, "[dxgi] CreateDXGIFactory1 failed.\n");
        return NULL;
    }

    IDXGIAdapter1 *adapter = NULL;
    if (FAILED(IDXGIFactory1_EnumAdapters1(factory, adapter_index, &adapter))) {
        IDXGIFactory1_Release(factory); return NULL;
    }

    IDXGIOutput *output = NULL;
    if (FAILED(IDXGIAdapter1_EnumOutputs(adapter, output_index, &output))) {
        IDXGIAdapter1_Release(adapter); IDXGIFactory1_Release(factory);
        return NULL;
    }

    IDXGIOutput1 *output1 = NULL;
    if (FAILED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1))) {
        IDXGIOutput_Release(output);
        IDXGIAdapter1_Release(adapter); IDXGIFactory1_Release(factory);
        return NULL;
    }

    /* Create D3D11 device */
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN,
                                  NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                  &device, &fl, &context))) {
        IDXGIOutput1_Release(output1); IDXGIOutput_Release(output);
        IDXGIAdapter1_Release(adapter); IDXGIFactory1_Release(factory);
        return NULL;
    }

    IDXGIOutputDuplication *duplication = NULL;
    HRESULT hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)device, &duplication);
    if (FAILED(hr)) {
        fprintf(stderr, "[dxgi] DuplicateOutput failed: 0x%08lX\n", hr);
        ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
        IDXGIOutput1_Release(output1); IDXGIOutput_Release(output);
        IDXGIAdapter1_Release(adapter); IDXGIFactory1_Release(factory);
        return NULL;
    }

    /* Acquire next frame (1 second timeout) */
    IDXGIResource *desktop_resource = NULL;
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    hr = IDXGIOutputDuplication_AcquireNextFrame(duplication, 1000,
                                                  &frame_info, &desktop_resource);
    if (FAILED(hr)) {
        fprintf(stderr, "[dxgi] AcquireNextFrame failed: 0x%08lX\n", hr);
        IDXGIOutputDuplication_Release(duplication);
        ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
        IDXGIOutput1_Release(output1); IDXGIOutput_Release(output);
        IDXGIAdapter1_Release(adapter); IDXGIFactory1_Release(factory);
        return NULL;
    }

    ID3D11Texture2D *gpu_texture = NULL;
    IDXGIResource_QueryInterface(desktop_resource, &IID_ID3D11Texture2D,
                                  (void **)&gpu_texture);
    IDXGIResource_Release(desktop_resource);

    /* Create a CPU-accessible staging texture */
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(gpu_texture, &desc);
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags      = 0;
    desc.MiscFlags      = 0;

    ID3D11Texture2D *cpu_texture = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &cpu_texture))) {
        ID3D11Texture2D_Release(gpu_texture);
        IDXGIOutputDuplication_ReleaseFrame(duplication);
        IDXGIOutputDuplication_Release(duplication);
        ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
        IDXGIOutput1_Release(output1); IDXGIOutput_Release(output);
        IDXGIAdapter1_Release(adapter); IDXGIFactory1_Release(factory);
        return NULL;
    }

    ID3D11DeviceContext_CopyResource(context,
                                      (ID3D11Resource *)cpu_texture,
                                      (ID3D11Resource *)gpu_texture);
    ID3D11Texture2D_Release(gpu_texture);
    IDXGIOutputDuplication_ReleaseFrame(duplication);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)cpu_texture,
                                        0, D3D11_MAP_READ, 0, &mapped))) {
        ID3D11Texture2D_Release(cpu_texture);
        IDXGIOutputDuplication_Release(duplication);
        ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
        IDXGIOutput1_Release(output1); IDXGIOutput_Release(output);
        IDXGIAdapter1_Release(adapter); IDXGIFactory1_Release(factory);
        return NULL;
    }

    int w = (int)desc.Width, h = (int)desc.Height;
    SnapxImage *img = snapx_image_alloc(w, h);
    if (img) {
        /* DXGI frame is BGRA; convert to RGBA */
        const uint8_t *src = (const uint8_t *)mapped.pData;
        for (int row = 0; row < h; row++) {
            const uint8_t *srow = src + row * mapped.RowPitch;
            uint8_t       *drow = img->data + row * img->stride;
            for (int col = 0; col < w; col++) {
                drow[col * 4 + 0] = srow[col * 4 + 2]; /* R */
                drow[col * 4 + 1] = srow[col * 4 + 1]; /* G */
                drow[col * 4 + 2] = srow[col * 4 + 0]; /* B */
                drow[col * 4 + 3] = 0xFF;
            }
        }
    }

    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)cpu_texture, 0);
    ID3D11Texture2D_Release(cpu_texture);
    IDXGIOutputDuplication_Release(duplication);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    IDXGIOutput1_Release(output1);
    IDXGIOutput_Release(output);
    IDXGIAdapter1_Release(adapter);
    IDXGIFactory1_Release(factory);
    return img;
}

#endif /* SNAPX_HAVE_DXGI */

/* ─── Backend capture dispatch ───────────────────────────────────────────── */

static SnapxImage *windows_capture(SnapxCaptureBackend *backend,
                                    const SnapxCaptureRequest *req)
{
    WindowsState *st = (WindowsState *)backend->priv;

    switch (req->mode) {
        case SNAPX_CAPTURE_FULLSCREEN: {
            int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
#ifdef SNAPX_HAVE_DXGI
            if (st->use_dxgi) {
                SnapxImage *img = dxgi_capture_output(0, 0);
                if (img) return img;
                fprintf(stderr, "[windows] DXGI failed, falling back to GDI.\n");
            }
#endif
            return gdi_capture_rect(sx, sy, sw, sh);
        }

        case SNAPX_CAPTURE_MONITOR: {
            SnapxMonitorInfo monitors[16];
            int n = windows_get_monitors(backend, monitors, 16);
            int idx = (req->monitor_index < 0) ? 0 : req->monitor_index;
            if (idx >= n) { fprintf(stderr, "[windows] Monitor %d not found.\n", idx); return NULL; }
#ifdef SNAPX_HAVE_DXGI
            if (st->use_dxgi) {
                SnapxImage *img = dxgi_capture_output(0, (UINT)idx);
                if (img) return img;
            }
#endif
            return gdi_capture_rect(monitors[idx].x, monitors[idx].y,
                                     monitors[idx].width, monitors[idx].height);
        }

        case SNAPX_CAPTURE_REGION:
            return gdi_capture_rect(req->region_x, req->region_y,
                                     req->region_w, req->region_h);

        case SNAPX_CAPTURE_ACTIVE_WINDOW:
        case SNAPX_CAPTURE_WINDOW: {
            HWND hwnd = GetForegroundWindow();
            if (!hwnd) return NULL;
            RECT wr;
            /* Use DWM extended frame for accurate bounds on Win10+ */
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                              &wr, sizeof(wr)))) {
                GetWindowRect(hwnd, &wr);
            }
            return gdi_capture_rect(wr.left, wr.top,
                                     wr.right - wr.left, wr.bottom - wr.top);
        }

        default:
            return NULL;
    }
    (void)st;
}

/* ─── Global hotkey (Win32) ──────────────────────────────────────────────── */
/* Implemented in utils/hotkey.c for Win32 */

/* ─── Destroy ────────────────────────────────────────────────────────────── */

static void windows_destroy(SnapxCaptureBackend *backend)
{
    WindowsState *st = (WindowsState *)backend->priv;
    free(st);
    backend->priv = NULL;
}

/* ─── Init ───────────────────────────────────────────────────────────────── */

int snapx_capture_windows_init(SnapxCaptureBackend *backend)
{
    WindowsState *st = calloc(1, sizeof(WindowsState));
    if (!st) return -1;

#ifdef SNAPX_HAVE_DXGI
    /* Probe DXGI availability */
    IDXGIFactory1 *factory = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) {
        st->use_dxgi = 1;
        IDXGIFactory1_Release(factory);
    }
#endif

    backend->priv         = st;
    backend->capture      = windows_capture;
    backend->get_monitors = windows_get_monitors;
    backend->destroy      = windows_destroy;
    backend->type         = st->use_dxgi ? SNAPX_BACKEND_WIN_DXGI
                                         : SNAPX_BACKEND_WIN_GDI;

    fprintf(stderr, "[windows] Backend initialised (%s)\n",
            st->use_dxgi ? "DXGI" : "GDI");
    return 0;
}

#endif /* SNAPX_PLATFORM_WINDOWS */

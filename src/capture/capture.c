/**
 * @file capture.c
 * @brief Common capture API: image allocation, backend init with fallback,
 *        delay logic, and backend dispatch.
 */

#include "capture.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef SNAPX_PLATFORM_LINUX
#  include <unistd.h>
#endif
#ifdef SNAPX_PLATFORM_WINDOWS
#  include <windows.h>
#endif
#ifdef SNAPX_PLATFORM_MACOS
#  include <unistd.h>
#endif

/* ─── Image helpers ─────────────────────────────────────────────────────── */

SnapxImage *snapx_image_alloc(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    SnapxImage *img = calloc(1, sizeof(SnapxImage));
    if (!img) return NULL;

    img->stride = width * 4;
    img->data   = calloc((size_t)(img->stride * height), 1);
    if (!img->data) { free(img); return NULL; }

    img->width  = width;
    img->height = height;
    img->scale  = 1;
    return img;
}

void snapx_image_free(SnapxImage *img)
{
    if (!img) return;
    free(img->data);
    free(img);
}

SnapxImage *snapx_image_crop(const SnapxImage *src,
                               int x, int y, int w, int h)
{
    if (!src || !src->data) return NULL;

    /* Clamp to source bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > src->width)  w = src->width  - x;
    if (y + h > src->height) h = src->height - y;
    if (w <= 0 || h <= 0) return NULL;

    SnapxImage *dst = snapx_image_alloc(w, h);
    if (!dst) return NULL;
    dst->scale = src->scale;

    for (int row = 0; row < h; row++) {
        const uint8_t *src_row = src->data + (y + row) * src->stride + x * 4;
        uint8_t       *dst_row = dst->data +        row * dst->stride;
        memcpy(dst_row, src_row, (size_t)(w * 4));
    }
    return dst;
}

static void desktop_bounds(const SnapxMonitorInfo *mons, int n,
                            int *virt_x, int *virt_y,
                            int *virt_w, int *virt_h)
{
    if (n <= 0) {
        *virt_x = *virt_y = 0;
        *virt_w = *virt_h = 0;
        return;
    }
    int min_x = mons[0].x, min_y = mons[0].y;
    int max_x = mons[0].x + mons[0].width;
    int max_y = mons[0].y + mons[0].height;
    for (int i = 1; i < n; i++) {
        if (mons[i].x < min_x) min_x = mons[i].x;
        if (mons[i].y < min_y) min_y = mons[i].y;
        int rx = mons[i].x + mons[i].width;
        int ry = mons[i].y + mons[i].height;
        if (rx > max_x) max_x = rx;
        if (ry > max_y) max_y = ry;
    }
    *virt_x = min_x;
    *virt_y = min_y;
    *virt_w = max_x - min_x;
    *virt_h = max_y - min_y;
}

SnapxImage *snapx_image_crop_desktop(const SnapxImage *src,
                                      int region_x, int region_y,
                                      int region_w, int region_h,
                                      const SnapxMonitorInfo *mons,
                                      int n_monitors)
{
    if (!src || !src->data || region_w <= 0 || region_h <= 0)
        return NULL;

    int virt_x, virt_y, virt_w, virt_h;
    desktop_bounds(mons, n_monitors, &virt_x, &virt_y, &virt_w, &virt_h);
    if (virt_w <= 0 || virt_h <= 0) {
        virt_x = virt_y = 0;
        virt_w = src->width;
        virt_h = src->height;
    }

    int px = (int)round((region_x - virt_x) * (double)src->width  / (double)virt_w);
    int py = (int)round((region_y - virt_y) * (double)src->height / (double)virt_h);
    int pw = (int)round((double)region_w * (double)src->width  / (double)virt_w);
    int ph = (int)round((double)region_h * (double)src->height / (double)virt_h);

    return snapx_image_crop(src, px, py, pw, ph);
}

/* ─── Backend init ───────────────────────────────────────────────────────── */

int snapx_capture_backend_init(SnapxCaptureBackend *backend, SnapxBackendType type)
{
    if (!backend) return -1;
    memset(backend, 0, sizeof(*backend));
    backend->type = type;

    switch (type) {
#if defined(SNAPX_PLATFORM_LINUX) && defined(SNAPX_HAVE_WAYLAND)
        case SNAPX_BACKEND_WAYLAND:
            return snapx_capture_wayland_init(backend);
#endif
#if defined(SNAPX_PLATFORM_LINUX) && defined(SNAPX_HAVE_X11)
        case SNAPX_BACKEND_X11:
            return snapx_capture_x11_init(backend);
#endif
#if defined(SNAPX_PLATFORM_WINDOWS)
        case SNAPX_BACKEND_WIN_GDI:
        case SNAPX_BACKEND_WIN_DXGI:
            return snapx_capture_windows_init(backend);
#endif
#if defined(SNAPX_PLATFORM_MACOS)
        case SNAPX_BACKEND_MACOS_CG:
        case SNAPX_BACKEND_MACOS_SCK:
            return snapx_capture_macos_init(backend);
#endif
        default:
            fprintf(stderr, "[capture] No backend compiled in for type %d (%s)\n",
                    type, snapx_backend_name(type));
            return -1;
    }
}

/**
 * @brief Init the best available backend using the platform probe result.
 *
 * Tries info->preferred first, then info->fallback.
 * Returns 0 on success with @p backend populated.
 */
int snapx_capture_backend_init_best(SnapxCaptureBackend *backend,
                                     const SnapxPlatformInfo *info)
{
    if (!backend || !info) return -1;

    if (info->preferred != SNAPX_BACKEND_UNKNOWN) {
        fprintf(stderr, "[capture] Trying backend: %s\n",
                snapx_backend_name(info->preferred));
        if (snapx_capture_backend_init(backend, info->preferred) == 0)
            return 0;
        fprintf(stderr, "[capture] Backend init failed.\n");
    }

    if (info->fallback != SNAPX_BACKEND_UNKNOWN) {
        fprintf(stderr, "[capture] Trying fallback: %s\n",
                snapx_backend_name(info->fallback));
        if (snapx_capture_backend_init(backend, info->fallback) == 0)
            return 0;
        fprintf(stderr, "[capture] Fallback init failed.\n");
    }

    fprintf(stderr, "[capture] ERROR: All backends exhausted. Cannot capture.\n");
    return -1;
}

/* ─── Capture with automatic fallback on failure ─────────────────────────── */

SnapxImage *snapx_capture(SnapxCaptureBackend *backend,
                           const SnapxCaptureRequest *req)
{
    if (!backend || !backend->capture || !req) return NULL;

    if (req->delay_sec > 0) {
        fprintf(stderr, "[capture] Waiting %d second(s)...\n", req->delay_sec);
#if defined(SNAPX_PLATFORM_LINUX) || defined(SNAPX_PLATFORM_MACOS)
        sleep((unsigned int)req->delay_sec);
#elif defined(SNAPX_PLATFORM_WINDOWS)
        Sleep((DWORD)(req->delay_sec * 1000));
#endif
    }

    SnapxImage *img = backend->capture(backend, req);

    /* On Linux: if Wayland portal timed-out/failed, silently retry on X11 */
#if defined(SNAPX_PLATFORM_LINUX) && defined(SNAPX_HAVE_X11)
    if (!img && backend->type == SNAPX_BACKEND_WAYLAND) {
        const char *xd = getenv("DISPLAY");
        if (xd && xd[0]) {
            fprintf(stderr, "[capture] Wayland capture failed — "
                    "retrying on X11/Xwayland...\n");
            SnapxCaptureBackend x11 = {0};
            if (snapx_capture_backend_init(&x11, SNAPX_BACKEND_X11) == 0) {
                img = x11.capture(&x11, req);
                if (img) {
                    snapx_capture_backend_destroy(backend);
                    *backend = x11;
                } else {
                    snapx_capture_backend_destroy(&x11);
                }
            }
        }
    }
#endif

#if defined(SNAPX_PLATFORM_WINDOWS)
    /* On Windows: DXGI can fail for protected content / some virtual desktops */
    if (!img && backend->type == SNAPX_BACKEND_WIN_DXGI) {
        fprintf(stderr, "[capture] DXGI capture failed — retrying with GDI...\n");
        SnapxCaptureBackend gdi = {0};
        if (snapx_capture_backend_init(&gdi, SNAPX_BACKEND_WIN_GDI) == 0) {
            img = gdi.capture(&gdi, req);
            if (img) {
                snapx_capture_backend_destroy(backend);
                *backend = gdi;
            } else {
                snapx_capture_backend_destroy(&gdi);
            }
        }
    }
#endif

#if defined(SNAPX_PLATFORM_MACOS)
    /* On macOS: SCK can fail without screen recording permission */
    if (!img && backend->type == SNAPX_BACKEND_MACOS_SCK) {
        fprintf(stderr, "[capture] ScreenCaptureKit failed — "
                "retrying with CoreGraphics...\n");
        SnapxCaptureBackend cg = {0};
        if (snapx_capture_backend_init(&cg, SNAPX_BACKEND_MACOS_CG) == 0) {
            img = cg.capture(&cg, req);
            if (img) {
                snapx_capture_backend_destroy(backend);
                *backend = cg;
            } else {
                snapx_capture_backend_destroy(&cg);
            }
        }
    }
#endif

    return img;
}

int snapx_get_monitors(SnapxCaptureBackend *backend,
                        SnapxMonitorInfo *out, int max)
{
    if (!backend || !backend->get_monitors || !out || max <= 0) return -1;
    return backend->get_monitors(backend, out, max);
}

void snapx_capture_backend_destroy(SnapxCaptureBackend *backend)
{
    if (backend && backend->destroy)
        backend->destroy(backend);
}

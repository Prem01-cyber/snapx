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

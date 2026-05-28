/**
 * @file capture.c
 * @brief Common capture API: image allocation, backend dispatch, delay logic.
 */

#include "capture.h"
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

/* ─── Backend router ─────────────────────────────────────────────────────── */

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
            fprintf(stderr, "[capture] No backend available for type %d\n", type);
            return -1;
    }
}

SnapxImage *snapx_capture(SnapxCaptureBackend *backend, const SnapxCaptureRequest *req)
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

    return backend->capture(backend, req);
}

int snapx_get_monitors(SnapxCaptureBackend *backend, SnapxMonitorInfo *out, int max)
{
    if (!backend || !backend->get_monitors || !out || max <= 0) return -1;
    return backend->get_monitors(backend, out, max);
}

void snapx_capture_backend_destroy(SnapxCaptureBackend *backend)
{
    if (backend && backend->destroy)
        backend->destroy(backend);
}

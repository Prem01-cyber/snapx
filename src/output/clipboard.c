/**
 * @file clipboard.c
 * @brief Platform clipboard copy for SnapxImage.
 *
 * Linux / macOS   → GDK Clipboard (GtkClipboard on GTK3, GdkClipboard on GTK4)
 *                   encodes image to PNG in memory and stores as image/png MIME type.
 * Windows         → Win32 OpenClipboard / SetClipboardData with CF_DIB.
 */

#include "clipboard.h"
#include "../capture/capture.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── GTK / GDK clipboard (Linux & macOS via GTK) ───────────────────────── */

#if defined(SNAPX_PLATFORM_LINUX) || defined(SNAPX_PLATFORM_MACOS)

#ifndef SNAPX_HEADLESS
#  include <gtk/gtk.h>
#  include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#if defined(SNAPX_USE_GTK4) && !defined(SNAPX_HEADLESS)
/* GTK4: GdkClipboard */

static void clipboard_store_ready(GObject *source, GAsyncResult *res, gpointer data)
{
    GdkTexture *tex = data;
    GError *err = NULL;
    gdk_clipboard_store_finish(GDK_CLIPBOARD(source), res, &err);
    if (err) {
        fprintf(stderr, "[clipboard] Store failed: %s\n", err->message);
        g_error_free(err);
    }
    if (tex)
        g_object_unref(tex);
}

void snapx_clipboard_copy_image(const SnapxImage *img)
{
    if (!img || !img->data) return;

    GdkDisplay *dpy = gdk_display_get_default();
    if (!dpy) return;

    size_t nbytes = (size_t)img->stride * (size_t)img->height;
    gpointer copy = g_memdup2(img->data, nbytes);
    if (!copy) {
        fprintf(stderr, "[clipboard] Failed to copy image data.\n");
        return;
    }

    GBytes *gbytes = g_bytes_new_with_free_func(copy, nbytes, g_free, copy);
    GdkTexture *tex = gdk_memory_texture_new(
        img->width, img->height,
        GDK_MEMORY_R8G8B8A8,
        gbytes,
        (gsize)img->stride);
    g_bytes_unref(gbytes);
    if (!tex) {
        fprintf(stderr, "[clipboard] Failed to create texture.\n");
        return;
    }

    GdkClipboard *cb = gdk_display_get_clipboard(dpy);
    gdk_clipboard_set_texture(cb, tex);

    gdk_clipboard_store_async(cb, G_PRIORITY_DEFAULT, NULL,
                               clipboard_store_ready, g_object_ref(tex));
    fprintf(stderr, "[clipboard] Image copied to clipboard.\n");
}

#elif !defined(SNAPX_HEADLESS)  /* GTK3 */

void snapx_clipboard_copy_image(const SnapxImage *img)
{
    if (!img) return;

    GdkPixbuf *pb = gdk_pixbuf_new_from_data(
        img->data, GDK_COLORSPACE_RGB, TRUE, 8,
        img->width, img->height, img->stride,
        NULL, NULL);
    if (!pb) { fprintf(stderr, "[clipboard] Failed to create pixbuf.\n"); return; }

    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_image(cb, pb);
    gtk_clipboard_store(cb);
    g_object_unref(pb);
    fprintf(stderr, "[clipboard] Image copied to clipboard.\n");
}

#else  /* SNAPX_HEADLESS - no clipboard support */

void snapx_clipboard_copy_image(const SnapxImage *img)
{
    (void)img;
    fprintf(stderr, "[clipboard] Clipboard not available in headless build.\n");
}

#endif  /* GTK3 / headless */

#endif  /* PLATFORM_LINUX || PLATFORM_MACOS */

/* ─── Windows clipboard ──────────────────────────────────────────────────── */

#ifdef SNAPX_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void snapx_clipboard_copy_image(const SnapxImage *img)
{
    if (!img) return;

    int w = img->width, h = img->height;
    DWORD header_size = sizeof(BITMAPINFOHEADER);
    DWORD data_size   = (DWORD)(w * h * 4);
    DWORD total_size  = header_size + data_size;

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, total_size);
    if (!hGlobal) return;

    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)GlobalLock(hGlobal);
    bih->biSize          = sizeof(BITMAPINFOHEADER);
    bih->biWidth         = w;
    bih->biHeight        = -h;  /* top-down */
    bih->biPlanes        = 1;
    bih->biBitCount      = 32;
    bih->biCompression   = BI_RGB;
    bih->biSizeImage     = data_size;

    uint8_t *dst = (uint8_t *)bih + header_size;
    /* Convert RGBA → BGRA for Windows DIB */
    for (int row = 0; row < h; row++) {
        const uint8_t *src = img->data + row * img->stride;
        uint8_t       *d   = dst + row * w * 4;
        for (int col = 0; col < w; col++) {
            d[col * 4 + 0] = src[col * 4 + 2]; /* B */
            d[col * 4 + 1] = src[col * 4 + 1]; /* G */
            d[col * 4 + 2] = src[col * 4 + 0]; /* R */
            d[col * 4 + 3] = src[col * 4 + 3]; /* A */
        }
    }
    GlobalUnlock(hGlobal);

    if (!OpenClipboard(NULL)) { GlobalFree(hGlobal); return; }
    EmptyClipboard();
    SetClipboardData(CF_DIB, hGlobal);
    CloseClipboard();
    fprintf(stderr, "[clipboard] Image copied to clipboard.\n");
}

#endif /* SNAPX_PLATFORM_WINDOWS */

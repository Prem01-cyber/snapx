/**
 * @file beautify.c
 * @brief Cairo implementation of the "beautiful screenshot" compositor.
 */

#include "beautify.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifdef SNAPX_HAVE_CAIRO
#  include <cairo/cairo.h>
#endif

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

void snapx_beautify_defaults(SnapxBeautifyConfig *cfg)
{
    if (!cfg) return;
    cfg->enabled       = 0;
    cfg->padding       = 64;
    cfg->bg_type       = SNAPX_BG_GRADIENT;
    cfg->bg_r  = 0.40; cfg->bg_g  = 0.49; cfg->bg_b  = 0.92;  /* indigo  */
    cfg->bg_r2 = 0.61; cfg->bg_g2 = 0.35; cfg->bg_b2 = 0.71;  /* violet  */
    cfg->corner_radius = 12;
    cfg->shadow        = 1;
    cfg->shadow_size   = 24;
}

#ifdef SNAPX_HAVE_CAIRO

/* ─── Pixel-format conversion ────────────────────────────────────────────── */

/** Wrap a SnapxImage (RGBA, straight alpha) as a premultiplied ARGB32 surface. */
static cairo_surface_t *src_to_argb32(const SnapxImage *img)
{
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img->width, img->height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_surface_flush(surf);
    unsigned char *dst    = cairo_image_surface_get_data(surf);
    int            dstride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < img->height; y++) {
        const uint8_t *srow = img->data + (size_t)y * img->stride;
        uint32_t      *drow = (uint32_t *)(dst + (size_t)y * dstride);
        for (int x = 0; x < img->width; x++) {
            uint8_t r = srow[x*4+0], g = srow[x*4+1];
            uint8_t b = srow[x*4+2], a = srow[x*4+3];
            if (a == 0xFF) {
                drow[x] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
            } else {
                uint32_t rm = (r*a+127)/255, gm = (g*a+127)/255, bm = (b*a+127)/255;
                drow[x] = ((uint32_t)a<<24) | (rm<<16) | (gm<<8) | bm;
            }
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

/** Convert a premultiplied ARGB32 surface into a new RGBA SnapxImage. */
static SnapxImage *argb32_to_image(cairo_surface_t *surf, int scale)
{
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    SnapxImage *out = snapx_image_alloc(w, h);
    if (!out) return NULL;
    out->scale = scale > 0 ? scale : 1;

    cairo_surface_flush(surf);
    const unsigned char *src = cairo_image_surface_get_data(surf);
    int sstride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < h; y++) {
        const uint32_t *srow = (const uint32_t *)(src + (size_t)y * sstride);
        uint8_t        *drow = out->data + (size_t)y * out->stride;
        for (int x = 0; x < w; x++) {
            uint32_t px = srow[x];
            uint8_t a = (px >> 24) & 0xFF;
            uint8_t r = (px >> 16) & 0xFF;
            uint8_t g = (px >>  8) & 0xFF;
            uint8_t b =  px        & 0xFF;
            if (a != 0 && a != 0xFF) {   /* un-premultiply */
                r = (uint8_t)((r * 255 + a/2) / a);
                g = (uint8_t)((g * 255 + a/2) / a);
                b = (uint8_t)((b * 255 + a/2) / a);
            }
            drow[x*4+0] = r; drow[x*4+1] = g; drow[x*4+2] = b; drow[x*4+3] = a;
        }
    }
    return out;
}

/* ─── Path helpers ───────────────────────────────────────────────────────── */

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r < 0.5) { cairo_rectangle(cr, x, y, w, h); return; }
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI/2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,        M_PI/2);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI/2,   M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI,     3*M_PI/2);
    cairo_close_path(cr);
}

#endif /* SNAPX_HAVE_CAIRO */

/* ─── Compositor ─────────────────────────────────────────────────────────── */

/** Allocate a plain RGBA copy of @p src (fallback / no-op path). */
static SnapxImage *plain_copy(const SnapxImage *src)
{
    SnapxImage *copy = snapx_image_alloc(src->width, src->height);
    if (!copy) return NULL;
    copy->scale = src->scale;
    for (int y = 0; y < src->height; y++)
        memcpy(copy->data + (size_t)y * copy->stride,
               src->data  + (size_t)y * src->stride,
               (size_t)src->width * 4);
    return copy;
}

SnapxImage *snapx_beautify_apply(const SnapxImage *src,
                                 const SnapxBeautifyConfig *cfg)
{
    if (!src || !src->data) return NULL;

    int pad    = (cfg && cfg->enabled) ? (cfg->padding > 0 ? cfg->padding : 0) : 0;
    int radius = (cfg && cfg->enabled) ? (cfg->corner_radius > 0 ? cfg->corner_radius : 0) : 0;
    int shadow = (cfg && cfg->enabled) ? cfg->shadow : 0;

    /* Nothing to do — return a plain copy so the caller can free uniformly. */
    if (!cfg || !cfg->enabled || (pad == 0 && radius == 0 && !shadow))
        return plain_copy(src);

#ifndef SNAPX_HAVE_CAIRO
    return plain_copy(src);   /* compositing unavailable without Cairo */
#else
    int ow = src->width  + pad * 2;
    int oh = src->height + pad * 2;

    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ow, oh);
    if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(out);
        return NULL;
    }
    cairo_t *cr = cairo_create(out);

    /* 1. Background ------------------------------------------------------- */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (cfg->bg_type == SNAPX_BG_TRANSPARENT) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
    } else if (cfg->bg_type == SNAPX_BG_GRADIENT) {
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, ow, oh);
        cairo_pattern_add_color_stop_rgb(g, 0.0, cfg->bg_r,  cfg->bg_g,  cfg->bg_b);
        cairo_pattern_add_color_stop_rgb(g, 1.0, cfg->bg_r2, cfg->bg_g2, cfg->bg_b2);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
    } else {
        cairo_set_source_rgb(cr, cfg->bg_r, cfg->bg_g, cfg->bg_b);
        cairo_paint(cr);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double ix = pad, iy = pad, iw = src->width, ih = src->height;

    /* 2. Drop shadow ------------------------------------------------------ */
    if (shadow && pad > 0) {
        int spread = cfg->shadow_size > 0 ? cfg->shadow_size : 24;
        if (spread > pad) spread = pad;            /* keep within the canvas */
        double dy = spread * 0.35;                 /* offset shadow downward */
        /* Layered translucent rounded rects approximate a soft penumbra. */
        for (int i = spread; i >= 1; i--) {
            double grow = i;
            double a = 0.38 * (1.0 - (double)i / (spread + 1)) / 2.2;
            cairo_set_source_rgba(cr, 0, 0, 0, a);
            rounded_rect(cr, ix - grow, iy - grow + dy,
                         iw + grow*2, ih + grow*2, radius + grow);
            cairo_fill(cr);
        }
    }

    /* 3. Screenshot (rounded-clipped) ------------------------------------- */
    cairo_surface_t *img = src_to_argb32(src);
    if (img) {
        cairo_save(cr);
        rounded_rect(cr, ix, iy, iw, ih, radius);
        cairo_clip(cr);
        cairo_set_source_surface(cr, img, ix, iy);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(img);
    }

    cairo_destroy(cr);

    SnapxImage *result = argb32_to_image(out, src->scale);
    cairo_surface_destroy(out);
    return result;
#endif /* SNAPX_HAVE_CAIRO */
}

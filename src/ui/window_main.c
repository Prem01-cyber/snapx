/**
 * @file window_main.c
 * @brief Main application window — screenshot preview + annotation toolbar.
 *
 * Performance highlights
 * ──────────────────────
 *  • display_surface  — RGBA→ARGB32 conversion done once per capture (not per frame).
 *  • scaled_surface   — the preview scaled to the drawing-area size; rebuilt only
 *                       when the image or window dimensions change, never per-frame.
 *  • annot_surface    — annotation strokes rendered onto a separate surface;
 *                       rebuilt only when the dirty flag from toolbar.c is set.
 *  • Zoom + pan       — Ctrl+scroll zooms; middle-drag pans.  The three surfaces
 *                       above are viewport-aware and invalidated on zoom/pan change.
 *
 * Layout (GTK4)
 * ─────────────
 *   GtkApplicationWindow
 *     └─ GtkHeaderBar  (capture buttons + settings + zoom label)
 *     └─ GtkBox (vertical)
 *           ├─ snapx-toolbar (annotation tools)
 *           ├─ GtkScrolledWindow → GtkDrawingArea  (preview + annotations)
 *           └─ GtkActionBar  (format / quality / copy / save)
 *           └─ GtkLabel (statusbar)
 */

#include "window_main.h"
#include "overlay.h"
#include "toolbar.h"
#include "settings_dialog.h"
#include "beautify_dialog.h"
#include "../output/beautify.h"
#include "../annotation/canvas.h"
#include "../annotation/annotation.h"
#include "../capture/capture.h"
#include "../capture/platform.h"
#include "../output/save.h"
#include "../output/clipboard.h"
#include "../output/upload.h"
#include "../output/ocr.h"
#include "pin_window.h"
#include "history_panel.h"
#include "tray.h"
#include "../ipc/dbus_service.h"
#include "../utils/config.h"
#include "../utils/hotkey.h"
#include "../utils/shortcut.h"
#include "../utils/monitor.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>

#ifdef GDK_WINDOWING_X11
#  include <gdk/x11/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#  include <gdk/wayland/gdkwayland.h>
#endif

/* ─── Zoom limits ────────────────────────────────────────────────────────── */
#define ZOOM_MIN   0.05   /* 5% */
#define ZOOM_MAX  16.0    /* 1600% */
#define ZOOM_STEP  0.15   /* per scroll tick (multiplicative) */

/* ─── Window state ───────────────────────────────────────────────────────── */

typedef struct {
    GtkApplicationWindow *win;
    GtkWidget            *drawing_area;
    GtkWidget            *toolbar_box;
    GtkWidget            *format_combo;
    GtkWidget            *quality_scale;
    GtkWidget            *quality_label;
    GtkWidget            *statusbar;
    GtkWidget            *zoom_label;    /**< Shows "100%" in the header    */
    GtkWidget            *btn_upload;
    GtkWidget            *btn_ocr;
    GtkWidget            *btn_recents;
    GtkWidget            *lbl_upload_status;
    GtkWidget            *lbl_ocr_status;

    SnapxConfig          *config;
    SnapxCaptureBackend  *backend;
    SnapxCaptureMode     *default_mode;

    /* ── Image / surfaces ─────────────────────────────────────────────── */
    SnapxImage           *current_image;

    /** RGBA→ARGB32 conversion cache (full resolution, one per capture). */
    cairo_surface_t      *display_surface;

    /** Scaled surface (fits drawing-area at current zoom).
     *  Invalidated by: new image, window resize, zoom change. */
    cairo_surface_t      *scaled_surface;
    int                   scaled_for_cw;   /**< canvas size when scaled_surface built */
    int                   scaled_for_ch;
    double                scaled_for_zoom;

    /** Annotation overlay (same size as display_surface, ARGB32).
     *  Invalidated by: new image, any annotation change. */
    cairo_surface_t      *annot_surface;
    gboolean              annot_surface_valid;

    SnapxAnnotationCanvas *canvas;

    /* ── Zoom + pan ───────────────────────────────────────────────────── */
    double  zoom;           /**< 0 = fit-to-window; >0 = explicit scale factor */
    double  pan_x, pan_y;  /**< view offset in image pixels                    */
    gboolean fit_mode;      /**< TRUE when zoom tracks window size              */

    /* ── Pan drag ─────────────────────────────────────────────────────── */
    gboolean panning;
    double   pan_last_x, pan_last_y;

    /* Saved before capture hide; restored in after_capture / cancel */
    gboolean          saved_window_state;
    int               saved_x, saved_y, saved_w, saved_h;
    GdkToplevelState  saved_toplevel_state;

    /* Coalesced drawing-area redraw (one per frame) */
    gboolean          draw_pending;
    guint             draw_tick_id;   /**< GTK4 tick callback id, or 0       */

    /* Capture-mode / action buttons */
    GtkWidget *btn_fullscreen;
    GtkWidget *btn_region;
    GtkWidget *btn_monitor;
    GtkWidget *btn_window;
    GtkWidget *btn_settings;
    GtkWidget *btn_fit;

    char last_save_path[512];  /**< Last successful save (for reveal in folder) */
    int  upload_busy;

    GtkWidget *history_panel;
    int        crop_active;
    gboolean   crop_dragging;
    double     crop_x1, crop_y1, crop_x2, crop_y2;

    /** Cached XDG parent handle ("x11:0x…" or "wayland:…") for portal modals */
    char portal_parent[128];
    /** Raw Wayland export handle (for gdk_wayland_toplevel_drop_exported_handle) */
    char wayland_exported_raw[64];
    GMainLoop *portal_export_loop;  /**< Non-NULL while blocking for export callback */
} MainWindow;

static MainWindow g_win;

/* ─── Forward declarations ───────────────────────────────────────────────── */

static void invalidate_scaled(MainWindow *mw);
static void redraw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static void do_capture(MainWindow *mw, SnapxCaptureMode mode, int delay);
static void do_upload_image(MainWindow *mw, SnapxImage *img);
static void on_crop(MainWindow *mw);
static void update_zoom_label(MainWindow *mw);
static void snapx_main_bind_canvas(MainWindow *mw);
static void snapx_main_update_workflow_ui(MainWindow *mw);
static void on_settings(GtkButton *b, gpointer d);

/* ─── CSS loading ─────────────────────────────────────────────────────────── */

static void load_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    /* Try bundled resource first */
#ifdef SNAPX_USE_GTK4
    gtk_css_provider_load_from_resource(provider,
        "/io/github/snapx/style/snapx.css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
#else
    gtk_css_provider_load_from_resource(provider,
        "/io/github/snapx/style/snapx.css");
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
#endif
    g_object_unref(provider);
}

/* ─── RGBA → Cairo ARGB32 ─────────────────────────────────────────────────── */

static cairo_surface_t *make_display_surface(const SnapxImage *img)
{
    if (!img) return NULL;
    int w = img->width, h = img->height;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf); return NULL;
    }

    cairo_surface_flush(surf);
    unsigned char *dst     = cairo_image_surface_get_data(surf);
    int            stride  = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < h; y++) {
        const uint8_t *src_row = img->data + y * img->stride;
        uint32_t      *dst_row = (uint32_t *)(dst + y * stride);
        for (int x = 0; x < w; x++) {
            uint8_t r = src_row[x*4+0];
            uint8_t g = src_row[x*4+1];
            uint8_t b = src_row[x*4+2];
            uint8_t a = src_row[x*4+3];
            if (a == 0xFF) {
                dst_row[x] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
            } else {
                uint32_t rm = (r*a+127)/255, gm = (g*a+127)/255, bm = (b*a+127)/255;
                dst_row[x] = ((uint32_t)a<<24) | (rm<<16) | (gm<<8) | bm;
            }
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

/**
 * Flatten annotations onto the current image and, when beautify is enabled,
 * compose the padded/background/shadow result.  Returns a newly allocated
 * image the caller must free, or NULL if there is nothing to flatten (caller
 * should fall back to mw->current_image).
 */
static SnapxImage *flatten_for_export(MainWindow *mw)
{
    if (!mw->current_image) return NULL;
    SnapxImage *flat = snapx_canvas_flatten(mw->canvas, mw->current_image);
    SnapxImage *base = flat ? flat : mw->current_image;

    if (mw->config && mw->config->beautify.enabled) {
        SnapxImage *pretty = snapx_beautify_apply(base, &mw->config->beautify);
        if (pretty) {
            if (flat) snapx_image_free(flat);
            return pretty;
        }
    }
    return flat;
}

/* ─── Scaled surface cache ───────────────────────────────────────────────── */

/**
 * Compute the viewport parameters: scale factor and pixel offset within the
 * canvas so the image is centred (fit mode) or positioned by pan.
 */
static void compute_viewport(MainWindow *mw, int cw, int ch,
                               double *out_scale, double *out_ox, double *out_oy)
{
    if (!mw->current_image) { *out_scale = 1; *out_ox = *out_oy = 0; return; }
    int iw = mw->current_image->width;
    int ih = mw->current_image->height;

    double scale;
    if (mw->fit_mode || mw->zoom <= 0) {
        double sx = (double)cw / iw;
        double sy = (double)ch / ih;
        scale = (sx < sy) ? sx : sy;
        if (scale > 1.0) scale = 1.0;   /* never upscale in fit mode */
    } else {
        scale = mw->zoom;
    }
    *out_scale = scale;
    *out_ox = ((double)cw - iw * scale) / 2.0 - mw->pan_x * scale;
    *out_oy = ((double)ch - ih * scale) / 2.0 - mw->pan_y * scale;
}

/** Map drawing-area widget coords to image pixel coords (inverse of viewport). */
static void widget_to_image_coords(MainWindow *mw, double wx, double wy,
                                    double *ix, double *iy)
{
    if (!mw->current_image || !mw->drawing_area) {
        *ix = wx;
        *iy = wy;
        return;
    }
    int cw, ch;
#ifdef SNAPX_USE_GTK4
    cw = gtk_widget_get_width(mw->drawing_area);
    ch = gtk_widget_get_height(mw->drawing_area);
#else
    GtkAllocation a;
    gtk_widget_get_allocation(mw->drawing_area, &a);
    cw = a.width;
    ch = a.height;
#endif
    if (cw <= 0 || ch <= 0) {
        *ix = wx;
        *iy = wy;
        return;
    }
    double scale, ox, oy;
    compute_viewport(mw, cw, ch, &scale, &ox, &oy);
    if (scale <= 0.0) {
        *ix = wx;
        *iy = wy;
        return;
    }
    *ix = (wx - ox) / scale;
    *iy = (wy - oy) / scale;
}

void snapx_main_widget_to_image(double wx, double wy, double *ix, double *iy)
{
    widget_to_image_coords(&g_win, wx, wy, ix, iy);
}

int snapx_main_sample_color(double ix, double iy, GdkRGBA *out)
{
    MainWindow *mw = &g_win;
    if (!out || !mw->current_image || !mw->current_image->data) return 0;
    int x = (int)ix, y = (int)iy;
    if (x < 0 || y < 0 ||
        x >= mw->current_image->width || y >= mw->current_image->height)
        return 0;
    const uint8_t *p = mw->current_image->data
                     + (size_t)y * mw->current_image->stride + (size_t)x * 4;
    out->red   = p[0] / 255.0;
    out->green = p[1] / 255.0;
    out->blue  = p[2] / 255.0;
    out->alpha = 1.0;
    return 1;
}

/** Cached 24×24 checkerboard tile (built once, tiled in rebuild_scaled). */
static cairo_surface_t *get_checker_tile(void)
{
    static cairo_surface_t *tile = NULL;
    if (tile) return tile;

    const int cs = 12;
    tile = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cs * 2, cs * 2);
    if (cairo_surface_status(tile) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(tile);
        tile = NULL;
        return NULL;
    }
    cairo_t *tcr = cairo_create(tile);
    for (int ty = 0; ty < 2; ty++) {
        for (int tx = 0; tx < 2; tx++) {
            int which = (tx + ty) & 1;
            cairo_set_source_rgb(tcr,
                which ? 0.13 : 0.10,
                which ? 0.13 : 0.10,
                which ? 0.14 : 0.11);
            cairo_rectangle(tcr, tx * cs, ty * cs, cs, cs);
            cairo_fill(tcr);
        }
    }
    cairo_destroy(tcr);
    cairo_surface_mark_dirty(tile);
    return tile;
}

static void paint_checkerboard_bg(cairo_t *cr, int cw, int ch)
{
    cairo_set_source_rgb(cr, 0.098, 0.098, 0.110);
    cairo_paint(cr);

    cairo_surface_t *tile = get_checker_tile();
    if (!tile) return;

    cairo_pattern_t *pat = cairo_pattern_create_for_surface(tile);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
    (void)cw;
    (void)ch;
}

/**
 * Rebuild the scaled_surface (preview at current zoom, sized to the canvas).
 * This avoids repeating the Cairo scale + paint on every frame.
 */
static void rebuild_scaled(MainWindow *mw)
{
    if (!mw->display_surface) return;

    int cw, ch;
#ifdef SNAPX_USE_GTK4
    cw = gtk_widget_get_width(mw->drawing_area);
    ch = gtk_widget_get_height(mw->drawing_area);
#else
    GtkAllocation a; gtk_widget_get_allocation(mw->drawing_area, &a);
    cw = a.width; ch = a.height;
#endif
    if (cw <= 0 || ch <= 0) return;

    double scale, ox, oy;
    compute_viewport(mw, cw, ch, &scale, &ox, &oy);

    if (mw->scaled_surface) cairo_surface_destroy(mw->scaled_surface);
    mw->scaled_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
    if (cairo_surface_status(mw->scaled_surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(mw->scaled_surface);
        mw->scaled_surface = NULL; return;
    }

    cairo_t *cr = cairo_create(mw->scaled_surface);
    paint_checkerboard_bg(cr, cw, ch);

    cairo_translate(cr, ox, oy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, mw->display_surface, 0, 0);
    /* Use bilinear filtering when downscaling, nearest when zoomed in >1× */
    cairo_pattern_set_filter(cairo_get_source(cr),
                              scale < 1.0 ? CAIRO_FILTER_BILINEAR
                                          : CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_destroy(cr);

    mw->scaled_for_cw   = cw;
    mw->scaled_for_ch   = ch;
    mw->scaled_for_zoom = mw->zoom;
}

/**
 * Rebuild the annotation surface (same size as drawing area).
 * Called only when annot_dirty is TRUE.
 */
static void rebuild_annot(MainWindow *mw)
{
    if (!mw->canvas) { mw->annot_surface_valid = FALSE; return; }

    int cw, ch;
#ifdef SNAPX_USE_GTK4
    cw = gtk_widget_get_width(mw->drawing_area);
    ch = gtk_widget_get_height(mw->drawing_area);
#else
    GtkAllocation a; gtk_widget_get_allocation(mw->drawing_area, &a);
    cw = a.width; ch = a.height;
#endif
    if (cw <= 0 || ch <= 0) return;

    double scale, ox, oy;
    compute_viewport(mw, cw, ch, &scale, &ox, &oy);

    if (mw->annot_surface) cairo_surface_destroy(mw->annot_surface);
    mw->annot_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
    cairo_t *cr = cairo_create(mw->annot_surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, scale, scale);
    /* Only render committed strokes — pending is composited live in redraw() */
    snapx_canvas_render_committed(mw->canvas, cr);
    cairo_destroy(cr);
    mw->annot_surface_valid = TRUE;
    snapx_toolbar_annot_clear_dirty();
}

static void invalidate_scaled(MainWindow *mw)
{
    if (mw->scaled_surface) {
        cairo_surface_destroy(mw->scaled_surface);
        mw->scaled_surface = NULL;
    }
    mw->scaled_for_cw = mw->scaled_for_ch = 0;
    mw->annot_surface_valid = FALSE;
}

/* ─── Paint callback ──────────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4
static void on_draw(GtkDrawingArea *da, cairo_t *cr,
                    int width, int height, gpointer data)
{
    (void)da; (void)width; (void)height;
    redraw(NULL, cr, data);
}
#else
static gboolean on_draw_gtk3(GtkWidget *w, cairo_t *cr, gpointer data)
{
    redraw(w, cr, data);
    return FALSE;
}
#endif

static void redraw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    MainWindow *mw = (MainWindow *)user_data;
    (void)widget;

    int cw, ch;
#ifdef SNAPX_USE_GTK4
    cw = gtk_widget_get_width(mw->drawing_area);
    ch = gtk_widget_get_height(mw->drawing_area);
#else
    GtkAllocation a; gtk_widget_get_allocation(mw->drawing_area, &a);
    cw = a.width; ch = a.height;
#endif

    if (!mw->current_image) {
        /* Welcome screen */
        cairo_set_source_rgb(cr, 0.098, 0.098, 0.110);
        cairo_paint(cr);
        /* Subtle grid dots */
        cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
        for (int y = 24; y < ch; y += 32)
            for (int x = 24; x < cw; x += 32) {
                cairo_arc(cr, x, y, 1.5, 0, 6.2832);
                cairo_fill(cr);
            }
        /* Centre message */
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 18.0);
        cairo_text_extents_t te;
        const char *msg = "Take a screenshot to get started";
        cairo_text_extents(cr, msg, &te);
        cairo_move_to(cr, (cw - te.width) / 2.0 - te.x_bearing,
                          (ch + te.height) / 2.0);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
        cairo_show_text(cr, msg);

        /* Keyboard hint */
        cairo_set_font_size(cr, 12.0);
        const char *hint = "Screen  •  Region  •  Window  in the header bar";
        cairo_text_extents(cr, hint, &te);
        cairo_move_to(cr, (cw - te.width) / 2.0, ch/2.0 + 32);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.13);
        cairo_show_text(cr, hint);
        return;
    }

    /* Rebuild scaled surface if canvas size or zoom changed */
    if (!mw->scaled_surface ||
        mw->scaled_for_cw != cw || mw->scaled_for_ch != ch ||
        fabs(mw->scaled_for_zoom - mw->zoom) > 1e-6) {
        rebuild_scaled(mw);
    }

    /* Blit the scaled preview */
    if (mw->scaled_surface) {
        cairo_set_source_surface(cr, mw->scaled_surface, 0, 0);
        cairo_paint(cr);
    }

    /* ── Annotation rendering ────────────────────────────────────────────── */
    if (!mw->canvas) return;

    if (snapx_toolbar_in_stroke()) {
        /* Mid-stroke: blit committed cache + draw pending live (no full rebuild). */
        if (!mw->annot_surface_valid)
            rebuild_annot(mw);

        if (mw->annot_surface) {
            cairo_set_source_surface(cr, mw->annot_surface, 0, 0);
            cairo_paint(cr);
        }

        /* Overlay the in-progress stroke directly */
        double scale, ox, oy;
        compute_viewport(mw, cw, ch, &scale, &ox, &oy);
        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, scale, scale);
        snapx_canvas_render_pending(mw->canvas, cr);
        cairo_restore(cr);
    } else {
        /* Idle: rebuild committed cache only when dirty (stroke was just committed
         * or undo/redo was triggered).  Then blit — O(1) per frame. */
        if (snapx_toolbar_annot_dirty() || !mw->annot_surface_valid)
            rebuild_annot(mw);

        if (mw->annot_surface && mw->annot_surface_valid) {
            cairo_set_source_surface(cr, mw->annot_surface, 0, 0);
            cairo_paint(cr);
        }
    }

    if (mw->crop_active && (mw->crop_dragging ||
        fabs(mw->crop_x2 - mw->crop_x1) > 1 ||
        fabs(mw->crop_y2 - mw->crop_y1) > 1)) {
        double scale, ox, oy;
        compute_viewport(mw, cw, ch, &scale, &ox, &oy);
        double x0 = mw->crop_x1 < mw->crop_x2 ? mw->crop_x1 : mw->crop_x2;
        double y0 = mw->crop_y1 < mw->crop_y2 ? mw->crop_y1 : mw->crop_y2;
        double rw = fabs(mw->crop_x2 - mw->crop_x1);
        double rh = fabs(mw->crop_y2 - mw->crop_y1);
        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, scale, scale);
        cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 0.9);
        cairo_set_line_width(cr, 2.0 / scale);
        cairo_rectangle(cr, x0, y0, rw, rh);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

/* ─── Portal parent window helper ─────────────────────────────────────────── */

#ifdef GDK_WINDOWING_WAYLAND
static void portal_wayland_exported(GdkToplevel *toplevel,
                                     const char *handle,
                                     gpointer user_data)
{
    (void)toplevel;
    MainWindow *mw = (MainWindow *)user_data;
    if (!handle || !handle[0]) return;

    snprintf(mw->portal_parent, sizeof(mw->portal_parent), "wayland:%s", handle);
    snprintf(mw->wayland_exported_raw, sizeof(mw->wayland_exported_raw),
             "%s", handle);

    if (mw->backend && mw->backend->type == SNAPX_BACKEND_WAYLAND)
        snapx_capture_wayland_set_parent_window(mw->backend, mw->portal_parent);

    if (mw->portal_export_loop)
        g_main_loop_quit(mw->portal_export_loop);
}
#endif

/**
 * Ensure the Wayland backend has a valid XDG parent_window handle.
 * @p blocking  If TRUE, wait (main-loop) until Wayland export completes.
 */
static void ensure_portal_parent(MainWindow *mw, gboolean blocking)
{
    if (!mw->backend || mw->backend->type != SNAPX_BACKEND_WAYLAND) return;

    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(mw->win));
    if (!surf) {
        snapx_capture_wayland_set_parent_window(mw->backend, mw->portal_parent);
        return;
    }

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_SURFACE(surf)) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        guint32 xid = (guint32)gdk_x11_surface_get_xid(surf);
        G_GNUC_END_IGNORE_DEPRECATIONS
        snprintf(mw->portal_parent, sizeof(mw->portal_parent), "x11:0x%x", xid);
        snapx_capture_wayland_set_parent_window(mw->backend, mw->portal_parent);
        return;
    }
#endif

#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_SURFACE(surf) && GDK_IS_TOPLEVEL(surf)) {
        if (mw->portal_parent[0] != '\0') {
            snapx_capture_wayland_set_parent_window(mw->backend, mw->portal_parent);
            return;
        }
        if (!gtk_widget_get_mapped(GTK_WIDGET(mw->win)))
            return;

        GdkToplevel *top = GDK_TOPLEVEL(surf);
        if (!gdk_wayland_toplevel_export_handle(top, portal_wayland_exported,
                                                mw, NULL))
            return;

        if (blocking && mw->portal_parent[0] == '\0') {
            mw->portal_export_loop = g_main_loop_new(NULL, FALSE);
            g_main_loop_run(mw->portal_export_loop);
            g_main_loop_unref(mw->portal_export_loop);
            mw->portal_export_loop = NULL;
        }
        return;
    }
#endif

    snapx_capture_wayland_set_parent_window(mw->backend, mw->portal_parent);
}

static void update_portal_parent(MainWindow *mw)
{
    ensure_portal_parent(mw, FALSE);
}

static void on_global_hotkey(SnapxHotkeyAction action, gpointer user_data);

static void snapx_main_register_hotkeys(MainWindow *mw)
{
    snapx_hotkey_cleanup();
    snapx_hotkey_set_callback(on_global_hotkey, mw);
#ifdef SNAPX_USE_GTK4
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(mw->win));
    if (app) {
        const char *app_id = g_application_get_application_id(G_APPLICATION(app));
        if (app_id && app_id[0])
            snapx_hotkey_set_application_id(app_id);
    }
#endif
    snapx_hotkey_set_parent_window(mw->portal_parent);
    snapx_hotkey_init(mw->config);
}

static gboolean portal_parent_on_mapped(gpointer data)
{
    MainWindow *mw = (MainWindow *)data;
    if (!gtk_widget_get_mapped(GTK_WIDGET(mw->win)))
        return G_SOURCE_CONTINUE;
    ensure_portal_parent(mw, TRUE);
    snapx_main_register_hotkeys(mw);
    return G_SOURCE_REMOVE;
}

static void portal_parent_drop_export(MainWindow *mw)
{
#ifdef GDK_WINDOWING_WAYLAND
    if (!mw->wayland_exported_raw[0]) return;
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(mw->win));
    if (surf && GDK_IS_WAYLAND_SURFACE(surf) && GDK_IS_TOPLEVEL(surf)) {
#if GTK_CHECK_VERSION(4, 12, 0)
        gdk_wayland_toplevel_drop_exported_handle(GDK_TOPLEVEL(surf),
                                                 mw->wayland_exported_raw);
#else
        gdk_wayland_toplevel_unexport_handle(GDK_TOPLEVEL(surf));
#endif
    }
    mw->wayland_exported_raw[0] = '\0';
    mw->portal_parent[0]       = '\0';
#endif
}

static void on_win_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    portal_parent_drop_export((MainWindow *)data);
}

/* ─── Status bar ──────────────────────────────────────────────────────────── */

static guint g_status_timeout = 0;

static gboolean clear_status(gpointer data)
{
    GtkWidget *lbl = (GtkWidget *)data;
    gtk_label_set_text(GTK_LABEL(lbl), "");
    g_status_timeout = 0;
    return G_SOURCE_REMOVE;
}

static void set_status(MainWindow *mw, const char *msg)
{
    if (!mw->statusbar) return;
    gtk_label_set_text(GTK_LABEL(mw->statusbar), msg);
    if (g_status_timeout) { g_source_remove(g_status_timeout); g_status_timeout = 0; }
    if (msg && msg[0])
        g_status_timeout = g_timeout_add_seconds(5, clear_status, mw->statusbar);
}

/* ─── Zoom helpers ────────────────────────────────────────────────────────── */

static void update_zoom_label(MainWindow *mw)
{
    if (!mw->zoom_label) return;
    char buf[16];
    if (mw->fit_mode)
        snprintf(buf, sizeof(buf), "Fit");
    else
        snprintf(buf, sizeof(buf), "%.0f%%", mw->zoom * 100.0);
    gtk_label_set_text(GTK_LABEL(mw->zoom_label), buf);
}

static void set_zoom(MainWindow *mw, double new_zoom)
{
    if (new_zoom < ZOOM_MIN) new_zoom = ZOOM_MIN;
    if (new_zoom > ZOOM_MAX) new_zoom = ZOOM_MAX;
    mw->fit_mode = FALSE;
    mw->zoom     = new_zoom;
    invalidate_scaled(mw);
    update_zoom_label(mw);
    if (mw->drawing_area) gtk_widget_queue_draw(mw->drawing_area);
}

static void set_fit(MainWindow *mw)
{
    mw->fit_mode = TRUE;
    mw->pan_x = mw->pan_y = 0;
    invalidate_scaled(mw);
    update_zoom_label(mw);
    if (mw->drawing_area) gtk_widget_queue_draw(mw->drawing_area);
}

/* ─── Coalesced redraw ────────────────────────────────────────────────────── */

#ifndef SNAPX_USE_GTK4
static gboolean draw_idle_redraw(gpointer data)
{
    MainWindow *mw = (MainWindow *)data;
    mw->draw_pending = FALSE;
    mw->draw_tick_id = 0;
    if (mw->drawing_area)
        gtk_widget_queue_draw(mw->drawing_area);
    return G_SOURCE_REMOVE;
}
#endif

#ifdef SNAPX_USE_GTK4
static gboolean on_draw_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    MainWindow *mw = (MainWindow *)data;
    if (mw->draw_pending) {
        mw->draw_pending = FALSE;
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}
#endif

void snapx_main_schedule_redraw(void)
{
    MainWindow *mw = &g_win;
    if (!mw->drawing_area) return;
    if (mw->draw_pending) return;
    mw->draw_pending = TRUE;

#ifdef SNAPX_USE_GTK4
    GdkFrameClock *fc = gtk_widget_get_frame_clock(mw->drawing_area);
    if (fc)
        gdk_frame_clock_request_phase(fc, GDK_FRAME_CLOCK_PHASE_PAINT);
#else
    if (!mw->draw_tick_id)
        mw->draw_tick_id = g_idle_add(draw_idle_redraw, mw);
#endif
}

/* ─── Window geometry save/restore ───────────────────────────────────────── */

static void snapx_save_window_state(MainWindow *mw)
{
    GtkWindow *win = GTK_WINDOW(mw->win);
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(mw->win));

#ifdef SNAPX_USE_GTK4
    if (gtk_widget_get_mapped(GTK_WIDGET(mw->win))) {
        mw->saved_w = gtk_widget_get_width(GTK_WIDGET(mw->win));
        mw->saved_h = gtk_widget_get_height(GTK_WIDGET(mw->win));
    } else {
        gtk_window_get_default_size(win, &mw->saved_w, &mw->saved_h);
    }
    mw->saved_x = 0;
    mw->saved_y = 0;
    if (surf && GDK_IS_TOPLEVEL(surf))
        mw->saved_toplevel_state = gdk_toplevel_get_state(GDK_TOPLEVEL(surf));
    else
        mw->saved_toplevel_state = 0;
#else
    if (!surf || !GDK_IS_TOPLEVEL(surf)) {
        mw->saved_window_state = FALSE;
        return;
    }
    GdkToplevel *top = GDK_TOPLEVEL(surf);
    GdkRectangle bounds = {0};
    gdk_toplevel_get_bounds(top, &bounds);
    mw->saved_x = bounds.x;
    mw->saved_y = bounds.y;
    mw->saved_w = bounds.width  > 0 ? bounds.width  : 900;
    mw->saved_h = bounds.height > 0 ? bounds.height : 640;
    mw->saved_toplevel_state = gdk_toplevel_get_state(top);
#endif

    if (mw->saved_w <= 0) mw->saved_w = 900;
    if (mw->saved_h <= 0) mw->saved_h = 640;
    mw->saved_window_state = TRUE;
}

static void snapx_restore_window_state(MainWindow *mw)
{
    update_portal_parent(mw);
    gtk_widget_set_visible(GTK_WIDGET(mw->win), TRUE);

    if (!mw->saved_window_state) {
        gtk_window_present(GTK_WINDOW(mw->win));
        return;
    }

    GtkWindow *win = GTK_WINDOW(mw->win);

    if (mw->saved_toplevel_state & GDK_TOPLEVEL_STATE_FULLSCREEN) {
        gtk_window_fullscreen(win);
    } else if (mw->saved_toplevel_state & GDK_TOPLEVEL_STATE_MAXIMIZED) {
        gtk_window_unfullscreen(win);
        gtk_window_maximize(win);
    } else {
        gtk_window_unfullscreen(win);
        gtk_window_unmaximize(win);
#ifdef SNAPX_USE_GTK4
        gtk_window_set_default_size(win, mw->saved_w, mw->saved_h);
#else
        gtk_window_resize(win, mw->saved_w, mw->saved_h);
#endif
    }

    gtk_window_present(win);
    mw->saved_window_state = FALSE;
}

/* ─── Capture ─────────────────────────────────────────────────────────────── */

/** Hide main window and settle before portal/X11 capture (no snapx UI in shot). */
static void snapx_hide_for_capture(MainWindow *mw)
{
    /* Refresh portal parent while the window is still mapped (Wayland export). */
    if (gtk_widget_get_visible(GTK_WIDGET(mw->win)))
        ensure_portal_parent(mw, TRUE);

    if (gtk_widget_get_visible(GTK_WIDGET(mw->win)))
        snapx_save_window_state(mw);
    gtk_widget_set_visible(GTK_WIDGET(mw->win), FALSE);

    /* Push the unmap request to the server immediately. */
    GdkDisplay *dpy = gtk_widget_get_display(GTK_WIDGET(mw->win));
    if (dpy)
        gdk_display_flush(dpy);

    /*
     * Wait until the snapx window is actually gone before capturing, but adapt
     * to how fast the compositor is instead of always blocking for a fixed
     * delay.  We pump the main loop until the toplevel is unmapped (bounded so
     * a slow/blocking compositor can never hang the capture), then add a margin
     * for the revealed area to be presented.
     */
    const int max_wait_us = 400 * 1000;   /* hard ceiling */
    const int step_us     =   5 * 1000;
    int waited = 0;
    while (waited < max_wait_us) {
        while (g_main_context_pending(NULL))
            g_main_context_iteration(NULL, FALSE);
        if (!gtk_widget_get_mapped(GTK_WIDGET(mw->win)))
            break;
        g_usleep(step_us);
        waited += step_us;
    }

    /*
     * Settle margin so the compositor presents a frame *without* snapx before
     * we grab the screen.  On Wayland this must outlast the compositor's
     * window-close animation (GNOME/Mutter fades a closing toplevel for
     * ~200 ms); capturing mid-animation is exactly what put the snapx window
     * into region/monitor freezes and forced users to drag snapx to another
     * display first.  X11 unmaps instantly, so it keeps the short margin.
     */
    gboolean wayland = (mw->backend && mw->backend->type == SNAPX_BACKEND_WAYLAND);
    int settle_us = wayland ? (320 * 1000) : (90 * 1000);
    g_usleep(settle_us);
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
    if (dpy)
        gdk_display_sync(dpy);
}

static gboolean clipboard_idle_cb(gpointer data)
{
    MainWindow *mw = data;
    if (mw->current_image)
        snapx_clipboard_copy_image(mw->current_image);
    return G_SOURCE_REMOVE;
}

static void after_capture(MainWindow *mw, SnapxImage *img, const char *ok_msg)
{
    snapx_restore_window_state(mw);

    if (!img) { set_status(mw, "Capture failed — check backend with snapx --info."); return; }

    snapx_image_free(mw->current_image);
    mw->current_image = img;

    if (mw->display_surface) { cairo_surface_destroy(mw->display_surface); mw->display_surface = NULL; }
    mw->display_surface = make_display_surface(img);
    invalidate_scaled(mw);

    if (mw->canvas) snapx_canvas_free(mw->canvas);
    mw->canvas = snapx_canvas_new(img->width, img->height);
    snapx_toolbar_set_canvas(mw->canvas, mw->drawing_area);
    snapx_main_bind_canvas(mw);

    /* Reset to fit view */
    set_fit(mw);

    /* Update window title with resolution */
    char title[128];
    snprintf(title, sizeof(title), "snapx — %d × %d", img->width, img->height);
    gtk_window_set_title(GTK_WINDOW(mw->win), title);

    set_status(mw, ok_msg);
    snapx_capture_wayland_save_token(mw->backend);
    if (mw->config->auto_clipboard)
        g_idle_add(clipboard_idle_cb, mw);
    if (mw->config->play_sound) {
        GdkDisplay *dpy = gtk_widget_get_display(GTK_WIDGET(mw->win));
        if (dpy)
            gdk_display_beep(dpy);
    }
    if (mw->config->upload_auto && snapx_upload_available()
        && mw->config->upload_service != SNAPX_UPLOAD_NONE) {
        SnapxImage *flat = flatten_for_export(mw);
        do_upload_image(mw, flat ? flat : mw->current_image);
        snapx_image_free(flat);
    }
}

static void snapx_main_sync_wayland_prefer(MainWindow *mw)
{
    if (!mw->backend || mw->backend->type != SNAPX_BACKEND_WAYLAND || !mw->config)
        return;
    snapx_capture_wayland_set_capture_prefer(mw->backend,
                                            mw->config->wayland_capture_prefer);
}

typedef struct {
    MainWindow       *mw;
    SnapxCaptureMode  mode;
    int               delay;
} FullCaptureCtx;

static void do_capture_done(SnapxImage *img, gpointer data)
{
    FullCaptureCtx *ctx = data;
    after_capture(ctx->mw, img, "Screenshot captured. Annotate or save.");
    g_free(ctx);
}

static void do_capture(MainWindow *mw, SnapxCaptureMode mode, int delay)
{
    snapx_hide_for_capture(mw);
    set_status(mw, "Capturing desktop…");

    FullCaptureCtx *ctx = g_new(FullCaptureCtx, 1);
    ctx->mw    = mw;
    ctx->mode  = mode;
    ctx->delay = delay;

    SnapxCaptureRequest req = {0};
    req.mode           = mode;
    req.delay_sec      = delay;
    req.monitor_index  = 0;
    req.include_cursor = mw->config->show_cursor;

    snapx_capture_async(mw->backend, &req, do_capture_done, ctx);
}

/* ─── Button callbacks ─────────────────────────────────────────────────────── */

static void on_capture_fullscreen(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    do_capture(mw, SNAPX_CAPTURE_FULLSCREEN, mw->config->default_delay);
}

typedef struct {
    MainWindow *mw;
} RegionCaptureCtx;

static void region_capture_desktop_done(SnapxImage *full, gpointer data)
{
    RegionCaptureCtx *ctx = data;
    MainWindow *mw = ctx->mw;
    g_free(ctx);

    if (!full) {
        snapx_restore_window_state(mw);
        set_status(mw, "Capture failed — check backend with snapx --info.");
        return;
    }

    SnapxMonitorInfo monitors[8] = {0};
    int n_mon = snapx_get_monitors(mw->backend, monitors, 8);

    SnapxRegion region = {0};
    int ok = snapx_overlay_select_region(NULL, full, monitors, n_mon, &mw->config->shortcuts, mw->config, &region);
    if (!ok) {
        snapx_image_free(full);
        snapx_restore_window_state(mw);
        return;
    }

    SnapxImage *img = snapx_image_crop_desktop(full, region.x, region.y,
                                                region.width, region.height,
                                                monitors, n_mon);
    snapx_image_free(full);

    after_capture(mw, img,
                  "Region captured — click Save to write a file.");
}

static void on_capture_region(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;

    snapx_hide_for_capture(mw);
    if (mw->config && mw->config->wayland_capture_prefer)
        set_status(mw, "Capturing desktop… Approve screen sharing if prompted.");
    else
        set_status(mw, "Capturing desktop…");

    RegionCaptureCtx *ctx = g_new(RegionCaptureCtx, 1);
    ctx->mw = mw;

    SnapxCaptureRequest full_req = {0};
    full_req.mode           = SNAPX_CAPTURE_FULLSCREEN;
    full_req.delay_sec      = mw->config->default_delay;
    full_req.include_cursor = mw->config->show_cursor;
    snapx_capture_async(mw->backend, &full_req, region_capture_desktop_done, ctx);
}

typedef struct {
    MainWindow       *mw;
    SnapxMonitorInfo  monitors[8];
    int               n;
    int               idx;
} MonitorCaptureCtx;

static void monitor_capture_done(SnapxImage *full_img, gpointer data)
{
    MonitorCaptureCtx *ctx = data;
    MainWindow *mw = ctx->mw;
    SnapxImage *img = NULL;
    if (full_img) {
        img = snapx_image_crop_desktop(full_img,
            ctx->monitors[ctx->idx].x, ctx->monitors[ctx->idx].y,
            ctx->monitors[ctx->idx].width, ctx->monitors[ctx->idx].height,
            ctx->monitors, ctx->n);
        snapx_image_free(full_img);
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Monitor %d captured (%d × %d).",
             ctx->idx + 1,
             ctx->monitors[ctx->idx].width,
             ctx->monitors[ctx->idx].height);
    after_capture(mw, img, msg);
    g_free(ctx);
}

static void on_capture_monitor_btn(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;

    /* Get monitor list from the backend */
    SnapxMonitorInfo monitors[8] = {0};
    int n = snapx_get_monitors(mw->backend, monitors, 8);
    if (n <= 0) {
        set_status(mw, "No monitors detected — try Screen capture instead.");
        return;
    }

    snapx_hide_for_capture(mw);

    /* Show monitor picker overlay */
    int idx = snapx_overlay_select_monitor(GTK_WINDOW(mw->win), monitors, n);

    if (idx < 0) {
        snapx_restore_window_state(mw);
        return;
    }

    /*
     * Wayland backends only support full-desktop capture; they ignore req.mode.
     * Capture the full desktop and crop to the selected monitor's rectangle.
     * On X11 the backend handles SNAPX_CAPTURE_MONITOR natively, but doing
     * full+crop also works, so we use the same path everywhere for simplicity.
     */
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
    g_usleep(80 * 1000);  /* compositor settle */

    MonitorCaptureCtx *ctx = g_new(MonitorCaptureCtx, 1);
    ctx->mw  = mw;
    ctx->n   = n;
    ctx->idx = idx;
    memcpy(ctx->monitors, monitors, sizeof(monitors));

    set_status(mw, "Capturing desktop…");

    SnapxCaptureRequest full_req = {0};
    full_req.mode           = SNAPX_CAPTURE_FULLSCREEN;
    full_req.delay_sec      = mw->config->default_delay;
    full_req.include_cursor = mw->config->show_cursor;
    snapx_capture_async(mw->backend, &full_req, monitor_capture_done, ctx);
}

static void on_capture_window(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    do_capture(mw, SNAPX_CAPTURE_ACTIVE_WINDOW, mw->config->default_delay);
}

static void trigger_capture_mode(MainWindow *mw, SnapxCaptureMode mode)
{
    switch (mode) {
    case SNAPX_CAPTURE_FULLSCREEN:
        on_capture_fullscreen(NULL, mw);
        break;
    case SNAPX_CAPTURE_MONITOR:
        on_capture_monitor_btn(NULL, mw);
        break;
    case SNAPX_CAPTURE_REGION:
        on_capture_region(NULL, mw);
        break;
    case SNAPX_CAPTURE_WINDOW:
    case SNAPX_CAPTURE_ACTIVE_WINDOW:
        on_capture_window(NULL, mw);
        break;
    }
}

typedef struct {
    SnapxHotkeyAction action;
    MainWindow       *mw;
} HotkeyIdleCtx;

static gboolean hotkey_idle_cb(gpointer data)
{
    HotkeyIdleCtx *ctx = data;
    switch (ctx->action) {
    case SNAPX_HOTKEY_DEFAULT_MODE:
        trigger_capture_mode(ctx->mw, ctx->mw->config->default_mode);
        break;
    case SNAPX_HOTKEY_CAPTURE_FULLSCREEN:
        trigger_capture_mode(ctx->mw, SNAPX_CAPTURE_FULLSCREEN);
        break;
    case SNAPX_HOTKEY_CAPTURE_MONITOR:
        trigger_capture_mode(ctx->mw, SNAPX_CAPTURE_MONITOR);
        break;
    case SNAPX_HOTKEY_CAPTURE_REGION:
        trigger_capture_mode(ctx->mw, SNAPX_CAPTURE_REGION);
        break;
    case SNAPX_HOTKEY_CAPTURE_WINDOW:
        trigger_capture_mode(ctx->mw, SNAPX_CAPTURE_ACTIVE_WINDOW);
        break;
    }
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

static void on_global_hotkey(SnapxHotkeyAction action, gpointer user_data)
{
    HotkeyIdleCtx *ctx = g_new(HotkeyIdleCtx, 1);
    ctx->action = action;
    ctx->mw     = user_data;
    g_idle_add(hotkey_idle_cb, ctx);
}

static void shortcut_tooltip(GtkWidget *btn, const char *action, const char *spec)
{
    char tip[160];
    if (spec && spec[0])
        snprintf(tip, sizeof(tip), "%s  (%s)", action, spec);
    else
        snprintf(tip, sizeof(tip), "%s", action);
    gtk_widget_set_tooltip_text(btn, tip);
}

static void snapx_main_refresh_shortcut_tooltips(MainWindow *mw)
{
    if (!mw || !mw->config) return;
    const SnapxShortcuts *sc = &mw->config->shortcuts;

    shortcut_tooltip(mw->btn_fullscreen,
                     "Capture full screen — all monitors", sc->capture_fullscreen);
    shortcut_tooltip(mw->btn_region, "Draw a selection region", sc->capture_region);
    shortcut_tooltip(mw->btn_monitor, "Pick a specific monitor to capture",
                     sc->capture_monitor);
    shortcut_tooltip(mw->btn_window, "Capture the active window", sc->capture_window);
    shortcut_tooltip(mw->btn_fit, "Reset zoom to fit window", sc->fit);

    char zoom_tip[256];
    snprintf(zoom_tip, sizeof(zoom_tip),
             "Current zoom.  %s / %s or Ctrl+scroll.",
             sc->zoom_in[0] ? sc->zoom_in : "Ctrl+Plus",
             sc->zoom_out[0] ? sc->zoom_out : "Ctrl+Minus");
    gtk_widget_set_tooltip_text(mw->zoom_label, zoom_tip);
}

static void on_save(GtkButton *b, gpointer d);
static void on_copy_clipboard(GtkButton *b, gpointer d);
static void on_upload(GtkButton *b, gpointer d);

static void upload_done_cb(int success, const char *url, const char *error, gpointer data)
{
    MainWindow *mw = data;
    mw->upload_busy = 0;
    snapx_main_update_workflow_ui(mw);
    if (success && url) {
        if (mw->config->upload_copy_url)
            snapx_clipboard_copy_text(url);
        char msg[384];
        snprintf(msg, sizeof(msg), "Uploaded — %s", url);
        set_status(mw, msg);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Upload failed: %s",
                 error ? error : "unknown error");
        set_status(mw, msg);
    }
}

static void ocr_done_cb(int success, const char *text, gpointer data)
{
    MainWindow *mw = data;
    snapx_main_update_workflow_ui(mw);
    if (success && text && text[0]) {
        snapx_clipboard_copy_text(text);
        set_status(mw, "Text copied from image.");
    } else {
        set_status(mw, text && text[0] ? text : "OCR failed.");
    }
}

static void snapx_main_update_workflow_ui(MainWindow *mw)
{
    if (!mw || !mw->config) return;
    char buf[256];

    snapx_upload_status_message(mw->config, buf, sizeof(buf));
    if (mw->lbl_upload_status) {
        gtk_label_set_text(GTK_LABEL(mw->lbl_upload_status), buf);
        gtk_widget_remove_css_class(mw->lbl_upload_status, "warn");
        gtk_widget_remove_css_class(mw->lbl_upload_status, "ok");
        if (snapx_upload_get_status(mw->config) == SNAPX_UPLOAD_STATUS_AVAILABLE)
            gtk_widget_add_css_class(mw->lbl_upload_status, "ok");
        else
            gtk_widget_add_css_class(mw->lbl_upload_status, "warn");
    }

    snapx_ocr_status_message(mw->config, buf, sizeof(buf));
    if (mw->lbl_ocr_status) {
        gtk_label_set_text(GTK_LABEL(mw->lbl_ocr_status), buf);
        gtk_widget_remove_css_class(mw->lbl_ocr_status, "warn");
        gtk_widget_remove_css_class(mw->lbl_ocr_status, "ok");
        if (snapx_ocr_get_status(mw->config) == SNAPX_OCR_STATUS_AVAILABLE)
            gtk_widget_add_css_class(mw->lbl_ocr_status, "ok");
        else
            gtk_widget_add_css_class(mw->lbl_ocr_status, "warn");
    }

    if (mw->btn_upload) {
        if (snapx_upload_busy()) {
            gtk_button_set_label(GTK_BUTTON(mw->btn_upload), "Stop");
            gtk_widget_set_sensitive(mw->btn_upload, TRUE);
            gtk_widget_set_tooltip_text(mw->btn_upload,
                                        "Cancel upload in progress");
        } else {
            gtk_button_set_label(GTK_BUTTON(mw->btn_upload), "Upload");
            SnapxUploadStatus st = snapx_upload_get_status(mw->config);
            gboolean upload_ok = st == SNAPX_UPLOAD_STATUS_AVAILABLE;
            gtk_widget_set_sensitive(mw->btn_upload,
                upload_ok || st == SNAPX_UPLOAD_STATUS_NOT_CONFIGURED);
            if (st == SNAPX_UPLOAD_STATUS_NOT_BUILT)
                gtk_widget_set_tooltip_text(mw->btn_upload,
                    "Upload requires libcurl — rebuild with libcurl-devel");
            else if (st == SNAPX_UPLOAD_STATUS_NOT_CONFIGURED)
                gtk_widget_set_tooltip_text(mw->btn_upload,
                    "Configure upload in Settings (click to open)");
            else
                gtk_widget_set_tooltip_text(mw->btn_upload,
                    "Upload and copy link  (Ctrl+U)");
        }
    }

    if (mw->btn_ocr) {
        if (snapx_ocr_busy()) {
            gtk_button_set_label(GTK_BUTTON(mw->btn_ocr), "Stop");
            gtk_widget_set_sensitive(mw->btn_ocr, TRUE);
            gtk_widget_set_tooltip_text(mw->btn_ocr,
                                        "Cancel OCR (result will be ignored)");
        } else {
            gtk_button_set_label(GTK_BUTTON(mw->btn_ocr), "Copy text");
            gboolean ok = snapx_ocr_get_status(mw->config) ==
                          SNAPX_OCR_STATUS_AVAILABLE;
            gtk_widget_set_sensitive(mw->btn_ocr, ok);
            gtk_widget_set_tooltip_text(mw->btn_ocr, ok
                ? "Copy text from image (OCR)  (Ctrl+Shift+T)"
                : "OCR requires Tesseract — rebuild with tesseract-devel");
        }
    }
}

static void do_upload_image(MainWindow *mw, SnapxImage *img)
{
    if (!img || !snapx_upload_available()) {
        set_status(mw, "Upload not available (install libcurl).");
        return;
    }
    if (mw->config->upload_service == SNAPX_UPLOAD_NONE) {
        set_status(mw, "Configure upload service in Settings.");
        return;
    }
    if (mw->upload_busy) return;

    SnapxOutputFormat fmt = mw->config->default_format;
    SnapxEncodedImage enc;
    if (snapx_image_encode(img, fmt, mw->config->jpeg_quality, &enc) != 0) {
        set_status(mw, "Failed to encode image for upload.");
        return;
    }
    mw->upload_busy = 1;
    snapx_main_update_workflow_ui(mw);
    snapx_upload_async(&enc, mw->config, upload_done_cb, mw);
}

static void on_upload(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;

    if (snapx_upload_busy()) {
        snapx_upload_cancel();
        mw->upload_busy = 0;
        snapx_main_update_workflow_ui(mw);
        set_status(mw, "Upload cancelled.");
        return;
    }

    SnapxUploadStatus st = snapx_upload_get_status(mw->config);
    if (st == SNAPX_UPLOAD_STATUS_NOT_CONFIGURED) {
        on_settings(NULL, mw);
        set_status(mw, "Configure upload service in Settings → Output.");
        return;
    }
    if (st == SNAPX_UPLOAD_STATUS_NOT_BUILT) {
        set_status(mw, "Upload not built in — install libcurl-devel and rebuild.");
        return;
    }
    if (!mw->current_image) { set_status(mw, "Nothing to upload."); return; }
    SnapxImage *flat = flatten_for_export(mw);
    do_upload_image(mw, flat ? flat : mw->current_image);
    snapx_image_free(flat);
}

static void on_ocr(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;

    if (snapx_ocr_busy()) {
        snapx_ocr_cancel();
        snapx_main_update_workflow_ui(mw);
        set_status(mw, "OCR cancelled.");
        return;
    }

    if (snapx_ocr_get_status(mw->config) != SNAPX_OCR_STATUS_AVAILABLE) {
        set_status(mw, "OCR not built in — install tesseract-devel and rebuild.");
        return;
    }
    if (!mw->current_image) { set_status(mw, "Nothing to OCR."); return; }

    SnapxImage *flat = snapx_canvas_flatten(mw->canvas, mw->current_image);
    SnapxImage *src = flat ? flat : mw->current_image;
    set_status(mw, "Running OCR…");
    snapx_main_update_workflow_ui(mw);
    snapx_ocr_async(src, mw->config->ocr_lang, ocr_done_cb, mw);
    snapx_image_free(flat);
}

static void on_pin(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    if (!mw->current_image) return;
    SnapxImage *flat = flatten_for_export(mw);
    snapx_pin_image(flat ? flat : mw->current_image);
    snapx_image_free(flat);
    set_status(mw, "Pinned to screen.");
}

static void on_beautify(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    if (!mw->current_image) { set_status(mw, "Capture an image first."); return; }
    SnapxImage *flat = snapx_canvas_flatten(mw->canvas, mw->current_image);
    snapx_beautify_dialog_show(GTK_WINDOW(mw->win), mw->config,
                               flat ? flat : mw->current_image);
    snapx_image_free(flat);
}

static void on_copy_clipboard(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    if (!mw->current_image) { set_status(mw, "Nothing to copy."); return; }
    SnapxImage *flat = flatten_for_export(mw);
    snapx_clipboard_copy_image(flat ? flat : mw->current_image);
    snapx_image_free(flat);
    set_status(mw, "Copied to clipboard.");
}

/** Open a directory in the system file manager. */
static void snapx_open_path_in_file_manager(MainWindow *mw, const char *path)
{
    if (!path || !path[0]) {
        set_status(mw, "No folder path configured.");
        return;
    }

    GFile *file = g_file_new_for_path(path);
    char *uri = g_file_get_uri(file);
    g_object_unref(file);
    if (!uri) {
        set_status(mw, "Could not open folder.");
        return;
    }

    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Could not open folder: %s",
                 err ? err->message : "unknown error");
        set_status(mw, msg);
        if (err) g_error_free(err);
    }
    g_free(uri);
}

static void on_open_folder(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    /* Reveal the folder of the last saved file when available, so users land
     * on the screenshot they just wrote rather than the generic directory. */
    if (mw->last_save_path[0]) {
        char *dir = g_path_get_dirname(mw->last_save_path);
        if (dir) {
            snapx_open_path_in_file_manager(mw, dir);
            g_free(dir);
            return;
        }
    }
    snapx_open_path_in_file_manager(mw, mw->config->save_dir);
}

static void on_save(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    if (!mw->current_image) { set_status(mw, "Nothing to save."); return; }

    SnapxOutputFormat fmt = SNAPX_FORMAT_PNG;
    if (mw->format_combo) {
#ifdef SNAPX_USE_GTK4
        fmt = (SnapxOutputFormat)gtk_drop_down_get_selected(GTK_DROP_DOWN(mw->format_combo));
#else
        fmt = (SnapxOutputFormat)gtk_combo_box_get_active(GTK_COMBO_BOX(mw->format_combo));
#endif
    }

    char path[512];
    snapx_config_build_path(mw->config, fmt, path, sizeof(path));

    SnapxImage *flat = flatten_for_export(mw);
    int quality = (int)(mw->quality_scale
                        ? gtk_range_get_value(GTK_RANGE(mw->quality_scale))
                        : mw->config->jpeg_quality);

    int ret = snapx_image_save(flat ? flat : mw->current_image, path, fmt, quality);
    snapx_image_free(flat);

    if (ret == 0) {
        snprintf(mw->last_save_path, sizeof(mw->last_save_path), "%s", path);
        if (mw->history_panel)
            snapx_history_panel_refresh(mw->history_panel, mw->config);
        char msg[600];
        snprintf(msg, sizeof(msg), "Saved: %s", path);
        set_status(mw, msg);
    } else {
        char msg[600];
        snprintf(msg, sizeof(msg),
                 "Save failed (%s): %s", path, strerror(errno));
        set_status(mw, msg);
    }
}

static void on_format_changed(GObject *obj, GParamSpec *pspec, gpointer data);

/** Sync action-bar widgets from config; clear stale save path if directory changed. */
static void snapx_main_apply_config(MainWindow *mw, const char *prev_save_dir)
{
    if (!mw->config) return;

    snapx_main_sync_wayland_prefer(mw);

    if (prev_save_dir && strcmp(prev_save_dir, mw->config->save_dir) != 0)
        mw->last_save_path[0] = '\0';

    if (mw->format_combo) {
#ifdef SNAPX_USE_GTK4
        gtk_drop_down_set_selected(GTK_DROP_DOWN(mw->format_combo),
                                   (guint)mw->config->default_format);
        on_format_changed(G_OBJECT(mw->format_combo), NULL, mw);
#else
        gtk_combo_box_set_active(GTK_COMBO_BOX(mw->format_combo),
                                 (int)mw->config->default_format);
        on_format_changed(G_OBJECT(mw->format_combo), NULL, mw);
#endif
    }
    if (mw->quality_scale)
        gtk_range_set_value(GTK_RANGE(mw->quality_scale),
                            (double)mw->config->jpeg_quality);
}

static void on_settings(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    char prev_save_dir[SNAPX_CONFIG_MAX_PATH];
    snprintf(prev_save_dir, sizeof(prev_save_dir), "%s", mw->config->save_dir);
    snapx_settings_dialog_show(GTK_WINDOW(mw->win), mw->config);
    ensure_portal_parent(mw, FALSE);
    snapx_main_register_hotkeys(mw);
    snapx_main_apply_config(mw, prev_save_dir);
    snapx_toolbar_apply_config(mw->config);
    snapx_main_refresh_shortcut_tooltips(mw);
    snapx_main_update_workflow_ui(mw);
    if (mw->history_panel)
        snapx_history_panel_refresh(mw->history_panel, mw->config);
}

static void on_fit(GtkButton *b, gpointer d) { (void)b; set_fit((MainWindow *)d); }

static void on_history_select(const char *path, gpointer userdata)
{
    MainWindow *mw = userdata;
    SnapxImage *img = snapx_image_load_file(path);
    if (!img) {
        set_status(mw, "Could not load image.");
        return;
    }
    snapx_window_main_set_image(img);
    snapx_history_panel_set_open(mw->history_panel, FALSE);
    if (mw->btn_recents)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mw->btn_recents), FALSE);
    set_status(mw, "Loaded from history.");
}

static void on_recents(GtkToggleButton *btn, gpointer userdata)
{
    MainWindow *mw = userdata;
    if (!mw->history_panel) return;
    gboolean open = gtk_toggle_button_get_active(btn);
    if (open)
        snapx_history_panel_refresh(mw->history_panel, mw->config);
    snapx_history_panel_set_open(mw->history_panel, open);
}

static void apply_crop_region(MainWindow *mw)
{
    if (!mw->current_image) return;
    int x0 = (int)(mw->crop_x1 < mw->crop_x2 ? mw->crop_x1 : mw->crop_x2);
    int y0 = (int)(mw->crop_y1 < mw->crop_y2 ? mw->crop_y1 : mw->crop_y2);
    int x1 = (int)(mw->crop_x1 > mw->crop_x2 ? mw->crop_x1 : mw->crop_x2);
    int y1 = (int)(mw->crop_y1 > mw->crop_y2 ? mw->crop_y1 : mw->crop_y2);
    int w = x1 - x0, h = y1 - y0;
    if (w < 4 || h < 4) {
        set_status(mw, "Crop region too small.");
        return;
    }
    SnapxImage *flat = snapx_canvas_flatten(mw->canvas, mw->current_image);
    SnapxImage *src = flat ? flat : mw->current_image;
    SnapxImage *cropped = snapx_image_crop(src, x0, y0, w, h);
    snapx_image_free(flat);
    if (!cropped) {
        set_status(mw, "Crop failed.");
        return;
    }
    after_capture(mw, cropped, "Image cropped.");
}

static void on_crop(MainWindow *mw)
{
    if (!mw->current_image) {
        set_status(mw, "Nothing to crop.");
        return;
    }
    mw->crop_active = !mw->crop_active;
    mw->crop_dragging = FALSE;
    snapx_toolbar_set_input_enabled(!mw->crop_active);
    set_status(mw, mw->crop_active
               ? "Drag to select crop area (Esc to cancel)"
               : "Crop cancelled.");
}

static gboolean on_close_request(GtkWindow *win, gpointer data)
{
    MainWindow *mw = data;
    if (mw->config && mw->config->close_to_tray) {
        gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
        snapx_tray_set_visible(1);
        return TRUE;
    }
    (void)win;
    return FALSE;
}

#ifdef SNAPX_USE_GTK4
static void crop_press(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)n;
    MainWindow *mw = d;
    if (!mw->crop_active) return;
    snapx_main_widget_to_image(x, y, &mw->crop_x1, &mw->crop_y1);
    mw->crop_x2 = mw->crop_x1;
    mw->crop_y2 = mw->crop_y1;
    mw->crop_dragging = TRUE;
    gtk_widget_queue_draw(mw->drawing_area);
}

static void crop_release(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)g; (void)n;
    MainWindow *mw = d;
    if (!mw->crop_active || !mw->crop_dragging) return;
    snapx_main_widget_to_image(x, y, &mw->crop_x2, &mw->crop_y2);
    mw->crop_dragging = FALSE;
    mw->crop_active = FALSE;
    snapx_toolbar_set_input_enabled(TRUE);
    apply_crop_region(mw);
}
#endif

static void on_format_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    MainWindow *mw = (MainWindow *)data;
    int active;
#ifdef SNAPX_USE_GTK4
    active = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
#else
    active = gtk_combo_box_get_active(GTK_COMBO_BOX(obj));
#endif
    if (mw->quality_scale) {
        gboolean vis = (active == (int)SNAPX_FORMAT_JPEG ||
                        active == (int)SNAPX_FORMAT_WEBP);
        gtk_widget_set_visible(mw->quality_scale, vis);
        gtk_widget_set_visible(mw->quality_label, vis);
    }
}

/* ─── Keyboard shortcuts ──────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4
static gboolean on_key_pressed(GtkEventControllerKey *ctrl,
                                 guint keyval, guint keycode,
                                 GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)keycode;
    MainWindow *mw = (MainWindow *)data;
    const SnapxShortcuts *sc = &mw->config->shortcuts;

    if (snapx_shortcut_match(sc->capture_fullscreen, keyval, state)) {
        trigger_capture_mode(mw, SNAPX_CAPTURE_FULLSCREEN); return TRUE;
    }
    if (snapx_shortcut_match(sc->capture_monitor, keyval, state)) {
        trigger_capture_mode(mw, SNAPX_CAPTURE_MONITOR); return TRUE;
    }
    if (snapx_shortcut_match(sc->capture_region, keyval, state)) {
        trigger_capture_mode(mw, SNAPX_CAPTURE_REGION); return TRUE;
    }
    if (snapx_shortcut_match(sc->capture_window, keyval, state)) {
        trigger_capture_mode(mw, SNAPX_CAPTURE_ACTIVE_WINDOW); return TRUE;
    }
    if (snapx_shortcut_match(sc->global_capture, keyval, state)) {
        trigger_capture_mode(mw, mw->config->default_mode); return TRUE;
    }
    if (snapx_shortcut_match(sc->save, keyval, state)) {
        on_save(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->copy, keyval, state)) {
        on_copy_clipboard(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->upload, keyval, state)) {
        on_upload(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->ocr, keyval, state)) {
        on_ocr(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->pin, keyval, state)) {
        on_pin(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->crop, keyval, state)) {
        on_crop(mw); return TRUE;
    }
    if (mw->crop_active && keyval == GDK_KEY_Escape) {
        mw->crop_active = FALSE;
        snapx_toolbar_set_input_enabled(TRUE);
        set_status(mw, "Crop cancelled.");
        gtk_widget_queue_draw(mw->drawing_area);
        return TRUE;
    }
    if (mw->canvas && snapx_shortcut_match(sc->undo, keyval, state)) {
        snapx_canvas_undo(mw->canvas);
        snapx_toolbar_set_canvas(mw->canvas, mw->drawing_area);
        mw->annot_surface_valid = FALSE;
        gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (mw->canvas && snapx_shortcut_match(sc->redo, keyval, state)) {
        snapx_canvas_redo(mw->canvas);
        mw->annot_surface_valid = FALSE;
        gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (snapx_shortcut_match(sc->fit, keyval, state)) {
        set_fit(mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->zoom_in, keyval, state)) {
        double cur = mw->fit_mode ? mw->scaled_for_zoom : mw->zoom;
        set_zoom(mw, cur > 0 ? cur * 1.25 : 1.25); return TRUE;
    }
    if (snapx_shortcut_match(sc->zoom_out, keyval, state)) {
        double cur = mw->fit_mode ? mw->scaled_for_zoom : mw->zoom;
        set_zoom(mw, cur > 0 ? cur / 1.25 : 1.0 / 1.25); return TRUE;
    }
    return FALSE;
}
#else
static gboolean on_key_press_gtk3(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    MainWindow *mw = (MainWindow *)data;
    const SnapxShortcuts *sc = &mw->config->shortcuts;

    if (snapx_shortcut_match(sc->capture_fullscreen, ev->keyval, ev->state)) {
        trigger_capture_mode(mw, SNAPX_CAPTURE_FULLSCREEN); return TRUE;
    }
    if (snapx_shortcut_match(sc->capture_monitor, ev->keyval, ev->state)) {
        trigger_capture_mode(mw, SNAPX_CAPTURE_MONITOR); return TRUE;
    }
    if (snapx_shortcut_match(sc->capture_region, ev->keyval, ev->state)) {
        trigger_capture_mode(mw, SNAPX_CAPTURE_REGION); return TRUE;
    }
    if (snapx_shortcut_match(sc->capture_window, ev->keyval, ev->state)) {
        trigger_capture_mode(mw, SNAPX_CAPTURE_ACTIVE_WINDOW); return TRUE;
    }
    if (snapx_shortcut_match(sc->global_capture, ev->keyval, ev->state)) {
        trigger_capture_mode(mw, mw->config->default_mode); return TRUE;
    }

    if (snapx_shortcut_match(sc->save, ev->keyval, ev->state)) {
        on_save(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->copy, ev->keyval, ev->state)) {
        on_copy_clipboard(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->upload, ev->keyval, ev->state)) {
        on_upload(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->ocr, ev->keyval, ev->state)) {
        on_ocr(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->pin, ev->keyval, ev->state)) {
        on_pin(NULL, mw); return TRUE;
    }
    if (snapx_shortcut_match(sc->crop, ev->keyval, ev->state)) {
        on_crop(mw); return TRUE;
    }
    if (mw->canvas && snapx_shortcut_match(sc->undo, ev->keyval, ev->state)) {
        snapx_canvas_undo(mw->canvas); mw->annot_surface_valid = FALSE;
        gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (mw->canvas && snapx_shortcut_match(sc->redo, ev->keyval, ev->state)) {
        snapx_canvas_redo(mw->canvas); mw->annot_surface_valid = FALSE;
        gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (snapx_shortcut_match(sc->fit, ev->keyval, ev->state)) { set_fit(mw); return TRUE; }
    if (snapx_shortcut_match(sc->zoom_in, ev->keyval, ev->state)) {
        double cur = mw->fit_mode ? mw->scaled_for_zoom : mw->zoom;
        set_zoom(mw, cur > 0 ? cur * 1.25 : 1.25); return TRUE;
    }
    if (snapx_shortcut_match(sc->zoom_out, ev->keyval, ev->state)) {
        double cur = mw->fit_mode ? mw->scaled_for_zoom : mw->zoom;
        set_zoom(mw, cur > 0 ? cur / 1.25 : 1.0 / 1.25); return TRUE;
    }
    return FALSE;
}
#endif

/* ─── Scroll (Ctrl+scroll = zoom) ─────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4
static gboolean on_scroll(GtkEventControllerScroll *ctrl,
                            double dx, double dy, gpointer data)
{
    (void)ctrl; (void)dx;
    MainWindow *mw = (MainWindow *)data;
    GdkModifierType state =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(ctrl));
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    double factor = (dy < 0) ? (1.0 + ZOOM_STEP) : (1.0 / (1.0 + ZOOM_STEP));
    double cur = mw->fit_mode ? mw->scaled_for_zoom : mw->zoom;
    set_zoom(mw, cur > 0 ? cur * factor : factor);
    return TRUE;
}
#endif

/* ─── Middle-button pan ───────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4
static void on_pan_begin(GtkGestureDrag *g, double sx, double sy, gpointer data)
{
    (void)g;
    MainWindow *mw = (MainWindow *)data;
    mw->panning    = TRUE;
    mw->pan_last_x = sx;
    mw->pan_last_y = sy;
}

static void on_pan_update(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    (void)g;
    MainWindow *mw = (MainWindow *)data;
    if (!mw->panning || !mw->current_image) return;
    int cw, ch;
    cw = gtk_widget_get_width(mw->drawing_area);
    ch = gtk_widget_get_height(mw->drawing_area);
    double scale, dummy;
    compute_viewport(mw, cw, ch, &scale, &dummy, &dummy);
    mw->pan_x -= ox / scale;
    mw->pan_y -= oy / scale;
    invalidate_scaled(mw);
    gtk_widget_queue_draw(mw->drawing_area);
}

static void on_pan_end(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    on_pan_update(g, ox, oy, data);
    ((MainWindow *)data)->panning = FALSE;
}
#endif

/* ─── Window resize callback ──────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4
static void on_resize(GtkDrawingArea *da, int w, int h, gpointer data)
{
    (void)da; (void)w; (void)h;
    invalidate_scaled((MainWindow *)data);
}
#else
static gboolean on_configure(GtkWidget *w, GdkEventConfigure *ev, gpointer data)
{
    (void)w; (void)ev;
    invalidate_scaled((MainWindow *)data);
    return FALSE;
}
#endif

/* ─── Window construction ─────────────────────────────────────────────────── */

void snapx_window_main_create(GtkApplication      *app,
                                SnapxConfig          *config,
                                SnapxCaptureBackend  *backend,
                                SnapxCaptureMode     *mode)
{
    MainWindow *mw = &g_win;
    memset(mw, 0, sizeof(*mw));
    mw->config       = config;
    mw->backend      = backend;
    mw->default_mode = mode;
    mw->fit_mode     = TRUE;
    mw->zoom         = 1.0;
    snapx_main_sync_wayland_prefer(mw);

    /* Load application CSS */
    load_css();

    /* ── Root window ──────────────────────────────────────────────────────── */
#ifdef SNAPX_USE_GTK4
    GtkWidget *win = gtk_application_window_new(app);
#else
    GtkWidget *win = gtk_application_window_new(app);
#endif
    mw->win = GTK_APPLICATION_WINDOW(win);
    gtk_window_set_title(GTK_WINDOW(win), "snapx");
    gtk_window_set_default_size(GTK_WINDOW(win), 1024, 680);

    /* ── Header bar ───────────────────────────────────────────────────────── */
    GtkWidget *header = gtk_header_bar_new();
#ifdef SNAPX_USE_GTK4
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
#else
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
#endif
    gtk_window_set_titlebar(GTK_WINDOW(win), header);

    mw->btn_fullscreen = gtk_button_new_with_label("Screen");
    mw->btn_region     = gtk_button_new_with_label("Region");
    mw->btn_monitor    = gtk_button_new_with_label("Monitor");
    mw->btn_window     = gtk_button_new_with_label("Window");
    mw->btn_settings   = gtk_button_new_with_label("Settings");
    mw->btn_fit        = gtk_button_new_with_label("Fit");
    mw->zoom_label     = gtk_label_new("Fit");

    gtk_widget_add_css_class(mw->btn_fullscreen, "snapx-capture");
    gtk_widget_add_css_class(mw->btn_region,     "snapx-capture");
    gtk_widget_add_css_class(mw->btn_monitor,    "snapx-capture");
    gtk_widget_add_css_class(mw->btn_window,     "snapx-capture");

    /* ── Main vertical box ───────────────────────────────────────────────── */
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_fullscreen);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_region);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_monitor);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_window);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->btn_settings);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->zoom_label);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->btn_fit);

    gtk_widget_add_css_class(mw->zoom_label, "snapx-zoom");

    g_signal_connect(mw->btn_fullscreen, "clicked", G_CALLBACK(on_capture_fullscreen),   mw);
    g_signal_connect(mw->btn_region,     "clicked", G_CALLBACK(on_capture_region),       mw);
    g_signal_connect(mw->btn_monitor,    "clicked", G_CALLBACK(on_capture_monitor_btn),  mw);
    g_signal_connect(mw->btn_window,     "clicked", G_CALLBACK(on_capture_window),       mw);
    g_signal_connect(mw->btn_settings,   "clicked", G_CALLBACK(on_settings),             mw);
    g_signal_connect(mw->btn_fit,        "clicked", G_CALLBACK(on_fit),                  mw);

    /* ── Main vertical box ───────────────────────────────────────────────── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#ifdef SNAPX_USE_GTK4
    gtk_window_set_child(GTK_WINDOW(win), vbox);
#else
    gtk_container_add(GTK_CONTAINER(win), vbox);
#endif

    /* ── Annotation toolbar ──────────────────────────────────────────────── */
    mw->toolbar_box = snapx_toolbar_create(NULL, NULL, config);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(vbox), mw->toolbar_box);
#else
    gtk_box_pack_start(GTK_BOX(vbox), mw->toolbar_box, FALSE, FALSE, 0);
#endif

    /* ── Drawing area (with optional history sidebar) ──────────────────── */
    GtkWidget *scroll;
#ifdef SNAPX_USE_GTK4
    scroll = gtk_scrolled_window_new();
#else
    scroll = gtk_scrolled_window_new(NULL, NULL);
#endif
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_NEVER);

    GtkWidget *canvas_overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(canvas_overlay, TRUE);
    gtk_widget_set_hexpand(canvas_overlay, TRUE);

#ifdef SNAPX_USE_GTK4
    gtk_overlay_set_child(GTK_OVERLAY(canvas_overlay), scroll);
#else
    gtk_container_add(GTK_CONTAINER(canvas_overlay), scroll);
#endif

    mw->history_panel = snapx_history_panel_new(config, on_history_select, mw);
#ifdef SNAPX_USE_GTK4
    gtk_overlay_add_overlay(GTK_OVERLAY(canvas_overlay), mw->history_panel);
#else
    gtk_overlay_add_overlay(GTK_OVERLAY(canvas_overlay), mw->history_panel);
#endif

#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(vbox), canvas_overlay);
#else
    gtk_box_pack_start(GTK_BOX(vbox), canvas_overlay, TRUE, TRUE, 0);
#endif

#ifdef SNAPX_USE_GTK4
    mw->drawing_area = gtk_drawing_area_new();
    gtk_widget_add_css_class(mw->drawing_area, "snapx-canvas");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(mw->drawing_area),
                                    (GtkDrawingAreaDrawFunc)on_draw, mw, NULL);
    g_signal_connect(mw->drawing_area, "resize", G_CALLBACK(on_resize), mw);
    gtk_widget_set_size_request(mw->drawing_area, 400, 300);
    gtk_widget_set_focusable(mw->drawing_area, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), mw->drawing_area);
    mw->draw_tick_id = gtk_widget_add_tick_callback(
        mw->drawing_area, on_draw_tick, mw, NULL);
#else
    mw->drawing_area = gtk_drawing_area_new();
    gtk_widget_add_css_class(mw->drawing_area, "snapx-canvas");
    gtk_widget_set_size_request(mw->drawing_area, 400, 300);
    g_signal_connect(mw->drawing_area, "draw",             G_CALLBACK(on_draw_gtk3),   mw);
    g_signal_connect(mw->drawing_area, "configure-event",  G_CALLBACK(on_configure),   mw);
    gtk_container_add(GTK_CONTAINER(scroll), mw->drawing_area);
#endif

    /* Annotation + pan events */
    snapx_toolbar_connect_canvas_events(mw->drawing_area, NULL);

#ifdef SNAPX_USE_GTK4
    /* Ctrl+scroll = zoom */
    GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_scroll), mw);
    gtk_widget_add_controller(mw->drawing_area, scroll_ctrl);

    /* Middle-drag = pan */
    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 2);  /* middle btn */
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_pan_begin),  mw);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_pan_update), mw);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(on_pan_end),    mw);
    gtk_widget_add_controller(mw->drawing_area, GTK_EVENT_CONTROLLER(drag));

    GtkGesture *crop_click = gtk_gesture_click_new();
    g_signal_connect(crop_click, "pressed",  G_CALLBACK(crop_press),   mw);
    g_signal_connect(crop_click, "released", G_CALLBACK(crop_release), mw);
    gtk_widget_add_controller(mw->drawing_area, GTK_EVENT_CONTROLLER(crop_click));
#endif

    /* ── Workflow status (upload / OCR) ──────────────────────────────────── */
    GtkWidget *workflow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(workflow, "snapx-actionbar");
    mw->lbl_upload_status = gtk_label_new("");
    mw->lbl_ocr_status    = gtk_label_new("");
    gtk_widget_add_css_class(mw->lbl_upload_status, "snapx-workflow-status");
    gtk_widget_add_css_class(mw->lbl_ocr_status, "snapx-workflow-status");
    gtk_label_set_xalign(GTK_LABEL(mw->lbl_upload_status), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(mw->lbl_ocr_status), 0.0f);
    gtk_box_append(GTK_BOX(workflow), mw->lbl_upload_status);
    gtk_box_append(GTK_BOX(workflow), mw->lbl_ocr_status);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(vbox), workflow);
#else
    gtk_box_pack_start(GTK_BOX(vbox), workflow, FALSE, FALSE, 0);
#endif

    /* ── Bottom action bar ───────────────────────────────────────────────── */
    GtkWidget *action_bar;
#ifdef SNAPX_USE_GTK4
    action_bar = gtk_action_bar_new();
    gtk_widget_add_css_class(action_bar, "snapx-actionbar");
    gtk_box_append(GTK_BOX(vbox), action_bar);
#else
    action_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(action_bar, 6);
    gtk_widget_set_margin_end(action_bar, 6);
    gtk_widget_set_margin_top(action_bar, 4);
    gtk_widget_set_margin_bottom(action_bar, 4);
    gtk_box_pack_end(GTK_BOX(vbox), action_bar, FALSE, FALSE, 0);
#endif

    /* Format selector */
    GtkWidget *fmt_label = gtk_label_new("Format:");
    gtk_widget_add_css_class(fmt_label, "snapx-format-label");
    const char *fmt_items[] = { "PNG", "JPEG", "WebP", NULL };
#ifdef SNAPX_USE_GTK4
    mw->format_combo = gtk_drop_down_new_from_strings(fmt_items);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(mw->format_combo),
                                (guint)config->default_format);
    g_signal_connect(mw->format_combo, "notify::selected",
                     G_CALLBACK(on_format_changed), mw);
#else
    mw->format_combo = gtk_combo_box_text_new();
    for (int fi = 0; fmt_items[fi]; fi++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mw->format_combo), fmt_items[fi]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(mw->format_combo), (int)config->default_format);
    g_signal_connect(mw->format_combo, "changed", G_CALLBACK(on_format_changed), mw);
#endif

    /* Quality slider */
    mw->quality_label = gtk_label_new("Quality:");
    gtk_widget_add_css_class(mw->quality_label, "snapx-format-label");
    mw->quality_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
    gtk_range_set_value(GTK_RANGE(mw->quality_scale), (double)config->jpeg_quality);
    gtk_scale_set_draw_value(GTK_SCALE(mw->quality_scale), TRUE);
    gtk_widget_set_size_request(mw->quality_scale, 130, -1);
    gtk_widget_set_visible(mw->quality_scale,
                            config->default_format != SNAPX_FORMAT_PNG);
    gtk_widget_set_visible(mw->quality_label,
                            config->default_format != SNAPX_FORMAT_PNG);

    GtkWidget *btn_copy = gtk_button_new_with_label("Copy");
    GtkWidget *btn_beautify = gtk_button_new_with_label("Beautify…");
    GtkWidget *btn_ocr = gtk_button_new_with_label("Copy text");
    GtkWidget *btn_upload = gtk_button_new_with_label("Upload");
    GtkWidget *btn_recents = gtk_toggle_button_new_with_label("Recents");
    GtkWidget *btn_save = gtk_button_new_with_label("Save");
    GtkWidget *btn_folder = gtk_button_new_with_label("Open folder");
    mw->btn_upload  = btn_upload;
    mw->btn_ocr     = btn_ocr;
    mw->btn_recents = btn_recents;
    gtk_widget_add_css_class(btn_save, "suggested-action");
    gtk_widget_set_tooltip_text(btn_copy, "Copy to clipboard  (Ctrl+C)");
    gtk_widget_set_tooltip_text(btn_beautify,
        "Add a background, padding, rounded corners and shadow");
    gtk_widget_set_tooltip_text(btn_ocr, "Copy text from image (OCR)");
    gtk_widget_set_tooltip_text(btn_upload, "Upload and copy link  (Ctrl+U)");
    gtk_widget_set_tooltip_text(btn_recents,
        "Show recent saves from screenshots folder");
    gtk_widget_set_tooltip_text(btn_save, "Save to file  (Ctrl+S)");
    gtk_widget_set_tooltip_text(btn_folder,
        "Open screenshots folder in file manager (last save location if available)");
    g_signal_connect(btn_copy,   "clicked", G_CALLBACK(on_copy_clipboard), mw);
    g_signal_connect(btn_beautify,"clicked", G_CALLBACK(on_beautify),       mw);
    g_signal_connect(btn_ocr,    "clicked", G_CALLBACK(on_ocr),            mw);
    g_signal_connect(btn_upload, "clicked", G_CALLBACK(on_upload),         mw);
    g_signal_connect(btn_recents,"toggled", G_CALLBACK(on_recents),        mw);
    g_signal_connect(btn_save,   "clicked", G_CALLBACK(on_save),           mw);
    g_signal_connect(btn_folder, "clicked", G_CALLBACK(on_open_folder),    mw);

#ifdef SNAPX_USE_GTK4
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), fmt_label);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), mw->format_combo);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), mw->quality_label);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), mw->quality_scale);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), btn_save);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), btn_folder);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), btn_upload);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), btn_ocr);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), btn_beautify);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), btn_copy);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), btn_recents);
#else
    gtk_box_pack_start(GTK_BOX(action_bar), fmt_label,         FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(action_bar), mw->format_combo,  FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(action_bar), mw->quality_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(action_bar), mw->quality_scale, FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_save,          FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_folder,        FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_upload,        FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_ocr,           FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_beautify,      FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_copy,          FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_recents,       FALSE, FALSE, 4);
#endif

    snapx_main_update_workflow_ui(mw);

    /* ── Status label ────────────────────────────────────────────────────── */
    mw->statusbar = gtk_label_new("");
    gtk_widget_add_css_class(mw->statusbar, "snapx-statusbar");
    gtk_widget_set_halign(mw->statusbar, GTK_ALIGN_START);
    gtk_widget_set_margin_start(mw->statusbar, 10);
    gtk_widget_set_margin_bottom(mw->statusbar, 3);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(vbox), mw->statusbar);
#else
    gtk_box_pack_end(GTK_BOX(vbox), mw->statusbar, FALSE, FALSE, 0);
#endif

    /* ── Keyboard shortcuts ──────────────────────────────────────────────── */
#ifdef SNAPX_USE_GTK4
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(key_ctrl),
                                                GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), mw);
    gtk_widget_add_controller(win, key_ctrl);
#else
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press_gtk3), mw);
#endif

    g_signal_connect(win, "destroy", G_CALLBACK(on_win_destroy), mw);
#ifdef SNAPX_USE_GTK4
    g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), mw);
#endif

    snapx_main_refresh_shortcut_tooltips(mw);

    snapx_tray_init(mw, app, config);
    snapx_dbus_service_start();

    if (config->start_in_tray)
        gtk_widget_set_visible(win, FALSE);
    else
        gtk_widget_set_visible(win, TRUE);
    g_idle_add(portal_parent_on_mapped, mw);
    set_status(mw, "Ready — click Screen, Region or Window to capture.");
}

/* ─── External image setter ───────────────────────────────────────────────── */

void snapx_window_main_set_image(SnapxImage *img)
{
    MainWindow *mw = &g_win;
    snapx_image_free(mw->current_image);
    mw->current_image = img;
    if (mw->display_surface) { cairo_surface_destroy(mw->display_surface); mw->display_surface = NULL; }
    mw->display_surface = make_display_surface(img);
    invalidate_scaled(mw);
    if (mw->canvas) snapx_canvas_free(mw->canvas);
    mw->canvas = img ? snapx_canvas_new(img->width, img->height) : NULL;
    snapx_toolbar_set_canvas(mw->canvas, mw->drawing_area);
    snapx_main_bind_canvas(mw);
    set_fit(mw);
    if (mw->drawing_area) gtk_widget_queue_draw(mw->drawing_area);
}

void snapx_window_main_show(void)
{
    if (g_win.win)
        gtk_window_present(GTK_WINDOW(g_win.win));
}

void snapx_window_main_capture_region(void)
{
    trigger_capture_mode(&g_win, SNAPX_CAPTURE_REGION);
}

GtkApplication *snapx_window_main_get_app(void)
{
    if (!g_win.win) return NULL;
    return gtk_window_get_application(GTK_WINDOW(g_win.win));
}

static void on_blur_committed(double x1, double y1, double x2, double y2,
                              gpointer userdata)
{
    (void)userdata;
    snapx_main_apply_blur_region(x1, y1, x2, y2);
}

static void snapx_main_bind_canvas(MainWindow *mw)
{
    if (mw && mw->canvas)
        snapx_canvas_set_blur_handler(mw->canvas, on_blur_committed, mw);
}

void snapx_main_apply_blur_region(double x1, double y1, double x2, double y2)
{
    MainWindow *mw = &g_win;
    if (!mw->current_image) {
        set_status(mw, "Capture an image before using blur.");
        return;
    }

    int iw  = (int)fabs(x2 - x1);
    int ih  = (int)fabs(y2 - y1);
    if (iw < 4 || ih < 4) {
        set_status(mw, "Drag a larger region to blur.");
        return;
    }

    int ix0 = (int)(x1 < x2 ? x1 : x2);
    int iy0 = (int)(y1 < y2 ? y1 : y2);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix0 + iw > mw->current_image->width)
        iw = mw->current_image->width - ix0;
    if (iy0 + ih > mw->current_image->height)
        ih = mw->current_image->height - iy0;
    if (iw < 4 || ih < 4) return;

    snapx_image_pixelate(mw->current_image, ix0, iy0, iw, ih, 8);

    if (mw->display_surface)
        cairo_surface_destroy(mw->display_surface);
    mw->display_surface = make_display_surface(mw->current_image);
    invalidate_scaled(mw);
    snapx_main_schedule_redraw();
    set_status(mw, "Region pixelated.");
}

const char *snapx_window_main_get_last_path(void)
{
    return g_win.last_save_path[0] ? g_win.last_save_path : "";
}

int snapx_window_main_save_to(const char *path)
{
    if (!path || !path[0] || !g_win.current_image) return -1;
    SnapxOutputFormat fmt = SNAPX_FORMAT_PNG;
    if (g_win.format_combo) {
#ifdef SNAPX_USE_GTK4
        fmt = (SnapxOutputFormat)gtk_drop_down_get_selected(
            GTK_DROP_DOWN(g_win.format_combo));
#else
        fmt = (SnapxOutputFormat)gtk_combo_box_get_active(
            GTK_COMBO_BOX(g_win.format_combo));
#endif
    }
    SnapxImage *flat = flatten_for_export(&g_win);
    int quality = g_win.quality_scale
        ? (int)gtk_range_get_value(GTK_RANGE(g_win.quality_scale))
        : g_win.config->jpeg_quality;
    int ret = snapx_image_save(flat ? flat : g_win.current_image, path, fmt, quality);
    snapx_image_free(flat);
    if (ret == 0)
        snprintf(g_win.last_save_path, sizeof(g_win.last_save_path), "%s", path);
    return ret;
}

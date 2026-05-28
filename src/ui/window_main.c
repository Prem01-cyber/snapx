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
#include "../annotation/canvas.h"
#include "../capture/capture.h"
#include "../capture/platform.h"
#include "../output/save.h"
#include "../output/clipboard.h"
#include "../utils/config.h"
#include "../utils/monitor.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

    /* Capture-mode / action buttons */
    GtkWidget *btn_fullscreen;
    GtkWidget *btn_region;
    GtkWidget *btn_window;
    GtkWidget *btn_settings;
    GtkWidget *btn_fit;
} MainWindow;

static MainWindow g_win;

/* ─── Forward declarations ───────────────────────────────────────────────── */

static void invalidate_scaled(MainWindow *mw);
static void redraw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static void do_capture(MainWindow *mw, SnapxCaptureMode mode, int delay);
static void update_zoom_label(MainWindow *mw);

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
    /* Dark workspace background */
    cairo_set_source_rgb(cr, 0.098, 0.098, 0.110);
    cairo_paint(cr);
    /* Subtle checkerboard pattern (shows transparency) */
    int cs = 12;
    for (int ty = 0; ty < ch; ty += cs) {
        for (int tx = 0; tx < cw; tx += cs) {
            int which = ((tx/cs) + (ty/cs)) & 1;
            cairo_set_source_rgb(cr, which ? 0.13 : 0.10, which ? 0.13 : 0.10,
                                     which ? 0.14 : 0.11);
            cairo_rectangle(cr, tx, ty, cs, cs);
            cairo_fill(cr);
        }
    }

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
    snapx_canvas_render(mw->canvas, cr);
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

    /* Rebuild annotation surface if dirty */
    if (snapx_toolbar_annot_dirty() || !mw->annot_surface_valid)
        rebuild_annot(mw);

    if (mw->annot_surface && mw->annot_surface_valid) {
        cairo_set_source_surface(cr, mw->annot_surface, 0, 0);
        cairo_paint(cr);
    }
}

/* ─── Portal parent window helper ─────────────────────────────────────────── */

static void update_portal_parent(MainWindow *mw)
{
    if (!mw->backend || mw->backend->type != SNAPX_BACKEND_WAYLAND) return;
    char parent[128] = "";
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(mw->win));
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_SURFACE(surf)) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        guint32 xid = (guint32)gdk_x11_surface_get_xid(surf);
        G_GNUC_END_IGNORE_DEPRECATIONS
        snprintf(parent, sizeof(parent), "x11:0x%x", xid);
    }
#endif
    snapx_capture_wayland_set_parent_window(mw->backend, parent);
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

/* ─── Capture ─────────────────────────────────────────────────────────────── */

static void after_capture(MainWindow *mw, SnapxImage *img, const char *ok_msg)
{
    gtk_widget_set_visible(GTK_WIDGET(mw->win), TRUE);
    gtk_window_present(GTK_WINDOW(mw->win));

    if (!img) { set_status(mw, "Capture failed — check backend with snapx --info."); return; }

    snapx_image_free(mw->current_image);
    mw->current_image = img;

    if (mw->display_surface) { cairo_surface_destroy(mw->display_surface); mw->display_surface = NULL; }
    mw->display_surface = make_display_surface(img);
    invalidate_scaled(mw);

    if (mw->canvas) snapx_canvas_free(mw->canvas);
    mw->canvas = snapx_canvas_new(img->width, img->height);
    snapx_toolbar_set_canvas(mw->canvas, mw->drawing_area);

    /* Reset to fit view */
    set_fit(mw);

    /* Update window title with resolution */
    char title[128];
    snprintf(title, sizeof(title), "snapx — %d × %d", img->width, img->height);
    gtk_window_set_title(GTK_WINDOW(mw->win), title);

    set_status(mw, ok_msg);
    snapx_capture_wayland_save_token(mw->backend);
    if (mw->config->auto_clipboard) snapx_clipboard_copy_image(img);
}

static void do_capture(MainWindow *mw, SnapxCaptureMode mode, int delay)
{
    update_portal_parent(mw);
    gtk_widget_set_visible(GTK_WIDGET(mw->win), FALSE);
    while (g_main_context_iteration(NULL, FALSE)) {}

    SnapxCaptureRequest req = {0};
    req.mode           = mode;
    req.delay_sec      = delay;
    req.monitor_index  = 0;
    req.include_cursor = mw->config->show_cursor;

    SnapxImage *img = snapx_capture(mw->backend, &req);
    after_capture(mw, img, "Screenshot captured. Annotate or save.");
}

/* ─── Button callbacks ─────────────────────────────────────────────────────── */

static void on_capture_fullscreen(GtkButton *b, gpointer d)
{
    (void)b; do_capture((MainWindow *)d, SNAPX_CAPTURE_FULLSCREEN, 0);
}

static void on_capture_region(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    SnapxRegion region = {0};
    if (!snapx_overlay_select_region(GTK_WINDOW(mw->win), &region)) return;

    SnapxCaptureRequest req = {0};
    req.mode           = SNAPX_CAPTURE_REGION;
    req.region_x       = region.x;
    req.region_y       = region.y;
    req.region_w       = region.width;
    req.region_h       = region.height;
    req.include_cursor = mw->config->show_cursor;

    gtk_widget_set_visible(GTK_WIDGET(mw->win), FALSE);
    while (g_main_context_iteration(NULL, FALSE)) {}
    SnapxImage *img = snapx_capture(mw->backend, &req);
    after_capture(mw, img, "Region captured.");
}

static void on_capture_window(GtkButton *b, gpointer d)
{
    (void)b; do_capture((MainWindow *)d, SNAPX_CAPTURE_ACTIVE_WINDOW, 0);
}

static void on_copy_clipboard(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    if (!mw->current_image) { set_status(mw, "Nothing to copy."); return; }
    snapx_clipboard_copy_image(mw->current_image);
    set_status(mw, "Copied to clipboard.");
}

static void on_save(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    if (!mw->current_image) { set_status(mw, "Nothing to save."); return; }

    SnapxImage *flat = snapx_canvas_flatten(mw->canvas, mw->current_image);

    char path[512];
    snapx_config_build_path(mw->config, path, sizeof(path));

    SnapxOutputFormat fmt = SNAPX_FORMAT_PNG;
    if (mw->format_combo) {
#ifdef SNAPX_USE_GTK4
        fmt = (SnapxOutputFormat)gtk_drop_down_get_selected(GTK_DROP_DOWN(mw->format_combo));
#else
        fmt = (SnapxOutputFormat)gtk_combo_box_get_active(GTK_COMBO_BOX(mw->format_combo));
#endif
    }
    int quality = (int)(mw->quality_scale
                        ? gtk_range_get_value(GTK_RANGE(mw->quality_scale))
                        : mw->config->jpeg_quality);

    int ret = snapx_image_save(flat ? flat : mw->current_image, path, fmt, quality);
    snapx_image_free(flat);

    if (ret == 0) {
        char msg[600];
        snprintf(msg, sizeof(msg), "Saved: %s", path);
        set_status(mw, msg);
    } else {
        set_status(mw, "Save failed — check permissions or disk space.");
    }
}

static void on_settings(GtkButton *b, gpointer d)
{
    (void)b;
    MainWindow *mw = (MainWindow *)d;
    snapx_settings_dialog_show(GTK_WINDOW(mw->win), mw->config);
}

static void on_fit(GtkButton *b, gpointer d) { (void)b; set_fit((MainWindow *)d); }

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
    gboolean ctrl_mod = (state & GDK_CONTROL_MASK) != 0;

    if (ctrl_mod && keyval == GDK_KEY_s) { on_save(NULL, mw); return TRUE; }
    if (ctrl_mod && keyval == GDK_KEY_c) { on_copy_clipboard(NULL, mw); return TRUE; }
    if (ctrl_mod && (keyval == GDK_KEY_z || keyval == GDK_KEY_Z) && mw->canvas) {
        snapx_canvas_undo(mw->canvas);
        snapx_toolbar_set_canvas(mw->canvas, mw->drawing_area);  /* re-marks dirty */
        mw->annot_surface_valid = FALSE;
        gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (ctrl_mod && (keyval == GDK_KEY_y || keyval == GDK_KEY_Y) && mw->canvas) {
        snapx_canvas_redo(mw->canvas);
        mw->annot_surface_valid = FALSE;
        gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (keyval == GDK_KEY_0 && ctrl_mod) { set_fit(mw); return TRUE; }
    if (keyval == GDK_KEY_equal && ctrl_mod) { set_zoom(mw, mw->zoom * 1.25); return TRUE; }
    if (keyval == GDK_KEY_minus && ctrl_mod) { set_zoom(mw, mw->zoom / 1.25); return TRUE; }
    return FALSE;
}
#else
static gboolean on_key_press_gtk3(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    MainWindow *mw = (MainWindow *)data;
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    if (ctrl && ev->keyval == GDK_KEY_s) { on_save(NULL, mw); return TRUE; }
    if (ctrl && ev->keyval == GDK_KEY_c) { on_copy_clipboard(NULL, mw); return TRUE; }
    if (ctrl && (ev->keyval == GDK_KEY_z || ev->keyval == GDK_KEY_Z) && mw->canvas) {
        snapx_canvas_undo(mw->canvas); mw->annot_surface_valid = FALSE;
        gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (ctrl && (ev->keyval == GDK_KEY_y || ev->keyval == GDK_KEY_Y) && mw->canvas) {
        snapx_canvas_redo(mw->canvas); mw->annot_surface_valid = FALSE;
        gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (ctrl && ev->keyval == GDK_KEY_0) { set_fit(mw); return TRUE; }
    if (ctrl && ev->keyval == GDK_KEY_equal) { set_zoom(mw, mw->zoom * 1.25); return TRUE; }
    if (ctrl && ev->keyval == GDK_KEY_minus) { set_zoom(mw, mw->zoom / 1.25); return TRUE; }
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
    mw->btn_window     = gtk_button_new_with_label("Window");
    mw->btn_settings   = gtk_button_new_with_label("Settings");
    mw->btn_fit        = gtk_button_new_with_label("Fit");
    mw->zoom_label     = gtk_label_new("Fit");

    gtk_widget_add_css_class(mw->btn_fullscreen, "snapx-capture");
    gtk_widget_add_css_class(mw->btn_region,     "snapx-capture");
    gtk_widget_add_css_class(mw->btn_window,     "snapx-capture");

    gtk_widget_set_tooltip_text(mw->btn_fullscreen,
        "Capture full screen  (PrintScreen)");
    gtk_widget_set_tooltip_text(mw->btn_region,
        "Select a region with mouse  (Shift+PrintScreen)");
    gtk_widget_set_tooltip_text(mw->btn_window,
        "Capture active window  (Alt+PrintScreen)");
    gtk_widget_set_tooltip_text(mw->btn_fit,
        "Reset zoom to fit window  (Ctrl+0)");
    gtk_widget_set_tooltip_text(mw->zoom_label,
        "Current zoom level.  Ctrl+scroll or Ctrl +/- to change.");

#ifdef SNAPX_USE_GTK4
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_fullscreen);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_region);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_window);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->btn_settings);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->zoom_label);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->btn_fit);
#else
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_fullscreen);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_region);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_window);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->btn_settings);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->zoom_label);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->btn_fit);
#endif

    gtk_widget_add_css_class(mw->zoom_label, "snapx-zoom");

    g_signal_connect(mw->btn_fullscreen, "clicked", G_CALLBACK(on_capture_fullscreen), mw);
    g_signal_connect(mw->btn_region,     "clicked", G_CALLBACK(on_capture_region),     mw);
    g_signal_connect(mw->btn_window,     "clicked", G_CALLBACK(on_capture_window),     mw);
    g_signal_connect(mw->btn_settings,   "clicked", G_CALLBACK(on_settings),           mw);
    g_signal_connect(mw->btn_fit,        "clicked", G_CALLBACK(on_fit),                mw);

    /* ── Main vertical box ───────────────────────────────────────────────── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#ifdef SNAPX_USE_GTK4
    gtk_window_set_child(GTK_WINDOW(win), vbox);
#else
    gtk_container_add(GTK_CONTAINER(win), vbox);
#endif

    /* ── Annotation toolbar ──────────────────────────────────────────────── */
    mw->toolbar_box = snapx_toolbar_create(NULL, NULL);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(vbox), mw->toolbar_box);
#else
    gtk_box_pack_start(GTK_BOX(vbox), mw->toolbar_box, FALSE, FALSE, 0);
#endif

    /* ── Drawing area ────────────────────────────────────────────────────── */
    GtkWidget *scroll;
#ifdef SNAPX_USE_GTK4
    scroll = gtk_scrolled_window_new();
#else
    scroll = gtk_scrolled_window_new(NULL, NULL);
#endif
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    /* Disable auto-scrollbars — we manage panning ourselves */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_NEVER);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(vbox), scroll);
#else
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
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

    /* Copy / Save */
    GtkWidget *btn_copy = gtk_button_new_with_label("Copy");
    GtkWidget *btn_save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(btn_save, "suggested-action");
    gtk_widget_set_tooltip_text(btn_copy, "Copy to clipboard  (Ctrl+C)");
    gtk_widget_set_tooltip_text(btn_save, "Save to file  (Ctrl+S)");
    g_signal_connect(btn_copy, "clicked", G_CALLBACK(on_copy_clipboard), mw);
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_save),           mw);

#ifdef SNAPX_USE_GTK4
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), fmt_label);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), mw->format_combo);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), mw->quality_label);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), mw->quality_scale);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), btn_save);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), btn_copy);
#else
    gtk_box_pack_start(GTK_BOX(action_bar), fmt_label,         FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(action_bar), mw->format_combo,  FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(action_bar), mw->quality_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(action_bar), mw->quality_scale, FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_save,          FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_copy,          FALSE, FALSE, 4);
#endif

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
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), mw);
    gtk_widget_add_controller(win, key_ctrl);
#else
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press_gtk3), mw);
#endif

    gtk_widget_set_visible(win, TRUE);
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
    set_fit(mw);
    if (mw->drawing_area) gtk_widget_queue_draw(mw->drawing_area);
}

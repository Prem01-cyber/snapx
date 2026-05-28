/**
 * @file window_main.c
 * @brief Main application window: screenshot preview + annotation toolbar + save bar.
 *
 * Layout (GTK4):
 *   GtkApplicationWindow
 *     └── GtkBox (vertical)
 *           ├── GtkHeaderBar  (title + capture buttons)
 *           ├── toolbar       (annotation tools row)
 *           ├── GtkScrolledWindow → GtkDrawingArea (preview + annotation canvas)
 *           └── GtkActionBar  (format selector, quality slider, copy/save buttons)
 */

#include "window_main.h"
#include "overlay.h"
#include "toolbar.h"
#include "settings_dialog.h"
#include "../annotation/canvas.h"
#include "../capture/capture.h"
#include "../output/save.h"
#include "../output/clipboard.h"
#include "../utils/config.h"
#include "../utils/monitor.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Platform-specific window handle for portal parent_window */
#ifdef GDK_WINDOWING_X11
#  include <gdk/x11/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#  include <gdk/wayland/gdkwayland.h>
#endif

/* ─── Window state ───────────────────────────────────────────────────────── */

typedef struct {
    GtkApplicationWindow *win;
    GtkWidget            *drawing_area;
    GtkWidget            *toolbar_box;
    GtkWidget            *format_combo;
    GtkWidget            *quality_scale;
    GtkWidget            *quality_label;
    GtkWidget            *statusbar;

    SnapxConfig          *config;
    SnapxCaptureBackend  *backend;
    SnapxCaptureMode     *default_mode;

    SnapxImage           *current_image;     /**< Raw captured image (RGBA)       */
    cairo_surface_t      *display_surface;   /**< ARGB32 cache for Cairo display  */
    SnapxAnnotationCanvas *canvas;           /**< Active annotation state         */

    /* Capture-mode menu buttons */
    GtkWidget *btn_fullscreen;
    GtkWidget *btn_region;
    GtkWidget *btn_monitor;
    GtkWidget *btn_window;
    GtkWidget *btn_delay_menu;
    GtkWidget *btn_settings;
} MainWindow;

static MainWindow g_main_win;

/* ─── Forward declarations ───────────────────────────────────────────────── */

static void redraw_canvas(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static void do_capture(MainWindow *mw, SnapxCaptureMode mode, int delay);

/* ─── RGBA → Cairo ARGB32 conversion ────────────────────────────────────── */

/**
 * Convert an RGBA SnapxImage into a cairo_surface_t with ARGB32 pixel layout.
 *
 * Cairo ARGB32 (little-endian): each 32-bit word = 0xAARRGGBB stored as
 *   byte[0]=B, byte[1]=G, byte[2]=R, byte[3]=A  (premultiplied alpha).
 * SnapxImage stores unpremultiplied RGBA: byte[0]=R, byte[1]=G, byte[2]=B, byte[3]=A.
 */
static cairo_surface_t *make_display_surface(const SnapxImage *img)
{
    if (!img) return NULL;
    int w = img->width, h = img->height;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) return NULL;

    cairo_surface_flush(surf);
    unsigned char *dst = cairo_image_surface_get_data(surf);
    int dst_stride     = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < h; y++) {
        const uint8_t *src_row = img->data + y * img->stride;
        uint32_t      *dst_row = (uint32_t *)(dst + y * dst_stride);
        for (int x = 0; x < w; x++) {
            uint8_t r = src_row[x * 4 + 0];
            uint8_t g = src_row[x * 4 + 1];
            uint8_t b = src_row[x * 4 + 2];
            uint8_t a = src_row[x * 4 + 3];
            /* Premultiply alpha for Cairo */
            if (a == 0xFF) {
                dst_row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            } else {
                uint32_t rm = (r * a + 127) / 255;
                uint32_t gm = (g * a + 127) / 255;
                uint32_t bm = (b * a + 127) / 255;
                dst_row[x] = ((uint32_t)a << 24) | (rm << 16) | (gm << 8) | bm;
            }
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

static void update_display_surface(MainWindow *mw, SnapxImage *img)
{
    if (mw->display_surface) {
        cairo_surface_destroy(mw->display_surface);
        mw->display_surface = NULL;
    }
    mw->display_surface = make_display_surface(img);
}

/* ─── Drawing area paint ────────────────────────────────────────────────────*/

#ifdef SNAPX_USE_GTK4
static void on_draw(GtkDrawingArea *da, cairo_t *cr,
                    int width, int height, gpointer data)
{
    (void)da; (void)width; (void)height;
    redraw_canvas(NULL, cr, data);
}
#else
static gboolean on_draw_gtk3(GtkWidget *w, cairo_t *cr, gpointer data)
{
    redraw_canvas(w, cr, data);
    return FALSE;
}
#endif

static void redraw_canvas(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    MainWindow *mw = (MainWindow *)user_data;
    (void)widget;

    /* Clear to dark grey */
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_paint(cr);

    if (!mw->current_image) {
        /* Show welcome text */
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 18.0);
        cairo_move_to(cr, 60, 160);
        cairo_show_text(cr, "Take a screenshot to get started");
        return;
    }

    /* Scale image to fit drawing area while preserving aspect ratio */
    int iw = mw->current_image->width;
    int ih = mw->current_image->height;

    int cw, ch;
#ifdef SNAPX_USE_GTK4
    cw = gtk_widget_get_width(GTK_WIDGET(g_main_win.drawing_area));
    ch = gtk_widget_get_height(GTK_WIDGET(g_main_win.drawing_area));
#else
    GtkAllocation alloc;
    gtk_widget_get_allocation(g_main_win.drawing_area, &alloc);
    cw = alloc.width; ch = alloc.height;
#endif

    double sx = (double)cw / iw;
    double sy = (double)ch / ih;
    double scale = (sx < sy) ? sx : sy;
    if (scale > 1.0) scale = 1.0;  /* Don't upscale */

    double draw_w = iw * scale;
    double draw_h = ih * scale;
    double ox = ((double)cw - draw_w) / 2.0;
    double oy = ((double)ch - draw_h) / 2.0;

    /* Render from the pre-converted ARGB32 display surface */
    cairo_surface_t *surface = mw->display_surface;
    if (!surface) return;

    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    /* Overlay annotation drawings */
    if (mw->canvas) {
        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, scale, scale);
        snapx_canvas_render(mw->canvas, cr);
        cairo_restore(cr);
    }
}

/* ─── Capture helpers ────────────────────────────────────────────────────── */

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
    /* On native Wayland we pass "" — portal handles it without association.
     * A proper async handle via gdk_wayland_toplevel_export_handle would be
     * ideal but requires a full async flow we keep simple here. */
    snapx_capture_wayland_set_parent_window(mw->backend, parent);
}

static void set_statusbar(MainWindow *mw, const char *msg)
{
    if (mw->statusbar)
        gtk_label_set_text(GTK_LABEL(mw->statusbar), msg);
}

static void do_capture(MainWindow *mw, SnapxCaptureMode mode, int delay)
{
    /* Tell Wayland backend our window handle for the portal parent */
    update_portal_parent(mw);

    /* Hide our own window before capturing */
    gtk_widget_set_visible(GTK_WIDGET(mw->win), FALSE);

    /* Flush pending redraws so window disappears */
    while (g_main_context_iteration(NULL, FALSE)) {}

    SnapxCaptureRequest req = {0};
    req.mode           = mode;
    req.delay_sec      = delay;
    req.monitor_index  = -1;
    req.include_cursor = mw->config->show_cursor;

    SnapxImage *img = snapx_capture(mw->backend, &req);

    gtk_widget_set_visible(GTK_WIDGET(mw->win), TRUE);
    gtk_window_present(GTK_WINDOW(mw->win));

    if (!img) {
        set_statusbar(mw, "Capture failed.");
        return;
    }

    /* Replace current image */
    snapx_image_free(mw->current_image);
    mw->current_image = img;
    update_display_surface(mw, img);

    /* Reset annotation canvas */
    if (mw->canvas) {
        snapx_canvas_free(mw->canvas);
    }
    mw->canvas = snapx_canvas_new(img->width, img->height);

    gtk_widget_queue_draw(mw->drawing_area);
    set_statusbar(mw, "Screenshot captured. Annotate or save.");

    /* Persist the Wayland restore_token so next capture is silent */
    snapx_capture_wayland_save_token(mw->backend);

    /* Auto-copy if configured */
    if (mw->config->auto_clipboard)
        snapx_clipboard_copy_image(img);
}

/* ─── Button callbacks ───────────────────────────────────────────────────── */

static void on_capture_fullscreen(GtkButton *btn, gpointer data)
{
    (void)btn;
    do_capture((MainWindow *)data, SNAPX_CAPTURE_FULLSCREEN, 0);
}

static void on_capture_region(GtkButton *btn, gpointer data)
{
    (void)btn;
    MainWindow *mw = (MainWindow *)data;
    /* Show the overlay for region selection */
    SnapxRegion region = {0};
    int ok = snapx_overlay_select_region(GTK_WINDOW(mw->win), &region);
    if (!ok) return;

    SnapxCaptureRequest req = {0};
    req.mode           = SNAPX_CAPTURE_REGION;
    req.delay_sec      = 0;
    req.region_x       = region.x;
    req.region_y       = region.y;
    req.region_w       = region.width;
    req.region_h       = region.height;
    req.include_cursor = mw->config->show_cursor;

    gtk_widget_set_visible(GTK_WIDGET(mw->win), FALSE);
    while (g_main_context_iteration(NULL, FALSE)) {}

    SnapxImage *img = snapx_capture(mw->backend, &req);

    gtk_widget_set_visible(GTK_WIDGET(mw->win), TRUE);
    gtk_window_present(GTK_WINDOW(mw->win));

    if (!img) { set_statusbar(mw, "Region capture failed."); return; }

    snapx_image_free(mw->current_image);
    mw->current_image = img;
    update_display_surface(mw, img);
    if (mw->canvas) snapx_canvas_free(mw->canvas);
    mw->canvas = snapx_canvas_new(img->width, img->height);
    gtk_widget_queue_draw(mw->drawing_area);
    set_statusbar(mw, "Region captured.");
}

static void on_capture_window(GtkButton *btn, gpointer data)
{
    (void)btn;
    do_capture((MainWindow *)data, SNAPX_CAPTURE_ACTIVE_WINDOW, 0);
}

static void on_copy_clipboard(GtkButton *btn, gpointer data)
{
    (void)btn;
    MainWindow *mw = (MainWindow *)data;
    if (!mw->current_image) { set_statusbar(mw, "Nothing to copy."); return; }
    snapx_clipboard_copy_image(mw->current_image);
    set_statusbar(mw, "Copied to clipboard.");
}

static void on_save(GtkButton *btn, gpointer data)
{
    (void)btn;
    MainWindow *mw = (MainWindow *)data;
    if (!mw->current_image) { set_statusbar(mw, "Nothing to save."); return; }

    /* Flatten annotation onto image before saving */
    SnapxImage *flat = snapx_canvas_flatten(mw->canvas, mw->current_image);

    /* Build output path from config pattern */
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
    int quality = mw->config->jpeg_quality;
    if (mw->quality_scale)
        quality = (int)gtk_range_get_value(GTK_RANGE(mw->quality_scale));

    int ret = snapx_image_save(flat ? flat : mw->current_image, path, fmt, quality);
    snapx_image_free(flat);

    if (ret == 0) {
        char msg[600];
        snprintf(msg, sizeof(msg), "Saved: %s", path);
        set_statusbar(mw, msg);
    } else {
        set_statusbar(mw, "Save failed — check permissions / disk space.");
    }
}

static void on_settings(GtkButton *btn, gpointer data)
{
    (void)btn;
    MainWindow *mw = (MainWindow *)data;
    snapx_settings_dialog_show(GTK_WINDOW(mw->win), mw->config);
}

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
        gboolean vis = (active == (int)SNAPX_FORMAT_JPEG);
        gtk_widget_set_visible(mw->quality_scale, vis);
        gtk_widget_set_visible(mw->quality_label, vis);
    }
}

/* ─── Keyboard shortcuts ─────────────────────────────────────────────────── */

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
    if (ctrl_mod && keyval == GDK_KEY_z && mw->canvas) {
        snapx_canvas_undo(mw->canvas); gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (ctrl_mod && keyval == GDK_KEY_y && mw->canvas) {
        snapx_canvas_redo(mw->canvas); gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
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
    if (ctrl && ev->keyval == GDK_KEY_z && mw->canvas) {
        snapx_canvas_undo(mw->canvas); gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    if (ctrl && ev->keyval == GDK_KEY_y && mw->canvas) {
        snapx_canvas_redo(mw->canvas); gtk_widget_queue_draw(mw->drawing_area); return TRUE;
    }
    return FALSE;
}
#endif

/* ─── Window construction ────────────────────────────────────────────────── */

void snapx_window_main_create(GtkApplication      *app,
                               SnapxConfig          *config,
                               SnapxCaptureBackend  *backend,
                               SnapxCaptureMode     *mode)
{
    MainWindow *mw = &g_main_win;
    memset(mw, 0, sizeof(*mw));
    mw->config       = config;
    mw->backend      = backend;
    mw->default_mode = mode;

    /* ── Root window ─────────────────────────────────────────────────────── */
#ifdef SNAPX_USE_GTK4
    GtkWidget *win_widget = gtk_application_window_new(app);
    mw->win = GTK_APPLICATION_WINDOW(win_widget);
    gtk_window_set_title(GTK_WINDOW(win_widget), "snapx");
    gtk_window_set_default_size(GTK_WINDOW(win_widget), 960, 640);
#else
    GtkWidget *win_widget = gtk_application_window_new(app);
    mw->win = GTK_APPLICATION_WINDOW(win_widget);
    gtk_window_set_title(GTK_WINDOW(win_widget), "snapx");
    gtk_window_set_default_size(GTK_WINDOW(win_widget), 960, 640);
#endif

    /* ── Header bar ──────────────────────────────────────────────────────── */
    GtkWidget *header = gtk_header_bar_new();
#ifdef SNAPX_USE_GTK4
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
#else
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(header), ":minimize,maximize,close");
#endif
    gtk_window_set_titlebar(GTK_WINDOW(win_widget), header);

    /* Capture mode buttons in header */
    mw->btn_fullscreen = gtk_button_new_with_label("⬛ Screen");
    mw->btn_region     = gtk_button_new_with_label("✂ Region");
    mw->btn_window     = gtk_button_new_with_label("🪟 Window");
    mw->btn_settings   = gtk_button_new_with_label("⚙");

#ifdef SNAPX_USE_GTK4
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_fullscreen);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_region);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_window);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->btn_settings);
#else
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_fullscreen);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_region);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mw->btn_window);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mw->btn_settings);
#endif

    g_signal_connect(mw->btn_fullscreen, "clicked", G_CALLBACK(on_capture_fullscreen), mw);
    g_signal_connect(mw->btn_region,     "clicked", G_CALLBACK(on_capture_region),     mw);
    g_signal_connect(mw->btn_window,     "clicked", G_CALLBACK(on_capture_window),      mw);
    g_signal_connect(mw->btn_settings,   "clicked", G_CALLBACK(on_settings),            mw);

    /* ── Main vertical box ───────────────────────────────────────────────── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#ifdef SNAPX_USE_GTK4
    gtk_window_set_child(GTK_WINDOW(win_widget), vbox);
#else
    gtk_container_add(GTK_CONTAINER(win_widget), vbox);
#endif

    /* ── Annotation toolbar ──────────────────────────────────────────────── */
    mw->toolbar_box = snapx_toolbar_create(mw->canvas,
                                            mw->drawing_area);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(vbox), mw->toolbar_box);
#else
    gtk_box_pack_start(GTK_BOX(vbox), mw->toolbar_box, FALSE, FALSE, 0);
#endif

    /* ── Scrolled window + drawing area ─────────────────────────────────── */
    GtkWidget *scroll = gtk_scrolled_window_new();
#ifndef SNAPX_USE_GTK4
    scroll = gtk_scrolled_window_new(NULL, NULL);
#endif
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(vbox), scroll);
#else
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
#endif

#ifdef SNAPX_USE_GTK4
    mw->drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(mw->drawing_area),
                                   (GtkDrawingAreaDrawFunc)on_draw, mw, NULL);
    gtk_widget_set_size_request(mw->drawing_area, 400, 300);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), mw->drawing_area);
#else
    mw->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(mw->drawing_area, 400, 300);
    g_signal_connect(mw->drawing_area, "draw", G_CALLBACK(on_draw_gtk3), mw);
    gtk_container_add(GTK_CONTAINER(scroll), mw->drawing_area);
#endif

    /* Mouse events for annotation on drawing area */
    snapx_toolbar_connect_canvas_events(mw->drawing_area, mw->canvas);

    /* ── Bottom action bar ───────────────────────────────────────────────── */
    GtkWidget *action_bar;
#ifdef SNAPX_USE_GTK4
    action_bar = gtk_action_bar_new();
    gtk_box_append(GTK_BOX(vbox), action_bar);
#else
    action_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(action_bar, 6);
    gtk_widget_set_margin_end(action_bar, 6);
    gtk_widget_set_margin_top(action_bar, 4);
    gtk_widget_set_margin_bottom(action_bar, 4);
    gtk_box_pack_end(GTK_BOX(vbox), action_bar, FALSE, FALSE, 0);
#endif

    /* Format selector — use GtkDropDown (GTK4 modern) */
    GtkWidget *fmt_label = gtk_label_new("Format:");
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

    /* Quality slider (JPEG only) */
    mw->quality_label = gtk_label_new("Quality:");
    mw->quality_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                  1, 100, 1);
    gtk_range_set_value(GTK_RANGE(mw->quality_scale), (double)config->jpeg_quality);
    gtk_widget_set_size_request(mw->quality_scale, 120, -1);
    gtk_widget_set_visible(mw->quality_scale,
                           config->default_format == SNAPX_FORMAT_JPEG);
    gtk_widget_set_visible(mw->quality_label,
                           config->default_format == SNAPX_FORMAT_JPEG);

    /* Copy / Save buttons */
    GtkWidget *btn_copy = gtk_button_new_with_label("📋 Copy");
    GtkWidget *btn_save = gtk_button_new_with_label("💾 Save");
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
    gtk_box_pack_start(GTK_BOX(action_bar), fmt_label,          FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(action_bar), mw->format_combo,   FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(action_bar), mw->quality_label,  FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(action_bar), mw->quality_scale,  FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_save,           FALSE, FALSE, 4);
    gtk_box_pack_end  (GTK_BOX(action_bar), btn_copy,           FALSE, FALSE, 4);
#endif

    /* ── Status label ────────────────────────────────────────────────────── */
    mw->statusbar = gtk_label_new("Ready. Click a capture button to get started.");
    gtk_widget_set_margin_start(mw->statusbar, 8);
    gtk_widget_set_margin_bottom(mw->statusbar, 4);
    gtk_widget_set_halign(mw->statusbar, GTK_ALIGN_START);
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(vbox), mw->statusbar);
#else
    gtk_box_pack_end(GTK_BOX(vbox), mw->statusbar, FALSE, FALSE, 0);
#endif

    /* ── Keyboard shortcuts ──────────────────────────────────────────────── */
#ifdef SNAPX_USE_GTK4
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), mw);
    gtk_widget_add_controller(win_widget, key_ctrl);
#else
    g_signal_connect(win_widget, "key-press-event", G_CALLBACK(on_key_press_gtk3), mw);
#endif

    gtk_widget_set_visible(win_widget, TRUE);
}

void snapx_window_main_set_image(SnapxImage *img)
{
    MainWindow *mw = &g_main_win;
    snapx_image_free(mw->current_image);
    mw->current_image = img;
    update_display_surface(mw, img);
    if (mw->canvas) snapx_canvas_free(mw->canvas);
    mw->canvas = img ? snapx_canvas_new(img->width, img->height) : NULL;
    if (mw->drawing_area)
        gtk_widget_queue_draw(mw->drawing_area);
}

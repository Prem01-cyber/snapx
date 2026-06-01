/**
 * @file beautify_dialog.c
 * @brief Live-preview editor for the beautiful-screenshot compositor.
 */

#include "beautify_dialog.h"
#include "../output/beautify.h"
#include "../output/save.h"
#include "../utils/config.h"

#include <cairo/cairo.h>
#include <string.h>
#include <stdio.h>

/* ─── Dialog state ───────────────────────────────────────────────────────── */

typedef struct {
    SnapxConfig     *config;
    SnapxImage      *flat;          /**< Owned copy of the source image          */
    GtkWindow       *win;

    GtkWidget       *preview;
    cairo_surface_t *preview_surf;  /**< Composited result, ARGB32               */

    GtkWidget       *status;

    /* Controls */
    GtkWidget       *sw_apply;      /**< "apply on export" switch / toggle       */
    GtkWidget       *sc_padding;
    GtkWidget       *dd_bg;
    GtkWidget       *btn_color1;
    GtkWidget       *btn_color2;
    GtkWidget       *sc_corner;
    GtkWidget       *chk_shadow;
    GtkWidget       *sc_shadow;

    gboolean         building;      /**< Suppress callbacks during construction  */
} BeautifyDialog;

/* ─── Small cross-version helpers ────────────────────────────────────────── */

static void box_add(GtkWidget *box, GtkWidget *child)
{
#ifdef SNAPX_USE_GTK4
    gtk_box_append(GTK_BOX(box), child);
#else
    gtk_box_pack_start(GTK_BOX(box), child, FALSE, FALSE, 2);
#endif
}

static void read_color(GtkWidget *btn, double *r, double *g, double *b)
{
    GdkRGBA c = { 0, 0, 0, 1 };
#if GTK_CHECK_VERSION(4, 10, 0)
    const GdkRGBA *cc = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(btn));
    if (cc) c = *cc;
#else
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
#endif
    *r = c.red; *g = c.green; *b = c.blue;
}

static GtkWidget *make_color_button(double r, double g, double b, const char *title)
{
    GdkRGBA c = { r, g, b, 1.0 };
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkColorDialog *cd = gtk_color_dialog_new();
    gtk_color_dialog_set_title(cd, title);
    gtk_color_dialog_set_with_alpha(cd, FALSE);
    GtkWidget *btn = gtk_color_dialog_button_new(cd);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(btn), &c);
    return btn;
#else
    GtkWidget *btn = gtk_color_button_new_with_rgba(&c);
    gtk_color_button_set_title(GTK_COLOR_BUTTON(btn), title);
    return btn;
#endif
}

/* ─── Convert a SnapxImage (RGBA) to a premultiplied ARGB32 surface ──────── */

static cairo_surface_t *image_to_argb32(const SnapxImage *img)
{
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img->width, img->height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_surface_flush(surf);
    unsigned char *dst    = cairo_image_surface_get_data(surf);
    int            stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < img->height; y++) {
        const uint8_t *s = img->data + (size_t)y * img->stride;
        uint32_t      *d = (uint32_t *)(dst + (size_t)y * stride);
        for (int x = 0; x < img->width; x++) {
            uint8_t r = s[x*4+0], g = s[x*4+1], b = s[x*4+2], a = s[x*4+3];
            if (a == 0xFF) {
                d[x] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
            } else {
                uint32_t rm = (r*a+127)/255, gm = (g*a+127)/255, bm = (b*a+127)/255;
                d[x] = ((uint32_t)a<<24) | (rm<<16) | (gm<<8) | bm;
            }
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

/* ─── Preview rendering ──────────────────────────────────────────────────── */

/** Build a beautify config from the dialog controls; @p force_enabled makes the
 *  preview composite even when "apply on export" is off. */
static SnapxBeautifyConfig dialog_beautify_cfg(BeautifyDialog *d, int force_enabled)
{
    SnapxBeautifyConfig c = d->config->beautify;
    if (force_enabled) c.enabled = 1;
    return c;
}

static void rebuild_preview(BeautifyDialog *d)
{
    if (d->preview_surf) {
        cairo_surface_destroy(d->preview_surf);
        d->preview_surf = NULL;
    }
    SnapxBeautifyConfig cfg = dialog_beautify_cfg(d, 1);
    SnapxImage *b = snapx_beautify_apply(d->flat, &cfg);
    if (b) {
        d->preview_surf = image_to_argb32(b);
        snapx_image_free(b);
    }
    if (d->preview) gtk_widget_queue_draw(d->preview);
}

#ifdef SNAPX_USE_GTK4
static void on_preview_draw(GtkDrawingArea *da, cairo_t *cr,
                            int w, int h, gpointer data)
{
    (void)da;
    BeautifyDialog *d = data;
#else
static gboolean on_preview_draw_gtk3(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    BeautifyDialog *d = data;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
#endif
    /* Checkerboard so transparent padding is visible. */
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.13);
    cairo_paint(cr);
    const int cs = 10;
    for (int y = 0; y < h; y += cs)
        for (int x = 0; x < w; x += cs)
            if (((x / cs) + (y / cs)) & 1) {
                cairo_set_source_rgb(cr, 0.16, 0.16, 0.17);
                cairo_rectangle(cr, x, y, cs, cs);
                cairo_fill(cr);
            }

    if (d->preview_surf) {
        int iw = cairo_image_surface_get_width(d->preview_surf);
        int ih = cairo_image_surface_get_height(d->preview_surf);
        double sx = (double)w / iw, sy = (double)h / ih;
        double scale = sx < sy ? sx : sy;
        if (scale > 1.0) scale = 1.0;
        double ox = (w - iw * scale) / 2.0;
        double oy = (h - ih * scale) / 2.0;
        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, d->preview_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    }
#ifndef SNAPX_USE_GTK4
    return FALSE;
#endif
}

/* ─── Control sync ───────────────────────────────────────────────────────── */

static void update_sensitivity(BeautifyDialog *d)
{
    int bg = d->config->beautify.bg_type;
    gtk_widget_set_sensitive(d->btn_color2, bg == SNAPX_BG_GRADIENT);
    gtk_widget_set_sensitive(d->btn_color1, bg != SNAPX_BG_TRANSPARENT);
    gtk_widget_set_sensitive(d->sc_shadow, d->config->beautify.shadow);
}

static void sync_from_controls(BeautifyDialog *d)
{
    if (d->building) return;
    SnapxBeautifyConfig *b = &d->config->beautify;

    b->padding = (int)gtk_range_get_value(GTK_RANGE(d->sc_padding));
    b->corner_radius = (int)gtk_range_get_value(GTK_RANGE(d->sc_corner));
    b->shadow_size = (int)gtk_range_get_value(GTK_RANGE(d->sc_shadow));

#ifdef SNAPX_USE_GTK4
    b->bg_type = (SnapxBeautifyBg)gtk_drop_down_get_selected(GTK_DROP_DOWN(d->dd_bg));
    b->shadow  = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->chk_shadow));
    b->enabled = gtk_switch_get_active(GTK_SWITCH(d->sw_apply));
#else
    b->bg_type = (SnapxBeautifyBg)gtk_combo_box_get_active(GTK_COMBO_BOX(d->dd_bg));
    b->shadow  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->chk_shadow));
    b->enabled = gtk_switch_get_active(GTK_SWITCH(d->sw_apply));
#endif
    read_color(d->btn_color1, &b->bg_r,  &b->bg_g,  &b->bg_b);
    read_color(d->btn_color2, &b->bg_r2, &b->bg_g2, &b->bg_b2);

    update_sensitivity(d);
    rebuild_preview(d);
}

static void on_control_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    sync_from_controls((BeautifyDialog *)data);
}

static void on_color_changed(GObject *o, GParamSpec *p, gpointer data)
{
    (void)o; (void)p;
    sync_from_controls((BeautifyDialog *)data);
}

static void on_switch_changed(GObject *o, GParamSpec *p, gpointer data)
{
    (void)o; (void)p;
    sync_from_controls((BeautifyDialog *)data);
}

/* ─── Save / copy ────────────────────────────────────────────────────────── */

static void set_status(BeautifyDialog *d, const char *msg)
{
    if (d->status) gtk_label_set_text(GTK_LABEL(d->status), msg);
}

static void on_copy(GtkButton *b, gpointer data)
{
    (void)b;
    BeautifyDialog *d = data;
    SnapxBeautifyConfig cfg = dialog_beautify_cfg(d, 1);
    SnapxImage *img = snapx_beautify_apply(d->flat, &cfg);
    if (!img) { set_status(d, "Could not compose image."); return; }
    snapx_clipboard_copy_image(img);
    snapx_image_free(img);
    set_status(d, "Copied beautified image to clipboard.");
}

static void on_save(GtkButton *b, gpointer data)
{
    (void)b;
    BeautifyDialog *d = data;
    SnapxBeautifyConfig cfg = dialog_beautify_cfg(d, 1);
    SnapxImage *img = snapx_beautify_apply(d->flat, &cfg);
    if (!img) { set_status(d, "Could not compose image."); return; }

    /* Transparent padding needs an alpha-capable format. */
    SnapxOutputFormat fmt = d->config->default_format;
    if (cfg.bg_type == SNAPX_BG_TRANSPARENT && fmt == SNAPX_FORMAT_JPEG)
        fmt = SNAPX_FORMAT_PNG;

    char path[SNAPX_CONFIG_MAX_PATH];
    snapx_config_build_path(d->config, fmt, path, sizeof(path));
    int ret = snapx_image_save(img, path, fmt, d->config->jpeg_quality);
    snapx_image_free(img);

    char msg[600];
    if (ret == 0) snprintf(msg, sizeof(msg), "Saved: %s", path);
    else          snprintf(msg, sizeof(msg), "Save failed: %s", path);
    set_status(d, msg);
}

/* ─── Lifecycle ──────────────────────────────────────────────────────────── */

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    BeautifyDialog *d = data;
    snapx_config_save(d->config);
    if (d->preview_surf) cairo_surface_destroy(d->preview_surf);
    snapx_image_free(d->flat);
    g_free(d);
}

static GtkWidget *labeled_row(const char *label, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_size_request(l, 110, -1);
    box_add(row, l);
    gtk_widget_set_hexpand(control, TRUE);
    box_add(row, control);
    return row;
}

void snapx_beautify_dialog_show(GtkWindow *parent, SnapxConfig *config,
                                const SnapxImage *image)
{
    if (!config || !image || !image->data) return;

    BeautifyDialog *d = g_new0(BeautifyDialog, 1);
    d->config = config;
    d->building = TRUE;

    /* Own a private copy of the image. */
    d->flat = snapx_image_alloc(image->width, image->height);
    if (!d->flat) { g_free(d); return; }
    d->flat->scale = image->scale;
    for (int y = 0; y < image->height; y++)
        memcpy(d->flat->data + (size_t)y * d->flat->stride,
               image->data  + (size_t)y * image->stride,
               (size_t)image->width * 4);

#ifdef SNAPX_USE_GTK4
    GtkWidget *win = gtk_window_new();
#else
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#endif
    d->win = GTK_WINDOW(win);
    gtk_window_set_title(GTK_WINDOW(win), "Beautify screenshot");
    gtk_window_set_default_size(GTK_WINDOW(win), 880, 560);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);
        gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    }

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 12);   gtk_widget_set_margin_bottom(root, 12);
#ifdef SNAPX_USE_GTK4
    gtk_window_set_child(GTK_WINDOW(win), root);
#else
    gtk_container_add(GTK_CONTAINER(win), root);
#endif

    /* ── Preview area ─────────────────────────────────────────────────────── */
    d->preview = gtk_drawing_area_new();
    gtk_widget_set_hexpand(d->preview, TRUE);
    gtk_widget_set_vexpand(d->preview, TRUE);
    gtk_widget_set_size_request(d->preview, 420, 360);
#ifdef SNAPX_USE_GTK4
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->preview),
                                   (GtkDrawingAreaDrawFunc)on_preview_draw, d, NULL);
#else
    g_signal_connect(d->preview, "draw", G_CALLBACK(on_preview_draw_gtk3), d);
#endif
    box_add(root, d->preview);

    /* ── Controls column ──────────────────────────────────────────────────── */
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(col, 300, -1);
    box_add(root, col);

    /* Apply-on-export switch */
    GtkWidget *apply_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *apply_lbl = gtk_label_new("Apply on Save/Copy/Upload");
    gtk_label_set_xalign(GTK_LABEL(apply_lbl), 0.0f);
    gtk_widget_set_hexpand(apply_lbl, TRUE);
    d->sw_apply = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(d->sw_apply), config->beautify.enabled);
    gtk_widget_set_halign(d->sw_apply, GTK_ALIGN_END);
    box_add(apply_row, apply_lbl);
    box_add(apply_row, d->sw_apply);
    box_add(col, apply_row);

    /* Padding */
    d->sc_padding = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 240, 4);
    gtk_range_set_value(GTK_RANGE(d->sc_padding), config->beautify.padding);
    gtk_scale_set_draw_value(GTK_SCALE(d->sc_padding), TRUE);
    box_add(col, labeled_row("Padding", d->sc_padding));

    /* Background type */
    const char *bg_items[] = { "Solid", "Gradient", "Transparent", NULL };
#ifdef SNAPX_USE_GTK4
    d->dd_bg = gtk_drop_down_new_from_strings(bg_items);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->dd_bg), (guint)config->beautify.bg_type);
#else
    d->dd_bg = gtk_combo_box_text_new();
    for (int i = 0; bg_items[i]; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->dd_bg), bg_items[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->dd_bg), (int)config->beautify.bg_type);
#endif
    box_add(col, labeled_row("Background", d->dd_bg));

    /* Colours */
    d->btn_color1 = make_color_button(config->beautify.bg_r, config->beautify.bg_g,
                                      config->beautify.bg_b, "Background colour");
    box_add(col, labeled_row("Colour", d->btn_color1));
    d->btn_color2 = make_color_button(config->beautify.bg_r2, config->beautify.bg_g2,
                                      config->beautify.bg_b2, "Gradient end colour");
    box_add(col, labeled_row("Gradient end", d->btn_color2));

    /* Corner radius */
    d->sc_corner = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 64, 1);
    gtk_range_set_value(GTK_RANGE(d->sc_corner), config->beautify.corner_radius);
    gtk_scale_set_draw_value(GTK_SCALE(d->sc_corner), TRUE);
    box_add(col, labeled_row("Corner radius", d->sc_corner));

    /* Shadow */
#ifdef SNAPX_USE_GTK4
    d->chk_shadow = gtk_check_button_new_with_label("Drop shadow");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->chk_shadow), config->beautify.shadow);
#else
    d->chk_shadow = gtk_check_button_new_with_label("Drop shadow");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->chk_shadow), config->beautify.shadow);
#endif
    box_add(col, d->chk_shadow);
    d->sc_shadow = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 2, 80, 1);
    gtk_range_set_value(GTK_RANGE(d->sc_shadow), config->beautify.shadow_size);
    gtk_scale_set_draw_value(GTK_SCALE(d->sc_shadow), TRUE);
    box_add(col, labeled_row("Shadow size", d->sc_shadow));

    /* Spacer + status */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    box_add(col, spacer);
    d->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->status), 0.0f);
#ifdef SNAPX_USE_GTK4
    gtk_label_set_wrap(GTK_LABEL(d->status), TRUE);
#else
    gtk_label_set_line_wrap(GTK_LABEL(d->status), TRUE);
#endif
    box_add(col, d->status);

    /* Action buttons */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_copy = gtk_button_new_with_label("Copy");
    GtkWidget *btn_save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(btn_save, "suggested-action");
    gtk_widget_set_hexpand(btn_copy, TRUE);
    gtk_widget_set_hexpand(btn_save, TRUE);
    box_add(btn_row, btn_copy);
    box_add(btn_row, btn_save);
    box_add(col, btn_row);

    /* ── Signals ──────────────────────────────────────────────────────────── */
    g_signal_connect(d->sc_padding, "value-changed", G_CALLBACK(on_control_changed), d);
    g_signal_connect(d->sc_corner,  "value-changed", G_CALLBACK(on_control_changed), d);
    g_signal_connect(d->sc_shadow,  "value-changed", G_CALLBACK(on_control_changed), d);
    g_signal_connect(d->chk_shadow, "toggled",       G_CALLBACK(on_control_changed), d);
    g_signal_connect(d->sw_apply,   "notify::active", G_CALLBACK(on_switch_changed), d);
#ifdef SNAPX_USE_GTK4
    g_signal_connect(d->dd_bg, "notify::selected", G_CALLBACK(on_color_changed), d);
#else
    g_signal_connect(d->dd_bg, "changed", G_CALLBACK(on_control_changed), d);
#endif
#if GTK_CHECK_VERSION(4, 10, 0)
    g_signal_connect(d->btn_color1, "notify::rgba", G_CALLBACK(on_color_changed), d);
    g_signal_connect(d->btn_color2, "notify::rgba", G_CALLBACK(on_color_changed), d);
#else
    g_signal_connect(d->btn_color1, "color-set", G_CALLBACK(on_control_changed), d);
    g_signal_connect(d->btn_color2, "color-set", G_CALLBACK(on_control_changed), d);
#endif
    g_signal_connect(btn_copy, "clicked", G_CALLBACK(on_copy), d);
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_save), d);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), d);

    d->building = FALSE;
    update_sensitivity(d);
    rebuild_preview(d);

    gtk_widget_set_visible(win, TRUE);
}

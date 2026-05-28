/**
 * @file overlay.c
 * @brief Full-screen freeze-frame region-selection overlay.
 *
 * How it works
 * ────────────
 * 1.  The caller captures the full virtual desktop BEFORE showing the overlay.
 * 2.  The overlay window is made fullscreen; the background screenshot is
 *     painted scaled-to-fit on every frame.
 * 3.  A semi-transparent dim layer is painted on top.
 * 4.  The selected area is "punched through" so the screenshot shows clearly.
 * 5.  On confirmation, the selected rectangle is mapped back from widget
 *     coordinates to image-pixel coordinates (accounting for scale + offset).
 * 6.  The caller crops the full screenshot to that rectangle.
 *
 * This approach is reliable on Wayland and all multi-monitor configurations:
 *  • No XGetImage of the root window (which returns black on Xwayland).
 *  • No dependency on the overlay window spanning multiple physical monitors.
 *  • The user always sees accurate screen content (not a live/dim overlay).
 *
 * Visual design
 * ─────────────
 *  • Screenshot background (scaled-to-fit) + 48% dim.
 *  • Selected area clears the dim so the real content is vivid.
 *  • 2px blue border + corner & edge handles on selection.
 *  • Dimension badge (width × height) in a pill above the selection.
 *  • Live cursor coordinates badge before dragging starts.
 *  • Crosshair lines before drag starts.
 *  • Bottom-of-screen hint: "Drag to select  |  Enter = confirm  |  Esc = cancel"
 */

#include "overlay.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

/* ─── State ──────────────────────────────────────────────────────────────── */

typedef struct {
    GtkWidget  *win;
    GMainLoop  *loop;
    SnapxRegion result;
    gboolean    confirmed;

    /* Drag state (in widget pixels) */
    gboolean    dragging;
    double      start_x, start_y;
    double      cur_x,   cur_y;
    double      mouse_x, mouse_y;

    /* Background screenshot (never modified) */
    const SnapxImage    *bg;
    cairo_surface_t     *bg_surf;   /**< ARGB32 surface from bg               */

    /* Viewport: how bg is scaled into the window */
    double   bg_scale;   /**< scale factor applied to bg image                */
    double   bg_ox;      /**< x offset of scaled bg inside the window         */
    double   bg_oy;      /**< y offset of scaled bg inside the window         */
    int      win_w;      /**< last known window width                          */
    int      win_h;      /**< last known window height                         */
} OverlayState;

/* ─── RGBA → Cairo ARGB32 (same conversion as window_main.c) ─────────────── */

static cairo_surface_t *image_to_cairo(const SnapxImage *img)
{
    if (!img) return NULL;
    int w = img->width, h = img->height;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s); return NULL;
    }
    cairo_surface_flush(s);
    unsigned char *dst    = cairo_image_surface_get_data(s);
    int            stride = cairo_image_surface_get_stride(s);

    for (int y = 0; y < h; y++) {
        const uint8_t *src_row = img->data + y * img->stride;
        uint32_t      *dst_row = (uint32_t *)(dst + y * stride);
        for (int x = 0; x < w; x++) {
            uint8_t r = src_row[x*4+0], g = src_row[x*4+1],
                    b = src_row[x*4+2], a = src_row[x*4+3];
            if (a == 0xFF)
                dst_row[x] = 0xFF000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
            else {
                uint32_t rm=(r*a+127)/255, gm=(g*a+127)/255, bm=(b*a+127)/255;
                dst_row[x] = ((uint32_t)a<<24)|(rm<<16)|(gm<<8)|bm;
            }
        }
    }
    cairo_surface_mark_dirty(s);
    return s;
}

/* ─── Coordinate helpers ─────────────────────────────────────────────────── */

static void normalise(double x1, double y1, double x2, double y2,
                       double *rx, double *ry, double *rw, double *rh)
{
    *rx = (x1 < x2) ? x1 : x2;
    *ry = (y1 < y2) ? y1 : y2;
    *rw = fabs(x2 - x1);
    *rh = fabs(y2 - y1);
}

/** Map widget pixel (wx, wy) → image pixel (ix, iy). */
static void widget_to_image(const OverlayState *st,
                              double wx, double wy,
                              double *ix, double *iy)
{
    if (st->bg_scale > 0) {
        *ix = (wx - st->bg_ox) / st->bg_scale;
        *iy = (wy - st->bg_oy) / st->bg_scale;
    } else {
        *ix = wx; *iy = wy;
    }
}

/* ─── Viewport recalculation ──────────────────────────────────────────────── */

static void recalc_viewport(OverlayState *st, int win_w, int win_h)
{
    st->win_w = win_w;
    st->win_h = win_h;
    if (!st->bg || !st->bg_surf) {
        st->bg_scale = 1; st->bg_ox = st->bg_oy = 0; return;
    }
    int iw = st->bg->width, ih = st->bg->height;
    double sx = (double)win_w / iw;
    double sy = (double)win_h / ih;
    st->bg_scale = (sx < sy) ? sx : sy;
    st->bg_ox = (win_w - iw * st->bg_scale) / 2.0;
    st->bg_oy = (win_h - ih * st->bg_scale) / 2.0;
}

/* ─── Drawing helpers ─────────────────────────────────────────────────────── */

static void draw_badge(cairo_t *cr, double cx, double cy,
                        const char *text, double font_size)
{
    cairo_select_font_face(cr, "Monospace",
                            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents_t te;
    cairo_text_extents(cr, text, &te);

    double pad = 6.0, r = 5.0;
    double bx = cx - te.width/2 - pad;
    double by = cy - te.height - pad;
    double bw = te.width  + pad*2;
    double bh = te.height + pad*2;

    cairo_set_source_rgba(cr, 0.08, 0.08, 0.12, 0.85);
    cairo_move_to(cr, bx+r, by);
    cairo_line_to(cr, bx+bw-r, by); cairo_arc(cr, bx+bw-r, by+r, r, -G_PI/2, 0);
    cairo_line_to(cr, bx+bw, by+bh-r); cairo_arc(cr, bx+bw-r, by+bh-r, r, 0, G_PI/2);
    cairo_line_to(cr, bx+r, by+bh); cairo_arc(cr, bx+r, by+bh-r, r, G_PI/2, G_PI);
    cairo_line_to(cr, bx, by+r); cairo_arc(cr, bx+r, by+r, r, G_PI, 3*G_PI/2);
    cairo_close_path(cr); cairo_fill(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_move_to(cr, cx - te.width/2 - te.x_bearing,
                      cy - te.height/2 - te.y_bearing - te.height/2 + 1);
    cairo_show_text(cr, text);
}

static void draw_handle(cairo_t *cr, double x, double y)
{
    double hs = 5.0;
    cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 1.0);
    cairo_rectangle(cr, x-hs, y-hs, hs*2, hs*2);
    cairo_fill(cr);
}

/* ─── Paint callback ──────────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4
static void on_overlay_draw(GtkDrawingArea *da, cairo_t *cr,
                              int w, int h, gpointer data)
{
    (void)da;
#else
static gboolean on_overlay_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    int w, h;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    w = alloc.width; h = alloc.height;
#endif
    OverlayState *st = (OverlayState *)data;

    /* Recalculate viewport if window size changed */
    if (w != st->win_w || h != st->win_h)
        recalc_viewport(st, w, h);

    /* ── Background screenshot ───────────────────────────────────────────── */
    if (st->bg_surf) {
        /* Fill with dark colour first (letterbox bars) */
        cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
        cairo_paint(cr);

        cairo_save(cr);
        cairo_translate(cr, st->bg_ox, st->bg_oy);
        cairo_scale(cr, st->bg_scale, st->bg_scale);
        cairo_set_source_surface(cr, st->bg_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
    }

    /* ── Semi-transparent dim overlay ───────────────────────────────────── */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.48);
    cairo_paint(cr);

    if (!st->dragging) {
        /* ── Crosshair ───────────────────────────────────────────────────── */
        double cx = st->mouse_x, cy = st->mouse_y;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.30);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, cx, 0); cairo_line_to(cr, cx, h);
        cairo_move_to(cr, 0, cy); cairo_line_to(cr, w, cy);
        cairo_stroke(cr);

        /* Cursor coordinate badge */
        double ix, iy;
        widget_to_image(st, cx, cy, &ix, &iy);
        char pos[32];
        snprintf(pos, sizeof(pos), "%d, %d", (int)ix, (int)iy);
        draw_badge(cr, cx + 62, cy - 22, pos, 12.0);

        /* Bottom hint */
        draw_badge(cr, w/2.0, h - 28,
            "Drag to select   |   Enter = confirm   |   Esc = cancel", 13.0);

#ifndef SNAPX_USE_GTK4
        return FALSE;
#else
        return;
#endif
    }

    /* ── Selection rect (widget coordinates) ────────────────────────────── */
    double rx, ry, rw, rh;
    normalise(st->start_x, st->start_y, st->cur_x, st->cur_y,
              &rx, &ry, &rw, &rh);

    /* Punch through the dim — reveal the background screenshot clearly */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_fill(cr);

    /* Re-paint background in the cleared area with a vivid tint */
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (st->bg_surf) {
        cairo_save(cr);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_clip(cr);
        cairo_translate(cr, st->bg_ox, st->bg_oy);
        cairo_scale(cr, st->bg_scale, st->bg_scale);
        cairo_set_source_surface(cr, st->bg_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    /* Selection border */
    cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 0.95);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_stroke(cr);

    /* Corner + edge handles */
    draw_handle(cr, rx,         ry);
    draw_handle(cr, rx+rw,      ry);
    draw_handle(cr, rx,         ry+rh);
    draw_handle(cr, rx+rw,      ry+rh);
    draw_handle(cr, rx+rw/2,    ry);
    draw_handle(cr, rx+rw/2,    ry+rh);
    draw_handle(cr, rx,         ry+rh/2);
    draw_handle(cr, rx+rw,      ry+rh/2);

    /* Dimension badge — show image-space dimensions */
    double ix1, iy1, ix2, iy2;
    widget_to_image(st, rx,    ry,    &ix1, &iy1);
    widget_to_image(st, rx+rw, ry+rh, &ix2, &iy2);
    int img_w = (int)(ix2 - ix1), img_h = (int)(iy2 - iy1);

    char dim[48];
    snprintf(dim, sizeof(dim), "%d × %d  px", img_w, img_h);
    double badge_y = (ry > 44) ? ry - 14 : ry + rh + 36;
    draw_badge(cr, rx + rw/2.0, badge_y, dim, 13.0);

#ifndef SNAPX_USE_GTK4
    return FALSE;
#endif
}

/* ─── Confirm selection ─────────────────────────────────────────────────── */

static void confirm_selection(OverlayState *st)
{
    double rx, ry, rw, rh;
    normalise(st->start_x, st->start_y, st->cur_x, st->cur_y,
              &rx, &ry, &rw, &rh);

    /* Map widget coords → image coords */
    double ix1, iy1, ix2, iy2;
    widget_to_image(st, rx,    ry,    &ix1, &iy1);
    widget_to_image(st, rx+rw, ry+rh, &ix2, &iy2);

    int iw = (int)(ix2 - ix1), ih = (int)(iy2 - iy1);
    if (iw < 4 || ih < 4) {
        st->confirmed = FALSE;
    } else {
        st->result.x = (int)ix1; st->result.y = (int)iy1;
        st->result.width = iw;   st->result.height = ih;
        st->confirmed = TRUE;
    }
    gtk_widget_set_visible(st->win, FALSE);
    g_main_loop_quit(st->loop);
}

/* ─── Input events ───────────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4

static void on_press(GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    (void)g; (void)n;
    OverlayState *st = (OverlayState *)data;
    st->dragging = TRUE;
    st->start_x = x; st->start_y = y;
    st->cur_x   = x; st->cur_y   = y;
    gtk_widget_queue_draw(st->win);
}

static void on_release(GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    (void)g; (void)n;
    OverlayState *st = (OverlayState *)data;
    st->cur_x = x; st->cur_y = y;
    confirm_selection(st);
}

static void on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer data)
{
    (void)ctrl;
    OverlayState *st = (OverlayState *)data;
    st->mouse_x = x; st->mouse_y = y;
    if (st->dragging) { st->cur_x = x; st->cur_y = y; }
    gtk_widget_queue_draw(st->win);
}

static gboolean on_key(GtkEventControllerKey *ctrl, guint keyval, guint code,
                         GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)code; (void)state;
    OverlayState *st = (OverlayState *)data;
    if (keyval == GDK_KEY_Escape) {
        st->confirmed = FALSE;
        gtk_widget_set_visible(st->win, FALSE);
        g_main_loop_quit(st->loop);
        return TRUE;
    }
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_space) && st->dragging) {
        confirm_selection(st); return TRUE;
    }
    return FALSE;
}

#else /* GTK3 */

static gboolean on_btn_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    OverlayState *st = (OverlayState *)data;
    if (ev->button == 1) {
        st->dragging = TRUE;
        st->start_x = ev->x; st->start_y = ev->y;
        st->cur_x   = ev->x; st->cur_y   = ev->y;
        gtk_widget_queue_draw(st->win);
    }
    return TRUE;
}

static gboolean on_btn_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    OverlayState *st = (OverlayState *)data;
    if (ev->button == 1) { st->cur_x = ev->x; st->cur_y = ev->y; confirm_selection(st); }
    return TRUE;
}

static gboolean on_motion_gtk3(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    (void)w;
    OverlayState *st = (OverlayState *)data;
    st->mouse_x = ev->x; st->mouse_y = ev->y;
    if (st->dragging) { st->cur_x = ev->x; st->cur_y = ev->y; }
    gtk_widget_queue_draw(st->win);
    return TRUE;
}

static gboolean on_key_gtk3(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    OverlayState *st = (OverlayState *)data;
    if (ev->keyval == GDK_KEY_Escape) {
        st->confirmed = FALSE;
        gtk_widget_set_visible(st->win, FALSE);
        g_main_loop_quit(st->loop);
        return TRUE;
    }
    if ((ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_space) && st->dragging) {
        confirm_selection(st); return TRUE;
    }
    return FALSE;
}

#endif /* GTK3 */

/* ─── Public API ─────────────────────────────────────────────────────────── */

int snapx_overlay_select_region(GtkWindow        *parent,
                                  const SnapxImage *background,
                                  SnapxRegion      *region)
{
    OverlayState st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);
    st.bg   = background;

    /* Convert background to Cairo surface once */
    if (background)
        st.bg_surf = image_to_cairo(background);

#ifdef SNAPX_USE_GTK4
    GtkWidget *win = gtk_window_new();
#else
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#endif
    st.win = win;
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(win), parent);

#ifndef SNAPX_USE_GTK4
    GdkScreen *screen = gtk_window_get_screen(GTK_WINDOW(win));
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(win, visual);
    gtk_widget_set_app_paintable(win, TRUE);
#endif

    gtk_window_fullscreen(GTK_WINDOW(win));

#ifdef SNAPX_USE_GTK4
    GtkWidget *da = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da),
                                    (GtkDrawingAreaDrawFunc)on_overlay_draw,
                                    &st, NULL);
    gtk_window_set_child(GTK_WINDOW(win), da);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed",  G_CALLBACK(on_press),   &st);
    g_signal_connect(click, "released", G_CALLBACK(on_release),  &st);
    gtk_widget_add_controller(da, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), &st);
    gtk_widget_add_controller(da, motion);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), &st);
    gtk_widget_add_controller(win, key);
    gtk_widget_set_focusable(win, TRUE);
#else
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_events(da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                              | GDK_POINTER_MOTION_MASK);
    g_signal_connect(da,  "draw",                 G_CALLBACK(on_overlay_draw),  &st);
    g_signal_connect(da,  "button-press-event",   G_CALLBACK(on_btn_press),     &st);
    g_signal_connect(da,  "button-release-event", G_CALLBACK(on_btn_release),   &st);
    g_signal_connect(da,  "motion-notify-event",  G_CALLBACK(on_motion_gtk3),   &st);
    g_signal_connect(win, "key-press-event",       G_CALLBACK(on_key_gtk3),     &st);
    gtk_container_add(GTK_CONTAINER(win), da);
    gtk_widget_show_all(win);
#endif

    GdkCursor *cursor = gdk_cursor_new_from_name("crosshair", NULL);
    if (cursor) {
#ifdef SNAPX_USE_GTK4
        gtk_widget_set_cursor(win, cursor);
#else
        gdk_window_set_cursor(gtk_widget_get_window(win), cursor);
#endif
        g_object_unref(cursor);
    }

    gtk_widget_set_visible(win, TRUE);
    gtk_widget_grab_focus(win);

    g_main_loop_run(st.loop);
    g_main_loop_unref(st.loop);
    gtk_window_destroy(GTK_WINDOW(win));

    if (st.bg_surf) cairo_surface_destroy(st.bg_surf);

    if (st.confirmed && region) { *region = st.result; return 1; }
    return 0;
}

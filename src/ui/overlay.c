/**
 * @file overlay.c
 * @brief Full-screen region-selection overlay.
 *
 * Visual design
 * ─────────────
 *  • 45% black tint over the whole screen.
 *  • Selected area is "punched through" so the screen shows clearly.
 *  • Blue 2px border + corner handle squares on the selection.
 *  • Dimension badge (width × height) with a dark pill background.
 *  • Live cursor position shown in the top-left corner.
 *  • Crosshair lines follow the cursor before a drag starts.
 *  • Hint text while idle; confirmation: release mouse or press Enter.
 *  • Escape always cancels.
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
    gboolean    dragging;
    double      start_x, start_y;
    double      cur_x,   cur_y;
    double      mouse_x, mouse_y;  /**< live cursor position (always updated) */
} OverlayState;

/* ─── Geometry helpers ───────────────────────────────────────────────────── */

static void normalise(double x1, double y1, double x2, double y2,
                       double *rx, double *ry, double *rw, double *rh)
{
    *rx = (x1 < x2) ? x1 : x2;
    *ry = (y1 < y2) ? y1 : y2;
    *rw = fabs(x2 - x1);
    *rh = fabs(y2 - y1);
}

/* Draw a pill-shaped label with dark background */
static void draw_badge(cairo_t *cr, double cx, double cy,
                        const char *text, double font_size)
{
    cairo_select_font_face(cr, "Monospace",
                            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents_t te;
    cairo_text_extents(cr, text, &te);

    double pad = 6.0, r = 5.0;
    double bx = cx - te.width / 2 - pad;
    double by = cy - te.height - pad;
    double bw = te.width  + pad * 2;
    double bh = te.height + pad * 2;

    /* Pill background */
    cairo_set_source_rgba(cr, 0.08, 0.08, 0.12, 0.82);
    cairo_move_to(cr, bx + r, by);
    cairo_line_to(cr, bx + bw - r, by);
    cairo_arc(cr, bx + bw - r, by + r, r, -G_PI/2, 0);
    cairo_line_to(cr, bx + bw, by + bh - r);
    cairo_arc(cr, bx + bw - r, by + bh - r, r, 0, G_PI/2);
    cairo_line_to(cr, bx + r, by + bh);
    cairo_arc(cr, bx + r, by + bh - r, r, G_PI/2, G_PI);
    cairo_line_to(cr, bx, by + r);
    cairo_arc(cr, bx + r, by + r, r, G_PI, 3*G_PI/2);
    cairo_close_path(cr);
    cairo_fill(cr);

    /* Text */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_move_to(cr, cx - te.width / 2 - te.x_bearing,
                      cy - te.height / 2 - te.y_bearing - te.height / 2 + 1);
    cairo_show_text(cr, text);
}

/* Small corner handle square */
static void draw_handle(cairo_t *cr, double x, double y)
{
    double hs = 5.0;
    cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 1.0);
    cairo_rectangle(cr, x - hs, y - hs, hs * 2, hs * 2);
    cairo_fill(cr);
}

/* ─── Draw callback ──────────────────────────────────────────────────────── */

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

    /* ── Full-screen dim ─────────────────────────────────────────────────── */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.48);
    cairo_paint(cr);

    if (!st->dragging) {
        /* ── Crosshair ───────────────────────────────────────────────────── */
        double cx = st->mouse_x, cy = st->mouse_y;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, cx, 0); cairo_line_to(cr, cx, h);
        cairo_move_to(cr, 0, cy); cairo_line_to(cr, w, cy);
        cairo_stroke(cr);

        /* ── Cursor position badge ───────────────────────────────────────── */
        char pos[32];
        snprintf(pos, sizeof(pos), "%d, %d", (int)cx, (int)cy);
        draw_badge(cr, cx + 60, cy - 20, pos, 12.0);

        /* ── Help hint ───────────────────────────────────────────────────── */
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 15.0);
        cairo_text_extents_t te;
        const char *hint = "Click and drag to select a region   |   Esc = cancel";
        cairo_text_extents(cr, hint, &te);
        draw_badge(cr, w / 2.0, h - 32, hint, 14.0);

#ifndef SNAPX_USE_GTK4
        return FALSE;
#else
        return;
#endif
    }

    /* ── Selection rect ──────────────────────────────────────────────────── */
    double rx, ry, rw, rh;
    normalise(st->start_x, st->start_y, st->cur_x, st->cur_y,
              &rx, &ry, &rw, &rh);

    /* Punch through the dim inside the selection */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_fill(cr);

    /* Draw selection border */
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 0.95);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_stroke(cr);

    /* Edge handles */
    draw_handle(cr, rx,         ry);
    draw_handle(cr, rx + rw,    ry);
    draw_handle(cr, rx,         ry + rh);
    draw_handle(cr, rx + rw,    ry + rh);
    draw_handle(cr, rx + rw/2,  ry);
    draw_handle(cr, rx + rw/2,  ry + rh);
    draw_handle(cr, rx,         ry + rh/2);
    draw_handle(cr, rx + rw,    ry + rh/2);

    /* Dimension badge above the selection */
    char dim[48];
    snprintf(dim, sizeof(dim), "%d × %d", (int)rw, (int)rh);
    double badge_y = (ry > 44) ? ry - 16 : ry + rh + 36;
    draw_badge(cr, rx + rw / 2.0, badge_y, dim, 13.0);

#ifndef SNAPX_USE_GTK4
    return FALSE;
#endif
}

/* ─── Confirm selection ──────────────────────────────────────────────────── */

static void confirm_selection(OverlayState *st)
{
    double rx, ry, rw, rh;
    normalise(st->start_x, st->start_y, st->cur_x, st->cur_y,
              &rx, &ry, &rw, &rh);
    if (rw < 4 || rh < 4) {
        st->confirmed = FALSE;
    } else {
        st->result.x = (int)rx; st->result.y = (int)ry;
        st->result.width = (int)rw; st->result.height = (int)rh;
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

static gboolean on_button_press_gtk3(GtkWidget *w, GdkEventButton *ev, gpointer data)
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

static gboolean on_button_release_gtk3(GtkWidget *w, GdkEventButton *ev, gpointer data)
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

int snapx_overlay_select_region(GtkWindow *parent, SnapxRegion *region)
{
    OverlayState st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);

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
    g_signal_connect(da,  "draw",                 G_CALLBACK(on_overlay_draw),       &st);
    g_signal_connect(da,  "button-press-event",   G_CALLBACK(on_button_press_gtk3),  &st);
    g_signal_connect(da,  "button-release-event", G_CALLBACK(on_button_release_gtk3),&st);
    g_signal_connect(da,  "motion-notify-event",  G_CALLBACK(on_motion_gtk3),        &st);
    g_signal_connect(win, "key-press-event",       G_CALLBACK(on_key_gtk3),          &st);
    gtk_container_add(GTK_CONTAINER(win), da);
    gtk_widget_show_all(win);
#endif

    /* Crosshair cursor */
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

    if (st.confirmed && region) { *region = st.result; return 1; }
    return 0;
}

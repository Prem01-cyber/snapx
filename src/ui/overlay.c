/**
 * @file overlay.c
 * @brief Full-screen translucent overlay for interactive region selection.
 *
 * Draws a dark dimming overlay across all monitors with a bright selection
 * rectangle that the user drags with the mouse.  Crosshair cursor is shown.
 *
 * Keyboard:
 *   Enter / Space  → confirm selection
 *   Escape         → cancel
 *
 * The overlay window is created as a popup (skip taskbar) with RGBA visual
 * so the areas outside the selection rectangle appear as a 50% grey tint.
 */

#include "overlay.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ─── State ──────────────────────────────────────────────────────────────── */

typedef struct {
    GtkWidget   *win;
    GMainLoop   *loop;
    SnapxRegion  result;
    gboolean     confirmed;
    gboolean     dragging;
    double       start_x, start_y;
    double       cur_x,   cur_y;
} OverlayState;

/* ─── Coordinate helpers ─────────────────────────────────────────────────── */

static void normalise_rect(double x1, double y1, double x2, double y2,
                            double *rx, double *ry, double *rw, double *rh)
{
    *rx = (x1 < x2) ? x1 : x2;
    *ry = (y1 < y2) ? y1 : y2;
    *rw = fabs(x2 - x1);
    *rh = fabs(y2 - y1);
}

/* ─── Drawing ────────────────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4
static void on_overlay_draw(GtkDrawingArea *da, cairo_t *cr,
                             int w, int h, gpointer data)
{
    (void)da; (void)w; (void)h;
#else
static gboolean on_overlay_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)widget;
#endif
    OverlayState *st = (OverlayState *)data;

    /* Dim the whole screen */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45);
    cairo_paint(cr);

    if (!st->dragging) {
        /* Draw crosshair hint */
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.6);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 16.0);
        cairo_move_to(cr, 20, 40);
        cairo_show_text(cr, "Click and drag to select a region.  Esc = cancel");
#ifndef SNAPX_USE_GTK4
        return FALSE;
#else
        return;
#endif
    }

    double rx, ry, rw, rh;
    normalise_rect(st->start_x, st->start_y, st->cur_x, st->cur_y,
                   &rx, &ry, &rw, &rh);

    /* Clear the selection area (punch through the dim) */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_fill(cr);

    /* Draw selection border */
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 0.9);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_stroke(cr);

    /* Dimension label */
    char label[64];
    snprintf(label, sizeof(label), "%d × %d", (int)rw, (int)rh);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_select_font_face(cr, "Monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14.0);
    double tx = rx + rw / 2 - 30;
    double ty = (ry > 24) ? ry - 8 : ry + rh + 20;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, label);

#ifndef SNAPX_USE_GTK4
    return FALSE;
#endif
}

/* ─── Input events ───────────────────────────────────────────────────────── */

static void confirm_selection(OverlayState *st)
{
    double rx, ry, rw, rh;
    normalise_rect(st->start_x, st->start_y, st->cur_x, st->cur_y,
                   &rx, &ry, &rw, &rh);
    if (rw < 4 || rh < 4) {
        st->confirmed = FALSE;
    } else {
        st->result.x      = (int)rx;
        st->result.y      = (int)ry;
        st->result.width  = (int)rw;
        st->result.height = (int)rh;
        st->confirmed = TRUE;
    }
    gtk_widget_set_visible(st->win, FALSE);
    g_main_loop_quit(st->loop);
}

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
    if (!st->dragging) return;
    st->cur_x = x; st->cur_y = y;
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
        confirm_selection(st);
        return TRUE;
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
    if (ev->button == 1) {
        st->cur_x = ev->x; st->cur_y = ev->y;
        confirm_selection(st);
    }
    return TRUE;
}

static gboolean on_motion_gtk3(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    (void)w;
    OverlayState *st = (OverlayState *)data;
    if (!st->dragging) return TRUE;
    st->cur_x = ev->x; st->cur_y = ev->y;
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
        confirm_selection(st);
        return TRUE;
    }
    return FALSE;
}

#endif /* GTK3 */

/* ─── Public API ─────────────────────────────────────────────────────────── */

int snapx_overlay_select_region(GtkWindow *parent, SnapxRegion *region)
{
    OverlayState st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);

    /* ── Create fullscreen transparent window ─────────────────────────── */
#ifdef SNAPX_USE_GTK4
    GtkWidget *win = gtk_window_new();
#else
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#endif
    st.win = win;

    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    /* GTK4: skip-taskbar / keep-above removed; fullscreen is sufficient */

    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);

    /* Request RGBA / composited visual */
#ifndef SNAPX_USE_GTK4
    GdkScreen *screen = gtk_window_get_screen(GTK_WINDOW(win));
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(win, visual);
    gtk_widget_set_app_paintable(win, TRUE);
#endif

    gtk_window_fullscreen(GTK_WINDOW(win));

    /* ── Drawing area ─────────────────────────────────────────────────── */
#ifdef SNAPX_USE_GTK4
    GtkWidget *da = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da),
                                   (GtkDrawingAreaDrawFunc)on_overlay_draw,
                                   &st, NULL);
    gtk_window_set_child(GTK_WINDOW(win), da);

    /* Input controllers */
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
    g_signal_connect(da, "draw",                  G_CALLBACK(on_overlay_draw),         &st);
    g_signal_connect(da, "button-press-event",    G_CALLBACK(on_button_press_gtk3),    &st);
    g_signal_connect(da, "button-release-event",  G_CALLBACK(on_button_release_gtk3),  &st);
    g_signal_connect(da, "motion-notify-event",   G_CALLBACK(on_motion_gtk3),          &st);
    g_signal_connect(win, "key-press-event",       G_CALLBACK(on_key_gtk3),            &st);
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

    /* Run nested event loop until selection done */
    g_main_loop_run(st.loop);
    g_main_loop_unref(st.loop);
    gtk_window_destroy(GTK_WINDOW(win));

    if (st.confirmed && region) {
        *region = st.result;
        return 1;
    }
    return 0;
}

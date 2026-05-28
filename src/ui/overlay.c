/**
 * @file overlay.c
 * @brief Two fullscreen overlays:
 *
 *  1. snapx_overlay_select_region() — Flameshot-style region selection.
 *     - Wayland/freeze-frame: caller supplies a full virtual-desktop screenshot;
 *       the overlay shows the relevant monitor slice as frozen background.
 *     - X11/live: no background; semi-transparent dim over live desktop.
 *
 *  2. snapx_overlay_select_monitor() — Virtual-desktop layout picker.
 *     All connected monitors are drawn to scale.  The user hovers/clicks one
 *     to select it; the 0-based monitor index is returned.
 */

#include "overlay.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>

/* ══════════════════════════════════════════════════════════════════════════
 *  COMMON HELPERS
 * ══════════════════════════════════════════════════════════════════════════ */

static void normalise(double x1, double y1, double x2, double y2,
                       double *rx, double *ry, double *rw, double *rh)
{
    *rx = (x1 < x2) ? x1 : x2;
    *ry = (y1 < y2) ? y1 : y2;
    *rw = fabs(x2 - x1);
    *rh = fabs(y2 - y1);
}

/** Draw a pill-shaped label with a dark background. */
static void draw_badge(cairo_t *cr, double cx, double cy,
                        const char *text, double font_size)
{
    cairo_select_font_face(cr, "Sans",
                            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents_t te;
    cairo_text_extents(cr, text, &te);
    double pad = 7.0, r = 6.0;
    double bx = cx - te.width/2 - pad,  by = cy - te.height - pad;
    double bw = te.width  + pad*2,       bh = te.height + pad*2;

    cairo_set_source_rgba(cr, 0.06, 0.06, 0.10, 0.88);
    cairo_move_to (cr, bx+r,    by);
    cairo_line_to (cr, bx+bw-r, by);
    cairo_arc     (cr, bx+bw-r, by+r,    r, -G_PI/2,      0);
    cairo_line_to (cr, bx+bw,   by+bh-r);
    cairo_arc     (cr, bx+bw-r, by+bh-r, r,  0,       G_PI/2);
    cairo_line_to (cr, bx+r,    by+bh);
    cairo_arc     (cr, bx+r,    by+bh-r, r,  G_PI/2,  G_PI);
    cairo_line_to (cr, bx,      by+r);
    cairo_arc     (cr, bx+r,    by+r,    r,  G_PI,  3*G_PI/2);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_move_to(cr,
                  cx - te.width/2  - te.x_bearing,
                  cy - te.height/2 - te.y_bearing - te.height/2 + 1);
    cairo_show_text(cr, text);
}

static void draw_handle(cairo_t *cr, double x, double y, double hs)
{
    cairo_set_source_rgba(cr, 0.25, 0.65, 1.0, 1.0);
    cairo_rectangle(cr, x-hs, y-hs, hs*2, hs*2);
    cairo_fill(cr);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  1. REGION SELECTION OVERLAY
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    GtkWidget        *win;
    GMainLoop        *loop;
    SnapxRegion       result;     /**< in virtual-desktop coords after confirm */
    gboolean          confirmed;

    gboolean          dragging;
    double            start_x, start_y;   /**< widget coords */
    double            cur_x,   cur_y;
    double            mouse_x, mouse_y;

    /* Monitor offset of this window (virtual-desktop coords of top-left) */
    int               mon_ox, mon_oy;
    int               mon_w,  mon_h;      /**< logical size of current monitor */

    /* Optional freeze-frame background (NULL → transparent live mode) */
    const SnapxImage *background;
    cairo_surface_t  *bg_surf;            /**< ARGB32 surface built from background */
} RegionState;

/* ── Forward declarations ────────────────────────────────────────────────── */

static void get_monitor_geom(GtkWidget *win,
                               int *ox, int *oy, int *ow, int *oh);

/* ── Build cairo surface from SnapxImage ─────────────────────────────────── */

/**
 * Convert an RGBA SnapxImage into an ARGB32 cairo image surface.
 * The caller owns the returned surface.
 */
static cairo_surface_t *image_to_cairo(const SnapxImage *img)
{
    if (!img || !img->data) return NULL;
    int w = img->width, h = img->height;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_surface_flush(surf);
    unsigned char *dst = cairo_image_surface_get_data(surf);
    int dst_stride     = cairo_image_surface_get_stride(surf);
    const unsigned char *src = img->data;
    int src_stride = img->stride > 0 ? img->stride : w * 4;
    for (int y = 0; y < h; y++) {
        const unsigned char *srow = src + y * src_stride;
        unsigned char       *drow = dst + y * dst_stride;
        for (int x = 0; x < w; x++) {
            /* RGBA → ARGB32 (pre-multiplied alpha not needed, alpha=255) */
            unsigned char r = srow[x*4+0];
            unsigned char g = srow[x*4+1];
            unsigned char b = srow[x*4+2];
            unsigned char a = srow[x*4+3];
            drow[x*4+0] = b;
            drow[x*4+1] = g;
            drow[x*4+2] = r;
            drow[x*4+3] = a;
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

/* ── Draw ───────────────────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4
static void region_draw(GtkDrawingArea *da, cairo_t *cr,
                         int w, int h, gpointer data)
{ (void)da;
#else
static gboolean region_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{ int w, h; GtkAllocation a;
  gtk_widget_get_allocation(widget, &a); w = a.width; h = a.height;
#endif

    RegionState *st = (RegionState *)data;

    /*
     * Refresh monitor geometry on every frame.  On Wayland the compositor
     * assigns the window to a monitor asynchronously; the first 1-2 frames
     * may not know the correct monitor position yet.  This is a cheap GDK
     * query and calling it per-frame ensures we always have current values.
     */
    get_monitor_geom(st->win, &st->mon_ox, &st->mon_oy, &st->mon_w, &st->mon_h);

    /* ── Step 1: Background ──────────────────────────────────────────────── */
    if (st->bg_surf) {
        /*
         * Freeze-frame mode: paint the monitor's slice of the full-desktop
         * screenshot.  The bg_surf covers the entire virtual desktop; we
         * translate the source so that (mon_ox, mon_oy) lands at widget (0,0).
         *
         * Scale only when the monitor's logical size differs from the widget
         * size (HiDPI): widget_w / mon_w  gives the render scale factor.
         */
        double sx = (st->mon_w > 0) ? (double)w / st->mon_w : 1.0;
        double sy = (st->mon_h > 0) ? (double)h / st->mon_h : 1.0;

        /* Paint the scaled & offset source to fill the whole drawing area */
        cairo_save(cr);
        cairo_scale(cr, sx, sy);
        /*
         * At user-space origin (0,0) we want bg pixel (mon_ox, mon_oy).
         * cairo_set_source_surface offset is: dest - src, i.e. 0 - mon_ox.
         */
        /*
         * Source offset is in user-space (post-scale).  User (0,0) must map
         * to bg pixel (mon_ox, mon_oy).  cairo_set_source_surface(surf, x0, y0)
         * means: at user (u,v) → surf pixel (u-x0, v-y0).
         * So x0 = -mon_ox (user units), which at scale sx is mon_ox logical px.
         */
        cairo_set_source_surface(cr, st->bg_surf,
                                 -(double)st->mon_ox,
                                 -(double)st->mon_oy);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        /* Transparent/live mode (X11 compositing): fully clear first */
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    }

    /* ── Step 2: Dim ─────────────────────────────────────────────────────── */
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (st->dragging) {
        double rx, ry, rw, rh;
        normalise(st->start_x, st->start_y, st->cur_x, st->cur_y,
                  &rx, &ry, &rw, &rh);

        /*
         * Paint dim everywhere EXCEPT the selection rectangle.
         * EVEN_ODD fill rule treats nested rectangles as holes:
         *   outer rect (0,0,w,h) = filled
         *   inner rect (rx,ry,rw,rh) = hole (not filled)
         * → only the area outside the selection is dimmed.
         */
        cairo_save(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.50);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_fill(cr);
        cairo_restore(cr);

        /* Bright blue selection border */
        cairo_set_source_rgba(cr, 0.25, 0.65, 1.0, 0.95);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_stroke(cr);

        /* Corner + edge handles */
        double hs = 5.0;
        draw_handle(cr, rx,        ry,        hs);
        draw_handle(cr, rx+rw,     ry,        hs);
        draw_handle(cr, rx,        ry+rh,     hs);
        draw_handle(cr, rx+rw,     ry+rh,     hs);
        draw_handle(cr, rx+rw/2,   ry,        hs);
        draw_handle(cr, rx+rw/2,   ry+rh,     hs);
        draw_handle(cr, rx,        ry+rh/2,   hs);
        draw_handle(cr, rx+rw,     ry+rh/2,   hs);

        /* Dimension badge (above or below selection) */
        char dim[48];
        snprintf(dim, sizeof(dim), "%d × %d", (int)rw, (int)rh);
        double badge_y = (ry > 50) ? ry - 16 : ry + rh + 38;
        draw_badge(cr, rx + rw/2.0, badge_y, dim, 13.0);
    } else {
        /* No selection yet — dim everything */
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.50);
        cairo_paint(cr);

        /* Crosshair */
        double cx = st->mouse_x, cy = st->mouse_y;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.40);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, cx, 0);   cairo_line_to(cr, cx, h);
        cairo_move_to(cr, 0,  cy);  cairo_line_to(cr, w,  cy);
        cairo_stroke(cr);

        /* Coordinate badge near the cursor */
        char pos[32];
        snprintf(pos, sizeof(pos), "%d, %d",
                 (int)cx + st->mon_ox, (int)cy + st->mon_oy);
        double bx = cx + 68;
        if (bx > w - 90) bx = cx - 68;
        double by = cy - 26;
        if (by < 24) by = cy + 30;
        draw_badge(cr, bx, by, pos, 12.0);

        /* Bottom hint */
        draw_badge(cr, w / 2.0, h - 30,
                   "Drag to select region   |   Esc = cancel", 13.5);
    }

#ifndef SNAPX_USE_GTK4
    return FALSE;
#endif
}

/* ── Confirm ─────────────────────────────────────────────────────────────── */

static void region_confirm(RegionState *st)
{
    double rx, ry, rw, rh;
    normalise(st->start_x, st->start_y, st->cur_x, st->cur_y,
              &rx, &ry, &rw, &rh);
    if (rw < 4 || rh < 4) { st->confirmed = FALSE; }
    else {
        /* Convert widget coords → virtual-desktop coords */
        st->result.x      = (int)rx + st->mon_ox;
        st->result.y      = (int)ry + st->mon_oy;
        st->result.width  = (int)rw;
        st->result.height = (int)rh;
        st->confirmed = TRUE;
    }
    gtk_widget_set_visible(st->win, FALSE);
    g_main_loop_quit(st->loop);
}

/* ── Input ───────────────────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4

static void reg_press(GtkGestureClick *g, int n, double x, double y, gpointer d)
{ (void)g;(void)n; RegionState *st=d;
  st->dragging=TRUE; st->start_x=x; st->start_y=y;
  st->cur_x=x; st->cur_y=y; gtk_widget_queue_draw(st->win); }

static void reg_release(GtkGestureClick *g, int n, double x, double y, gpointer d)
{ (void)g;(void)n; RegionState *st=d; st->cur_x=x; st->cur_y=y; region_confirm(st); }

static void reg_motion(GtkEventControllerMotion *c, double x, double y, gpointer d)
{ (void)c; RegionState *st=d; st->mouse_x=x; st->mouse_y=y;
  if(st->dragging){st->cur_x=x;st->cur_y=y;}
  gtk_widget_queue_draw(st->win); }

static gboolean reg_key(GtkEventControllerKey *c, guint kv, guint kc,
                          GdkModifierType mod, gpointer d)
{ (void)c;(void)kc;(void)mod; RegionState *st=d;
  if(kv==GDK_KEY_Escape){
      st->confirmed=FALSE; gtk_widget_set_visible(st->win,FALSE);
      g_main_loop_quit(st->loop); return TRUE; }
  if((kv==GDK_KEY_Return||kv==GDK_KEY_space)&&st->dragging){
      region_confirm(st); return TRUE; }
  return FALSE; }

#else /* GTK3 */

static gboolean reg_bp(GtkWidget *w,GdkEventButton *e,gpointer d)
{ (void)w; RegionState *st=d;
  if(e->button==1){ st->dragging=TRUE;
    st->start_x=e->x;st->start_y=e->y;st->cur_x=e->x;st->cur_y=e->y;
    gtk_widget_queue_draw(st->win); } return TRUE; }

static gboolean reg_br(GtkWidget *w,GdkEventButton *e,gpointer d)
{ (void)w; RegionState *st=d;
  if(e->button==1){st->cur_x=e->x;st->cur_y=e->y;region_confirm(st);} return TRUE; }

static gboolean reg_mo(GtkWidget *w,GdkEventMotion *e,gpointer d)
{ (void)w; RegionState *st=d; st->mouse_x=e->x;st->mouse_y=e->y;
  if(st->dragging){st->cur_x=e->x;st->cur_y=e->y;}
  gtk_widget_queue_draw(st->win); return TRUE; }

static gboolean reg_kp(GtkWidget *w,GdkEventKey *e,gpointer d)
{ (void)w; RegionState *st=d;
  if(e->keyval==GDK_KEY_Escape){
      st->confirmed=FALSE;gtk_widget_set_visible(st->win,FALSE);
      g_main_loop_quit(st->loop);return TRUE;}
  if((e->keyval==GDK_KEY_Return||e->keyval==GDK_KEY_space)&&st->dragging){
      region_confirm(st);return TRUE;} return FALSE; }

#endif

/* ── Helper: get monitor geometry of a GtkWindow ────────────────────────── */

static void get_monitor_geom(GtkWidget *win,
                               int *ox, int *oy, int *ow, int *oh)
{
    *ox = *oy = 0; *ow = 1920; *oh = 1080;
#ifdef SNAPX_USE_GTK4
    GdkDisplay *dpy  = gdk_display_get_default();
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(win));
    if (!surf) return;
    GdkMonitor *mon = gdk_display_get_monitor_at_surface(dpy, surf);
    if (!mon) return;
    GdkRectangle geom = {0};
    gdk_monitor_get_geometry(mon, &geom);
    *ox = geom.x; *oy = geom.y; *ow = geom.width; *oh = geom.height;
#else
    GdkWindow  *gw  = gtk_widget_get_window(win);
    if (!gw) return;
    GdkDisplay *dpy = gdk_window_get_display(gw);
    GdkMonitor *mon = gdk_display_get_monitor_at_window(dpy, gw);
    if (!mon) return;
    GdkRectangle geom = {0};
    gdk_monitor_get_geometry(mon, &geom);
    *ox = geom.x; *oy = geom.y; *ow = geom.width; *oh = geom.height;
#endif
}

/* ── Public: region selection ────────────────────────────────────────────── */

int snapx_overlay_select_region(GtkWindow        *parent,
                                  const SnapxImage *background,
                                  SnapxRegion      *region)
{
    RegionState st = {0};
    st.loop       = g_main_loop_new(NULL, FALSE);
    st.background = background;
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

    /* For the transparent (X11) path, request an RGBA visual */
    if (!background) {
#ifndef SNAPX_USE_GTK4
        GdkScreen *scr = gtk_window_get_screen(GTK_WINDOW(win));
        GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
        if (vis) gtk_widget_set_visual(win, vis);
        gtk_widget_set_app_paintable(win, TRUE);
#endif
        /* GTK4: set CSS transparent background */
#ifdef SNAPX_USE_GTK4
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_string(css,
            "window { background-color: transparent; }");
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
        g_object_unref(css);
#endif
    }

    gtk_window_fullscreen(GTK_WINDOW(win));

#ifdef SNAPX_USE_GTK4
    GtkWidget *da = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da),
                                    (GtkDrawingAreaDrawFunc)region_draw, &st, NULL);
    gtk_window_set_child(GTK_WINDOW(win), da);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed",  G_CALLBACK(reg_press),   &st);
    g_signal_connect(click, "released", G_CALLBACK(reg_release),  &st);
    gtk_widget_add_controller(da, GTK_EVENT_CONTROLLER(click));

    GtkEventController *mot = gtk_event_controller_motion_new();
    g_signal_connect(mot, "motion", G_CALLBACK(reg_motion), &st);
    gtk_widget_add_controller(da, mot);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(reg_key), &st);
    gtk_widget_add_controller(win, key);
    gtk_widget_set_focusable(win, TRUE);
#else
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_events(da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                              | GDK_POINTER_MOTION_MASK);
    g_signal_connect(da,  "draw",                 G_CALLBACK(region_draw), &st);
    g_signal_connect(da,  "button-press-event",   G_CALLBACK(reg_bp),      &st);
    g_signal_connect(da,  "button-release-event", G_CALLBACK(reg_br),      &st);
    g_signal_connect(da,  "motion-notify-event",  G_CALLBACK(reg_mo),      &st);
    g_signal_connect(win, "key-press-event",       G_CALLBACK(reg_kp),     &st);
    gtk_container_add(GTK_CONTAINER(win), da);
    gtk_widget_show_all(win);
#endif

#ifdef SNAPX_USE_GTK4
    GdkCursor *cur = gdk_cursor_new_from_name("crosshair", NULL);
#else
    GdkCursor *cur = gdk_cursor_new_from_name(
        gtk_widget_get_display(win), "crosshair");
#endif
    if (cur) {
#ifdef SNAPX_USE_GTK4
        gtk_widget_set_cursor(win, cur);
#else
        gdk_window_set_cursor(gtk_widget_get_window(win), cur);
#endif
        g_object_unref(cur);
    }

    gtk_widget_set_visible(win, TRUE);
    gtk_widget_grab_focus(win);

    /* Let GTK process pending events so the window is actually mapped and
     * we can query which monitor it landed on. */
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
    get_monitor_geom(win, &st.mon_ox, &st.mon_oy, &st.mon_w, &st.mon_h);

    g_main_loop_run(st.loop);
    g_main_loop_unref(st.loop);

    if (st.bg_surf) cairo_surface_destroy(st.bg_surf);
    gtk_window_destroy(GTK_WINDOW(win));

    if (st.confirmed && region) { *region = st.result; return 1; }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  2. MONITOR PICKER OVERLAY
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    GtkWidget              *win;
    GMainLoop              *loop;
    const SnapxMonitorInfo *monitors;
    int                     n;
    int                     hover;      /**< index of hovered monitor, or -1 */
    int                     selected;   /**< confirmed index, or -1          */

    /* Virtual desktop bounds */
    int virt_x, virt_y, virt_w, virt_h;

    /* Scale: virtual desktop → window widget */
    double scale;
    double off_x, off_y;   /**< centering offset inside widget */
} MonPickState;

static void mon_recalc_viewport(MonPickState *st, int ww, int wh)
{
    double pad = 60.0;
    double sx = (ww - pad*2) / (double)st->virt_w;
    double sy = (wh - pad*2 - 60) / (double)st->virt_h;  /* reserve 60px for title */
    st->scale = (sx < sy) ? sx : sy;
    if (st->scale > 1.5) st->scale = 1.5;  /* don't over-enlarge tiny setups */
    st->off_x = (ww - st->virt_w * st->scale) / 2.0;
    st->off_y = (wh - st->virt_h * st->scale) / 2.0 + 20;
}

/** Map virtual-desktop coords → widget coords. */
static void vd_to_widget(const MonPickState *st, int vx, int vy,
                           double *wx, double *wy)
{
    *wx = (vx - st->virt_x) * st->scale + st->off_x;
    *wy = (vy - st->virt_y) * st->scale + st->off_y;
}

/** Return which monitor index the widget point (mx, my) is inside, or -1. */
static int mon_hit_test(const MonPickState *st, double mx, double my)
{
    for (int i = 0; i < st->n; i++) {
        double wx, wy;
        vd_to_widget(st, st->monitors[i].x, st->monitors[i].y, &wx, &wy);
        double mw = st->monitors[i].width  * st->scale;
        double mh = st->monitors[i].height * st->scale;
        if (mx >= wx && mx <= wx+mw && my >= wy && my <= wy+mh)
            return i;
    }
    return -1;
}

#ifdef SNAPX_USE_GTK4
static void mon_draw(GtkDrawingArea *da, cairo_t *cr,
                      int w, int h, gpointer data)
{ (void)da;
#else
static gboolean mon_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{ int w, h; GtkAllocation a;
  gtk_widget_get_allocation(widget, &a); w=a.width; h=a.height;
#endif
    MonPickState *st = (MonPickState *)data;
    mon_recalc_viewport(st, w, h);

    /* Background */
    cairo_set_source_rgba(cr, 0.04, 0.04, 0.08, 0.94);
    cairo_paint(cr);

    /* Title */
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 20.0);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
    cairo_text_extents_t te;
    const char *title = "Select a monitor to capture";
    cairo_text_extents(cr, title, &te);
    cairo_move_to(cr, (w - te.width)/2.0, 36);
    cairo_show_text(cr, title);

    /* Draw each monitor */
    for (int i = 0; i < st->n; i++) {
        const SnapxMonitorInfo *m = &st->monitors[i];
        double wx, wy;
        vd_to_widget(st, m->x, m->y, &wx, &wy);
        double mw = m->width  * st->scale;
        double mh = m->height * st->scale;

        gboolean hov = (st->hover == i);

        /* Background fill */
        if (hov)
            cairo_set_source_rgba(cr, 0.15, 0.40, 0.75, 0.70);
        else
            cairo_set_source_rgba(cr, 0.10, 0.10, 0.18, 0.70);
        cairo_rectangle(cr, wx, wy, mw, mh);
        cairo_fill(cr);

        /* Screen bezel simulation (inner rounded border) */
        double bz = 6.0;
        if (hov)
            cairo_set_source_rgba(cr, 0.30, 0.65, 1.00, 0.90);
        else
            cairo_set_source_rgba(cr, 0.35, 0.35, 0.50, 0.80);
        cairo_set_line_width(cr, hov ? 3.0 : 1.5);
        cairo_rectangle(cr, wx+bz, wy+bz, mw-bz*2, mh-bz*2);
        cairo_stroke(cr);

        /* Outer glow for hovered monitor */
        if (hov) {
            cairo_set_source_rgba(cr, 0.25, 0.60, 1.00, 0.30);
            cairo_set_line_width(cr, 8.0);
            cairo_rectangle(cr, wx-2, wy-2, mw+4, mh+4);
            cairo_stroke(cr);
        }

        /* Monitor number (large, centred) */
        char numstr[16];
        snprintf(numstr, sizeof(numstr), "%d", i+1);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_BOLD);
        double nfsize = mh * 0.28;
        if (nfsize > 72) nfsize = 72;
        if (nfsize < 20) nfsize = 20;
        cairo_set_font_size(cr, nfsize);
        cairo_text_extents(cr, numstr, &te);
        cairo_set_source_rgba(cr, 1, 1, 1, hov ? 0.95 : 0.55);
        cairo_move_to(cr, wx + (mw - te.width)/2  - te.x_bearing,
                          wy + (mh - te.height)/2 - te.y_bearing
                              - te.height/2);
        cairo_show_text(cr, numstr);

        /* Resolution label */
        char res[32];
        snprintf(res, sizeof(res), "%d × %d", m->width, m->height);
        cairo_set_font_size(cr, 13.0);
        cairo_text_extents(cr, res, &te);
        cairo_set_source_rgba(cr, 1, 1, 1, hov ? 0.85 : 0.45);
        cairo_move_to(cr, wx + (mw - te.width)/2 - te.x_bearing,
                          wy + mh - 22);
        cairo_show_text(cr, res);

        /* "Primary" badge */
        if (m->is_primary) {
            draw_badge(cr, wx + mw/2, wy + 20, "Primary", 11.0);
        }
    }

    /* Bottom hint */
    cairo_set_font_size(cr, 13.0);
    draw_badge(cr, w/2.0, h - 28, "Click a monitor to capture it   |   Esc = cancel", 13.0);

#ifndef SNAPX_USE_GTK4
    return FALSE;
#endif
}

/* ── Input ───────────────────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4

static void mon_press(GtkGestureClick *g, int n, double x, double y, gpointer d)
{ (void)g;(void)n; MonPickState *st=d;
  int idx = mon_hit_test(st, x, y);
  if (idx >= 0) {
      st->selected = idx;
      gtk_widget_set_visible(st->win, FALSE);
      g_main_loop_quit(st->loop); } }

static void mon_motion(GtkEventControllerMotion *c, double x, double y, gpointer d)
{ (void)c; MonPickState *st=d;
  int idx = mon_hit_test(st, x, y);
  if (idx != st->hover) { st->hover = idx; gtk_widget_queue_draw(st->win); } }

static gboolean mon_key(GtkEventControllerKey *c, guint kv, guint kc,
                          GdkModifierType mod, gpointer d)
{ (void)c;(void)kc;(void)mod; MonPickState *st=d;
  if (kv==GDK_KEY_Escape) {
      st->selected=-1; gtk_widget_set_visible(st->win,FALSE);
      g_main_loop_quit(st->loop); return TRUE; }
  /* 1-9 number keys */
  if (kv >= GDK_KEY_1 && kv <= GDK_KEY_9) {
      int idx = (int)(kv - GDK_KEY_1);
      if (idx < st->n) {
          st->selected = idx;
          gtk_widget_set_visible(st->win, FALSE);
          g_main_loop_quit(st->loop); return TRUE; } }
  return FALSE; }

#else /* GTK3 */

static gboolean mon_bp(GtkWidget *w,GdkEventButton *e,gpointer d)
{ (void)w; MonPickState *st=d;
  if(e->button==1){
      int idx=mon_hit_test(st,e->x,e->y);
      if(idx>=0){ st->selected=idx;
          gtk_widget_set_visible(st->win,FALSE);
          g_main_loop_quit(st->loop);}} return TRUE; }

static gboolean mon_mo(GtkWidget *w,GdkEventMotion *e,gpointer d)
{ (void)w; MonPickState *st=d;
  int idx=mon_hit_test(st,e->x,e->y);
  if(idx!=st->hover){st->hover=idx;gtk_widget_queue_draw(st->win);} return TRUE; }

static gboolean mon_kp(GtkWidget *w,GdkEventKey *e,gpointer d)
{ (void)w; MonPickState *st=d;
  if(e->keyval==GDK_KEY_Escape){
      st->selected=-1;gtk_widget_set_visible(st->win,FALSE);
      g_main_loop_quit(st->loop);return TRUE;}
  if(e->keyval>=GDK_KEY_1&&e->keyval<=GDK_KEY_9){
      int idx=(int)(e->keyval-GDK_KEY_1);
      if(idx<st->n){st->selected=idx;
          gtk_widget_set_visible(st->win,FALSE);
          g_main_loop_quit(st->loop);return TRUE;}}
  return FALSE; }

#endif

/* ── Public: monitor picker ──────────────────────────────────────────────── */

int snapx_overlay_select_monitor(GtkWindow              *parent,
                                   const SnapxMonitorInfo *monitors,
                                   int                     n)
{
    if (n <= 0 || !monitors) return -1;

    /* If only one monitor, skip the picker */
    if (n == 1) return 0;

    MonPickState st = {0};
    st.loop     = g_main_loop_new(NULL, FALSE);
    st.monitors = monitors;
    st.n        = n;
    st.hover    = -1;
    st.selected = -1;

    /* Compute virtual desktop bounding box */
    int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
    for (int i = 0; i < n; i++) {
        if (monitors[i].x < minx) minx = monitors[i].x;
        if (monitors[i].y < miny) miny = monitors[i].y;
        if (monitors[i].x + monitors[i].width  > maxx) maxx = monitors[i].x + monitors[i].width;
        if (monitors[i].y + monitors[i].height > maxy) maxy = monitors[i].y + monitors[i].height;
    }
    st.virt_x = minx; st.virt_y = miny;
    st.virt_w = maxx - minx; st.virt_h = maxy - miny;
    if (st.virt_w <= 0) st.virt_w = 1920;
    if (st.virt_h <= 0) st.virt_h = 1080;

#ifdef SNAPX_USE_GTK4
    GtkWidget *win = gtk_window_new();
#else
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#endif
    st.win = win;
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 560);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(win), parent);

#ifndef SNAPX_USE_GTK4
    GdkScreen *scr = gtk_window_get_screen(GTK_WINDOW(win));
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (vis) gtk_widget_set_visual(win, vis);
    gtk_widget_set_app_paintable(win, TRUE);
#endif

#ifdef SNAPX_USE_GTK4
    GtkWidget *da = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da),
                                    (GtkDrawingAreaDrawFunc)mon_draw, &st, NULL);
    gtk_widget_set_size_request(da, 900, 560);
    gtk_window_set_child(GTK_WINDOW(win), da);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(mon_press), &st);
    gtk_widget_add_controller(da, GTK_EVENT_CONTROLLER(click));

    GtkEventController *mot = gtk_event_controller_motion_new();
    g_signal_connect(mot, "motion", G_CALLBACK(mon_motion), &st);
    gtk_widget_add_controller(da, mot);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(mon_key), &st);
    gtk_widget_add_controller(win, key);
    gtk_widget_set_focusable(win, TRUE);
#else
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 900, 560);
    gtk_widget_set_events(da, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(da,  "draw",               G_CALLBACK(mon_draw), &st);
    g_signal_connect(da,  "button-press-event", G_CALLBACK(mon_bp),   &st);
    g_signal_connect(da,  "motion-notify-event",G_CALLBACK(mon_mo),   &st);
    g_signal_connect(win, "key-press-event",    G_CALLBACK(mon_kp),   &st);
    gtk_container_add(GTK_CONTAINER(win), da);
    gtk_widget_show_all(win);
#endif

    gtk_widget_set_visible(win, TRUE);
    gtk_widget_grab_focus(win);

    g_main_loop_run(st.loop);
    g_main_loop_unref(st.loop);
    gtk_window_destroy(GTK_WINDOW(win));

    return st.selected;
}

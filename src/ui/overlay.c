/**
 * @file overlay.c
 * @brief Two fullscreen overlays:
 *
 *  1. snapx_overlay_select_region() — Flameshot-style region selection.
 *     - Multi-monitor + freeze-frame: one fullscreen overlay per display at 1:1.
 *     - Single monitor: fullscreen freeze on the cursor display.
 *     - X11/live: no background; semi-transparent dim over live desktop.
 *
 *  2. snapx_overlay_select_monitor() — Virtual-desktop layout picker.
 *     All connected monitors are drawn to scale.  The user hovers/clicks one
 *     to select it; the 0-based monitor index is returned.
 */

#include "overlay.h"
#include "../utils/shortcut.h"
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
    GtkWidget        *win;        /**< single-window mode only                */
    GtkWidget        *da;
    GtkWidget        *wins[8];    /**< per-monitor mode: one per display      */
    GtkWidget        *das[8];
    int               n_windows;
    GMainLoop        *loop;
    SnapxRegion       result;
    gboolean          confirmed;

    /* Selection: widget-local (single) or virtual-desktop (per-monitor) */
    gboolean          pressing;
    double            start_x, start_y;
    double            cur_x,   cur_y;
    double            mouse_x, mouse_y;
    int               start_vx, start_vy;
    int               cur_vx,   cur_vy;
    int               mouse_vx, mouse_vy;

    gboolean          per_monitor_mode;
    int               mon_ox, mon_oy;
    int               mon_w,  mon_h;

    SnapxMonitorInfo  monitors[8];
    int               n_monitors;
    int               virt_x, virt_y, virt_w, virt_h;

    const SnapxImage *background;
    cairo_surface_t  *bg_surf;
    guint             logged_slice_mask;

    const SnapxShortcuts *shortcuts;
} RegionState;

/** Per-window draw context (GTK4); avoids GINT_TO_POINTER(0) == NULL. */
typedef struct {
    RegionState *st;
    int          mon_idx;
} RegionDrawCtx;

#define SNAPX_MON_IDX_KEY "snapx-mon-idx"

/* ── Forward declarations ────────────────────────────────────────────────── */

static void get_monitor_geom(GtkWidget *win,
                               int *ox, int *oy, int *ow, int *oh);
static void region_queue_draw_all(RegionState *st);
static void region_close_all(RegionState *st);
static gboolean region_get_pointer_vd(int *vx, int *vy);
static GdkMonitor *find_gdk_monitor_for_info(GdkDisplay *dpy,
                                               const SnapxMonitorInfo *m);

static void region_widget_set_mon_idx(GtkWidget *w, int idx)
{
    g_object_set_data(G_OBJECT(w), SNAPX_MON_IDX_KEY, GINT_TO_POINTER(idx + 1));
}

static int region_widget_get_mon_idx(GtkWidget *w)
{
    gpointer p = g_object_get_data(G_OBJECT(w), SNAPX_MON_IDX_KEY);
    if (!p)
        return -1;
    return GPOINTER_TO_INT(p) - 1;
}

static void region_draw_per_monitor(RegionState *st, int idx,
                                      cairo_t *cr, int w, int h);

static void region_calc_virt_bounds(const SnapxMonitorInfo *mons, int n,
                                    int *vx, int *vy, int *vw, int *vh)
{
    if (n <= 0 || !mons) {
        *vx = *vy = 0;
        *vw = 1920;
        *vh = 1080;
        return;
    }
    int minx = mons[0].x, miny = mons[0].y;
    int maxx = mons[0].x + mons[0].width;
    int maxy = mons[0].y + mons[0].height;
    for (int i = 1; i < n; i++) {
        if (mons[i].x < minx) minx = mons[i].x;
        if (mons[i].y < miny) miny = mons[i].y;
        if (mons[i].x + mons[i].width  > maxx) maxx = mons[i].x + mons[i].width;
        if (mons[i].y + mons[i].height > maxy) maxy = mons[i].y + mons[i].height;
    }
    *vx = minx;
    *vy = miny;
    *vw = maxx - minx;
    *vh = maxy - miny;
    if (*vw <= 0) *vw = 1920;
    if (*vh <= 0) *vh = 1080;
}

static void region_queue_draw_all(RegionState *st)
{
    if (st->per_monitor_mode) {
        for (int i = 0; i < st->n_windows; i++) {
            if (st->das[i])
                gtk_widget_queue_draw(st->das[i]);
        }
    } else if (st->da) {
        gtk_widget_queue_draw(st->da);
    }
}

static void region_close_all(RegionState *st)
{
    if (st->per_monitor_mode) {
        for (int i = 0; i < st->n_windows; i++) {
            if (st->wins[i]) {
                gtk_window_destroy(GTK_WINDOW(st->wins[i]));
                st->wins[i] = NULL;
                st->das[i]  = NULL;
            }
        }
        st->n_windows = 0;
    } else if (st->win) {
        gtk_widget_set_visible(st->win, FALSE);
    }
}

static void region_local_to_vd(const RegionState *st, int idx,
                                 double lx, double ly, int *vx, int *vy)
{
    const SnapxMonitorInfo *m = &st->monitors[idx];
    int ww = m->width, wh = m->height;
    if (st->das[idx]) {
        int dw = gtk_widget_get_width(st->das[idx]);
        int dh = gtk_widget_get_height(st->das[idx]);
        if (dw > 0) ww = dw;
        if (dh > 0) wh = dh;
    }
    *vx = m->x + (int)round(lx * (double)m->width  / (double)ww);
    *vy = m->y + (int)round(ly * (double)m->height / (double)wh);
}

static int region_win_index(const RegionState *st, GtkWidget *win)
{
    int idx = region_widget_get_mon_idx(win);
    if (idx >= 0)
        return idx;
    (void)st;
    return 0;
}

static gboolean region_get_pointer_vd(int *vx, int *vy)
{
    GdkDisplay *dpy = gdk_display_get_default();
    if (!dpy) return FALSE;
    GdkSeat *seat = gdk_display_get_default_seat(dpy);
    if (!seat) return FALSE;
    GdkDevice *dev = gdk_seat_get_pointer(seat);
    if (!dev) return FALSE;

#ifdef SNAPX_USE_GTK4
    double lx = 0, ly = 0;
    GdkSurface *surf = gdk_device_get_surface_at_position(dev, &lx, &ly);
    if (!surf) return FALSE;
    GdkMonitor *gmon = gdk_display_get_monitor_at_surface(dpy, surf);
    if (!gmon) return FALSE;
    GdkRectangle g = {0};
    gdk_monitor_get_geometry(gmon, &g);
    *vx = g.x + (int)round(lx);
    *vy = g.y + (int)round(ly);
#else
    gint x = 0, y = 0;
    gdk_device_get_position(dev, &x, &y, NULL);
    *vx = x;
    *vy = y;
#endif
    return TRUE;
}

static void region_vd_to_local(const SnapxMonitorInfo *m, int w, int h,
                                 int vx, int vy, double *lx, double *ly)
{
    double sx = (m->width  > 0) ? (double)w / (double)m->width  : 1.0;
    double sy = (m->height > 0) ? (double)h / (double)m->height : 1.0;
    *lx = (vx - m->x) * sx;
    *ly = (vy - m->y) * sy;
}

static int region_monitor_at_vd(const RegionState *st, int vx, int vy)
{
    for (int i = 0; i < st->n_monitors; i++) {
        const SnapxMonitorInfo *m = &st->monitors[i];
        if (vx >= m->x && vx < m->x + m->width &&
            vy >= m->y && vy < m->y + m->height)
            return i;
    }
    return 0;
}

static GdkMonitor *find_gdk_monitor_for_info(GdkDisplay *dpy,
                                               const SnapxMonitorInfo *m)
{
    if (!dpy || !m) return NULL;
    GListModel *ml = gdk_display_get_monitors(dpy);
    if (!ml) return NULL;

    guint n = g_list_model_get_n_items(ml);
    for (guint i = 0; i < n; i++) {
        GdkMonitor *mon = GDK_MONITOR(g_list_model_get_item(ml, i));
        if (!mon) continue;
        GdkRectangle g = {0};
        gdk_monitor_get_geometry(mon, &g);
        if (g.x == m->x && g.y == m->y &&
            g.width == m->width && g.height == m->height)
            return mon;
        g_object_unref(mon);
    }

    if (m->index >= 0 && (guint)m->index < n) {
        GdkMonitor *mon = GDK_MONITOR(g_list_model_get_item(ml, (guint)m->index));
        if (mon)
            return mon;
    }
    return NULL;
}

/* ── RGBA SnapxImage → Cairo ARGB32 surface ──────────────────────────────── */

static cairo_surface_t *image_to_cairo(const SnapxImage *img)
{
    if (!img || !img->data) return NULL;
    int iw = img->width, ih = img->height;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf); return NULL;
    }
    cairo_surface_flush(surf);
    uint8_t       *dst      = cairo_image_surface_get_data(surf);
    int            dst_str  = cairo_image_surface_get_stride(surf);
    const uint8_t *src      = img->data;
    int            src_str  = img->stride > 0 ? img->stride : iw * 4;
    for (int y = 0; y < ih; y++) {
        const uint8_t *sr = src + y * src_str;
        uint32_t      *dr = (uint32_t *)(dst + y * dst_str);
        for (int x = 0; x < iw; x++) {
            uint8_t r = sr[x*4+0], g = sr[x*4+1], b = sr[x*4+2], a = sr[x*4+3];
            /* Cairo ARGB32 (pre-mul, but a=255 for screenshots so no-op) */
            dr[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                    ((uint32_t)g <<  8) |  (uint32_t)b;
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

/* ── Paint background slice ──────────────────────────────────────────────── */

/**
 * Render the monitor's slice of bg_surf into the drawing area.
 * The source is a full virtual-desktop screenshot; we translate it so that
 * the monitor at (mon_ox, mon_oy) fills the widget from (0,0).
 * Scale handles HiDPI where the widget is larger than the logical monitor size.
 */
static void paint_background(cairo_t *cr, int w, int h,
                               cairo_surface_t *bg_surf,
                               int mon_ox, int mon_oy,
                               int mon_w,  int mon_h)
{
    if (!bg_surf) {
        /* Transparent/live X11 mode: clear surface so compositor shows through */
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        return;
    }

    /*
     * Scale: widget_pixels / monitor_logical_pixels
     * For non-HiDPI this is 1.0.  For HiDPI the widget may be 2× the logical
     * monitor size so we scale the bg up to fill.
     */
    double sx = (mon_w > 0) ? (double)w / mon_w : 1.0;
    double sy = (mon_h > 0) ? (double)h / mon_h : 1.0;

    cairo_save(cr);
    cairo_scale(cr, sx, sy);

    /*
     * cairo_set_source_surface(surf, x0, y0):
     *   At user-space point (u, v), source pixel = surf[(u - x0), (v - y0)].
     * We want user (0, 0) → surf pixel (mon_ox, mon_oy), so x0 = -mon_ox.
     */
    cairo_set_source_surface(cr, bg_surf,
                             -(double)mon_ox,
                             -(double)mon_oy);
    cairo_paint(cr);
    cairo_restore(cr);
}

/**
 * Map logical virtual-desktop coords to image pixels (same as snapx_image_crop_desktop).
 */
static void paint_background_desktop(RegionState *st, cairo_t *cr, int w, int h,
                                       int mon_idx)
{
    if (!st->bg_surf) {
        paint_background(cr, w, h, NULL, 0, 0, 0, 0);
        return;
    }

    static gboolean logged_image = FALSE;
    if (!logged_image) {
        logged_image = TRUE;
        int img_w = cairo_image_surface_get_width(st->bg_surf);
        int img_h = cairo_image_surface_get_height(st->bg_surf);
        fprintf(stderr,
                "[overlay] freeze image %dx%d, virtual desktop %dx%d at (%d,%d)\n",
                img_w, img_h, st->virt_w, st->virt_h, st->virt_x, st->virt_y);
        if (st->virt_w > 0 && img_w < st->virt_w)
            fprintf(stderr,
                    "[overlay] warning: screenshot may not cover full desktop "
                    "(image width < virtual width)\n");
    }

    if (mon_idx < 0 || mon_idx >= st->n_monitors)
        return;

    const SnapxMonitorInfo *m = &st->monitors[mon_idx];
    int img_w = cairo_image_surface_get_width(st->bg_surf);
    int img_h = cairo_image_surface_get_height(st->bg_surf);
    int virt_w = st->virt_w > 0 ? st->virt_w : img_w;
    int virt_h = st->virt_h > 0 ? st->virt_h : img_h;
    if (virt_w <= 0 || virt_h <= 0)
        return;

    double px = (double)img_w / (double)virt_w;
    double py = (double)img_h / (double)virt_h;

    int sx = (int)round((m->x - st->virt_x) * px);
    int sy = (int)round((m->y - st->virt_y) * py);
    int sw = (int)round((double)m->width  * px);
    int sh = (int)round((double)m->height * py);
    if (sw <= 0 || sh <= 0)
        return;

    if (mon_idx < 32 && !(st->logged_slice_mask & (1u << mon_idx))) {
        st->logged_slice_mask |= (1u << mon_idx);
        fprintf(stderr,
                "[overlay] monitor %d slice px=%d,%d %dx%d (logical %d,%d %dx%d)\n",
                mon_idx, sx, sy, sw, sh,
                m->x, m->y, m->width, m->height);
    }

    double scale_x = (double)w / (double)sw;
    double scale_y = (double)h / (double)sh;

    cairo_save(cr);
    cairo_scale(cr, scale_x, scale_y);
    cairo_set_source_surface(cr, st->bg_surf, -(double)sx, -(double)sy);
    cairo_paint(cr);
    cairo_restore(cr);
}

static int region_mon_idx_for_geom(const RegionState *st,
                                     int ox, int oy, int ow, int oh)
{
    for (int i = 0; i < st->n_monitors; i++) {
        const SnapxMonitorInfo *m = &st->monitors[i];
        if (m->x == ox && m->y == oy &&
            m->width == ow && m->height == oh)
            return i;
    }
    return 0;
}

/* ── Paint dim (four explicit rects around the selection) ────────────────── */

static void paint_dim(cairo_t *cr, int w, int h,
                       double rx, double ry, double rw, double rh)
{
    /*
     * Four-rectangle approach — no fill-rule quirks.
     * Top / Bottom / Left / Right strips surrounding the selection hole.
     *
     *  ┌──────────────────────────────┐
     *  │          TOP (full width)    │
     *  ├───────┬──────────┬───────────┤
     *  │ LEFT  │  (clear) │   RIGHT   │
     *  ├───────┴──────────┴───────────┤
     *  │         BOTTOM (full width)  │
     *  └──────────────────────────────┘
     */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.50);

    /* Top strip */
    if (ry > 0)
        cairo_rectangle(cr, 0, 0, w, ry);
    /* Bottom strip */
    if (ry + rh < h)
        cairo_rectangle(cr, 0, ry + rh, w, h - ry - rh);
    /* Left strip (only for the selection's row range) */
    if (rx > 0)
        cairo_rectangle(cr, 0, ry, rx, rh);
    /* Right strip */
    if (rx + rw < w)
        cairo_rectangle(cr, rx + rw, ry, w - rx - rw, rh);

    cairo_fill(cr);
}

/* ── Draw: per-monitor native freeze (1:1 on each display) ──────────────── */

static void region_draw_per_monitor(RegionState *st, int idx,
                                      cairo_t *cr, int w, int h)
{
    paint_background_desktop(st, cr, w, h, idx);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const SnapxMonitorInfo *m = &st->monitors[idx];

    double cx, cy;
    region_vd_to_local(m, w, h, st->mouse_vx, st->mouse_vy, &cx, &cy);
    int active = region_monitor_at_vd(st, st->mouse_vx, st->mouse_vy);

    if (st->pressing) {
        int vx0 = st->start_vx < st->cur_vx ? st->start_vx : st->cur_vx;
        int vy0 = st->start_vy < st->cur_vy ? st->start_vy : st->cur_vy;
        int vx1 = st->start_vx > st->cur_vx ? st->start_vx : st->cur_vx;
        int vy1 = st->start_vy > st->cur_vy ? st->start_vy : st->cur_vy;

        int cx0 = vx0 < m->x ? m->x : vx0;
        int cy0 = vy0 < m->y ? m->y : vy0;
        int cx1 = vx1 > m->x + m->width  ? m->x + m->width  : vx1;
        int cy1 = vy1 > m->y + m->height ? m->y + m->height : vy1;
        if (cx1 <= cx0 || cy1 <= cy0)
            goto idle_overlay;

        double rx, ry, rw, rh;
        region_vd_to_local(m, w, h, cx0, cy0, &rx, &ry);
        double x1, y1;
        region_vd_to_local(m, w, h, cx1, cy1, &x1, &y1);
        rw = x1 - rx;
        rh = y1 - ry;

        paint_dim(cr, w, h, rx, ry, rw, rh);

        cairo_set_source_rgba(cr, 0.20, 0.60, 1.0, 0.95);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_stroke(cr);

        double hs = 5.0;
        draw_handle(cr, rx,      ry,      hs);
        draw_handle(cr, rx + rw, ry,      hs);
        draw_handle(cr, rx,      ry + rh, hs);
        draw_handle(cr, rx + rw, ry + rh, hs);

        if (idx == active) {
            char dim[48];
            snprintf(dim, sizeof(dim), "%d × %d", vx1 - vx0, vy1 - vy0);
            double by = (ry > 50) ? ry - 16 : ry + rh + 38;
            draw_badge(cr, rx + rw / 2.0, by, dim, 13.0);
            draw_badge(cr, w / 2.0, h - 30,
                       "Release to capture   |   Esc = cancel", 13.0);
        }
        return;
    }

idle_overlay:
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.50);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, cx, 0);
    cairo_line_to(cr, cx, h);
    cairo_move_to(cr, 0, cy);
    cairo_line_to(cr, w, cy);
    cairo_stroke(cr);

    if (idx == active) {
        char pos[32];
        snprintf(pos, sizeof(pos), "%d, %d", st->mouse_vx, st->mouse_vy);
        double bx = cx + 72;
        if (bx > w - 100) bx = cx - 72;
        double by = cy - 28;
        if (by < 24) by = cy + 32;
        draw_badge(cr, bx, by, pos, 12.0);
        draw_badge(cr, w / 2.0, h - 30,
                   "Drag across monitors to select   |   Esc = cancel", 13.5);
    }
}

/* ── Draw callback ───────────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4
static void region_draw_pm(GtkDrawingArea *area, cairo_t *cr,
                            int w, int h, gpointer data)
{
    (void)area;
    RegionDrawCtx *ctx = data;
    region_draw_per_monitor(ctx->st, ctx->mon_idx, cr, w, h);
}

static void region_draw(GtkDrawingArea *area, cairo_t *cr,
                         int w, int h, gpointer data)
#else
static gboolean region_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
#endif
{
#ifdef SNAPX_USE_GTK4
    RegionState *st = (RegionState *)data;
    (void)area;
    if (st->per_monitor_mode)
        return;
#else
    RegionState *st = (RegionState *)data;
    int w, h;
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    w = a.width;
    h = a.height;

    if (st->per_monitor_mode) {
        int idx = region_widget_get_mon_idx(widget);
        if (idx >= 0) {
            region_draw_per_monitor(st, idx, cr, w, h);
            return FALSE;
        }
        return FALSE;
    }
#endif

    if (!st->win || !GTK_IS_NATIVE(st->win))
        return;
#ifndef SNAPX_USE_GTK4
    (void)widget;
#endif

    get_monitor_geom(st->win, &st->mon_ox, &st->mon_oy, &st->mon_w, &st->mon_h);
    if (st->bg_surf && st->n_monitors > 0) {
        int midx = region_mon_idx_for_geom(st, st->mon_ox, st->mon_oy,
                                           st->mon_w, st->mon_h);
        paint_background_desktop(st, cr, w, h, midx);
    } else {
        paint_background(cr, w, h, st->bg_surf,
                         st->mon_ox, st->mon_oy, st->mon_w, st->mon_h);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (st->pressing) {
        double rx, ry, rw, rh;
        normalise(st->start_x, st->start_y, st->cur_x, st->cur_y,
                  &rx, &ry, &rw, &rh);
        paint_dim(cr, w, h, rx, ry, rw, rh);
        cairo_set_source_rgba(cr, 0.20, 0.60, 1.0, 0.95);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_stroke(cr);
        double hs = 5.0;
        draw_handle(cr, rx, ry, hs);
        draw_handle(cr, rx + rw, ry, hs);
        draw_handle(cr, rx, ry + rh, hs);
        draw_handle(cr, rx + rw, ry + rh, hs);
        char dim[48];
        snprintf(dim, sizeof(dim), "%d × %d", (int)rw, (int)rh);
        double by = (ry > 50) ? ry - 16 : ry + rh + 38;
        draw_badge(cr, rx + rw / 2.0, by, dim, 13.0);
        draw_badge(cr, w / 2.0, h - 30,
                   "Release to capture   |   Esc = cancel", 13.0);
    } else {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.50);
        cairo_paint(cr);
        double cx = st->mouse_x, cy = st->mouse_y;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, cx, 0);
        cairo_line_to(cr, cx, h);
        cairo_move_to(cr, 0, cy);
        cairo_line_to(cr, w, cy);
        cairo_stroke(cr);
        char pos[32];
        snprintf(pos, sizeof(pos), "%d, %d",
                 (int)cx + st->mon_ox, (int)cy + st->mon_oy);
        double bx = cx + 72;
        if (bx > w - 100) bx = cx - 72;
        double by = cy - 28;
        if (by < 24) by = cy + 32;
        draw_badge(cr, bx, by, pos, 12.0);
        draw_badge(cr, w / 2.0, h - 30,
                   "Click and drag to select region   |   Esc = cancel", 13.5);
    }

#ifndef SNAPX_USE_GTK4
    return FALSE;
#endif
}

/* ── Confirm (called on mouse-release) ──────────────────────────────────── */

static void region_confirm(RegionState *st)
{
    st->pressing = FALSE;
    if (st->per_monitor_mode) {
        int x0 = st->start_vx < st->cur_vx ? st->start_vx : st->cur_vx;
        int y0 = st->start_vy < st->cur_vy ? st->start_vy : st->cur_vy;
        int x1 = st->start_vx > st->cur_vx ? st->start_vx : st->cur_vx;
        int y1 = st->start_vy > st->cur_vy ? st->start_vy : st->cur_vy;
        if (x1 - x0 >= 4 && y1 - y0 >= 4) {
            st->result.x      = x0;
            st->result.y      = y0;
            st->result.width  = x1 - x0;
            st->result.height = y1 - y0;
            st->confirmed     = TRUE;
        } else {
            st->confirmed = FALSE;
        }
    } else {
        double rx, ry, rw, rh;
        normalise(st->start_x, st->start_y, st->cur_x, st->cur_y,
                  &rx, &ry, &rw, &rh);
        if (rw >= 4 && rh >= 4) {
            st->result.x      = (int)rx + st->mon_ox;
            st->result.y      = (int)ry + st->mon_oy;
            st->result.width  = (int)rw;
            st->result.height = (int)rh;
            st->confirmed     = TRUE;
        } else {
            st->confirmed = FALSE;
        }
    }
    region_close_all(st);
    g_main_loop_quit(st->loop);
}

static gboolean region_key_cancel(const RegionState *st, guint kv, GdkModifierType mod)
{
#if !defined(SNAPX_HEADLESS) && (defined(SNAPX_USE_GTK4) || defined(SNAPX_USE_GTK3))
    if (st->shortcuts && st->shortcuts->region_cancel[0])
        return snapx_shortcut_match(st->shortcuts->region_cancel, kv, mod);
#endif
    return kv == GDK_KEY_Escape;
}

static gboolean region_key_confirm(const RegionState *st, guint kv, GdkModifierType mod)
{
#if !defined(SNAPX_HEADLESS) && (defined(SNAPX_USE_GTK4) || defined(SNAPX_USE_GTK3))
    if (st->shortcuts && st->shortcuts->region_confirm[0])
        return snapx_shortcut_match(st->shortcuts->region_confirm, kv, mod);
#endif
    return kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter;
}

/* ── Input event handlers ────────────────────────────────────────────────── */

#ifdef SNAPX_USE_GTK4

static void reg_press(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)n;
    RegionState *st = d;
    st->pressing = TRUE;
    if (st->per_monitor_mode) {
        GtkWidget *win = gtk_event_controller_get_widget(
            GTK_EVENT_CONTROLLER(g));
        int idx = region_win_index(st, win);
        int vx, vy;
        region_local_to_vd(st, idx, x, y, &vx, &vy);
        st->start_vx = st->cur_vx = st->mouse_vx = vx;
        st->start_vy = st->cur_vy = st->mouse_vy = vy;
    } else {
        st->start_x = x;
        st->start_y = y;
        st->cur_x   = x;
        st->cur_y   = y;
    }
    region_queue_draw_all(st);
}

static void reg_release(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)n;
    RegionState *st = d;
    if (st->per_monitor_mode) {
        GtkWidget *win = gtk_event_controller_get_widget(
            GTK_EVENT_CONTROLLER(g));
        int idx = region_win_index(st, win);
        int vx, vy;
        region_local_to_vd(st, idx, x, y, &vx, &vy);
        st->cur_vx = vx;
        st->cur_vy = vy;
    } else {
        st->cur_x = x;
        st->cur_y = y;
    }
    region_confirm(st);
}

static void reg_motion(GtkEventControllerMotion *c, double x, double y, gpointer d)
{
    RegionState *st = d;
    if (st->per_monitor_mode) {
        GtkWidget *win = gtk_event_controller_get_widget(
            GTK_EVENT_CONTROLLER(c));
        int idx = region_win_index(st, win);
        int vx, vy;
        region_local_to_vd(st, idx, x, y, &vx, &vy);
        st->mouse_vx = vx;
        st->mouse_vy = vy;
        if (st->pressing) {
            st->cur_vx = vx;
            st->cur_vy = vy;
        }
    } else {
        (void)c;
        st->mouse_x = x;
        st->mouse_y = y;
        if (st->pressing) {
            st->cur_x = x;
            st->cur_y = y;
        }
    }
    region_queue_draw_all(st);
}

static gboolean reg_key(GtkEventControllerKey *c, guint kv, guint kc,
                          GdkModifierType mod, gpointer d)
{
    (void)c; (void)kc;
    RegionState *st = d;
    if (region_key_cancel(st, kv, mod)) {
        st->confirmed = FALSE;
        region_close_all(st);
        g_main_loop_quit(st->loop);
        return TRUE;
    }
    if (st->pressing && region_key_confirm(st, kv, mod)) {
        region_confirm(st);
        return TRUE;
    }
    return FALSE;
}

#else /* GTK3 */

static gboolean reg_bp(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    RegionState *st = d;
    if (e->button == 1) {
        st->pressing = TRUE;
        if (st->per_monitor_mode) {
            GtkWidget *win = gtk_widget_get_toplevel(w);
            int idx = region_win_index(st, win);
            int vx, vy;
            region_local_to_vd(st, idx, e->x, e->y, &vx, &vy);
            st->start_vx = st->cur_vx = st->mouse_vx = vx;
            st->start_vy = st->cur_vy = st->mouse_vy = vy;
        } else {
            st->start_x = e->x;
            st->start_y = e->y;
            st->cur_x   = e->x;
            st->cur_y   = e->y;
        }
        region_queue_draw_all(st);
    }
    return TRUE;
}

static gboolean reg_br(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    RegionState *st = d;
    if (e->button == 1) {
        if (st->per_monitor_mode) {
            GtkWidget *win = gtk_widget_get_toplevel(w);
            int idx = region_win_index(st, win);
            int vx, vy;
            region_local_to_vd(st, idx, e->x, e->y, &vx, &vy);
            st->cur_vx = vx;
            st->cur_vy = vy;
        } else {
            st->cur_x = e->x;
            st->cur_y = e->y;
        }
        region_confirm(st);
    }
    return TRUE;
}

static gboolean reg_mo(GtkWidget *w, GdkEventMotion *e, gpointer d)
{
    RegionState *st = d;
    if (st->per_monitor_mode) {
        GtkWidget *win = gtk_widget_get_toplevel(w);
        int idx = region_win_index(st, win);
        int vx, vy;
        region_local_to_vd(st, idx, e->x, e->y, &vx, &vy);
        st->mouse_vx = vx;
        st->mouse_vy = vy;
        if (st->pressing) {
            st->cur_vx = vx;
            st->cur_vy = vy;
        }
    } else {
        (void)w;
        st->mouse_x = e->x;
        st->mouse_y = e->y;
        if (st->pressing) {
            st->cur_x = e->x;
            st->cur_y = e->y;
        }
    }
    region_queue_draw_all(st);
    return TRUE;
}

static gboolean reg_kp(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w;
    RegionState *st = d;
    if (region_key_cancel(st, e->keyval, e->state)) {
        st->confirmed = FALSE;
        region_close_all(st);
        g_main_loop_quit(st->loop);
        return TRUE;
    }
    if (st->pressing && region_key_confirm(st, e->keyval, e->state)) {
        region_confirm(st);
        return TRUE;
    }
    return FALSE;
}

#endif

/* ── Helper: monitor under cursor (Flameshot-style target display) ──────── */

static GdkMonitor *get_monitor_at_cursor(GdkDisplay *dpy)
{
    if (!dpy) return NULL;

    GdkSeat *seat = gdk_display_get_default_seat(dpy);
    if (seat) {
        GdkDevice *pointer = gdk_seat_get_pointer(seat);
        if (pointer) {
#ifdef SNAPX_USE_GTK4
            double wx = 0, wy = 0;
            GdkSurface *surf = gdk_device_get_surface_at_position(
                pointer, &wx, &wy);
            if (surf) {
                GdkMonitor *mon = gdk_display_get_monitor_at_surface(dpy, surf);
                if (mon) return mon;
            }
#else
            gint px = 0, py = 0;
            gdk_device_get_position(pointer, &px, &py, NULL);
            GdkMonitor *mon = gdk_display_get_monitor_at_point(dpy, px, py);
            if (mon) return mon;
#endif
        }
    }

    {
        GListModel *mons = gdk_display_get_monitors(dpy);
        guint n = g_list_model_get_n_items(mons);
        if (n > 0)
            return GDK_MONITOR(g_list_model_get_item(mons, 0));
    }
    return NULL;
}

static void seed_monitor_geom(GdkMonitor *mon,
                                int *ox, int *oy, int *ow, int *oh)
{
    *ox = 0; *oy = 0; *ow = 1920; *oh = 1080;
    if (!mon) return;
    GdkRectangle geom = {0};
    gdk_monitor_get_geometry(mon, &geom);
    *ox = geom.x; *oy = geom.y; *ow = geom.width; *oh = geom.height;
}

/* ── Helper: get monitor geometry of a GtkWindow ────────────────────────── */

static void get_monitor_geom(GtkWidget *win,
                               int *ox, int *oy, int *ow, int *oh)
{
    *ox = *oy = 0; *ow = 1920; *oh = 1080;
    if (!win || !GTK_IS_NATIVE(win))
        return;
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

#ifdef SNAPX_USE_GTK4
static void region_attach_controllers(GtkWidget *win, RegionState *st)
{
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed",  G_CALLBACK(reg_press),   st);
    g_signal_connect(click, "released", G_CALLBACK(reg_release), st);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(click));

    GtkEventController *mot = gtk_event_controller_motion_new();
    g_signal_connect(mot, "motion", G_CALLBACK(reg_motion), st);
    gtk_widget_add_controller(win, mot);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(reg_key), st);
    gtk_widget_add_controller(win, key);

    gtk_widget_set_focusable(win, TRUE);
    gtk_widget_set_can_focus(win, TRUE);
}
#endif

static void region_set_crosshair_cursor(GtkWidget *win)
{
#ifdef SNAPX_USE_GTK4
    GdkCursor *cur = gdk_cursor_new_from_name("crosshair", NULL);
    if (cur) {
        gtk_widget_set_cursor(win, cur);
        g_object_unref(cur);
    }
#else
    GdkCursor *cur = gdk_cursor_new_from_name(
        gtk_widget_get_display(win), "crosshair");
    if (cur) {
        gdk_window_set_cursor(gtk_widget_get_window(win), cur);
        g_object_unref(cur);
    }
#endif
}

static gboolean region_run_per_monitor(RegionState *st, GdkDisplay *dpy)
{
    for (int i = 0; i < st->n_monitors; i++) {
        GdkMonitor *gmon = find_gdk_monitor_for_info(dpy, &st->monitors[i]);
        if (!gmon)
            continue;

#ifdef SNAPX_USE_GTK4
        GtkWidget *win = gtk_window_new();
#else
        GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#endif
        int wi = st->n_windows;
        if (wi >= 8) {
            g_object_unref(gmon);
            continue;
        }

        st->wins[wi] = win;
        gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
        region_widget_set_mon_idx(win, i);

        GtkWidget *da = gtk_drawing_area_new();
        st->das[wi] = da;

#ifdef SNAPX_USE_GTK4
        RegionDrawCtx *ctx = g_new(RegionDrawCtx, 1);
        ctx->st      = st;
        ctx->mon_idx = i;
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da),
            (GtkDrawingAreaDrawFunc)region_draw_pm, ctx,
            (GDestroyNotify)g_free);
        gtk_window_set_child(GTK_WINDOW(win), da);
        region_attach_controllers(win, st);
#else
        region_widget_set_mon_idx(da, i);
        gtk_widget_set_events(da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                  | GDK_POINTER_MOTION_MASK);
        g_signal_connect(da, "draw", G_CALLBACK(region_draw), st);
        g_signal_connect(da, "button-press-event", G_CALLBACK(reg_bp), st);
        g_signal_connect(da, "button-release-event", G_CALLBACK(reg_br), st);
        g_signal_connect(da, "motion-notify-event", G_CALLBACK(reg_mo), st);
        g_signal_connect(win, "key-press-event", G_CALLBACK(reg_kp), st);
        gtk_container_add(GTK_CONTAINER(win), da);
#endif

        gtk_window_fullscreen_on_monitor(GTK_WINDOW(win), gmon);
        g_object_unref(gmon);

        gtk_widget_set_visible(win, TRUE);
        region_set_crosshair_cursor(win);
        if (wi == 0)
            gtk_widget_grab_focus(win);
        st->n_windows++;
    }

    if (st->n_windows == 0)
        return FALSE;

#ifndef SNAPX_USE_GTK4
    gtk_widget_show_all(st->wins[0]);
#endif

    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);

    int vx, vy;
    if (region_get_pointer_vd(&vx, &vy)) {
        st->mouse_vx = vx;
        st->mouse_vy = vy;
    }

    g_main_loop_run(st->loop);
    return TRUE;
}

/* ── Public: region selection ────────────────────────────────────────────── */

int snapx_overlay_select_region(GtkWindow              *parent,
                                  const SnapxImage       *background,
                                  const SnapxMonitorInfo *monitors,
                                  int                     n_monitors,
                                  const SnapxShortcuts   *shortcuts,
                                  SnapxRegion            *region)
{
    RegionState st = {0};
    st.loop       = g_main_loop_new(NULL, FALSE);
    st.background = background;
    st.shortcuts  = shortcuts;
    (void)parent;
    if (background)
        st.bg_surf = image_to_cairo(background);

    if (background && monitors && n_monitors > 0) {
        int copy_n = n_monitors > 8 ? 8 : n_monitors;
        memcpy(st.monitors, monitors, (size_t)copy_n * sizeof(SnapxMonitorInfo));
        st.n_monitors = copy_n;
        region_calc_virt_bounds(st.monitors, st.n_monitors,
                                &st.virt_x, &st.virt_y, &st.virt_w, &st.virt_h);
        if (copy_n > 1)
            st.per_monitor_mode = TRUE;
    } else if (background && monitors && n_monitors == 1) {
        memcpy(st.monitors, monitors, sizeof(SnapxMonitorInfo));
        st.n_monitors = 1;
        region_calc_virt_bounds(st.monitors, 1,
                                &st.virt_x, &st.virt_y, &st.virt_w, &st.virt_h);
    }

    GdkDisplay *dpy = gdk_display_get_default();

    if (st.per_monitor_mode && dpy) {
        if (!region_run_per_monitor(&st, dpy)) {
            g_main_loop_unref(st.loop);
            if (st.bg_surf) cairo_surface_destroy(st.bg_surf);
            return 0;
        }
        g_main_loop_unref(st.loop);
        if (st.bg_surf) cairo_surface_destroy(st.bg_surf);
        if (st.confirmed && region) {
            *region = st.result;
            return 1;
        }
        return 0;
    }

    /* Single-monitor: fullscreen on cursor display */
#ifdef SNAPX_USE_GTK4
    GtkWidget *win = gtk_window_new();
#else
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#endif
    st.win = win;
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);

    GdkMonitor *target_mon = get_monitor_at_cursor(dpy);
    seed_monitor_geom(target_mon, &st.mon_ox, &st.mon_oy, &st.mon_w, &st.mon_h);

    if (!background) {
#ifndef SNAPX_USE_GTK4
        GdkScreen *scr = gtk_window_get_screen(GTK_WINDOW(win));
        GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
        if (vis) gtk_widget_set_visual(win, vis);
        gtk_widget_set_app_paintable(win, TRUE);
#else
        gtk_widget_add_css_class(win, "snapx-region-live");
#endif
    }

    GtkWidget *da = gtk_drawing_area_new();
    st.da = da;

    if (target_mon)
        gtk_window_fullscreen_on_monitor(GTK_WINDOW(win), target_mon);
    else
        gtk_window_fullscreen(GTK_WINDOW(win));

#ifdef SNAPX_USE_GTK4
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da),
                                    (GtkDrawingAreaDrawFunc)region_draw, &st, NULL);
    gtk_window_set_child(GTK_WINDOW(win), da);
    region_attach_controllers(win, &st);
#else
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

    region_set_crosshair_cursor(win);
    gtk_widget_set_visible(win, TRUE);
    gtk_widget_grab_focus(win);

    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
    get_monitor_geom(win, &st.mon_ox, &st.mon_oy, &st.mon_w, &st.mon_h);

    g_main_loop_run(st.loop);
    g_main_loop_unref(st.loop);

    if (st.bg_surf) cairo_surface_destroy(st.bg_surf);
    if (st.win)
        gtk_window_destroy(GTK_WINDOW(st.win));

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

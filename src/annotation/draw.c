/**
 * @file draw.c
 * @brief Cairo-based annotation rendering: rectangles, arrows, pen, text, blur, highlight.
 */

#include "annotation.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>   /* G_PI */

#ifndef M_PI
#  define M_PI G_PI
#endif

/* ─── Arrow rendering ────────────────────────────────────────────────────── */

/**
 * @brief Draw an arrowhead at point (x2,y2) pointing from (x1,y1).
 */
static void draw_arrowhead(cairo_t *cr, double x1, double y1, double x2, double y2,
                            double head_len)
{
    double angle = atan2(y2 - y1, x2 - x1);
    double spread = M_PI / 6.0;   /* 30° each side */

    double ax1 = x2 - head_len * cos(angle - spread);
    double ay1 = y2 - head_len * sin(angle - spread);
    double ax2 = x2 - head_len * cos(angle + spread);
    double ay2 = y2 - head_len * sin(angle + spread);

    cairo_move_to(cr, x2, y2);
    cairo_line_to(cr, ax1, ay1);
    cairo_move_to(cr, x2, y2);
    cairo_line_to(cr, ax2, ay2);
    cairo_stroke(cr);
}

/* ─── Blur / pixelate ────────────────────────────────────────────────────── */

void snapx_draw_blur(cairo_t *cr, cairo_surface_t *surface,
                     double rx, double ry, double rw, double rh,
                     int radius)
{
    /* Simple box pixelation: divide region into blocks, fill each with average colour */
    cairo_surface_flush(surface);
    unsigned char *data   = cairo_image_surface_get_data(surface);
    int            stride = cairo_image_surface_get_stride(surface);
    int            sw     = cairo_image_surface_get_width(surface);
    int            sh     = cairo_image_surface_get_height(surface);

    int x0 = (int)rx, y0 = (int)ry;
    int x1 = (int)(rx + rw), y1 = (int)(ry + rh);
    if (x0 < 0)  x0 = 0;
    if (y0 < 0)  y0 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (radius < 2) radius = 8;

    for (int by = y0; by < y1; by += radius) {
        for (int bx = x0; bx < x1; bx += radius) {
            long sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int py = by; py < by + radius && py < y1; py++) {
                for (int px = bx; px < bx + radius && px < x1; px++) {
                    unsigned char *p = data + py * stride + px * 4;
                    sb += p[0]; sg += p[1]; sr += p[2]; cnt++;
                }
            }
            if (cnt == 0) continue;
            unsigned char ar = (unsigned char)(sr / cnt);
            unsigned char ag = (unsigned char)(sg / cnt);
            unsigned char ab = (unsigned char)(sb / cnt);
            for (int py = by; py < by + radius && py < y1; py++) {
                for (int px = bx; px < bx + radius && px < x1; px++) {
                    unsigned char *p = data + py * stride + px * 4;
                    p[0] = ab; p[1] = ag; p[2] = ar; p[3] = 0xFF;
                }
            }
        }
    }
    cairo_surface_mark_dirty(surface);
    /* No Cairo paint needed; we modified the surface pixels directly */
    (void)cr;
}

/* ─── Main render dispatch ───────────────────────────────────────────────── */

void snapx_draw_annotation(cairo_t *cr, const SnapxAnnotation *ann)
{
    if (!cr || !ann) return;

    cairo_save(cr);
    gdk_cairo_set_source_rgba(cr, &ann->color);
    cairo_set_line_width(cr, ann->line_width > 0 ? ann->line_width : 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    switch (ann->tool) {

        case SNAPX_TOOL_RECT: {
            double x = ann->rect.x1, y = ann->rect.y1;
            double w = ann->rect.x2 - ann->rect.x1;
            double h = ann->rect.y2 - ann->rect.y1;
            cairo_rectangle(cr, x, y, w, h);
            cairo_stroke(cr);
            break;
        }

        case SNAPX_TOOL_ARROW: {
            cairo_move_to(cr, ann->rect.x1, ann->rect.y1);
            cairo_line_to(cr, ann->rect.x2, ann->rect.y2);
            cairo_stroke(cr);
            double len = hypot(ann->rect.x2 - ann->rect.x1,
                               ann->rect.y2 - ann->rect.y1);
            draw_arrowhead(cr, ann->rect.x1, ann->rect.y1,
                               ann->rect.x2, ann->rect.y2,
                               len * 0.15 + 10);
            break;
        }

        case SNAPX_TOOL_PEN: {
            if (ann->points.n < 2) break;
            cairo_move_to(cr, ann->points.xs[0], ann->points.ys[0]);
            for (int i = 1; i < ann->points.n; i++) {
                if (i + 1 < ann->points.n) {
                    /* Smooth curve via midpoints */
                    double mx = (ann->points.xs[i] + ann->points.xs[i + 1]) / 2.0;
                    double my = (ann->points.ys[i] + ann->points.ys[i + 1]) / 2.0;
                    cairo_curve_to(cr,
                                   ann->points.xs[i], ann->points.ys[i],
                                   ann->points.xs[i], ann->points.ys[i],
                                   mx, my);
                } else {
                    cairo_line_to(cr, ann->points.xs[i], ann->points.ys[i]);
                }
            }
            cairo_stroke(cr);
            break;
        }

        case SNAPX_TOOL_TEXT: {
            cairo_select_font_face(cr, "Sans",
                                   CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, ann->font_size > 0 ? ann->font_size : 18.0);
            cairo_move_to(cr, ann->rect.x1, ann->rect.y1);
            cairo_show_text(cr, ann->text);
            break;
        }

        case SNAPX_TOOL_BLUR:
            /* Blur is applied directly to surface pixels in snapx_draw_blur;
             * nothing to paint here at render time. */
            break;

        case SNAPX_TOOL_HIGHLIGHT: {
            /* Semi-transparent filled rectangle */
            GdkRGBA hi = ann->color;
            hi.alpha = 0.35;
            gdk_cairo_set_source_rgba(cr, &hi);
            double x = ann->rect.x1, y = ann->rect.y1;
            double w = ann->rect.x2 - ann->rect.x1;
            double h = ann->rect.y2 - ann->rect.y1;
            cairo_rectangle(cr, x, y, w, h);
            cairo_fill(cr);
            break;
        }

        case SNAPX_TOOL_REDACT: {
            cairo_set_source_rgb(cr, 0, 0, 0);
            double x = ann->rect.x1, y = ann->rect.y1;
            double w = ann->rect.x2 - ann->rect.x1;
            double h = ann->rect.y2 - ann->rect.y1;
            cairo_rectangle(cr, x, y, w, h);
            cairo_fill(cr);
            break;
        }

        case SNAPX_TOOL_CALLOUT: {
            double cx = ann->rect.x1, cy = ann->rect.y1;
            double r = 14.0;
            cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
            cairo_fill_preserve(cr);
            cairo_stroke(cr);
            char num[16];
            snprintf(num, sizeof(num), "%d",
                     ann->callout_num > 0 ? ann->callout_num : 1);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_select_font_face(cr, "Sans",
                                   CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 14);
            cairo_text_extents_t te;
            cairo_text_extents(cr, num, &te);
            cairo_move_to(cr, cx - te.width / 2 - te.x_bearing,
                          cy - te.height / 2 - te.y_bearing);
            cairo_show_text(cr, num);
            if (fabs(ann->rect.x2 - ann->rect.x1) > 4 || fabs(ann->rect.y2 - ann->rect.y1) > 4) {
                gdk_cairo_set_source_rgba(cr, &ann->color);
                cairo_move_to(cr, cx, cy);
                cairo_line_to(cr, ann->rect.x2, ann->rect.y2);
                cairo_stroke(cr);
                draw_arrowhead(cr, cx, cy, ann->rect.x2, ann->rect.y2, 10);
            }
            break;
        }
    }

    cairo_restore(cr);
}

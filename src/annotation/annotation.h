/**
 * @file annotation.h
 * @brief Annotation types and tool definitions shared across annotation modules.
 */

#ifndef SNAPX_ANNOTATION_H
#define SNAPX_ANNOTATION_H

#include <cairo/cairo.h>

#ifdef SNAPX_USE_GTK4
#  include <gtk/gtk.h>
#elif defined(SNAPX_USE_GTK3)
#  include <gtk/gtk.h>
#endif

/* ─── Tool types ─────────────────────────────────────────────────────────── */

#ifndef SNAPX_ANNOTATION_TOOL_DEFINED
#define SNAPX_ANNOTATION_TOOL_DEFINED
typedef enum {
    SNAPX_TOOL_RECT      = 0,
    SNAPX_TOOL_ARROW     = 1,
    SNAPX_TOOL_PEN       = 2,
    SNAPX_TOOL_TEXT      = 3,
    SNAPX_TOOL_BLUR      = 4,
    SNAPX_TOOL_HIGHLIGHT = 5,
} SnapxAnnotationTool;
#endif

/* ─── A single drawn shape ───────────────────────────────────────────────── */

/** @brief Maximum number of points in a freehand stroke. */
#define SNAPX_PEN_MAX_POINTS 8192

typedef struct {
    double x1, y1;  /**< Start point / first corner               */
    double x2, y2;  /**< End point / opposite corner              */
} SnapxRect;

typedef struct {
    double *xs;   /**< Array of x coordinates (heap)              */
    double *ys;   /**< Array of y coordinates (heap)              */
    int     n;    /**< Number of points stored                    */
    int     cap;  /**< Current allocation capacity                */
} SnapxStrokePoints;

typedef struct SnapxAnnotation {
    SnapxAnnotationTool tool;
    GdkRGBA             color;
    double              line_width;

    /* Shape data — union-like usage depending on tool */
    SnapxRect           rect;         /**< RECT, BLUR, HIGHLIGHT           */
    SnapxStrokePoints   points;       /**< PEN                             */
    char                text[256];    /**< TEXT                            */
    double              font_size;    /**< TEXT                            */

    struct SnapxAnnotation *next;     /**< Linked list (undo stack node)   */
} SnapxAnnotation;

/* ─── Drawing primitives (used by draw.c) ───────────────────────────────── */

/**
 * @brief Render a single annotation onto a Cairo context.
 * @param cr   Cairo context with coordinates in image pixels.
 * @param ann  Annotation to render.
 */
void snapx_draw_annotation(cairo_t *cr, const SnapxAnnotation *ann);

/**
 * @brief Draw a blur/pixelate effect over the given rectangle.
 * @param cr        Cairo context.
 * @param surface   The image surface being annotated (for source pixels).
 * @param rx,ry     Top-left of blur region.
 * @param rw,rh     Width/height of blur region.
 * @param radius    Blur/pixelation block size (pixels).
 */
void snapx_draw_blur(cairo_t *cr, cairo_surface_t *surface,
                     double rx, double ry, double rw, double rh,
                     int radius);

#endif /* SNAPX_ANNOTATION_H */

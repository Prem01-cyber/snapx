/**
 * @file canvas.h
 * @brief Annotation canvas: manages the undo/redo stack and stroke state.
 */

#ifndef SNAPX_CANVAS_H
#define SNAPX_CANVAS_H

#include "annotation.h"
#include "../capture/capture.h"

/* ─── Canvas ─────────────────────────────────────────────────────────────── */

typedef struct SnapxAnnotationCanvas SnapxAnnotationCanvas;

/**
 * @brief Allocate a new annotation canvas for an image of given dimensions.
 */
SnapxAnnotationCanvas *snapx_canvas_new(int width, int height);

/**
 * @brief Free the canvas and all stored annotations.
 */
void snapx_canvas_free(SnapxAnnotationCanvas *canvas);

/**
 * @brief Set the active annotation tool.
 */
void snapx_canvas_set_tool(SnapxAnnotationCanvas *canvas, SnapxAnnotationTool tool);

/**
 * @brief Set the active annotation colour.
 */
void snapx_canvas_set_color(SnapxAnnotationCanvas *canvas, const GdkRGBA *color);

/**
 * @brief Begin a new stroke at image coordinates (x, y).
 */
void snapx_canvas_stroke_begin(SnapxAnnotationCanvas *canvas, double x, double y);

/**
 * @brief Update the current stroke with a new point.
 */
void snapx_canvas_stroke_update(SnapxAnnotationCanvas *canvas, double x, double y);

/**
 * @brief Finish the current stroke, committing it to the undo stack.
 */
void snapx_canvas_stroke_end(SnapxAnnotationCanvas *canvas, double x, double y);

/** Commit pending TEXT annotation with @p text (popover entry). */
void snapx_canvas_stroke_end_text(SnapxAnnotationCanvas *canvas,
                                  double x, double y, const char *text);

/**
 * @brief Undo the last committed annotation.
 */
void snapx_canvas_undo(SnapxAnnotationCanvas *canvas);

/**
 * @brief Redo the most recently undone annotation.
 */
void snapx_canvas_redo(SnapxAnnotationCanvas *canvas);

/**
 * @brief Render all committed annotations onto the Cairo context.
 * Also renders the in-progress (pending) stroke at 80% opacity.
 * @param canvas  The annotation canvas.
 * @param cr      Cairo context (image coordinates).
 */
void snapx_canvas_render(const SnapxAnnotationCanvas *canvas, cairo_t *cr);

/**
 * @brief Render ONLY committed (finished) annotations — no pending stroke.
 *
 * Use this to populate a long-lived cache surface.  The cache is valid
 * until the next stroke is committed (i.e. stroke_end is called).
 */
void snapx_canvas_render_committed(const SnapxAnnotationCanvas *canvas, cairo_t *cr);

/**
 * @brief Render ONLY the in-progress (pending) stroke.
 *
 * Returns immediately if there is no pending stroke.
 * Composite this on top of a committed-cache surface each frame for
 * zero-lag live preview without rebuilding the full cache.
 */
void snapx_canvas_render_pending(const SnapxAnnotationCanvas *canvas, cairo_t *cr);

/**
 * @brief Return non-zero if a stroke is currently in progress.
 */
int snapx_canvas_has_pending(const SnapxAnnotationCanvas *canvas);

/**
 * @brief Create a new SnapxImage with all annotations composited onto it.
 * @return Newly allocated flat image; caller must call snapx_image_free().
 */
SnapxImage *snapx_canvas_flatten(const SnapxAnnotationCanvas *canvas,
                                  const SnapxImage *base_image);

#endif /* SNAPX_CANVAS_H */

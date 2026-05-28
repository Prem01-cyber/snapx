/**
 * @file toolbar.h
 * @brief Annotation toolbar declarations.
 */

#ifndef SNAPX_TOOLBAR_H
#define SNAPX_TOOLBAR_H

#ifdef SNAPX_USE_GTK4
#  include <gtk/gtk.h>
#elif defined(SNAPX_USE_GTK3)
#  include <gtk/gtk.h>
#endif

#include "../annotation/canvas.h"

/**
 * @brief Create the annotation toolbar widget.
 *
 * @param canvas  Annotation canvas (may be NULL on first call; updated later).
 * @param drawing_area  Drawing area to queue redraws on.
 * @return A GtkBox widget containing the toolbar.
 */
GtkWidget *snapx_toolbar_create(SnapxAnnotationCanvas *canvas,
                                 GtkWidget *drawing_area);

/**
 * @brief Connect mouse/gesture events on @p drawing_area to the annotation
 *        canvas.  Call this after the canvas and drawing area are both ready.
 */
void snapx_toolbar_connect_canvas_events(GtkWidget *drawing_area,
                                          SnapxAnnotationCanvas *canvas);

#endif /* SNAPX_TOOLBAR_H */

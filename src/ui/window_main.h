/**
 * @file window_main.h
 * @brief Main application window declarations.
 */

#ifndef SNAPX_WINDOW_MAIN_H
#define SNAPX_WINDOW_MAIN_H

#ifdef SNAPX_USE_GTK4
#  include <gtk/gtk.h>
#elif defined(SNAPX_USE_GTK3)
#  include <gtk/gtk.h>
#endif

#include "../capture/capture.h"
#include "../utils/config.h"

/**
 * @brief Create and show the main snapx window.
 *
 * @param app     The GtkApplication owning the window.
 * @param config  Pointer to the global config (shared).
 * @param backend Active capture backend.
 * @param mode    Initial capture mode to use on first launch.
 */
void snapx_window_main_create(GtkApplication      *app,
                               SnapxConfig          *config,
                               SnapxCaptureBackend  *backend,
                               SnapxCaptureMode     *mode);

/**
 * @brief Load a captured image into the main window preview area.
 *
 * @param img  Image to display (window takes ownership of the pixel data).
 */
void snapx_window_main_set_image(SnapxImage *img);

/** Request a single coalesced redraw of the preview/annotation canvas. */
void snapx_main_schedule_redraw(void);

/**
 * Convert drawing-area coordinates to image pixel coordinates.
 * Use before passing points to the annotation canvas.
 */
void snapx_main_widget_to_image(double wx, double wy, double *ix, double *iy);

#endif /* SNAPX_WINDOW_MAIN_H */

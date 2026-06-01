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

/** Apply pixelate blur to the current image (image coordinates). */
void snapx_main_apply_blur_region(double x1, double y1, double x2, double y2);

/**
 * Convert drawing-area coordinates to image pixel coordinates.
 * Use before passing points to the annotation canvas.
 */
void snapx_main_widget_to_image(double wx, double wy, double *ix, double *iy);

/**
 * Sample the colour of the current image at image pixel (ix, iy).
 * @param out  Receives the colour (RGBA, components 0..1).
 * @return 1 if a colour was sampled, 0 if no image or out of bounds.
 */
int snapx_main_sample_color(double ix, double iy, GdkRGBA *out);

/** Tray / D-Bus: trigger capture without including window internals. */
void snapx_window_main_capture_region(void);
void snapx_window_main_show(void);
GtkApplication *snapx_window_main_get_app(void);
const char *snapx_window_main_get_last_path(void);
int snapx_window_main_save_to(const char *path);

#endif /* SNAPX_WINDOW_MAIN_H */

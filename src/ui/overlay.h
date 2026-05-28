/**
 * @file overlay.h
 * @brief Full-screen region-selection overlay declarations.
 */

#ifndef SNAPX_OVERLAY_H
#define SNAPX_OVERLAY_H

#ifdef SNAPX_USE_GTK4
#  include <gtk/gtk.h>
#elif defined(SNAPX_USE_GTK3)
#  include <gtk/gtk.h>
#endif

/** @brief Selected rectangular region in screen coordinates. */
typedef struct {
    int x, y;
    int width, height;
} SnapxRegion;

/**
 * @brief Show a full-screen transparent overlay and let the user draw a region.
 *
 * Blocks until the user confirms (Enter / mouse release) or cancels (Escape).
 *
 * @param parent  Parent window (may be NULL).
 * @param region  Output region in root/screen coordinates.
 * @return 1 if a region was selected, 0 if cancelled.
 */
int snapx_overlay_select_region(GtkWindow *parent, SnapxRegion *region);

#endif /* SNAPX_OVERLAY_H */

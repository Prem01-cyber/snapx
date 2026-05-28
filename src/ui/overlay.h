/**
 * @file overlay.h
 * @brief Full-screen region-selection overlay declarations.
 */

#ifndef SNAPX_OVERLAY_H
#define SNAPX_OVERLAY_H

#if defined(SNAPX_USE_GTK4) || defined(SNAPX_USE_GTK3)
#  include <gtk/gtk.h>
#endif

#include "../capture/capture.h"

/** @brief Selected rectangular region in image coordinates. */
typedef struct {
    int x, y;
    int width, height;
} SnapxRegion;

/**
 * @brief Show a fullscreen freeze-frame region-selection overlay.
 *
 * Displays @p background as a full-screen frozen screenshot so the user
 * can see exactly what they are cropping across all monitors.
 * The returned region coordinates are in @p background pixel space.
 *
 * @param parent     Transient-for window (may be NULL).
 * @param background Pre-captured full-desktop screenshot.  If NULL the overlay
 *                   falls back to a semi-transparent dim (legacy mode).
 * @param region     Output: selected rectangle in background pixel coordinates.
 * @return 1 if a region was confirmed, 0 if cancelled.
 */
int snapx_overlay_select_region(GtkWindow        *parent,
                                  const SnapxImage *background,
                                  SnapxRegion      *region);

#endif /* SNAPX_OVERLAY_H */

/**
 * @file overlay.h
 * @brief Full-screen overlay declarations — region selection + monitor picker.
 */

#ifndef SNAPX_OVERLAY_H
#define SNAPX_OVERLAY_H

#if defined(SNAPX_USE_GTK4) || defined(SNAPX_USE_GTK3)
#  include <gtk/gtk.h>
#endif

#include "../capture/capture.h"

/** @brief Selected rectangular region in virtual-desktop pixel coordinates. */
typedef struct {
    int x, y;          /**< Top-left, in virtual-desktop space (accounts for   */
    int width, height; /**< monitor offsets on multi-monitor systems).          */
} SnapxRegion;

/**
 * @brief Show a fullscreen region-selection overlay on the cursor monitor.
 *
 * The overlay is opened fullscreen on the monitor under the pointer (Flameshot
 * style), not necessarily the primary display.
 *
 * Two modes depending on @p background:
 *
 *  - **background != NULL (freeze-frame / Wayland mode)**:
 *    The full virtual-desktop screenshot is shown as the background.
 *    Only that monitor's slice of @p background is rendered.  The returned
 *    rectangle is in virtual-desktop coordinates; the caller should crop a
 *    **fresh** capture for the final image (not @p background).
 *
 *  - **background == NULL (transparent / X11 mode)**:
 *    The overlay is visually transparent over the live desktop.  The returned
 *    rectangle is still in virtual-desktop coordinates.
 *
 * @param parent      Unused (pass NULL); kept for API stability.
 * @param background  Full virtual-desktop screenshot for overlay UX, or NULL.
 * @param region      Output: selected rectangle in virtual-desktop coordinates.
 * @return 1 if a region was confirmed, 0 if cancelled.
 */
int snapx_overlay_select_region(GtkWindow        *parent,
                                  const SnapxImage *background,
                                  SnapxRegion      *region);

/**
 * @brief Show a fullscreen monitor-picker overlay.
 *
 * Draws the virtual desktop layout to scale.  Each monitor is shown as a
 * labelled rectangle; hovering highlights it; clicking confirms the choice.
 *
 * @param parent    Transient-for window (may be NULL).
 * @param monitors  Array of monitor descriptors from snapx_get_monitors().
 * @param n         Number of entries in @p monitors.
 * @return 0-based index of the selected monitor, or -1 if cancelled.
 */
int snapx_overlay_select_monitor(GtkWindow              *parent,
                                   const SnapxMonitorInfo *monitors,
                                   int                     n);

#endif /* SNAPX_OVERLAY_H */

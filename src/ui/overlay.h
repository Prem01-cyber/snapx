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
 * When @p background is non-NULL, a freeze-frame of the desktop is shown under
 * the dim (recommended on Wayland — one capture, then crop the same buffer).
 *
 * When @p background is NULL, a semi-transparent dim is used over the live
 * desktop (X11 compositing); on Wayland this may appear as a black overlay.
 *
 * When @p monitors and @p n_monitors > 1 with a full-desktop @p background,
 * one native-resolution freeze overlay is shown on each monitor (regions can
 * span displays via cross-monitor drag).
 *
 * @param parent      Unused (pass NULL); kept for API stability.
 * @param background  Optional freeze-frame for overlay only, or NULL.
 * @param monitors    Monitor layout from snapx_get_monitors (may be NULL).
 * @param n_monitors  Number of entries in @p monitors (0 if NULL).
 * @param region      Output: selected rectangle in virtual-desktop coordinates.
 * @return 1 if a region was confirmed, 0 if cancelled.
 */
int snapx_overlay_select_region(GtkWindow              *parent,
                                  const SnapxImage       *background,
                                  const SnapxMonitorInfo *monitors,
                                  int                     n_monitors,
                                  SnapxRegion            *region);

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

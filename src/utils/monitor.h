/**
 * @file monitor.h
 * @brief Multi-monitor detection and management helpers.
 */

#ifndef SNAPX_MONITOR_H
#define SNAPX_MONITOR_H

#include "../capture/capture.h"

#define SNAPX_MAX_MONITORS 16

/**
 * @brief Enumerate all connected monitors via the active backend.
 *
 * @param backend     Active capture backend (may be NULL for GDK-only query).
 * @param out         Output array of at least SNAPX_MAX_MONITORS entries.
 * @param max         Maximum number of monitors to write.
 * @return Number of monitors found, or -1 on error.
 */
int snapx_monitors_enumerate(SnapxCaptureBackend *backend,
                              SnapxMonitorInfo *out, int max);

/**
 * @brief Print monitor info to stderr (debugging helper).
 */
void snapx_monitors_print(const SnapxMonitorInfo *monitors, int count);

/**
 * @brief Return the bounding rect of all monitors combined (virtual desktop).
 */
void snapx_monitors_virtual_desktop(const SnapxMonitorInfo *monitors, int count,
                                     int *x, int *y, int *w, int *h);

#endif /* SNAPX_MONITOR_H */

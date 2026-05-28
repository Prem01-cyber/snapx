/**
 * @file monitor.c
 * @brief Multi-monitor enumeration and utility functions.
 */

#include "monitor.h"
#include <stdio.h>
#include <string.h>

int snapx_monitors_enumerate(SnapxCaptureBackend *backend,
                              SnapxMonitorInfo *out, int max)
{
    if (!out || max <= 0) return -1;
    if (backend && backend->get_monitors)
        return backend->get_monitors(backend, out, max);
    return 0;
}

void snapx_monitors_print(const SnapxMonitorInfo *monitors, int count)
{
    fprintf(stderr, "[monitors] %d monitor(s) detected:\n", count);
    for (int i = 0; i < count; i++) {
        const SnapxMonitorInfo *m = &monitors[i];
        fprintf(stderr, "  [%d] %s — %dx%d @ (%d,%d)%s scale=%d\n",
                m->index, m->name, m->width, m->height, m->x, m->y,
                m->is_primary ? " [primary]" : "", m->scale);
    }
}

void snapx_monitors_virtual_desktop(const SnapxMonitorInfo *monitors, int count,
                                     int *x, int *y, int *w, int *h)
{
    if (!monitors || count <= 0) { *x = *y = 0; *w = *h = 0; return; }
    int x0 = monitors[0].x, y0 = monitors[0].y;
    int x1 = x0 + monitors[0].width;
    int y1 = y0 + monitors[0].height;
    for (int i = 1; i < count; i++) {
        if (monitors[i].x < x0) x0 = monitors[i].x;
        if (monitors[i].y < y0) y0 = monitors[i].y;
        int rx = monitors[i].x + monitors[i].width;
        int ry = monitors[i].y + monitors[i].height;
        if (rx > x1) x1 = rx;
        if (ry > y1) y1 = ry;
    }
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

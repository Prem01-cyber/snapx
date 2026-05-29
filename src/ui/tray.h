/**
 * @file tray.h
 * @brief System tray / background mode.
 */

#ifndef SNAPX_TRAY_H
#define SNAPX_TRAY_H

#include <gtk/gtk.h>
#include "../utils/config.h"

void snapx_tray_init(void *mw_opaque, GtkApplication *app, SnapxConfig *config);
void snapx_tray_set_visible(int visible);
int  snapx_tray_is_background(void);

#endif /* SNAPX_TRAY_H */

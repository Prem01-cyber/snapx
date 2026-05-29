/**
 * @file history_panel.h
 * @brief Recent captures thumbnail sidebar.
 */

#ifndef SNAPX_HISTORY_PANEL_H
#define SNAPX_HISTORY_PANEL_H

#include <gtk/gtk.h>
#include "../utils/config.h"

typedef void (*SnapxHistorySelectFn)(const char *path, gpointer userdata);

GtkWidget *snapx_history_panel_new(const SnapxConfig *config,
                                    SnapxHistorySelectFn on_select,
                                    gpointer userdata);
void snapx_history_panel_refresh(GtkWidget *panel, const SnapxConfig *config);

#endif /* SNAPX_HISTORY_PANEL_H */

/**
 * @file history_panel.h
 * @brief Recent captures file-list overlay.
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
void snapx_history_panel_set_open(GtkWidget *panel, gboolean open);
gboolean snapx_history_panel_is_open(GtkWidget *panel);
void snapx_history_panel_toggle(GtkWidget *panel);

#endif /* SNAPX_HISTORY_PANEL_H */

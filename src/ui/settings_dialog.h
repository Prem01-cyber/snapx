/**
 * @file settings_dialog.h
 * @brief Settings/preferences dialog declarations.
 */

#ifndef SNAPX_SETTINGS_DIALOG_H
#define SNAPX_SETTINGS_DIALOG_H

#ifdef SNAPX_USE_GTK4
#  include <gtk/gtk.h>
#elif defined(SNAPX_USE_GTK3)
#  include <gtk/gtk.h>
#endif

#include "../utils/config.h"

/**
 * @brief Open the settings dialog modally.
 *
 * Changes are applied to @p config immediately when the user clicks Apply/OK.
 * The dialog saves the config file upon close.
 *
 * @param parent  Transient parent window.
 * @param config  Config to read from and write to.
 */
void snapx_settings_dialog_show(GtkWindow *parent, SnapxConfig *config);

#endif /* SNAPX_SETTINGS_DIALOG_H */

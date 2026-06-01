/**
 * @file beautify_dialog.h
 * @brief Live-preview dialog for the "beautiful screenshot" compositor.
 */

#ifndef SNAPX_BEAUTIFY_DIALOG_H
#define SNAPX_BEAUTIFY_DIALOG_H

#ifdef SNAPX_USE_GTK4
#  include <gtk/gtk.h>
#elif defined(SNAPX_USE_GTK3)
#  include <gtk/gtk.h>
#endif

#include "../capture/capture.h"
#include "../utils/config.h"

/**
 * @brief Open the beautify dialog for @p image.
 *
 * The dialog takes its own copy of @p image (the caller keeps ownership of the
 * passed pointer) and lets the user tweak padding / background / shadow with a
 * live preview, then copy or save the composited result.  Changes are written
 * back to @p config (and persisted on close).
 */
void snapx_beautify_dialog_show(GtkWindow *parent, SnapxConfig *config,
                                const SnapxImage *image);

#endif /* SNAPX_BEAUTIFY_DIALOG_H */

/**
 * @file clipboard.h
 * @brief Clipboard copy declarations.
 */

#ifndef SNAPX_CLIPBOARD_H
#define SNAPX_CLIPBOARD_H

#include "../capture/capture.h"

/**
 * @brief Copy a SnapxImage to the system clipboard as a PNG image.
 *
 * On Linux (X11/Wayland) uses GDK clipboard.
 * On Windows uses OpenClipboard / CF_DIB.
 * On macOS uses NSPasteboard.
 */
void snapx_clipboard_copy_image(const SnapxImage *img);

/** Copy plain text (e.g. upload URL) to the system clipboard. */
void snapx_clipboard_copy_text(const char *text);

#endif /* SNAPX_CLIPBOARD_H */

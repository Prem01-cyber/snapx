/**
 * @file hotkey.h
 * @brief Global hotkey registration (platform-specific).
 */

#ifndef SNAPX_HOTKEY_H
#define SNAPX_HOTKEY_H

#include "config.h"

/**
 * @brief Register the global hotkey from config->hotkey.
 *
 * On Linux/Wayland: uses a background thread watching for key events via
 *                   evdev or a DBus inhibit shortcut (best-effort).
 * On Linux/X11:     uses XGrabKey on the root window.
 * On Windows:       RegisterHotKey() Win32 API.
 * On macOS:         CGEventTap (requires Accessibility permission).
 *
 * This function is non-fatal: if hotkey registration fails it logs a warning
 * and returns without crashing.
 *
 * @param config  Config containing hotkey string.
 */
void snapx_hotkey_init(SnapxConfig *config);

/**
 * @brief Unregister hotkeys and clean up.
 */
void snapx_hotkey_cleanup(void);

/**
 * @brief Callback invoked when the global hotkey is pressed.
 *        Set by the main window after creation.
 */
typedef void (*SnapxHotkeyCallback)(void);
void snapx_hotkey_set_callback(SnapxHotkeyCallback cb);

#endif /* SNAPX_HOTKEY_H */

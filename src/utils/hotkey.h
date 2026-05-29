/**
 * @file hotkey.h
 * @brief Global hotkey registration (platform-specific).
 */

#ifndef SNAPX_HOTKEY_H
#define SNAPX_HOTKEY_H

#include "config.h"

/** Actions dispatched when a globally registered hotkey is pressed. */
typedef enum {
    SNAPX_HOTKEY_DEFAULT_MODE = 0,
    SNAPX_HOTKEY_CAPTURE_FULLSCREEN,
    SNAPX_HOTKEY_CAPTURE_MONITOR,
    SNAPX_HOTKEY_CAPTURE_REGION,
    SNAPX_HOTKEY_CAPTURE_WINDOW,
} SnapxHotkeyAction;

/**
 * @brief Register global capture hotkeys from config.
 *
 * On Linux/X11: XGrabKey on the root window for each capture shortcut.
 * On Linux/Wayland: not supported (compositor restriction).
 * On Windows: RegisterHotKey() for each capture shortcut.
 * On macOS: CGEventTap (best-effort; requires Accessibility permission).
 *
 * Non-fatal: logs warnings and skips grabs that fail.
 */
void snapx_hotkey_init(SnapxConfig *config);

/** Unregister hotkeys and clean up. */
void snapx_hotkey_cleanup(void);

/** Callback invoked on the platform hotkey thread; marshal to GTK main loop. */
typedef void (*SnapxHotkeyCallback)(SnapxHotkeyAction action, gpointer user_data);

void snapx_hotkey_set_callback(SnapxHotkeyCallback cb, gpointer user_data);

#endif /* SNAPX_HOTKEY_H */

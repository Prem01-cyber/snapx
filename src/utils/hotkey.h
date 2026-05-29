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
 * On Linux/Wayland: xdg-desktop-portal GlobalShortcuts (system-wide when compositor supports it).
 * On Linux/X11: XGrabKey on the root window for each capture shortcut.
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

/** Parent window id for Wayland portal shortcut binding (wayland:… / x11:0x…). */
void snapx_hotkey_set_application_id(const char *app_id);

void snapx_hotkey_set_parent_window(const char *parent_window);

#endif /* SNAPX_HOTKEY_H */

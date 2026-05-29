/**
 * @file hotkey_wayland.h
 * @brief Global shortcuts on Wayland via xdg-desktop-portal GlobalShortcuts.
 */

#ifndef SNAPX_HOTKEY_WAYLAND_H
#define SNAPX_HOTKEY_WAYLAND_H

#include "config.h"

#ifdef SNAPX_HAVE_WAYLAND

void snapx_hotkey_wayland_set_application_id(const char *app_id);

/** Set parent window handle (wayland:… or x11:0x…) before or after init. */
void snapx_hotkey_wayland_set_parent_window(const char *parent_window);

/** Register shortcuts with the compositor portal. */
void snapx_hotkey_wayland_init(SnapxConfig *config);

void snapx_hotkey_wayland_cleanup(void);

/** 1 when portal GlobalShortcuts session is active. */
int snapx_hotkey_wayland_active(void);

#endif /* SNAPX_HAVE_WAYLAND */

#endif /* SNAPX_HOTKEY_WAYLAND_H */

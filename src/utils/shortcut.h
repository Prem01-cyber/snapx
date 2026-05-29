/**
 * @file shortcut.h
 * @brief Keyboard shortcut parse/match helpers (GTK in-app; string form for hotkeys).
 */

#ifndef SNAPX_SHORTCUT_H
#define SNAPX_SHORTCUT_H

#include <stddef.h>

#define SNAPX_SHORTCUT_MAX 64

typedef struct {
    char global_capture[SNAPX_SHORTCUT_MAX];
    char capture_fullscreen[SNAPX_SHORTCUT_MAX];
    char capture_monitor[SNAPX_SHORTCUT_MAX];
    char capture_region[SNAPX_SHORTCUT_MAX];
    char capture_window[SNAPX_SHORTCUT_MAX];
    char save[SNAPX_SHORTCUT_MAX];
    char copy[SNAPX_SHORTCUT_MAX];
    char undo[SNAPX_SHORTCUT_MAX];
    char redo[SNAPX_SHORTCUT_MAX];
    char fit[SNAPX_SHORTCUT_MAX];
    char zoom_in[SNAPX_SHORTCUT_MAX];
    char zoom_out[SNAPX_SHORTCUT_MAX];
    char region_confirm[SNAPX_SHORTCUT_MAX];
    char region_cancel[SNAPX_SHORTCUT_MAX];
} SnapxShortcuts;

void snapx_shortcuts_set_defaults(SnapxShortcuts *sc);

#if !defined(SNAPX_HEADLESS) && (defined(SNAPX_USE_GTK4) || defined(SNAPX_USE_GTK3))

#include <gtk/gtk.h>

/** Return TRUE if @p keyval/state match the shortcut string @p spec. */
gboolean snapx_shortcut_match(const char *spec, guint keyval,
                               GdkModifierType state);

/** Format keyval+modifiers into a shortcut string (e.g. ctrl+shift+s). */
void snapx_shortcut_from_event(guint keyval, GdkModifierType state,
                                char *buf, size_t bufsz);

/** Parse @p spec into keyval + modifiers. Returns 1 on success. */
int snapx_shortcut_parse(const char *spec, guint *key_out, guint *mods_out);

#endif /* GTK */

/** Return 1 if no duplicate non-empty bindings; 0 if duplicate found. */
int snapx_shortcuts_validate(const SnapxShortcuts *sc, char *err, size_t errsz);

#endif /* SNAPX_SHORTCUT_H */

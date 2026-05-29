/**
 * @file shortcut.c
 * @brief Shortcut string parsing and GTK event matching.
 */

#include "shortcut.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

void snapx_shortcuts_set_defaults(SnapxShortcuts *sc)
{
    if (!sc) return;
    snprintf(sc->global_capture, sizeof(sc->global_capture), "super+shift+s");
    snprintf(sc->capture_fullscreen, sizeof(sc->capture_fullscreen), "ctrl+shift+1");
    snprintf(sc->capture_monitor, sizeof(sc->capture_monitor), "ctrl+shift+2");
    snprintf(sc->capture_region, sizeof(sc->capture_region), "ctrl+shift+3");
    snprintf(sc->capture_window, sizeof(sc->capture_window), "ctrl+shift+4");
    snprintf(sc->save, sizeof(sc->save), "ctrl+s");
    snprintf(sc->copy, sizeof(sc->copy), "ctrl+c");
    snprintf(sc->undo, sizeof(sc->undo), "ctrl+z");
    snprintf(sc->redo, sizeof(sc->redo), "ctrl+y");
    snprintf(sc->fit, sizeof(sc->fit), "ctrl+0");
    snprintf(sc->zoom_in, sizeof(sc->zoom_in), "ctrl+plus");
    snprintf(sc->zoom_out, sizeof(sc->zoom_out), "ctrl+minus");
    snprintf(sc->region_confirm, sizeof(sc->region_confirm), "return");
    snprintf(sc->region_cancel, sizeof(sc->region_cancel), "escape");
}

#if !defined(SNAPX_HEADLESS) && (defined(SNAPX_USE_GTK4) || defined(SNAPX_USE_GTK3))

#include <gdk/gdkkeysyms.h>

static guint snapx_shortcut_mod_mask(void)
{
    return GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK
#ifdef GDK_META_MASK
           | GDK_META_MASK
#endif
#ifdef GDK_SUPER_MASK
           | GDK_SUPER_MASK
#endif
           ;
}

static guint keyval_from_name(const char *name)
{
    if (!name || !*name) return 0;

    if (strcasecmp(name, "return") == 0 || strcasecmp(name, "enter") == 0)
        return GDK_KEY_Return;
    if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0)
        return GDK_KEY_Escape;
    if (strcasecmp(name, "space") == 0)
        return GDK_KEY_space;
    if (strcasecmp(name, "plus") == 0 || strcasecmp(name, "equal") == 0)
        return GDK_KEY_plus;
    if (strcasecmp(name, "minus") == 0)
        return GDK_KEY_minus;
    if (strcasecmp(name, "tab") == 0)
        return GDK_KEY_Tab;

    if (strlen(name) == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z')
            return GDK_KEY_a + (c - 'a');
        if (c >= 'A' && c <= 'Z')
            return GDK_KEY_A + (c - 'A');
        if (c >= '0' && c <= '9')
            return GDK_KEY_0 + (c - '0');
    }

    if (strncasecmp(name, "f", 1) == 0 && isdigit((unsigned char)name[1])) {
        int fn = atoi(name + 1);
        if (fn >= 1 && fn <= 24)
            return GDK_KEY_F1 + (fn - 1);
    }

    return gdk_keyval_from_name(name);
}

static int parse_spec(const char *spec, guint *mods_out, guint *key_out)
{
    if (!spec || !*spec) return 0;

    char buf[SNAPX_SHORTCUT_MAX];
    snprintf(buf, sizeof(buf), "%s", spec);

    guint mods = 0;
    char *save = NULL;
    char *tok = strtok_r(buf, "+", &save);
    guint key = 0;

    while (tok) {
        if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0)
            mods |= GDK_CONTROL_MASK;
        else if (strcasecmp(tok, "shift") == 0)
            mods |= GDK_SHIFT_MASK;
        else if (strcasecmp(tok, "alt") == 0 || strcasecmp(tok, "mod1") == 0)
            mods |= GDK_ALT_MASK;
        else if (strcasecmp(tok, "super") == 0 || strcasecmp(tok, "meta") == 0
                 || strcasecmp(tok, "win") == 0 || strcasecmp(tok, "primary") == 0)
#ifdef GDK_SUPER_MASK
            mods |= GDK_SUPER_MASK;
#else
            mods |= GDK_META_MASK;
#endif
        else
            key = keyval_from_name(tok);
        tok = strtok_r(NULL, "+", &save);
    }

    if (key == 0) return 0;
    *mods_out = mods;
    *key_out  = key;
    return 1;
}

int snapx_shortcut_parse(const char *spec, guint *key_out, guint *mods_out)
{
    return parse_spec(spec, mods_out, key_out);
}

static guint normalize_modifiers(guint mods)
{
    guint m = mods & snapx_shortcut_mod_mask();
#if defined(GDK_SUPER_MASK) && defined(GDK_META_MASK)
    if (m & GDK_SUPER_MASK)
        m |= GDK_META_MASK;
    if (m & GDK_META_MASK)
        m |= GDK_SUPER_MASK;
#endif
    return m;
}

static guint normalize_keyval(guint keyval, GdkModifierType state)
{
    guint kv = keyval;

    if (kv >= GDK_KEY_a && kv <= GDK_KEY_z)
        kv = GDK_KEY_a + (kv - GDK_KEY_a);
    if (kv >= GDK_KEY_A && kv <= GDK_KEY_Z)
        kv = GDK_KEY_a + (kv - GDK_KEY_A);

    if (kv >= GDK_KEY_KP_0 && kv <= GDK_KEY_KP_9)
        kv = GDK_KEY_0 + (kv - GDK_KEY_KP_0);

    /* Ctrl+Shift+digit: match physical number row, not shifted symbol. */
    if (state & GDK_CONTROL_MASK) {
        switch (kv) {
        case GDK_KEY_exclam: return GDK_KEY_1;
        case GDK_KEY_at:     return GDK_KEY_2;
        case GDK_KEY_numbersign: return GDK_KEY_3;
        case GDK_KEY_dollar: return GDK_KEY_4;
        case GDK_KEY_percent: return GDK_KEY_5;
        case GDK_KEY_asciicircum: return GDK_KEY_6;
        case GDK_KEY_ampersand: return GDK_KEY_7;
        case GDK_KEY_asterisk: return GDK_KEY_8;
        case GDK_KEY_parenleft: return GDK_KEY_9;
        case GDK_KEY_parenright: return GDK_KEY_0;
        default: break;
        }
    }

    if (kv == GDK_KEY_plus || kv == GDK_KEY_equal)
        return GDK_KEY_equal;

    return kv;
}

gboolean snapx_shortcut_match(const char *spec, guint keyval,
                               GdkModifierType state)
{
    if (!spec || !*spec) return FALSE;

    guint want_mods = 0, want_key = 0;
    if (!parse_spec(spec, &want_mods, &want_key)) return FALSE;

    guint state_mods = normalize_modifiers((guint)(state & snapx_shortcut_mod_mask()));
    want_mods = normalize_modifiers(want_mods);

    guint kv = normalize_keyval(keyval, state);
    guint want = normalize_keyval(want_key, (GdkModifierType)want_mods);

    if (want >= GDK_KEY_a && want <= GDK_KEY_z)
        want = GDK_KEY_a + (want - GDK_KEY_a);

    return (kv == want || keyval == want_key) && state_mods == want_mods;
}

static void append_mod(char *buf, size_t bufsz, GdkModifierType mod, GdkModifierType flag,
                        const char *name)
{
    if (!(mod & flag)) return;
    if (buf[0]) strncat(buf, "+", bufsz - strlen(buf) - 1);
    strncat(buf, name, bufsz - strlen(buf) - 1);
}

void snapx_shortcut_from_event(guint keyval, GdkModifierType state,
                                char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return;
    buf[0] = '\0';

    append_mod(buf, bufsz, state, GDK_CONTROL_MASK, "ctrl");
    append_mod(buf, bufsz, state, GDK_SHIFT_MASK, "shift");
    append_mod(buf, bufsz, state, GDK_ALT_MASK, "alt");
#ifdef GDK_SUPER_MASK
    append_mod(buf, bufsz, state, GDK_SUPER_MASK, "super");
#endif
#ifdef GDK_META_MASK
    append_mod(buf, bufsz, state, GDK_META_MASK, "meta");
#endif

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (buf[0]) strncat(buf, "+", bufsz - strlen(buf) - 1);
        strncat(buf, "return", bufsz - strlen(buf) - 1);
        return;
    }
    if (keyval == GDK_KEY_Escape) {
        if (buf[0]) strncat(buf, "+", bufsz - strlen(buf) - 1);
        strncat(buf, "escape", bufsz - strlen(buf) - 1);
        return;
    }
    if (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal) {
        if (buf[0]) strncat(buf, "+", bufsz - strlen(buf) - 1);
        strncat(buf, "plus", bufsz - strlen(buf) - 1);
        return;
    }
    if (keyval == GDK_KEY_minus) {
        if (buf[0]) strncat(buf, "+", bufsz - strlen(buf) - 1);
        strncat(buf, "minus", bufsz - strlen(buf) - 1);
        return;
    }

    char keyname[64];
    if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z) {
        keyname[0] = (char)('a' + (keyval - GDK_KEY_a));
        keyname[1] = '\0';
    } else if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z) {
        keyname[0] = (char)('a' + (keyval - GDK_KEY_A));
        keyname[1] = '\0';
    } else if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) {
        keyname[0] = (char)('0' + (keyval - GDK_KEY_0));
        keyname[1] = '\0';
    } else {
        g_snprintf(keyname, sizeof(keyname), "%s",
                   gdk_keyval_name(keyval) ? gdk_keyval_name(keyval) : "?");
    }

    if (buf[0]) strncat(buf, "+", bufsz - strlen(buf) - 1);
    strncat(buf, keyname, bufsz - strlen(buf) - 1);
}

#endif /* GTK */

static int binding_dup(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0]) return 0;
    return strcasecmp(a, b) == 0;
}

int snapx_shortcuts_validate(const SnapxShortcuts *sc, char *err, size_t errsz)
{
    if (!sc) return 0;
    const char *keys[] = {
        sc->global_capture, sc->capture_fullscreen, sc->capture_monitor,
        sc->capture_region, sc->capture_window,
        sc->save, sc->copy, sc->undo, sc->redo,
        sc->fit, sc->zoom_in, sc->zoom_out,
        sc->region_confirm, sc->region_cancel
    };
    const char *names[] = {
        "Global capture", "Capture full screen", "Capture monitor",
        "Capture region", "Capture window",
        "Save", "Copy", "Undo", "Redo",
        "Fit", "Zoom in", "Zoom out",
        "Region confirm", "Region cancel"
    };
    const int n = (int)(sizeof(keys) / sizeof(keys[0]));

    for (int i = 0; i < n; i++) {
        if (!keys[i][0]) continue;
        for (int j = i + 1; j < n; j++) {
            if (binding_dup(keys[i], keys[j])) {
                if (err && errsz)
                    snprintf(err, errsz, "Duplicate shortcut: %s and %s both use \"%s\"",
                             names[i], names[j], keys[i]);
                return 0;
            }
        }
    }
    return 1;
}

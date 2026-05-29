/**
 * @file hotkey_wayland.c
 * @brief Wayland global shortcuts via org.freedesktop.portal.GlobalShortcuts.
 */

#ifdef SNAPX_HAVE_WAYLAND

#include "hotkey_wayland.h"
#include "hotkey.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include <glib.h>
#include <gio/gio.h>

#define PORTAL_BUS       "org.freedesktop.portal.Desktop"
#define PORTAL_PATH      "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE_GS  "org.freedesktop.portal.GlobalShortcuts"
#define PORTAL_IFACE_REQ "org.freedesktop.portal.Request"
#define PORTAL_IFACE_SES "org.freedesktop.portal.Session"

#define SNAPX_GS_MAX 8

typedef struct {
    SnapxHotkeyAction action;
    const char       *id;
    const char       *description;
} GsShortcutDef;

typedef struct {
    GDBusConnection *dbus;
    char             sender_token[64];
    guint32          request_counter;
    char             session_handle[256];
    char             parent_window[128];
    SnapxConfig     *config;
    guint            activated_sub;
    int              bound;
    int              n_defs;
    SnapxHotkeyAction actions[SNAPX_GS_MAX];
    char             ids[SNAPX_GS_MAX][32];
} PortalHotkeyState;

static PortalHotkeyState g_gs = {0};
static char              g_portal_app_id[64] = "io.github.snapx";

extern SnapxHotkeyCallback g_hotkey_cb_for_portal;
extern gpointer            g_hotkey_user_data_for_portal;

static void sanitise_sender(const char *sender, char *out, size_t outsz)
{
    size_t j = 0;
    size_t i = (sender[0] == ':') ? 1 : 0;
    for (; sender[i] && j + 1 < outsz; i++) {
        char c = sender[i];
        out[j++] = (c == '.') ? '_' : c;
    }
    out[j] = '\0';
}

static const char *next_token(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "snapx_gs_%u", ++g_gs.request_counter);
    return buf;
}

static void build_request_path(const char *token, char *path, size_t pathsz)
{
    snprintf(path, pathsz,
             "/org/freedesktop/portal/desktop/request/%s/%s",
             g_gs.sender_token, token);
}

typedef struct {
    GMainLoop *loop;
    GVariant  *results;
    gboolean   done;
} PortalResponse;

static void portal_response_cb(GDBusConnection *conn  __attribute__((unused)),
                               const gchar *sender    __attribute__((unused)),
                               const gchar *obj_path  __attribute__((unused)),
                               const gchar *iface     __attribute__((unused)),
                               const gchar *signal    __attribute__((unused)),
                               GVariant    *parameters,
                               gpointer     user_data)
{
    PortalResponse *pr = user_data;
    guint32   response = 2;
    GVariant *results  = NULL;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);
    if (response == 0)
        pr->results = results;
    else if (results)
        g_variant_unref(results);
    pr->done = TRUE;
    g_main_loop_quit(pr->loop);
}

static GVariant *portal_call_sync(const char *iface,
                                   const char *method,
                                   GVariant   *params,
                                   int         timeout_ms)
{
    if (!g_gs.dbus) return NULL;

    GError *err = NULL;
    PortalResponse pr = { .loop = g_main_loop_new(NULL, FALSE) };

    char token[64];
    next_token(token, sizeof(token));
    char req_path[256];
    build_request_path(token, req_path, sizeof(req_path));

    GVariantBuilder new_params;
    g_variant_builder_init(&new_params, G_VARIANT_TYPE_TUPLE);
    GVariant *params_sink = g_variant_ref_sink(params);
    gsize n = g_variant_n_children(params_sink);
    for (gsize i = 0; i < n - 1; i++)
        g_variant_builder_add_value(&new_params,
                                    g_variant_get_child_value(params_sink, i));

    GVariant *last = g_variant_get_child_value(params_sink, n - 1);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    GVariantIter iter;
    g_variant_iter_init(&iter, last);
    const gchar *key;
    GVariant *val;
    while (g_variant_iter_next(&iter, "{sv}", &key, &val)) {
        g_variant_builder_add(&opts, "{sv}", key, val);
        g_variant_unref(val);
    }
    g_variant_unref(last);
    g_variant_builder_add(&opts, "{sv}", "handle_token",
                          g_variant_new_string(token));
    g_variant_builder_add_value(&new_params, g_variant_builder_end(&opts));
    g_variant_unref(params_sink);

    guint sub = g_dbus_connection_signal_subscribe(
        g_gs.dbus, NULL, PORTAL_IFACE_REQ, "Response", req_path, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, portal_response_cb, &pr, NULL);

    GVariant *ret = g_dbus_connection_call_sync(
        g_gs.dbus, PORTAL_BUS, PORTAL_PATH, iface, method,
        g_variant_builder_end(&new_params),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!ret) {
        fprintf(stderr, "[hotkey/portal] %s.%s failed: %s\n", iface, method,
                err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_dbus_connection_signal_unsubscribe(g_gs.dbus, sub);
        g_main_loop_unref(pr.loop);
        return NULL;
    }
    g_variant_unref(ret);

    GSource *tsrc = g_timeout_source_new(timeout_ms);
    g_source_set_callback(tsrc, G_SOURCE_FUNC(g_main_loop_quit), pr.loop, NULL);
    g_source_attach(tsrc, NULL);
    if (!pr.done) g_main_loop_run(pr.loop);
    g_source_destroy(tsrc);
    g_source_unref(tsrc);

    g_dbus_connection_signal_unsubscribe(g_gs.dbus, sub);
    g_main_loop_unref(pr.loop);
    return pr.results;
}

static void spec_to_portal_trigger(const char *spec, char *out, size_t outsz)
{
    char buf[SNAPX_SHORTCUT_MAX];
    snprintf(buf, sizeof(buf), "%s", spec ? spec : "");
    out[0] = '\0';

    char *tok = strtok(buf, "+");
    int first = 1;
    while (tok) {
        if (!first)
            strncat(out, "+", outsz - strlen(out) - 1);
        first = 0;

        if (strcasecmp(tok, "super") == 0 || strcasecmp(tok, "meta") == 0
            || strcasecmp(tok, "mod4") == 0) {
            strncat(out, "LOGO", outsz - strlen(out) - 1);
        } else if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0) {
            strncat(out, "CTRL", outsz - strlen(out) - 1);
        } else if (strcasecmp(tok, "alt") == 0 || strcasecmp(tok, "mod1") == 0) {
            strncat(out, "ALT", outsz - strlen(out) - 1);
        } else if (strcasecmp(tok, "shift") == 0) {
            strncat(out, "SHIFT", outsz - strlen(out) - 1);
        } else {
            char upper[32];
            snprintf(upper, sizeof(upper), "%s", tok);
            for (char *p = upper; *p; p++)
                *p = (char)toupper((unsigned char)*p);
            strncat(out, upper, outsz - strlen(out) - 1);
        }
        tok = strtok(NULL, "+");
    }
}

static SnapxHotkeyAction action_for_id(const char *id)
{
    for (int i = 0; i < g_gs.n_defs; i++) {
        if (strcmp(g_gs.ids[i], id) == 0)
            return g_gs.actions[i];
    }
    return SNAPX_HOTKEY_DEFAULT_MODE;
}

static void portal_activated_cb(GDBusConnection *conn  __attribute__((unused)),
                                const gchar *sender    __attribute__((unused)),
                                const gchar *obj_path  __attribute__((unused)),
                                const gchar *iface     __attribute__((unused)),
                                const gchar *signal    __attribute__((unused)),
                                GVariant    *parameters,
                                gpointer     user_data __attribute__((unused)))
{
    const char *session = NULL;
    const char *shortcut_id = NULL;
    guint64      timestamp = 0;
    g_autoptr(GVariant) options = NULL;

    g_variant_get(parameters, "(&o&st@a{sv})",
                  &session, &shortcut_id, &timestamp, &options);

    if (!shortcut_id || !shortcut_id[0]) return;
    if (g_gs.session_handle[0] &&
        session && strcmp(session, g_gs.session_handle) != 0)
        return;

    if (g_hotkey_cb_for_portal)
        g_hotkey_cb_for_portal(action_for_id(shortcut_id),
                               g_hotkey_user_data_for_portal);
}

static int portal_create_session(void)
{
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    char sess_token[64];
    snprintf(sess_token, sizeof(sess_token), "snapx_gs_sess_%u",
             g_gs.request_counter + 1);
    g_variant_builder_add(&opts, "{sv}", "session_handle_token",
                          g_variant_new_string(sess_token));

    GVariant *res = portal_call_sync(PORTAL_IFACE_GS, "CreateSession",
        g_variant_new("(@a{sv})", g_variant_builder_end(&opts)), 30000);
    if (!res) return 0;

    GVariant *sh = g_variant_lookup_value(res, "session_handle",
                                          G_VARIANT_TYPE_OBJECT_PATH);
    if (!sh)
        sh = g_variant_lookup_value(res, "session_handle",
                                    G_VARIANT_TYPE_STRING);
    if (sh) {
        const char *h = NULL;
        if (g_variant_is_of_type(sh, G_VARIANT_TYPE_OBJECT_PATH) ||
            g_variant_is_of_type(sh, G_VARIANT_TYPE_STRING))
            h = g_variant_get_string(sh, NULL);
        if (h && g_variant_is_object_path(h))
            snprintf(g_gs.session_handle, sizeof(g_gs.session_handle), "%s", h);
        g_variant_unref(sh);
    }
    g_variant_unref(res);

    if (g_gs.session_handle[0] == '\0') {
        snprintf(g_gs.session_handle, sizeof(g_gs.session_handle),
                 "/org/freedesktop/portal/desktop/session/%s/%s",
                 g_gs.sender_token, sess_token);
    }
    return 1;
}

static int portal_bind_shortcuts(SnapxConfig *config)
{
    if (!config || g_gs.bound || g_gs.session_handle[0] == '\0')
        return 0;

    static const GsShortcutDef defs[] = {
        { SNAPX_HOTKEY_DEFAULT_MODE,       "global_capture",
          "Open snapx (default capture mode)" },
        { SNAPX_HOTKEY_CAPTURE_FULLSCREEN, "capture_fullscreen",
          "Capture full screen" },
        { SNAPX_HOTKEY_CAPTURE_MONITOR,    "capture_monitor",
          "Capture a monitor" },
        { SNAPX_HOTKEY_CAPTURE_REGION,     "capture_region",
          "Capture a region" },
        { SNAPX_HOTKEY_CAPTURE_WINDOW,     "capture_window",
          "Capture the active window" },
    };

    const char *specs[] = {
        config->shortcuts.global_capture,
        config->shortcuts.capture_fullscreen,
        config->shortcuts.capture_monitor,
        config->shortcuts.capture_region,
        config->shortcuts.capture_window,
    };

    GVariantBuilder shortcuts;
    g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
    g_gs.n_defs = 0;

    for (size_t i = 0; i < G_N_ELEMENTS(defs); i++) {
        const char *spec = specs[i];
        if (!spec || !spec[0]) continue;

        char trigger[SNAPX_SHORTCUT_MAX];
        spec_to_portal_trigger(spec, trigger, sizeof(trigger));

        GVariantBuilder dict;
        g_variant_builder_init(&dict, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&dict, "{sv}", "description",
                              g_variant_new_string(defs[i].description));
        if (trigger[0])
            g_variant_builder_add(&dict, "{sv}", "preferred_trigger",
                                  g_variant_new_string(trigger));

        g_variant_builder_add(&shortcuts, "(sa{sv})",
                              defs[i].id, &dict);

        snprintf(g_gs.ids[g_gs.n_defs], sizeof(g_gs.ids[g_gs.n_defs]),
                 "%s", defs[i].id);
        g_gs.actions[g_gs.n_defs] = defs[i].action;
        g_gs.n_defs++;
    }

    if (g_gs.n_defs == 0) return 0;

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);

    const char *parent = g_gs.parent_window[0] ? g_gs.parent_window : "";

    GVariant *children[4];
    children[0] = g_variant_new_object_path(g_gs.session_handle);
    children[1] = g_variant_builder_end(&shortcuts);
    children[2] = g_variant_new_string(parent);
    children[3] = g_variant_builder_end(&opts);
    GVariant *params = g_variant_new_tuple(children, 4);

    GVariant *res = portal_call_sync(PORTAL_IFACE_GS, "BindShortcuts", params,
        120000);
    g_variant_unref(params);

    if (!res) {
        fprintf(stderr,
                "[hotkey/portal] BindShortcuts failed or cancelled "
                "(approve shortcuts in the portal dialog if shown).\n");
        return 0;
    }
    g_variant_unref(res);
    g_gs.bound = 1;
    fprintf(stderr,
            "[hotkey/portal] Registered %d global shortcut(s) via portal.\n",
            g_gs.n_defs);
    return 1;
}

int snapx_hotkey_wayland_active(void)
{
    return g_gs.bound;
}

void snapx_hotkey_wayland_set_application_id(const char *app_id)
{
    if (app_id && app_id[0])
        snprintf(g_portal_app_id, sizeof(g_portal_app_id), "%s", app_id);
}

void snapx_hotkey_wayland_set_parent_window(const char *parent_window)
{
    if (parent_window && parent_window[0])
        snprintf(g_gs.parent_window, sizeof(g_gs.parent_window),
                 "%s", parent_window);
    if (g_gs.config && !g_gs.bound)
        portal_bind_shortcuts(g_gs.config);
}

void snapx_hotkey_wayland_init(SnapxConfig *config)
{
    if (!config) return;
    if (g_gs.bound) return;

    g_gs.config = config;

    GError *err = NULL;
    g_gs.dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!g_gs.dbus) {
        fprintf(stderr, "[hotkey/portal] Session bus unavailable: %s\n",
                err ? err->message : "unknown");
        if (err) g_error_free(err);
        return;
    }

    const char *sender = g_dbus_connection_get_unique_name(g_gs.dbus);
    if (sender)
        sanitise_sender(sender, g_gs.sender_token, sizeof(g_gs.sender_token));

    if (!portal_create_session()) {
        fprintf(stderr, "[hotkey/portal] CreateSession failed "
                        "(portal may not support GlobalShortcuts).\n");
        g_object_unref(g_gs.dbus);
        g_gs.dbus = NULL;
        return;
    }

    g_gs.activated_sub = g_dbus_connection_signal_subscribe(
        g_gs.dbus, PORTAL_BUS, PORTAL_IFACE_GS, "Activated", PORTAL_PATH,
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, portal_activated_cb, NULL, NULL);

    if (!portal_bind_shortcuts(config) && !g_gs.parent_window[0]) {
        fprintf(stderr,
                "[hotkey/portal] Waiting for window handle to bind shortcuts.\n");
    }
}

void snapx_hotkey_wayland_cleanup(void)
{
    if (g_gs.dbus && g_gs.activated_sub)
        g_dbus_connection_signal_unsubscribe(g_gs.dbus, g_gs.activated_sub);

    if (g_gs.dbus && g_gs.session_handle[0]) {
        GError *err = NULL;
        g_dbus_connection_call_sync(g_gs.dbus, PORTAL_BUS, g_gs.session_handle,
            PORTAL_IFACE_SES, "Close", NULL, NULL,
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
        if (err) g_error_free(err);
    }

    if (g_gs.dbus) {
        g_object_unref(g_gs.dbus);
        g_gs.dbus = NULL;
    }

    memset(&g_gs, 0, sizeof(g_gs));
}

#endif /* SNAPX_HAVE_WAYLAND */

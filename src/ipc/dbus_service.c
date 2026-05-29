/**
 * @file dbus_service.c
 * @brief Session D-Bus interface io.github.snapx.
 */

#include "dbus_service.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

extern void snapx_window_main_capture_region(void);
extern void snapx_window_main_show(void);
extern const char *snapx_window_main_get_last_path(void);
extern int snapx_window_main_save_to(const char *path);

static guint g_owner_id;
static GDBusNodeInfo *g_introspection;

static const char introspection_xml[] =
    "<node>"
    "  <interface name='io.github.snapx'>"
    "    <method name='CaptureRegion'/>"
    "    <method name='Show'/>"
    "    <method name='Save'>"
    "      <arg type='s' name='path' direction='in'/>"
    "    </method>"
    "    <method name='GetLastPath'>"
    "      <arg type='s' name='path' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static void handle_method(GDBusConnection *conn, const char *sender,
                           const char *path, const char *iface,
                           const char *method, GVariant *params,
                           GDBusMethodInvocation *inv, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;

    if (g_strcmp0(method, "CaptureRegion") == 0) {
        snapx_window_main_capture_region();
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "Show") == 0) {
        snapx_window_main_show();
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "Save") == 0) {
        const char *save_path = NULL;
        g_variant_get(params, "(&s)", &save_path);
        if (!save_path || !save_path[0]) {
            g_dbus_method_invocation_return_error(inv,
                G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "path required");
            return;
        }
        if (snapx_window_main_save_to(save_path) != 0) {
            g_dbus_method_invocation_return_error(inv,
                G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Save failed");
            return;
        }
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "GetLastPath") == 0) {
        const char *last = snapx_window_main_get_last_path();
        g_dbus_method_invocation_return_value(inv,
            g_variant_new("(s)", last ? last : ""));
    } else {
        g_dbus_method_invocation_return_error(inv,
            G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown method %s", method);
    }
}

static const GDBusInterfaceVTable vtable = { .method_call = handle_method };

static void on_bus_acquired(GDBusConnection *conn, const char *name, gpointer data)
{
    (void)data;
    if (!g_introspection)
        g_introspection = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    if (!g_introspection) return;

    g_dbus_connection_register_object(conn, "/io/github/snapx",
        g_introspection->interfaces[0], &vtable, NULL, NULL, NULL);
    fprintf(stderr, "[dbus] Service registered at %s\n", name);
}

static void on_name_acquired(GDBusConnection *c, const char *n, gpointer d)
{
    (void)c; (void)n; (void)d;
}

static void on_name_lost(GDBusConnection *c, const char *n, gpointer d)
{
    (void)c; (void)n; (void)d;
}

void snapx_dbus_service_start(void)
{
    if (g_owner_id) return;
    g_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
        "io.github.snapx",
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired, on_name_acquired, on_name_lost,
        NULL, NULL);
}

void snapx_dbus_service_stop(void)
{
    if (g_owner_id) {
        g_bus_unown_name(g_owner_id);
        g_owner_id = 0;
    }
}

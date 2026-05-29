/**
 * @file portal_host.c
 * @brief Associate this process with an application id for xdg-desktop-portal.
 *
 * GlobalShortcuts (and some other portals) require a non-empty app id. For
 * native builds launched outside a desktop launcher cgroup, call Register on
 * org.freedesktop.host.portal.Registry before any other portal method.
 */

#if defined(SNAPX_PLATFORM_LINUX)

#include "portal_host.h"

#include <stdio.h>
#include <string.h>

#include <gio/gio.h>

#define PORTAL_BUS      "org.freedesktop.portal.Desktop"
#define PORTAL_PATH     "/org/freedesktop/portal/desktop"
#define REGISTRY_IFACE  "org.freedesktop.host.portal.Registry"

static int g_portal_registered;

int snapx_portal_host_register(const char *app_id)
{
    if (g_portal_registered)
        return 1;
    if (!app_id || !app_id[0])
        return 0;

    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) {
        fprintf(stderr, "[portal] Session bus unavailable: %s\n",
                err ? err->message : "unknown");
        if (err) g_error_free(err);
        return 0;
    }

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);

    g_dbus_connection_call_sync(
        conn, PORTAL_BUS, PORTAL_PATH, REGISTRY_IFACE, "Register",
        g_variant_new("(sa{sv})", app_id, &opts),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    g_object_unref(conn);

    if (err) {
        fprintf(stderr, "[portal] Host Register(%s): %s\n", app_id, err->message);
        if (strstr(err->message, "App info not found"))
            fprintf(stderr,
                    "[portal] Global shortcuts need snapx installed "
                    "(io.github.snapx.desktop and snapx in PATH).\n");
        g_error_free(err);
        return 0;
    }

    g_portal_registered = 1;
    fprintf(stderr, "[portal] Registered host app id: %s\n", app_id);
    return 1;
}

#else

#include "portal_host.h"

int snapx_portal_host_register(const char *app_id)
{
    (void)app_id;
    return 0;
}

#endif /* SNAPX_PLATFORM_LINUX */

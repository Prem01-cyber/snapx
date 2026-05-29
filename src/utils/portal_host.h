/**
 * @file portal_host.h
 * @brief Register native app id with xdg-desktop-portal (host Registry API).
 */

#ifndef SNAPX_PORTAL_HOST_H
#define SNAPX_PORTAL_HOST_H

/** Register @p app_id with org.freedesktop.host.portal.Registry. Returns 1 on success. */
int snapx_portal_host_register(const char *app_id);

#endif /* SNAPX_PORTAL_HOST_H */

/**
 * @file platform.h
 * @brief Runtime platform detection — OS, distro, display server, desktop,
 *        and capture backend availability.
 *
 * Call snapx_platform_probe() once at startup.  The resulting
 * SnapxPlatformInfo struct is passed around wherever platform context is
 * needed (backend selection, --info printing, error messages).
 */

#pragma once
#include "capture.h"   /* SnapxBackendType */

/* Maximum number of backends tracked */
#define SNAPX_MAX_BACKENDS 8

/* ─── Display-server type (Linux only) ───────────────────────────────────── */
typedef enum {
    SNAPX_SESSION_UNKNOWN = 0,
    SNAPX_SESSION_X11,
    SNAPX_SESSION_WAYLAND,
    SNAPX_SESSION_MIR,
} SnapxSessionType;

/* ─── Desktop environment (Linux only) ──────────────────────────────────── */
typedef enum {
    SNAPX_DE_UNKNOWN = 0,
    SNAPX_DE_GNOME,
    SNAPX_DE_KDE,
    SNAPX_DE_XFCE,
    SNAPX_DE_MATE,
    SNAPX_DE_CINNAMON,
    SNAPX_DE_BUDGIE,
    SNAPX_DE_I3,
    SNAPX_DE_SWAY,
    SNAPX_DE_OTHER,
} SnapxDesktopEnv;

/* ─── Full platform information ──────────────────────────────────────────── */
typedef struct SnapxPlatformInfo {
    /* ── OS identity ──────────────────────────────────────────────────── */
    char os_name[32];        /**< "Linux", "Windows", "macOS"              */
    char os_version[64];     /**< Human-readable: "Fedora 44", "Win 11 23H2" */
    int  os_major;           /**< Major version number                     */
    int  os_minor;
    int  os_patch;

    /* ── Linux-specific ──────────────────────────────────────────────── */
    char                distro_id[32];      /**< "fedora", "ubuntu", "arch"  */
    char                distro_version[16]; /**< "44", "24.04", "rolling"    */
    char                distro_pretty[64];  /**< Pretty-print name           */
    SnapxSessionType    session_type;
    char                session_str[16];    /**< "wayland", "x11", ...       */
    SnapxDesktopEnv     desktop_env;
    char                desktop_str[32];    /**< "GNOME", "KDE Plasma", ...  */

    /* ── Feature flags (runtime) ─────────────────────────────────────── */
    int  wayland_display_set;    /**< WAYLAND_DISPLAY env is non-empty     */
    int  x11_display_set;        /**< DISPLAY env is non-empty             */
    int  portal_reachable;       /**< XDG Desktop Portal answered on DBus  */
    int  pipewire_reachable;     /**< PipeWire socket reachable            */

    /* ── Backend probe results ───────────────────────────────────────── */
    /**< backend_ok[type] = 1 if that backend compiled in + init succeeded */
    int              backend_ok[SNAPX_MAX_BACKENDS];
    SnapxBackendType preferred;  /**< Best backend to use                  */
    SnapxBackendType fallback;   /**< Next best if preferred fails at capture time */
} SnapxPlatformInfo;

/* ─── Public API ─────────────────────────────────────────────────────────── */

/**
 * @brief Probe the current runtime environment and fill @p info.
 *
 * This is the single entry-point.  It:
 *  - Reads /etc/os-release (Linux), RtlGetVersion (Windows), NSProcessInfo (macOS)
 *  - Checks environment variables (XDG_SESSION_TYPE, WAYLAND_DISPLAY, DISPLAY, ...)
 *  - Pings the XDG Desktop Portal over DBus (Linux/Wayland)
 *  - Attempts to init each compiled-in backend and records success/failure
 *  - Selects preferred + fallback backends
 *
 * On Linux it probes in order: ScreenCast/PipeWire → Screenshot portal → X11.
 * On Windows: DXGI Desktop Duplication → GDI BitBlt.
 * On macOS:  ScreenCaptureKit → CoreGraphics.
 *
 * @param info   Output; zeroed then filled.
 * @param quiet  If non-zero, suppress per-backend init log lines.
 */
void snapx_platform_probe(SnapxPlatformInfo *info, int quiet);

/**
 * @brief Print a human-readable summary of detected platform info to stderr.
 */
void snapx_platform_print(const SnapxPlatformInfo *info);

/**
 * @brief Return a short human-readable name for a backend type.
 */
const char *snapx_backend_name(SnapxBackendType type);

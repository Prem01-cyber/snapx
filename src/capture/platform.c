/**
 * @file platform.c
 * @brief Runtime platform detection — OS, distro, display server, desktop,
 *        and capture backend availability.
 */

#include "platform.h"
#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Backend name table ─────────────────────────────────────────────────── */

const char *snapx_backend_name(SnapxBackendType type)
{
    switch (type) {
        case SNAPX_BACKEND_WAYLAND:   return "Wayland (ScreenCast/PipeWire)";
        case SNAPX_BACKEND_X11:       return "X11 (XGetImage)";
        case SNAPX_BACKEND_WIN_DXGI:  return "Windows DXGI Desktop Duplication";
        case SNAPX_BACKEND_WIN_GDI:   return "Windows GDI BitBlt";
        case SNAPX_BACKEND_MACOS_SCK: return "macOS ScreenCaptureKit";
        case SNAPX_BACKEND_MACOS_CG:  return "macOS CoreGraphics";
        default:                      return "Unknown";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LINUX
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef SNAPX_PLATFORM_LINUX

#include <unistd.h>
#include <strings.h>   /* strcasecmp */

/* ─── /etc/os-release parser ─────────────────────────────────────────────── */

/**
 * Extract the value for a KEY="value" line from /etc/os-release.
 * Returns 1 on success, 0 if key not found.
 */
static int osrel_get(const char *key, char *out, size_t outsz)
{
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) f = fopen("/usr/lib/os-release", "r");
    if (!f) return 0;

    char line[256];
    int found = 0;
    size_t klen = strlen(key);

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *val = line + klen + 1;
            /* Strip surrounding quotes and trailing newline */
            if (*val == '"') val++;
            size_t vlen = strlen(val);
            while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r'
                                  || val[vlen-1] == '"'))
                val[--vlen] = '\0';
            snprintf(out, outsz, "%s", val);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* ─── Desktop environment detection ─────────────────────────────────────── */

static SnapxDesktopEnv detect_desktop(char *out, size_t outsz)
{
    const char *de = getenv("XDG_CURRENT_DESKTOP");
    if (!de || !de[0]) de = getenv("DESKTOP_SESSION");
    if (!de || !de[0]) de = getenv("GDMSESSION");
    if (!de) de = "";

    struct { const char *key; SnapxDesktopEnv id; const char *label; } map[] = {
        { "gnome",    SNAPX_DE_GNOME,    "GNOME"      },
        { "kde",      SNAPX_DE_KDE,      "KDE Plasma" },
        { "xfce",     SNAPX_DE_XFCE,     "XFCE"       },
        { "mate",     SNAPX_DE_MATE,     "MATE"       },
        { "cinnamon", SNAPX_DE_CINNAMON, "Cinnamon"   },
        { "budgie",   SNAPX_DE_BUDGIE,   "Budgie"     },
        { "i3",       SNAPX_DE_I3,       "i3"         },
        { "sway",     SNAPX_DE_SWAY,     "Sway"       },
        { NULL, 0, NULL }
    };

    char lower[64]; int li = 0;
    for (const char *p = de; *p && li < 63; p++) {
        lower[li++] = (*p >= 'A' && *p <= 'Z') ? (*p + 32) : *p;
    }
    lower[li] = '\0';

    for (int i = 0; map[i].key; i++) {
        if (strstr(lower, map[i].key)) {
            snprintf(out, outsz, "%s", map[i].label);
            return map[i].id;
        }
    }

    /* Fall back to raw value if non-empty */
    snprintf(out, outsz, "%s", de[0] ? de : "Unknown");
    return de[0] ? SNAPX_DE_OTHER : SNAPX_DE_UNKNOWN;
}

/* ─── Session type detection ─────────────────────────────────────────────── */

static SnapxSessionType detect_session(char *out, size_t outsz)
{
    const char *s = getenv("XDG_SESSION_TYPE");
    if (s && strcasecmp(s, "wayland") == 0) {
        snprintf(out, outsz, "wayland");
        return SNAPX_SESSION_WAYLAND;
    }
    if (s && strcasecmp(s, "x11") == 0) {
        snprintf(out, outsz, "x11");
        return SNAPX_SESSION_X11;
    }
    if (s && strcasecmp(s, "mir") == 0) {
        snprintf(out, outsz, "mir");
        return SNAPX_SESSION_MIR;
    }
    /* Fall back to env variable presence */
    if (getenv("WAYLAND_DISPLAY")) {
        snprintf(out, outsz, "wayland");
        return SNAPX_SESSION_WAYLAND;
    }
    if (getenv("DISPLAY")) {
        snprintf(out, outsz, "x11");
        return SNAPX_SESSION_X11;
    }
    snprintf(out, outsz, "unknown");
    return SNAPX_SESSION_UNKNOWN;
}

/* ─── Quick portal reachability check (no capture, just name lookup) ─────── */

#ifdef SNAPX_HAVE_WAYLAND
#include <gio/gio.h>

static int portal_reachable(void)
{
    GError *err = NULL;
    GDBusConnection *dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!dbus) { if (err) g_error_free(err); return 0; }

    GVariant *ret = g_dbus_connection_call_sync(
        dbus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", "org.freedesktop.portal.Desktop"),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
    g_object_unref(dbus);
    if (!ret) { if (err) g_error_free(err); return 0; }
    g_variant_unref(ret);
    return 1;
}
#endif /* SNAPX_HAVE_WAYLAND */

/* ─── PipeWire socket reachability ──────────────────────────────────────── */

static int pipewire_reachable(void)
{
    /* PipeWire typically exposes a socket at $XDG_RUNTIME_DIR/pipewire-0 */
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !runtime[0]) return 0;
    char path[256];
    snprintf(path, sizeof(path), "%s/pipewire-0", runtime);
    return (access(path, R_OK | W_OK) == 0) ? 1 : 0;
}

/* ─── Kernel version from /proc/version ─────────────────────────────────── */

static void read_kernel_version(int *major, int *minor, int *patch)
{
    *major = *minor = *patch = 0;
    FILE *f = fopen("/proc/version", "r");
    if (!f) return;
    /* Format: "Linux version M.m.p ..." */
    if (fscanf(f, "Linux version %d.%d.%d", major, minor, patch) < 1) {
        *major = *minor = *patch = 0;
    }
    fclose(f);
}

/* ─── Linux probe ────────────────────────────────────────────────────────── */

static void probe_linux(SnapxPlatformInfo *info, int quiet)
{
    snprintf(info->os_name, sizeof(info->os_name), "Linux");

    /* Kernel version */
    read_kernel_version(&info->os_major, &info->os_minor, &info->os_patch);

    /* Distro from /etc/os-release */
    char id[32] = "", version[32] = "", pretty[64] = "";
    osrel_get("ID",            id,      sizeof(id));
    osrel_get("VERSION_ID",    version, sizeof(version));
    osrel_get("PRETTY_NAME",   pretty,  sizeof(pretty));
    snprintf(info->distro_id,      sizeof(info->distro_id),      "%s", id);
    snprintf(info->distro_version, sizeof(info->distro_version), "%s", version);
    snprintf(info->distro_pretty,  sizeof(info->distro_pretty),
             "%s", pretty[0] ? pretty : id);
    snprintf(info->os_version, sizeof(info->os_version),
             "%s", pretty[0] ? pretty : "Linux");

    /* Session type + desktop */
    info->session_type = detect_session(info->session_str, sizeof(info->session_str));
    info->desktop_env  = detect_desktop(info->desktop_str, sizeof(info->desktop_str));

    /* Display availability */
    const char *wl = getenv("WAYLAND_DISPLAY");
    const char *x  = getenv("DISPLAY");
    info->wayland_display_set = (wl && wl[0]) ? 1 : 0;
    info->x11_display_set     = (x  && x[0])  ? 1 : 0;

    /* Portal / PipeWire reachability */
#ifdef SNAPX_HAVE_WAYLAND
    info->portal_reachable = portal_reachable();
#else
    info->portal_reachable = 0;
#endif
    info->pipewire_reachable = pipewire_reachable();

    /* ── Probe backends in preference order ─────────────────────────── */
    SnapxCaptureBackend tmp;

    /* 1. Wayland/ScreenCast (best: no X11 involved, native Wayland pixels) */
#ifdef SNAPX_HAVE_WAYLAND
    if (info->session_type == SNAPX_SESSION_WAYLAND && info->portal_reachable) {
        memset(&tmp, 0, sizeof(tmp));
        if (snapx_capture_backend_init(&tmp, SNAPX_BACKEND_WAYLAND) == 0) {
            info->backend_ok[SNAPX_BACKEND_WAYLAND] = 1;
            snapx_capture_backend_destroy(&tmp);
        }
    }
#endif

    /* 2. X11 (works on X11 sessions, and as Xwayland fallback) */
#ifdef SNAPX_HAVE_X11
    if (info->x11_display_set) {
        memset(&tmp, 0, sizeof(tmp));
        if (snapx_capture_backend_init(&tmp, SNAPX_BACKEND_X11) == 0) {
            info->backend_ok[SNAPX_BACKEND_X11] = 1;
            snapx_capture_backend_destroy(&tmp);
        }
    }
#endif

    /* Select preferred + fallback */
    if (info->backend_ok[SNAPX_BACKEND_WAYLAND]) {
        info->preferred = SNAPX_BACKEND_WAYLAND;
        info->fallback  = info->backend_ok[SNAPX_BACKEND_X11]
                          ? SNAPX_BACKEND_X11 : SNAPX_BACKEND_UNKNOWN;
    } else if (info->backend_ok[SNAPX_BACKEND_X11]) {
        info->preferred = SNAPX_BACKEND_X11;
        info->fallback  = SNAPX_BACKEND_UNKNOWN;
    } else {
        info->preferred = SNAPX_BACKEND_UNKNOWN;
        info->fallback  = SNAPX_BACKEND_UNKNOWN;
        if (!quiet)
            fprintf(stderr, "[platform] WARNING: No capture backend available.\n"
                    "  Wayland display: %s, X11 display: %s, Portal: %s\n",
                    info->wayland_display_set ? "yes" : "no",
                    info->x11_display_set     ? "yes" : "no",
                    info->portal_reachable    ? "yes" : "no");
    }
}

#endif /* SNAPX_PLATFORM_LINUX */

/* ═══════════════════════════════════════════════════════════════════════════
 *  WINDOWS
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef SNAPX_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* RtlGetVersion doesn't lie about Win10+ (VerifyVersionInfo does) */
typedef NTSTATUS (WINAPI *RtlGetVersion_t)(LPOSVERSIONINFOEXW);

static void get_windows_version(int *major, int *minor, int *build)
{
    *major = *minor = *build = 0;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    RtlGetVersion_t fn = (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
    if (!fn) return;
    OSVERSIONINFOEXW vi = { sizeof(vi) };
    if (fn(&vi) == 0) {
        *major = (int)vi.dwMajorVersion;
        *minor = (int)vi.dwMinorVersion;
        *build = (int)vi.dwBuildNumber;
    }
}

static void windows_version_label(int major, int minor, int build, char *out, size_t outsz)
{
    /* Windows 11: build >= 22000 */
    if (major == 10 && build >= 22000)
        snprintf(out, outsz, "Windows 11 (build %d)", build);
    else if (major == 10)
        snprintf(out, outsz, "Windows 10 (build %d)", build);
    else if (major == 6 && minor == 3)
        snprintf(out, outsz, "Windows 8.1");
    else if (major == 6 && minor == 2)
        snprintf(out, outsz, "Windows 8");
    else if (major == 6 && minor == 1)
        snprintf(out, outsz, "Windows 7");
    else if (major == 6 && minor == 0)
        snprintf(out, outsz, "Windows Vista");
    else if (major == 5 && minor == 1)
        snprintf(out, outsz, "Windows XP");
    else
        snprintf(out, outsz, "Windows %d.%d (build %d)", major, minor, build);
}

static void probe_windows(SnapxPlatformInfo *info, int quiet)
{
    snprintf(info->os_name, sizeof(info->os_name), "Windows");
    get_windows_version(&info->os_major, &info->os_minor, &info->os_patch);
    windows_version_label(info->os_major, info->os_minor, info->os_patch,
                          info->os_version, sizeof(info->os_version));

    SnapxCaptureBackend tmp;

    /* Prefer DXGI on Win8+ (build 9600+) — it captures hardware-accelerated
     * and multi-monitor content correctly */
#ifdef SNAPX_HAVE_DXGI
    if (info->os_major > 6 ||
        (info->os_major == 6 && info->os_minor >= 2)) {
        memset(&tmp, 0, sizeof(tmp));
        if (snapx_capture_backend_init(&tmp, SNAPX_BACKEND_WIN_DXGI) == 0) {
            info->backend_ok[SNAPX_BACKEND_WIN_DXGI] = 1;
            snapx_capture_backend_destroy(&tmp);
        }
    }
#endif

    /* GDI works on all Windows versions */
    memset(&tmp, 0, sizeof(tmp));
    if (snapx_capture_backend_init(&tmp, SNAPX_BACKEND_WIN_GDI) == 0) {
        info->backend_ok[SNAPX_BACKEND_WIN_GDI] = 1;
        snapx_capture_backend_destroy(&tmp);
    }

    if (info->backend_ok[SNAPX_BACKEND_WIN_DXGI]) {
        info->preferred = SNAPX_BACKEND_WIN_DXGI;
        info->fallback  = info->backend_ok[SNAPX_BACKEND_WIN_GDI]
                          ? SNAPX_BACKEND_WIN_GDI : SNAPX_BACKEND_UNKNOWN;
    } else if (info->backend_ok[SNAPX_BACKEND_WIN_GDI]) {
        info->preferred = SNAPX_BACKEND_WIN_GDI;
        info->fallback  = SNAPX_BACKEND_UNKNOWN;
    } else {
        info->preferred = SNAPX_BACKEND_UNKNOWN;
        info->fallback  = SNAPX_BACKEND_UNKNOWN;
        if (!quiet)
            fprintf(stderr, "[platform] WARNING: No Windows capture backend available.\n");
    }
}

#endif /* SNAPX_PLATFORM_WINDOWS */

/* ═══════════════════════════════════════════════════════════════════════════
 *  macOS
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef SNAPX_PLATFORM_MACOS

#include <sys/sysctl.h>

static void get_macos_version(int *major, int *minor, int *patch)
{
    *major = *minor = *patch = 0;
    /* Read kern.osproductversion sysctl (macOS 10.13.4+) */
    char buf[32] = "";
    size_t len = sizeof(buf);
    sysctlbyname("kern.osproductversion", buf, &len, NULL, 0);
    sscanf(buf, "%d.%d.%d", major, minor, patch);
}

static void macos_version_label(int major, int minor, char *out, size_t outsz)
{
    /* macOS marketing names */
    struct { int maj; int min; const char *name; } tbl[] = {
        {15, -1, "Sequoia"},  {14, -1, "Sonoma"},
        {13, -1, "Ventura"},  {12, -1, "Monterey"},
        {11, -1, "Big Sur"},  {10, 15, "Catalina"},
        {10, 14, "Mojave"},   {10, 13, "High Sierra"},
        {10, 12, "Sierra"},   {10, 11, "El Capitan"},
        {10, 10, "Yosemite"}, {10,  9, "Mavericks"},
        {0, 0, NULL}
    };
    for (int i = 0; tbl[i].name; i++) {
        if (tbl[i].maj == major &&
            (tbl[i].min == -1 || tbl[i].min == minor)) {
            snprintf(out, outsz, "macOS %s (%d.%d)", tbl[i].name, major, minor);
            return;
        }
    }
    snprintf(out, outsz, "macOS %d.%d", major, minor);
}

static void probe_macos(SnapxPlatformInfo *info, int quiet)
{
    snprintf(info->os_name, sizeof(info->os_name), "macOS");
    get_macos_version(&info->os_major, &info->os_minor, &info->os_patch);
    macos_version_label(info->os_major, info->os_minor,
                        info->os_version, sizeof(info->os_version));

    SnapxCaptureBackend tmp;

    /* ScreenCaptureKit requires macOS 12.3+, works best on 13+ */
#ifdef SNAPX_HAVE_SCREENCAPTUREKIT
    if (info->os_major >= 13 ||
        (info->os_major == 12 && info->os_minor >= 3)) {
        memset(&tmp, 0, sizeof(tmp));
        if (snapx_capture_backend_init(&tmp, SNAPX_BACKEND_MACOS_SCK) == 0) {
            info->backend_ok[SNAPX_BACKEND_MACOS_SCK] = 1;
            snapx_capture_backend_destroy(&tmp);
        }
    }
#endif

    /* CoreGraphics works on all supported macOS versions */
    memset(&tmp, 0, sizeof(tmp));
    if (snapx_capture_backend_init(&tmp, SNAPX_BACKEND_MACOS_CG) == 0) {
        info->backend_ok[SNAPX_BACKEND_MACOS_CG] = 1;
        snapx_capture_backend_destroy(&tmp);
    }

    if (info->backend_ok[SNAPX_BACKEND_MACOS_SCK]) {
        info->preferred = SNAPX_BACKEND_MACOS_SCK;
        info->fallback  = info->backend_ok[SNAPX_BACKEND_MACOS_CG]
                          ? SNAPX_BACKEND_MACOS_CG : SNAPX_BACKEND_UNKNOWN;
    } else if (info->backend_ok[SNAPX_BACKEND_MACOS_CG]) {
        info->preferred = SNAPX_BACKEND_MACOS_CG;
        info->fallback  = SNAPX_BACKEND_UNKNOWN;
    } else {
        info->preferred = SNAPX_BACKEND_UNKNOWN;
        info->fallback  = SNAPX_BACKEND_UNKNOWN;
        if (!quiet)
            fprintf(stderr, "[platform] WARNING: No macOS capture backend available.\n");
    }
}

#endif /* SNAPX_PLATFORM_MACOS */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

void snapx_platform_probe(SnapxPlatformInfo *info, int quiet)
{
    memset(info, 0, sizeof(*info));

#if defined(SNAPX_PLATFORM_LINUX)
    probe_linux(info, quiet);
#elif defined(SNAPX_PLATFORM_WINDOWS)
    probe_windows(info, quiet);
#elif defined(SNAPX_PLATFORM_MACOS)
    probe_macos(info, quiet);
#else
    snprintf(info->os_name,    sizeof(info->os_name),    "Unknown");
    snprintf(info->os_version, sizeof(info->os_version), "Unknown");
    info->preferred = SNAPX_BACKEND_UNKNOWN;
    info->fallback  = SNAPX_BACKEND_UNKNOWN;
    if (!quiet)
        fprintf(stderr, "[platform] Unsupported OS — no capture backend available.\n");
#endif
}

void snapx_platform_print(const SnapxPlatformInfo *info)
{
    printf("snapx platform information\n");
    printf("══════════════════════════\n");
    printf("  OS          : %s\n", info->os_version[0]
                                   ? info->os_version : info->os_name);

#ifdef SNAPX_PLATFORM_LINUX
    printf("  Distro      : %s\n", info->distro_pretty[0]
                                   ? info->distro_pretty : "Unknown");
    printf("  Kernel      : %d.%d.%d\n",
           info->os_major, info->os_minor, info->os_patch);
    printf("  Session     : %s\n", info->session_str[0]
                                   ? info->session_str : "unknown");
    printf("  Desktop     : %s\n", info->desktop_str[0]
                                   ? info->desktop_str : "unknown");
    printf("  Wayland     : %s\n", info->wayland_display_set ? "yes" : "no");
    printf("  X11         : %s\n", info->x11_display_set     ? "yes" : "no");
    printf("  XDG Portal  : %s\n", info->portal_reachable    ? "available" : "not found");
    printf("  PipeWire    : %s\n", info->pipewire_reachable   ? "available" : "not found");
#else
    printf("  Version     : %d.%d\n", info->os_major, info->os_minor);
#endif

    printf("\n  Compiled-in backends:\n");
    const SnapxBackendType all[] = {
        SNAPX_BACKEND_WAYLAND,
        SNAPX_BACKEND_X11,
        SNAPX_BACKEND_WIN_DXGI,
        SNAPX_BACKEND_WIN_GDI,
        SNAPX_BACKEND_MACOS_SCK,
        SNAPX_BACKEND_MACOS_CG,
    };
    for (size_t i = 0; i < sizeof(all)/sizeof(all[0]); i++) {
        SnapxBackendType t = all[i];
        if (t >= SNAPX_MAX_BACKENDS) continue;
        /* Only print backends that are compiled in (ok field only set if compiled) */
#if defined(SNAPX_HAVE_WAYLAND)
        if (t == SNAPX_BACKEND_WAYLAND)
            printf("    [%s] %s\n",
                   info->backend_ok[t] ? "OK" : "--",
                   snapx_backend_name(t));
#endif
#if defined(SNAPX_HAVE_X11)
        if (t == SNAPX_BACKEND_X11)
            printf("    [%s] %s\n",
                   info->backend_ok[t] ? "OK" : "--",
                   snapx_backend_name(t));
#endif
#if defined(SNAPX_PLATFORM_WINDOWS)
        if (t == SNAPX_BACKEND_WIN_DXGI || t == SNAPX_BACKEND_WIN_GDI)
            printf("    [%s] %s\n",
                   info->backend_ok[t] ? "OK" : "--",
                   snapx_backend_name(t));
#endif
#if defined(SNAPX_PLATFORM_MACOS)
        if (t == SNAPX_BACKEND_MACOS_SCK || t == SNAPX_BACKEND_MACOS_CG)
            printf("    [%s] %s\n",
                   info->backend_ok[t] ? "OK" : "--",
                   snapx_backend_name(t));
#endif
    }

    printf("\n  Selected backend : %s\n", snapx_backend_name(info->preferred));
    if (info->fallback != SNAPX_BACKEND_UNKNOWN)
        printf("  Fallback backend : %s\n", snapx_backend_name(info->fallback));
    else
        printf("  Fallback backend : none\n");
}

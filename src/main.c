/**
 * @file main.c
 * @brief snapx entry point — platform detection, argument parsing, GTK init.
 *
 * Platform detection order:
 *   Linux  → check $XDG_SESSION_TYPE (wayland / x11)
 *   Windows→ VerifyVersionInfo for Win10+
 *   macOS  → NSProcessInfo operatingSystemVersion
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SNAPX_PLATFORM_LINUX
#  include <unistd.h>
#  include <strings.h>   /* strcasecmp */
#endif

#ifdef SNAPX_PLATFORM_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#ifndef SNAPX_HEADLESS
#  if defined(SNAPX_USE_GTK4) || defined(SNAPX_USE_GTK3)
#    include <gtk/gtk.h>
#  endif
#endif

#include "capture/capture.h"
#ifndef SNAPX_HEADLESS
#  include "ui/window_main.h"
#endif
#include "utils/config.h"
#include "utils/hotkey.h"
#include "utils/monitor.h"

/* ─── Version ─────────────────────────────────────────────────────────────── */
#define SNAPX_VERSION_MAJOR 1
#define SNAPX_VERSION_MINOR 0
#define SNAPX_VERSION_PATCH 0
#define SNAPX_VERSION_STR   "1.0.0"

/* ─── CLI option structure ────────────────────────────────────────────────── */
typedef struct {
    SnapxCaptureMode mode;          /**< Capture mode requested via CLI        */
    int              delay_sec;     /**< Pre-capture delay in seconds           */
    int              monitor_index; /**< -1 = all monitors                      */
    int              no_gui;        /**< Headless / CLI-only mode               */
    char             output_path[512]; /**< Explicit output file path           */
    SnapxOutputFormat format;       /**< Output format (PNG/JPEG/WEBP)          */
    int              jpeg_quality;  /**< JPEG quality 1–100                     */
    int              copy_clipboard;/**< Copy result to clipboard               */
    int              show_version;
    int              show_help;
} SnapxCliOptions;

/* ─── Application state ──────────────────────────────────────────────────── */
typedef struct {
    SnapxConfig       config;
    SnapxCaptureBackend backend;
    SnapxCliOptions   cli;
} SnapxApp;

static SnapxApp g_app;

/* ─── Platform detection ─────────────────────────────────────────────────── */

/**
 * @brief Detect the active display server on Linux.
 * @return SNAPX_BACKEND_WAYLAND or SNAPX_BACKEND_X11.
 */
#ifdef SNAPX_PLATFORM_LINUX
static SnapxBackendType detect_linux_backend(void)
{
    const char *session = getenv("XDG_SESSION_TYPE");
    if (session) {
        if (strcasecmp(session, "wayland") == 0) {
            /* Verify compositor is actually running */
            const char *display = getenv("WAYLAND_DISPLAY");
            if (display && display[0] != '\0')
                return SNAPX_BACKEND_WAYLAND;
        }
    }
    /* Fallback: check DISPLAY env for X11 */
    const char *xdisplay = getenv("DISPLAY");
    if (xdisplay && xdisplay[0] != '\0')
        return SNAPX_BACKEND_X11;
    /* Last resort: try Wayland */
    return SNAPX_BACKEND_WAYLAND;
}
#endif /* SNAPX_PLATFORM_LINUX */

/**
 * @brief Detect Windows version and choose GDI+ vs DXGI backend.
 */
#ifdef SNAPX_PLATFORM_WINDOWS
static SnapxBackendType detect_windows_backend(void)
{
    OSVERSIONINFOEXW vi;
    DWORDLONG mask = 0;
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    VER_SET_CONDITION(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    vi.dwMajorVersion = 10;
    vi.dwMinorVersion = 0;
    if (VerifyVersionInfoW(&vi, VER_MAJORVERSION | VER_MINORVERSION, mask)) {
#  ifdef SNAPX_HAVE_DXGI
        return SNAPX_BACKEND_WIN_DXGI;
#  else
        return SNAPX_BACKEND_WIN_GDI;
#  endif
    }
    return SNAPX_BACKEND_WIN_GDI;
}
#endif /* SNAPX_PLATFORM_WINDOWS */

/**
 * @brief Detect macOS version; capture_macos.c upgrades to SCK if available.
 */
#ifdef SNAPX_PLATFORM_MACOS
static SnapxBackendType detect_macos_backend(void)
{
    return SNAPX_BACKEND_MACOS_CG;
}
#endif /* SNAPX_PLATFORM_MACOS */

/**
 * @brief Auto-detect the best capture backend for the current platform.
 */
static SnapxBackendType detect_backend(void)
{
#if defined(SNAPX_PLATFORM_LINUX)
    return detect_linux_backend();
#elif defined(SNAPX_PLATFORM_WINDOWS)
    return detect_windows_backend();
#elif defined(SNAPX_PLATFORM_MACOS)
    return detect_macos_backend();
#else
    return SNAPX_BACKEND_UNKNOWN;
#endif
}

/* ─── CLI argument parsing ────────────────────────────────────────────────── */

static void print_version(void)
{
    printf("snapx %s\n", SNAPX_VERSION_STR);
    printf("A lightweight cross-platform screenshot utility.\n");
}

static void print_help(const char *argv0)
{
    printf("Usage: %s [OPTIONS]\n\n", argv0);
    printf("Options:\n");
    printf("  -m, --mode <mode>        Capture mode: fullscreen, monitor, region,\n");
    printf("                           window, active  (default: fullscreen)\n");
    printf("  -d, --delay <sec>        Delay before capture in seconds (0–60)\n");
    printf("  -M, --monitor <index>    Monitor index to capture (0-based, -1 = all)\n");
    printf("  -o, --output <path>      Output file path (enables headless mode)\n");
    printf("  -f, --format <fmt>       Output format: png, jpeg, webp (default: png)\n");
    printf("  -q, --quality <1-100>    JPEG quality (default: 90)\n");
    printf("  -c, --clipboard          Also copy to clipboard\n");
    printf("  -n, --no-gui             Headless mode: capture without showing window\n");
    printf("  -v, --version            Show version and exit\n");
    printf("  -h, --help               Show this help and exit\n");
    printf("\nExamples:\n");
    printf("  %s                       Open GUI for interactive capture\n", argv0);
    printf("  %s -m region             Open region selection overlay\n", argv0);
    printf("  %s -m fullscreen -n -o screenshot.png\n", argv0);
    printf("                           Headless full-screen capture to file\n");
    printf("  %s -d 5 -m window        Capture active window after 5s delay\n", argv0);
}

/**
 * @brief Parse command-line arguments into SnapxCliOptions.
 * @return 0 on success, non-zero to exit immediately.
 */
static int parse_args(int argc, char **argv, SnapxCliOptions *opts)
{
    opts->mode           = SNAPX_CAPTURE_FULLSCREEN;
    opts->delay_sec      = 0;
    opts->monitor_index  = -1;
    opts->no_gui         = 0;
    opts->output_path[0] = '\0';
    opts->format         = SNAPX_FORMAT_PNG;
    opts->jpeg_quality   = 90;
    opts->copy_clipboard = 0;
    opts->show_version   = 0;
    opts->show_help      = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            opts->show_version = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            opts->show_help = 1;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--clipboard") == 0) {
            opts->copy_clipboard = 1;
        } else if (strcmp(a, "-n") == 0 || strcmp(a, "--no-gui") == 0) {
            opts->no_gui = 1;
        } else if ((strcmp(a, "-m") == 0 || strcmp(a, "--mode") == 0) && i + 1 < argc) {
            const char *mode = argv[++i];
            if      (strcmp(mode, "fullscreen") == 0) opts->mode = SNAPX_CAPTURE_FULLSCREEN;
            else if (strcmp(mode, "monitor")    == 0) opts->mode = SNAPX_CAPTURE_MONITOR;
            else if (strcmp(mode, "region")     == 0) opts->mode = SNAPX_CAPTURE_REGION;
            else if (strcmp(mode, "window")     == 0) opts->mode = SNAPX_CAPTURE_WINDOW;
            else if (strcmp(mode, "active")     == 0) opts->mode = SNAPX_CAPTURE_ACTIVE_WINDOW;
            else { fprintf(stderr, "Unknown mode: %s\n", mode); return 1; }
        } else if ((strcmp(a, "-d") == 0 || strcmp(a, "--delay") == 0) && i + 1 < argc) {
            opts->delay_sec = atoi(argv[++i]);
            if (opts->delay_sec < 0 || opts->delay_sec > 60) {
                fprintf(stderr, "Delay must be 0–60 seconds.\n"); return 1;
            }
        } else if ((strcmp(a, "-M") == 0 || strcmp(a, "--monitor") == 0) && i + 1 < argc) {
            opts->monitor_index = atoi(argv[++i]);
        } else if ((strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) && i + 1 < argc) {
            snprintf(opts->output_path, sizeof(opts->output_path), "%s", argv[++i]);
            opts->no_gui = 1;
        } else if ((strcmp(a, "-f") == 0 || strcmp(a, "--format") == 0) && i + 1 < argc) {
            const char *fmt = argv[++i];
            if      (strcmp(fmt, "png")  == 0) opts->format = SNAPX_FORMAT_PNG;
            else if (strcmp(fmt, "jpeg") == 0 || strcmp(fmt, "jpg") == 0)
                                               opts->format = SNAPX_FORMAT_JPEG;
            else if (strcmp(fmt, "webp") == 0) opts->format = SNAPX_FORMAT_WEBP;
            else { fprintf(stderr, "Unknown format: %s\n", fmt); return 1; }
        } else if ((strcmp(a, "-q") == 0 || strcmp(a, "--quality") == 0) && i + 1 < argc) {
            opts->jpeg_quality = atoi(argv[++i]);
            if (opts->jpeg_quality < 1 || opts->jpeg_quality > 100) {
                fprintf(stderr, "Quality must be 1–100.\n"); return 1;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\nRun '%s --help' for usage.\n", a, argv[0]);
            return 1;
        }
    }
    return 0;
}

/* ─── Headless capture path ──────────────────────────────────────────────── */

/**
 * @brief Perform a capture without showing any GUI windows.
 */
static int run_headless(SnapxApp *app)
{
    SnapxCaptureRequest req = {0};
    req.mode           = app->cli.mode;
    req.delay_sec      = app->cli.delay_sec;
    req.monitor_index  = app->cli.monitor_index;
    req.include_cursor = app->config.show_cursor;

    fprintf(stderr, "[snapx] Headless capture: mode=%d delay=%ds\n",
            req.mode, req.delay_sec);

    SnapxImage *img = snapx_capture(&app->backend, &req);
#ifdef SNAPX_PLATFORM_LINUX
    /* Wayland portal may fail (e.g. no user present to approve dialog).
     * Automatically fall back to X11/Xwayland capture. */
    if (!img && app->backend.type == SNAPX_BACKEND_WAYLAND) {
        fprintf(stderr, "[snapx] Wayland portal capture failed, "
                        "falling back to X11/Xwayland...\n");
        SnapxCaptureBackend x11_backend = {0};
        if (snapx_capture_backend_init(&x11_backend, SNAPX_BACKEND_X11) == 0) {
            img = snapx_capture(&x11_backend, &req);
            if (img) {
                snapx_capture_backend_destroy(&app->backend);
                app->backend = x11_backend;
            } else {
                snapx_capture_backend_destroy(&x11_backend);
            }
        }
    }
#endif
    if (!img) {
        fprintf(stderr, "[snapx] Capture failed.\n");
        return 1;
    }

    const char *outpath = app->cli.output_path[0]
                            ? app->cli.output_path
                            : "screenshot.png";

    int ret = snapx_image_save(img, outpath, app->cli.format, app->cli.jpeg_quality);
    if (ret == 0)
        fprintf(stdout, "[snapx] Saved to %s\n", outpath);
    else
        fprintf(stderr, "[snapx] Failed to save %s\n", outpath);

    if (app->cli.copy_clipboard)
        snapx_clipboard_copy_image(img);

    snapx_image_free(img);
    return ret;
}

/* ─── GTK application callbacks ─────────────────────────────────────────────*/

#ifndef SNAPX_HEADLESS
static void on_activate(GtkApplication *gapp, gpointer user_data)
{
    SnapxApp *app = (SnapxApp *)user_data;
    snapx_window_main_create(gapp, &app->config, &app->backend, &app->cli.mode);
}
#endif

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Parse CLI first (before GTK consumes argv) */
    if (parse_args(argc, argv, &g_app.cli) != 0)
        return 1;

    if (g_app.cli.show_version) { print_version(); return 0; }
    if (g_app.cli.show_help)    { print_help(argv[0]); return 0; }

    /* Load persistent configuration */
    snapx_config_load(&g_app.config);

    /* Apply CLI overrides to config */
    if (g_app.cli.jpeg_quality != 90)
        g_app.config.jpeg_quality = g_app.cli.jpeg_quality;
    if (g_app.cli.format != SNAPX_FORMAT_PNG)
        g_app.config.default_format = g_app.cli.format;

    /* Detect and initialise capture backend */
    SnapxBackendType backend_type = detect_backend();
    if (snapx_capture_backend_init(&g_app.backend, backend_type) != 0) {
        fprintf(stderr, "[snapx] Failed to initialise capture backend (type=%d). "
                        "Trying fallback...\n", backend_type);
#ifdef SNAPX_PLATFORM_LINUX
        /* Wayland failed → try X11 */
        if (backend_type == SNAPX_BACKEND_WAYLAND) {
            if (snapx_capture_backend_init(&g_app.backend, SNAPX_BACKEND_X11) != 0) {
                fprintf(stderr, "[snapx] X11 fallback also failed. Exiting.\n");
                return 1;
            }
        }
#endif
    }

    /* Register global hotkey (best-effort; failure is non-fatal) */
    snapx_hotkey_init(&g_app.config);

    /* Headless path */
    if (g_app.cli.no_gui) {
        int ret = run_headless(&g_app);
        snapx_capture_backend_destroy(&g_app.backend);
        snapx_hotkey_cleanup();
        return ret;
    }

    /* GUI path via GTK */
#ifndef SNAPX_HEADLESS
    GtkApplication *gapp = gtk_application_new("io.github.snapx",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &g_app);

    int status = g_application_run(G_APPLICATION(gapp), argc, argv);

    g_object_unref(gapp);
    snapx_capture_backend_destroy(&g_app.backend);
    snapx_hotkey_cleanup();
    snapx_config_save(&g_app.config);

    return status;
#else
    fprintf(stderr, "[snapx] GUI not available in this build. Use --no-gui / -o <path>.\n");
    snapx_capture_backend_destroy(&g_app.backend);
    snapx_hotkey_cleanup();
    return 1;
#endif
}

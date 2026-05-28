/**
 * @file main.c
 * @brief snapx entry point — platform probe, argument parsing, backend init,
 *        headless capture, and GTK GUI launch.
 *
 * Runtime detection order (all happens via snapx_platform_probe):
 *   Linux  → ScreenCast/PipeWire → X11 → fail
 *   Windows→ DXGI (Win8+) → GDI (all versions) → fail
 *   macOS  → ScreenCaptureKit (13+) → CoreGraphics → fail
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SNAPX_PLATFORM_LINUX
#  include <unistd.h>
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
#include "capture/platform.h"
#ifndef SNAPX_HEADLESS
#  include "ui/window_main.h"
#endif
#include "utils/config.h"
#include "utils/hotkey.h"
#include "utils/monitor.h"
#include "output/clipboard.h"
#include "output/save.h"

/* ─── Version ─────────────────────────────────────────────────────────────── */
#define SNAPX_VERSION_STR "1.0.0"

/* ─── CLI options ─────────────────────────────────────────────────────────── */
typedef struct {
    SnapxCaptureMode  mode;
    int               delay_sec;
    int               monitor_index;
    int               no_gui;
    char              output_path[512];
    SnapxOutputFormat format;
    int               jpeg_quality;
    int               copy_clipboard;
    int               show_version;
    int               show_help;
    int               show_info;     /**< --info: print platform probe and exit */
} SnapxCliOptions;

/* ─── Application state ───────────────────────────────────────────────────── */
typedef struct {
    SnapxConfig         config;
    SnapxCaptureBackend backend;
    SnapxCliOptions     cli;
    SnapxPlatformInfo   platform;
} SnapxApp;

static SnapxApp g_app;

/* ─── Help / version ─────────────────────────────────────────────────────── */

static void print_version(const SnapxPlatformInfo *p)
{
    printf("snapx %s\n", SNAPX_VERSION_STR);
    printf("A lightweight cross-platform screenshot utility.\n");
    printf("OS: %s | Backend: %s\n",
           p->os_version[0] ? p->os_version : p->os_name,
           snapx_backend_name(p->preferred));
}

static void print_help(const char *argv0)
{
    printf("Usage: %s [OPTIONS]\n\n", argv0);
    printf("Options:\n");
    printf("  -m, --mode <mode>      Capture mode:\n");
    printf("                           fullscreen  Full virtual desktop (default)\n");
    printf("                           monitor     Single monitor (use -M for index)\n");
    printf("                           region      Interactive region selection\n");
    printf("                           window      Active window\n");
    printf("                           active      Same as window\n");
    printf("  -d, --delay <sec>      Delay before capture in seconds (0–60)\n");
    printf("  -M, --monitor <idx>    Monitor index (0-based, default = primary)\n");
    printf("  -o, --output <path>    Output file (implies --no-gui)\n");
    printf("  -f, --format <fmt>     Output format: png (default), jpeg, webp\n");
    printf("  -q, --quality <1-100>  JPEG/WebP quality (default: 90)\n");
    printf("  -c, --clipboard        Copy to clipboard after capture\n");
    printf("  -n, --no-gui           Headless / CLI-only mode\n");
    printf("      --info             Print detected platform info and exit\n");
    printf("  -v, --version          Show version and exit\n");
    printf("  -h, --help             Show this help and exit\n");
    printf("\nExamples:\n");
    printf("  %s                              Open GUI\n", argv0);
    printf("  %s --info                       Show detected OS / backend info\n", argv0);
    printf("  %s -n -o shot.png               Headless fullscreen to PNG\n", argv0);
    printf("  %s -n -m monitor -M 1 -o m1.png Capture monitor 1\n", argv0);
    printf("  %s -n -d 3 -f jpeg -q 85 -o out.jpg  Delayed JPEG capture\n", argv0);
    printf("  %s -n -o out.webp -f webp        WebP capture\n", argv0);
    printf("  %s -n -c -o clip.png             Capture + copy to clipboard\n", argv0);
}

/* ─── Argument parsing ───────────────────────────────────────────────────── */

static int parse_args(int argc, char **argv, SnapxCliOptions *opts)
{
    opts->mode           = SNAPX_CAPTURE_FULLSCREEN;
    opts->delay_sec      = 0;
    opts->monitor_index  = 0;
    opts->no_gui         = 0;
    opts->output_path[0] = '\0';
    opts->format         = SNAPX_FORMAT_PNG;
    opts->jpeg_quality   = 90;
    opts->copy_clipboard = 0;
    opts->show_version   = 0;
    opts->show_help      = 0;
    opts->show_info      = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (strcmp(a, "-v") == 0 || strcmp(a, "--version")   == 0)
            opts->show_version = 1;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help")      == 0)
            opts->show_help = 1;
        else if (strcmp(a, "--info") == 0)
            opts->show_info = 1;
        else if (strcmp(a, "-c") == 0 || strcmp(a, "--clipboard") == 0)
            opts->copy_clipboard = 1;
        else if (strcmp(a, "-n") == 0 || strcmp(a, "--no-gui")    == 0)
            opts->no_gui = 1;
        else if ((strcmp(a, "-m") == 0 || strcmp(a, "--mode") == 0) && i+1 < argc) {
            const char *m = argv[++i];
            if      (strcmp(m, "fullscreen") == 0) opts->mode = SNAPX_CAPTURE_FULLSCREEN;
            else if (strcmp(m, "monitor")    == 0) opts->mode = SNAPX_CAPTURE_MONITOR;
            else if (strcmp(m, "region")     == 0) opts->mode = SNAPX_CAPTURE_REGION;
            else if (strcmp(m, "window")     == 0 ||
                     strcmp(m, "active")     == 0) opts->mode = SNAPX_CAPTURE_ACTIVE_WINDOW;
            else { fprintf(stderr, "Unknown mode: %s\n", m); return 1; }
        }
        else if ((strcmp(a, "-d") == 0 || strcmp(a, "--delay") == 0) && i+1 < argc) {
            opts->delay_sec = atoi(argv[++i]);
            if (opts->delay_sec < 0 || opts->delay_sec > 60) {
                fprintf(stderr, "Delay must be 0–60 seconds.\n"); return 1;
            }
        }
        else if ((strcmp(a, "-M") == 0 || strcmp(a, "--monitor") == 0) && i+1 < argc)
            opts->monitor_index = atoi(argv[++i]);
        else if ((strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) && i+1 < argc) {
            snprintf(opts->output_path, sizeof(opts->output_path), "%s", argv[++i]);
            opts->no_gui = 1;
        }
        else if ((strcmp(a, "-f") == 0 || strcmp(a, "--format") == 0) && i+1 < argc) {
            const char *f = argv[++i];
            if      (strcmp(f, "png")  == 0) opts->format = SNAPX_FORMAT_PNG;
            else if (strcmp(f, "jpeg") == 0 ||
                     strcmp(f, "jpg")  == 0) opts->format = SNAPX_FORMAT_JPEG;
            else if (strcmp(f, "webp") == 0) opts->format = SNAPX_FORMAT_WEBP;
            else { fprintf(stderr, "Unknown format: %s\n", f); return 1; }
        }
        else if ((strcmp(a, "-q") == 0 || strcmp(a, "--quality") == 0) && i+1 < argc) {
            opts->jpeg_quality = atoi(argv[++i]);
            if (opts->jpeg_quality < 1 || opts->jpeg_quality > 100) {
                fprintf(stderr, "Quality must be 1–100.\n"); return 1;
            }
        }
        else {
            fprintf(stderr, "Unknown option: %s\nRun '%s --help' for usage.\n",
                    a, argv[0]);
            return 1;
        }
    }
    return 0;
}

/* ─── Headless capture ───────────────────────────────────────────────────── */

static int run_headless(SnapxApp *app)
{
    SnapxCaptureRequest req = {0};
    req.mode           = app->cli.mode;
    req.delay_sec      = app->cli.delay_sec;
    req.monitor_index  = app->cli.monitor_index;
    req.include_cursor = app->config.show_cursor;

    const char *mode_names[] = {
        "fullscreen", "monitor", "region", "window", "active window"
    };
    fprintf(stderr, "[snapx] Capturing: mode=%s delay=%ds backend=%s\n",
            mode_names[req.mode < 5 ? req.mode : 0],
            req.delay_sec,
            snapx_backend_name(app->backend.type));

    SnapxImage *img = snapx_capture(&app->backend, &req);
    if (!img) {
        fprintf(stderr, "[snapx] Capture failed.\n"
                "  OS: %s\n"
                "  Try: snapx --info  to diagnose backend availability.\n",
                app->platform.os_version[0]
                    ? app->platform.os_version
                    : app->platform.os_name);
        return 1;
    }

    const char *outpath = app->cli.output_path[0]
                            ? app->cli.output_path
                            : "screenshot.png";

    int ret = snapx_image_save(img, outpath, app->cli.format, app->cli.jpeg_quality);
    if (ret == 0)
        printf("[snapx] Saved: %s  (%dx%d, %s)\n",
               outpath, img->width, img->height,
               app->cli.format == SNAPX_FORMAT_JPEG ? "JPEG" :
               app->cli.format == SNAPX_FORMAT_WEBP ? "WebP" : "PNG");
    else
        fprintf(stderr, "[snapx] Save failed: %s\n", outpath);

    if (app->cli.copy_clipboard) {
        snapx_clipboard_copy_image(img);
        fprintf(stderr, "[snapx] Copied to clipboard.\n");
    }

    snapx_image_free(img);
    return ret;
}

/* ─── GTK activate ───────────────────────────────────────────────────────── */

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
    /* Parse CLI before anything else */
    if (parse_args(argc, argv, &g_app.cli) != 0)
        return 1;

    /* Platform probe (quiet for --version/--help to avoid clutter) */
    int quiet = g_app.cli.show_version || g_app.cli.show_help;
    snapx_platform_probe(&g_app.platform, quiet);

    if (g_app.cli.show_version) { print_version(&g_app.platform); return 0; }
    if (g_app.cli.show_help)    { print_help(argv[0]);             return 0; }
    if (g_app.cli.show_info)    { snapx_platform_print(&g_app.platform); return 0; }

    /* Abort early if no backend found */
    if (g_app.platform.preferred == SNAPX_BACKEND_UNKNOWN) {
        fprintf(stderr,
            "[snapx] No capture backend available on this system.\n"
            "  Run: %s --info  for details.\n", argv[0]);
        return 1;
    }

    /* Load config */
    snapx_config_load(&g_app.config);
    if (g_app.cli.jpeg_quality != 90)
        g_app.config.jpeg_quality = g_app.cli.jpeg_quality;
    if (g_app.cli.format != SNAPX_FORMAT_PNG)
        g_app.config.default_format = g_app.cli.format;

    /* Init backend using probe result (tries preferred then fallback) */
    if (snapx_capture_backend_init_best(&g_app.backend, &g_app.platform) != 0) {
        fprintf(stderr, "[snapx] Could not initialise any capture backend.\n"
                "  Run: %s --info\n", argv[0]);
        return 1;
    }

    fprintf(stderr, "[snapx] Backend: %s\n",
            snapx_backend_name(g_app.backend.type));

    /* Register global hotkey (best-effort) */
    snapx_hotkey_init(&g_app.config);

    /* Headless path */
    if (g_app.cli.no_gui) {
        int ret = run_headless(&g_app);
        snapx_capture_backend_destroy(&g_app.backend);
        snapx_hotkey_cleanup();
        return ret;
    }

    /* GUI path */
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
    fprintf(stderr,
        "[snapx] GUI not available in this build. Use -n / -o <path>.\n"
        "  Example: %s -n -o screenshot.png\n", argv[0]);
    snapx_capture_backend_destroy(&g_app.backend);
    snapx_hotkey_cleanup();
    return 1;
#endif
}

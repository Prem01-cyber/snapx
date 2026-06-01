/**
 * @file config.c
 * @brief Read/write config.ini for persistent application settings.
 */

#include "config.h"
#include "pattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef SNAPX_PLATFORM_WINDOWS
#  include <shlobj.h>
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0755)
#endif

/* ─── Platform config directory ─────────────────────────────────────────── */

void snapx_config_get_dir(char *buf, size_t bufsz)
{
#ifdef SNAPX_PLATFORM_WINDOWS
    char appdata[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        snprintf(buf, bufsz, "%s\\snapx", appdata);
    } else {
        snprintf(buf, bufsz, "C:\\snapx");
    }
#elif defined(SNAPX_PLATFORM_MACOS)
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, bufsz, "%s/Library/Application Support/snapx", home);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        snprintf(buf, bufsz, "%s/snapx", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(buf, bufsz, "%s/.config/snapx", home);
    }
#endif
}

static void get_config_path(char *buf, size_t bufsz)
{
    char dir[SNAPX_CONFIG_MAX_PATH - 32];
    snapx_config_get_dir(dir, sizeof(dir));
#ifdef SNAPX_PLATFORM_WINDOWS
    snprintf(buf, bufsz, "%s\\config.ini", dir);
#else
    snprintf(buf, bufsz, "%s/config.ini", dir);
#endif
}

static void get_default_save_dir(char *buf, size_t bufsz)
{
#ifdef SNAPX_PLATFORM_WINDOWS
    char pictures[MAX_PATH] = {0};
    SHGetFolderPathA(NULL, CSIDL_MYPICTURES, NULL, 0, pictures);
    snprintf(buf, bufsz, "%s\\Screenshots", pictures);
#elif defined(SNAPX_PLATFORM_MACOS)
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, bufsz, "%s/Pictures/Screenshots", home);
#else
    const char *xdg_pics = getenv("XDG_PICTURES_DIR");
    if (xdg_pics && xdg_pics[0] != '\0') {
        snprintf(buf, bufsz, "%s/Screenshots", xdg_pics);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(buf, bufsz, "%s/Pictures/Screenshots", home);
    }
#endif
}

static void apply_defaults(SnapxConfig *c)
{
    get_default_save_dir(c->save_dir, sizeof(c->save_dir));
    snprintf(c->filename_pattern, sizeof(c->filename_pattern),
             "screenshot_%%Y%%m%%d_%%H%%M%%S");
    c->default_mode    = SNAPX_CAPTURE_FULLSCREEN;
    c->default_delay   = 0;
    c->show_cursor     = 0;
    c->wayland_capture_prefer = 0;
    c->default_format  = SNAPX_FORMAT_PNG;
    c->jpeg_quality    = 90;
    c->auto_clipboard  = 1;
    c->play_sound      = 1;
    c->upload_service  = SNAPX_UPLOAD_NONE;
    snprintf(c->upload_custom_field, sizeof(c->upload_custom_field), "file");
    c->upload_custom_url_field[0] = '\0';
    c->upload_copy_url = 1;
    c->upload_auto     = 0;
    c->magnifier_enabled = 1;
    c->magnifier_zoom    = 8;
    c->window_snap_enabled = 1;
    c->close_to_tray   = 0;
    c->start_in_tray   = 0;
    snprintf(c->ocr_lang, sizeof(c->ocr_lang), "eng");
    snapx_beautify_defaults(&c->beautify);
    c->default_tool    = SNAPX_TOOL_RECT;
    c->default_color_r = 1.0;
    c->default_color_g = 0.2;
    c->default_color_b = 0.2;
    c->default_color_a = 1.0;
    snapx_shortcuts_set_defaults(&c->shortcuts);
    snprintf(c->hotkey, sizeof(c->hotkey), "%s", c->shortcuts.global_capture);
    c->window_x = 100; c->window_y = 100;
    c->window_w = 960; c->window_h = 640;
}

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == ' ' || s[l-1] == '\t' ||
                     s[l-1] == '\r' || s[l-1] == '\n')) s[--l] = '\0';
}

static void parse_ini(FILE *fp, SnapxConfig *c)
{
    char line[512];
    char section[64] = "";
    int shortcuts_section_seen = 0;

    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == ';' || line[0] == '\0')
            continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%.63s", line + 1);
                trim(section);
                if (strcmp(section, "shortcuts") == 0)
                    shortcuts_section_seen = 1;
            }
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line; trim(key);
        char *val = eq + 1; trim(val);

        if (strcmp(section, "shortcuts") == 0) {
            if      (strcmp(key, "global_capture") == 0) snprintf(c->shortcuts.global_capture, sizeof(c->shortcuts.global_capture), "%s", val);
            else if (strcmp(key, "capture_fullscreen") == 0) snprintf(c->shortcuts.capture_fullscreen, sizeof(c->shortcuts.capture_fullscreen), "%s", val);
            else if (strcmp(key, "capture_monitor") == 0) snprintf(c->shortcuts.capture_monitor, sizeof(c->shortcuts.capture_monitor), "%s", val);
            else if (strcmp(key, "capture_region") == 0) snprintf(c->shortcuts.capture_region, sizeof(c->shortcuts.capture_region), "%s", val);
            else if (strcmp(key, "capture_window") == 0) snprintf(c->shortcuts.capture_window, sizeof(c->shortcuts.capture_window), "%s", val);
            else if (strcmp(key, "save") == 0) snprintf(c->shortcuts.save, sizeof(c->shortcuts.save), "%s", val);
            else if (strcmp(key, "copy") == 0) snprintf(c->shortcuts.copy, sizeof(c->shortcuts.copy), "%s", val);
            else if (strcmp(key, "undo") == 0) snprintf(c->shortcuts.undo, sizeof(c->shortcuts.undo), "%s", val);
            else if (strcmp(key, "redo") == 0) snprintf(c->shortcuts.redo, sizeof(c->shortcuts.redo), "%s", val);
            else if (strcmp(key, "fit") == 0) snprintf(c->shortcuts.fit, sizeof(c->shortcuts.fit), "%s", val);
            else if (strcmp(key, "zoom_in") == 0) snprintf(c->shortcuts.zoom_in, sizeof(c->shortcuts.zoom_in), "%s", val);
            else if (strcmp(key, "zoom_out") == 0) snprintf(c->shortcuts.zoom_out, sizeof(c->shortcuts.zoom_out), "%s", val);
            else if (strcmp(key, "region_confirm") == 0) snprintf(c->shortcuts.region_confirm, sizeof(c->shortcuts.region_confirm), "%s", val);
            else if (strcmp(key, "region_cancel") == 0) snprintf(c->shortcuts.region_cancel, sizeof(c->shortcuts.region_cancel), "%s", val);
            else if (strcmp(key, "upload") == 0) snprintf(c->shortcuts.upload, sizeof(c->shortcuts.upload), "%s", val);
            else if (strcmp(key, "ocr") == 0) snprintf(c->shortcuts.ocr, sizeof(c->shortcuts.ocr), "%s", val);
            else if (strcmp(key, "pin") == 0) snprintf(c->shortcuts.pin, sizeof(c->shortcuts.pin), "%s", val);
            else if (strcmp(key, "crop") == 0) snprintf(c->shortcuts.crop, sizeof(c->shortcuts.crop), "%s", val);
            continue;
        }

        if      (strcmp(key, "save_dir")          == 0) snprintf(c->save_dir, sizeof(c->save_dir), "%s", val);
        else if (strcmp(key, "filename_pattern")   == 0) snprintf(c->filename_pattern, sizeof(c->filename_pattern), "%s", val);
        else if (strcmp(key, "default_mode")       == 0) c->default_mode    = (SnapxCaptureMode)atoi(val);
        else if (strcmp(key, "default_delay")      == 0) c->default_delay   = atoi(val);
        else if (strcmp(key, "show_cursor")        == 0) c->show_cursor     = atoi(val);
        else if (strcmp(key, "wayland_capture_prefer") == 0) {
            c->wayland_capture_prefer = (strcmp(val, "screencast") == 0) ? 1 : 0;
        }
        else if (strcmp(key, "default_format")     == 0) c->default_format  = (SnapxOutputFormat)atoi(val);
        else if (strcmp(key, "jpeg_quality")       == 0) c->jpeg_quality    = atoi(val);
        else if (strcmp(key, "auto_clipboard")     == 0) c->auto_clipboard  = atoi(val);
        else if (strcmp(key, "play_sound")         == 0) c->play_sound      = atoi(val);
        else if (strcmp(key, "upload_service")     == 0) {
            if (strcmp(val, "imgur") == 0) c->upload_service = SNAPX_UPLOAD_IMGUR;
            else if (strcmp(val, "custom") == 0) c->upload_service = SNAPX_UPLOAD_CUSTOM;
            else c->upload_service = SNAPX_UPLOAD_NONE;
        }
        else if (strcmp(key, "imgur_client_id")    == 0) snprintf(c->upload_imgur_client_id, sizeof(c->upload_imgur_client_id), "%s", val);
        else if (strcmp(key, "custom_url")         == 0) snprintf(c->upload_custom_url, sizeof(c->upload_custom_url), "%s", val);
        else if (strcmp(key, "custom_field")       == 0) snprintf(c->upload_custom_field, sizeof(c->upload_custom_field), "%s", val);
        else if (strcmp(key, "custom_url_field")   == 0) snprintf(c->upload_custom_url_field, sizeof(c->upload_custom_url_field), "%s", val);
        else if (strcmp(key, "copy_url_after_upload") == 0) c->upload_copy_url = atoi(val);
        else if (strcmp(key, "auto_upload")        == 0) c->upload_auto = atoi(val);
        else if (strcmp(key, "magnifier_enabled")  == 0) c->magnifier_enabled = atoi(val);
        else if (strcmp(key, "magnifier_zoom")     == 0) c->magnifier_zoom = atoi(val);
        else if (strcmp(key, "window_snap_enabled")== 0) c->window_snap_enabled = atoi(val);
        else if (strcmp(key, "close_to_tray")       == 0) c->close_to_tray = atoi(val);
        else if (strcmp(key, "start_in_tray")       == 0) c->start_in_tray = atoi(val);
        else if (strcmp(key, "ocr_lang")           == 0) snprintf(c->ocr_lang, sizeof(c->ocr_lang), "%s", val);
        else if (strcmp(key, "beautify_enabled")   == 0) c->beautify.enabled = atoi(val);
        else if (strcmp(key, "beautify_padding")   == 0) c->beautify.padding = atoi(val);
        else if (strcmp(key, "beautify_bg_type")   == 0) c->beautify.bg_type = (SnapxBeautifyBg)atoi(val);
        else if (strcmp(key, "beautify_bg_r")      == 0) c->beautify.bg_r  = atof(val);
        else if (strcmp(key, "beautify_bg_g")      == 0) c->beautify.bg_g  = atof(val);
        else if (strcmp(key, "beautify_bg_b")      == 0) c->beautify.bg_b  = atof(val);
        else if (strcmp(key, "beautify_bg_r2")     == 0) c->beautify.bg_r2 = atof(val);
        else if (strcmp(key, "beautify_bg_g2")     == 0) c->beautify.bg_g2 = atof(val);
        else if (strcmp(key, "beautify_bg_b2")     == 0) c->beautify.bg_b2 = atof(val);
        else if (strcmp(key, "beautify_corner")    == 0) c->beautify.corner_radius = atoi(val);
        else if (strcmp(key, "beautify_shadow")    == 0) c->beautify.shadow = atoi(val);
        else if (strcmp(key, "beautify_shadow_size") == 0) c->beautify.shadow_size = atoi(val);
        else if (strcmp(key, "default_tool")       == 0) c->default_tool    = (SnapxAnnotationTool)atoi(val);
        else if (strcmp(key, "default_color_r")    == 0) c->default_color_r = atof(val);
        else if (strcmp(key, "default_color_g")    == 0) c->default_color_g = atof(val);
        else if (strcmp(key, "default_color_b")    == 0) c->default_color_b = atof(val);
        else if (strcmp(key, "default_color_a")    == 0) c->default_color_a = atof(val);
        else if (strcmp(key, "hotkey")             == 0) snprintf(c->hotkey, sizeof(c->hotkey), "%s", val);
        else if (strcmp(key, "window_x")           == 0) c->window_x = atoi(val);
        else if (strcmp(key, "window_y")           == 0) c->window_y = atoi(val);
        else if (strcmp(key, "window_w")           == 0) c->window_w = atoi(val);
        else if (strcmp(key, "window_h")           == 0) c->window_h = atoi(val);
    }

    if (!shortcuts_section_seen && c->hotkey[0])
        snprintf(c->shortcuts.global_capture, sizeof(c->shortcuts.global_capture),
                 "%s", c->hotkey);
    snprintf(c->hotkey, sizeof(c->hotkey), "%s", c->shortcuts.global_capture);
}

void snapx_config_load(SnapxConfig *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    apply_defaults(config);

    char path[SNAPX_CONFIG_MAX_PATH];
    get_config_path(path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (!fp) return;
    parse_ini(fp, config);
    fclose(fp);
}

void snapx_config_save(const SnapxConfig *config)
{
    if (!config) return;

    char dir[SNAPX_CONFIG_MAX_PATH];
    snapx_config_get_dir(dir, sizeof(dir));
    MKDIR(dir);

    char path[SNAPX_CONFIG_MAX_PATH];
    get_config_path(path, sizeof(path));

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "[config] Cannot write %s: %s\n", path, strerror(errno));
        return;
    }

    fprintf(fp, "# snapx configuration\n");
    fprintf(fp, "# Auto-generated — edit with care.\n\n");
    fprintf(fp, "[save]\n");
    fprintf(fp, "save_dir         = %s\n", config->save_dir);
    fprintf(fp, "filename_pattern = %s\n", config->filename_pattern);
    fprintf(fp, "\n[capture]\n");
    fprintf(fp, "default_mode     = %d\n", (int)config->default_mode);
    fprintf(fp, "default_delay    = %d\n", config->default_delay);
    fprintf(fp, "show_cursor      = %d\n", config->show_cursor);
    fprintf(fp, "wayland_capture_prefer = %s\n",
            config->wayland_capture_prefer ? "screencast" : "screenshot");
    fprintf(fp, "\n[output]\n");
    fprintf(fp, "default_format   = %d\n", (int)config->default_format);
    fprintf(fp, "jpeg_quality     = %d\n", config->jpeg_quality);
    fprintf(fp, "auto_clipboard   = %d\n", config->auto_clipboard);
    fprintf(fp, "play_sound       = %d\n", config->play_sound);
    fprintf(fp, "\n[upload]\n");
    fprintf(fp, "upload_service   = %s\n",
            config->upload_service == SNAPX_UPLOAD_IMGUR ? "imgur" :
            config->upload_service == SNAPX_UPLOAD_CUSTOM ? "custom" : "none");
    fprintf(fp, "imgur_client_id  = %s\n", config->upload_imgur_client_id);
    fprintf(fp, "custom_url       = %s\n", config->upload_custom_url);
    fprintf(fp, "custom_field     = %s\n", config->upload_custom_field);
    fprintf(fp, "custom_url_field = %s\n", config->upload_custom_url_field);
    fprintf(fp, "copy_url_after_upload = %d\n", config->upload_copy_url);
    fprintf(fp, "auto_upload      = %d\n", config->upload_auto);
    fprintf(fp, "\n[overlay]\n");
    fprintf(fp, "magnifier_enabled = %d\n", config->magnifier_enabled);
    fprintf(fp, "magnifier_zoom    = %d\n", config->magnifier_zoom);
    fprintf(fp, "window_snap_enabled = %d\n", config->window_snap_enabled);
    fprintf(fp, "\n[tray]\n");
    fprintf(fp, "close_to_tray    = %d\n", config->close_to_tray);
    fprintf(fp, "start_in_tray    = %d\n", config->start_in_tray);
    fprintf(fp, "\n[ocr]\n");
    fprintf(fp, "ocr_lang         = %s\n", config->ocr_lang);
    fprintf(fp, "\n[beautify]\n");
    fprintf(fp, "beautify_enabled = %d\n", config->beautify.enabled);
    fprintf(fp, "beautify_padding = %d\n", config->beautify.padding);
    fprintf(fp, "beautify_bg_type = %d\n", (int)config->beautify.bg_type);
    fprintf(fp, "beautify_bg_r    = %.4f\n", config->beautify.bg_r);
    fprintf(fp, "beautify_bg_g    = %.4f\n", config->beautify.bg_g);
    fprintf(fp, "beautify_bg_b    = %.4f\n", config->beautify.bg_b);
    fprintf(fp, "beautify_bg_r2   = %.4f\n", config->beautify.bg_r2);
    fprintf(fp, "beautify_bg_g2   = %.4f\n", config->beautify.bg_g2);
    fprintf(fp, "beautify_bg_b2   = %.4f\n", config->beautify.bg_b2);
    fprintf(fp, "beautify_corner  = %d\n", config->beautify.corner_radius);
    fprintf(fp, "beautify_shadow  = %d\n", config->beautify.shadow);
    fprintf(fp, "beautify_shadow_size = %d\n", config->beautify.shadow_size);
    fprintf(fp, "\n[annotation]\n");
    fprintf(fp, "default_tool     = %d\n", (int)config->default_tool);
    fprintf(fp, "default_color_r  = %.4f\n", config->default_color_r);
    fprintf(fp, "default_color_g  = %.4f\n", config->default_color_g);
    fprintf(fp, "default_color_b  = %.4f\n", config->default_color_b);
    fprintf(fp, "default_color_a  = %.4f\n", config->default_color_a);
    fprintf(fp, "\n[shortcuts]\n");
    fprintf(fp, "global_capture   = %s\n", config->shortcuts.global_capture);
    fprintf(fp, "capture_fullscreen = %s\n", config->shortcuts.capture_fullscreen);
    fprintf(fp, "capture_monitor  = %s\n", config->shortcuts.capture_monitor);
    fprintf(fp, "capture_region   = %s\n", config->shortcuts.capture_region);
    fprintf(fp, "capture_window   = %s\n", config->shortcuts.capture_window);
    fprintf(fp, "save             = %s\n", config->shortcuts.save);
    fprintf(fp, "copy             = %s\n", config->shortcuts.copy);
    fprintf(fp, "undo             = %s\n", config->shortcuts.undo);
    fprintf(fp, "redo             = %s\n", config->shortcuts.redo);
    fprintf(fp, "fit              = %s\n", config->shortcuts.fit);
    fprintf(fp, "zoom_in          = %s\n", config->shortcuts.zoom_in);
    fprintf(fp, "zoom_out         = %s\n", config->shortcuts.zoom_out);
    fprintf(fp, "region_confirm   = %s\n", config->shortcuts.region_confirm);
    fprintf(fp, "region_cancel    = %s\n", config->shortcuts.region_cancel);
    fprintf(fp, "upload           = %s\n", config->shortcuts.upload);
    fprintf(fp, "ocr              = %s\n", config->shortcuts.ocr);
    fprintf(fp, "pin              = %s\n", config->shortcuts.pin);
    fprintf(fp, "crop             = %s\n", config->shortcuts.crop);
    fprintf(fp, "\n[window]\n");
    fprintf(fp, "window_x         = %d\n", config->window_x);
    fprintf(fp, "window_y         = %d\n", config->window_y);
    fprintf(fp, "window_w         = %d\n", config->window_w);
    fprintf(fp, "window_h         = %d\n", config->window_h);

    fclose(fp);
}

void snapx_config_build_path(const SnapxConfig *config, SnapxOutputFormat fmt,
                             char *buf, size_t bufsz)
{
    char fname[SNAPX_CONFIG_MAX_PATTERN * 2];
    snapx_pattern_expand_basename(config, fmt, fname, sizeof(fname));

    const char *ext = ".png";
    if      (fmt == SNAPX_FORMAT_JPEG) ext = ".jpg";
    else if (fmt == SNAPX_FORMAT_WEBP) ext = ".webp";

#ifdef SNAPX_PLATFORM_WINDOWS
    snprintf(buf, bufsz, "%s\\%s%s", config->save_dir, fname, ext);
#else
    snprintf(buf, bufsz, "%s/%s%s", config->save_dir, fname, ext);
#endif
}

/**
 * @file config.c
 * @brief Read/write config.ini for persistent application settings.
 *
 * Simple hand-written INI parser — no external dependency.
 * Supports sections [section] and key=value pairs.
 * Comments start with # or ;.
 */

#include "config.h"

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
    /* Linux: prefer XDG_CONFIG_HOME */
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
    /* Use a larger intermediate buffer so the path + "/config.ini" fits */
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
    /* Try $XDG_PICTURES_DIR first */
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

/* ─── Defaults ───────────────────────────────────────────────────────────── */

static void apply_defaults(SnapxConfig *c)
{
    get_default_save_dir(c->save_dir, sizeof(c->save_dir));
    snprintf(c->filename_pattern, sizeof(c->filename_pattern),
             "screenshot_%%Y%%m%%d_%%H%%M%%S");
    c->default_mode    = SNAPX_CAPTURE_FULLSCREEN;
    c->default_delay   = 0;
    c->show_cursor     = 0;
    c->default_format  = SNAPX_FORMAT_PNG;
    c->jpeg_quality    = 90;
    c->auto_clipboard  = 0;
    c->play_sound      = 1;
    c->default_tool    = SNAPX_TOOL_RECT;
    c->default_color_r = 1.0;
    c->default_color_g = 0.2;
    c->default_color_b = 0.2;
    c->default_color_a = 1.0;
    snprintf(c->hotkey, sizeof(c->hotkey), "super+shift+s");
    c->window_x = 100; c->window_y = 100;
    c->window_w = 960; c->window_h = 640;
}

/* ─── Tiny INI parser ────────────────────────────────────────────────────── */

static void trim(char *s)
{
    /* Leading whitespace */
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    /* Trailing whitespace */
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == ' ' || s[l-1] == '\t' ||
                     s[l-1] == '\r' || s[l-1] == '\n')) s[--l] = '\0';
}

static void parse_ini(FILE *fp, SnapxConfig *c)
{
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == ';' || line[0] == '[' || line[0] == '\0')
            continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line; trim(key);
        char *val = eq + 1; trim(val);

        if      (strcmp(key, "save_dir")          == 0) snprintf(c->save_dir, sizeof(c->save_dir), "%s", val);
        else if (strcmp(key, "filename_pattern")   == 0) snprintf(c->filename_pattern, sizeof(c->filename_pattern), "%s", val);
        else if (strcmp(key, "default_mode")       == 0) c->default_mode    = (SnapxCaptureMode)atoi(val);
        else if (strcmp(key, "default_delay")      == 0) c->default_delay   = atoi(val);
        else if (strcmp(key, "show_cursor")        == 0) c->show_cursor     = atoi(val);
        else if (strcmp(key, "default_format")     == 0) c->default_format  = (SnapxOutputFormat)atoi(val);
        else if (strcmp(key, "jpeg_quality")       == 0) c->jpeg_quality    = atoi(val);
        else if (strcmp(key, "auto_clipboard")     == 0) c->auto_clipboard  = atoi(val);
        else if (strcmp(key, "play_sound")         == 0) c->play_sound      = atoi(val);
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
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

void snapx_config_load(SnapxConfig *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    apply_defaults(config);

    char path[SNAPX_CONFIG_MAX_PATH];
    get_config_path(path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (!fp) return;  /* First run: use defaults */
    parse_ini(fp, config);
    fclose(fp);
}

void snapx_config_save(const SnapxConfig *config)
{
    if (!config) return;

    /* Ensure directory exists */
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
    fprintf(fp, "\n[output]\n");
    fprintf(fp, "default_format   = %d\n", (int)config->default_format);
    fprintf(fp, "jpeg_quality     = %d\n", config->jpeg_quality);
    fprintf(fp, "auto_clipboard   = %d\n", config->auto_clipboard);
    fprintf(fp, "play_sound       = %d\n", config->play_sound);
    fprintf(fp, "\n[annotation]\n");
    fprintf(fp, "default_tool     = %d\n", (int)config->default_tool);
    fprintf(fp, "default_color_r  = %.4f\n", config->default_color_r);
    fprintf(fp, "default_color_g  = %.4f\n", config->default_color_g);
    fprintf(fp, "default_color_b  = %.4f\n", config->default_color_b);
    fprintf(fp, "default_color_a  = %.4f\n", config->default_color_a);
    fprintf(fp, "\n[hotkey]\n");
    fprintf(fp, "hotkey           = %s\n", config->hotkey);
    fprintf(fp, "\n[window]\n");
    fprintf(fp, "window_x         = %d\n", config->window_x);
    fprintf(fp, "window_y         = %d\n", config->window_y);
    fprintf(fp, "window_w         = %d\n", config->window_w);
    fprintf(fp, "window_h         = %d\n", config->window_h);

    fclose(fp);
}

/* ─── Path builder with token expansion ─────────────────────────────────── */

void snapx_config_build_path(const SnapxConfig *config, char *buf, size_t bufsz)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    /* Expand the filename pattern */
    char fname[SNAPX_CONFIG_MAX_PATTERN * 2];
    char *p = fname;
    size_t rem = sizeof(fname) - 1;
    const char *s = config->filename_pattern;

    while (*s && rem > 0) {
        if (*s == '%' && *(s+1)) {
            char c = *(s+1);
            char tmp[32] = {0};
            switch (c) {
                case 'Y': snprintf(tmp, sizeof(tmp), "%04d", tm->tm_year + 1900); break;
                case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
                case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
                case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
                case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
                case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
                case 'n': {
                    /* Auto-increment: scan save_dir for existing files */
                    static int counter = 1;
                    snprintf(tmp, sizeof(tmp), "%04d", counter++);
                    break;
                }
                case '%': snprintf(tmp, sizeof(tmp), "%%"); break;
                default:
                    tmp[0] = '%'; tmp[1] = c; tmp[2] = '\0'; break;
            }
            size_t tl = strlen(tmp);
            if (tl > rem) tl = rem;
            memcpy(p, tmp, tl);
            p += tl; rem -= tl;
            s += 2;
        } else {
            *p++ = *s++; rem--;
        }
    }
    *p = '\0';

    /* Determine extension */
    const char *ext = ".png";
    if      (config->default_format == SNAPX_FORMAT_JPEG) ext = ".jpg";
    else if (config->default_format == SNAPX_FORMAT_WEBP) ext = ".webp";

    /* Combine dir + filename + extension */
#ifdef SNAPX_PLATFORM_WINDOWS
    snprintf(buf, bufsz, "%s\\%s%s", config->save_dir, fname, ext);
#else
    snprintf(buf, bufsz, "%s/%s%s", config->save_dir, fname, ext);
#endif
}

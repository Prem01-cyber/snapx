/**
 * @file config.h
 * @brief Application configuration: persistence via INI file.
 *
 * Config file locations:
 *   Linux / macOS  → ~/.config/snapx/config.ini
 *   macOS (alt)    → ~/Library/Application Support/snapx/config.ini
 *   Windows        → %APPDATA%\snapx\config.ini
 */

#ifndef SNAPX_CONFIG_H
#define SNAPX_CONFIG_H

#include "../capture/capture.h"

#define SNAPX_CONFIG_MAX_PATH    512
#define SNAPX_CONFIG_MAX_PATTERN 128

/* Annotation tool enum — guarded to avoid duplicate with annotation.h */
#ifndef SNAPX_ANNOTATION_TOOL_DEFINED
#define SNAPX_ANNOTATION_TOOL_DEFINED
typedef enum {
    SNAPX_TOOL_RECT      = 0,
    SNAPX_TOOL_ARROW     = 1,
    SNAPX_TOOL_PEN       = 2,
    SNAPX_TOOL_TEXT      = 3,
    SNAPX_TOOL_BLUR      = 4,
    SNAPX_TOOL_HIGHLIGHT = 5,
} SnapxAnnotationTool;
#endif

/** @brief All persistent application settings. */
typedef struct {
    /* Save location */
    char             save_dir[SNAPX_CONFIG_MAX_PATH];
    char             filename_pattern[SNAPX_CONFIG_MAX_PATTERN];

    /* Capture defaults */
    SnapxCaptureMode default_mode;
    int              default_delay;  /**< Seconds */
    int              show_cursor;

    /* Output */
    SnapxOutputFormat default_format;
    int               jpeg_quality;  /**< 1–100 */
    int               auto_clipboard;
    int               play_sound;

    /* Annotation defaults */
    SnapxAnnotationTool default_tool;
    double              default_color_r;
    double              default_color_g;
    double              default_color_b;
    double              default_color_a;

    /* Hotkey (platform string, e.g. "super+shift+s") */
    char hotkey[64];

    /* Window state */
    int window_x, window_y;
    int window_w, window_h;
} SnapxConfig;

/**
 * @brief Load configuration from disk.  Sets sane defaults if file missing.
 * @param config  Output config structure.
 */
void snapx_config_load(SnapxConfig *config);

/**
 * @brief Persist configuration to disk.
 * @param config  Config to write.
 */
void snapx_config_save(const SnapxConfig *config);

/**
 * @brief Build a full output file path from config settings.
 *
 * Expands tokens in config->filename_pattern:
 *   %Y %m %d %H %M %S → date/time components
 *   %n → auto-incremented screenshot number
 *
 * @param config  Active config.
 * @param buf     Output buffer.
 * @param bufsz   Buffer size in bytes.
 */
void snapx_config_build_path(const SnapxConfig *config, char *buf, size_t bufsz);

/**
 * @brief Return the platform-specific config directory path.
 * @param buf    Output buffer.
 * @param bufsz  Buffer size.
 */
void snapx_config_get_dir(char *buf, size_t bufsz);

#endif /* SNAPX_CONFIG_H */

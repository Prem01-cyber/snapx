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
#include "shortcut.h"

#define SNAPX_CONFIG_MAX_PATH    512
#define SNAPX_CONFIG_MAX_PATTERN 128
#define SNAPX_CONFIG_MAX_UPLOAD  256

/** Upload destination (Wayland/Linux workflow). */
typedef enum {
    SNAPX_UPLOAD_NONE   = 0,
    SNAPX_UPLOAD_IMGUR  = 1,
    SNAPX_UPLOAD_CUSTOM = 2,
} SnapxUploadService;

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
    SNAPX_TOOL_CALLOUT   = 6,
    SNAPX_TOOL_REDACT    = 7,
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

    /** Wayland only: 0 = Screenshot portal first (GNOME/Fedora-like), 1 = ScreenCast first */
    int              wayland_capture_prefer;

    /* Output */
    SnapxOutputFormat default_format;
    int               jpeg_quality;  /**< 1–100 */
    int               auto_clipboard;
    int               play_sound;

    /* Upload / share */
    SnapxUploadService upload_service;
    char               upload_imgur_client_id[SNAPX_CONFIG_MAX_UPLOAD];
    char               upload_custom_url[SNAPX_CONFIG_MAX_PATH];
    char               upload_custom_field[64];
    char               upload_custom_url_field[64];
    int                upload_copy_url;
    int                upload_auto;

    /* Capture overlay */
    int                magnifier_enabled;
    int                magnifier_zoom;
    int                window_snap_enabled;

    /* Tray / background */
    int                close_to_tray;
    int                start_in_tray;

    /* OCR */
    char               ocr_lang[16];

    /* Annotation defaults */
    SnapxAnnotationTool default_tool;
    double              default_color_r;
    double              default_color_g;
    double              default_color_b;
    double              default_color_a;

    /* Hotkey (legacy; migrated to shortcuts.global_capture) */
    char hotkey[64];

    /* Keyboard shortcuts */
    SnapxShortcuts shortcuts;

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
 *   %n → auto-incremented screenshot number (4 digits, 0001…)
 *   %d %i %u %03d … → printf-style counter (scans save directory)
 *
 * @param config  Active config.
 * @param buf     Output buffer.
 * @param bufsz   Buffer size in bytes.
 */
void snapx_config_build_path(const SnapxConfig *config, SnapxOutputFormat fmt,
                             char *buf, size_t bufsz);

/**
 * @brief Return the platform-specific config directory path.
 * @param buf    Output buffer.
 * @param bufsz  Buffer size.
 */
void snapx_config_get_dir(char *buf, size_t bufsz);

#endif /* SNAPX_CONFIG_H */

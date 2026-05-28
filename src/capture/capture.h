/**
 * @file capture.h
 * @brief Unified capture abstraction — platform-independent interface.
 *
 * All platform capture backends implement the function pointers in
 * SnapxCaptureBackend.  Callers should use the public API functions
 * (snapx_capture_backend_init, snapx_capture, …) rather than calling
 * backend function pointers directly.
 */

#ifndef SNAPX_CAPTURE_H
#define SNAPX_CAPTURE_H

#include <stdint.h>
#include <stddef.h>

/* ─── Forward declarations ───────────────────────────────────────────────── */
typedef struct SnapxImage        SnapxImage;
typedef struct SnapxMonitorInfo  SnapxMonitorInfo;
typedef struct SnapxCaptureBackend SnapxCaptureBackend;
typedef struct SnapxCaptureRequest SnapxCaptureRequest;

/* ─── Enumerations ────────────────────────────────────────────────────────── */

/** @brief Which display server / OS API to use for capture. */
typedef enum {
    SNAPX_BACKEND_UNKNOWN      = 0,
    SNAPX_BACKEND_WAYLAND      = 1,  /**< Linux: XDG portal + PipeWire          */
    SNAPX_BACKEND_X11          = 2,  /**< Linux: libX11 + XRandR                */
    SNAPX_BACKEND_WIN_GDI      = 3,  /**< Windows 7/8/10: GDI+ BitBlt           */
    SNAPX_BACKEND_WIN_DXGI     = 4,  /**< Windows 10/11: DXGI Desktop Dup.      */
    SNAPX_BACKEND_MACOS_CG     = 5,  /**< macOS: CoreGraphics CGWindowList      */
    SNAPX_BACKEND_MACOS_SCK    = 6,  /**< macOS 12.3+: ScreenCaptureKit         */
} SnapxBackendType;

/** @brief Capture scope / mode. */
typedef enum {
    SNAPX_CAPTURE_FULLSCREEN    = 0, /**< All monitors combined                 */
    SNAPX_CAPTURE_MONITOR       = 1, /**< Single monitor by index               */
    SNAPX_CAPTURE_REGION        = 2, /**< User-selected rectangular region      */
    SNAPX_CAPTURE_WINDOW        = 3, /**< User-selected window                  */
    SNAPX_CAPTURE_ACTIVE_WINDOW = 4, /**< Currently focused window              */
} SnapxCaptureMode;

/** @brief Output file format. */
typedef enum {
    SNAPX_FORMAT_PNG  = 0,
    SNAPX_FORMAT_JPEG = 1,
    SNAPX_FORMAT_WEBP = 2,
} SnapxOutputFormat;

/* ─── Image container ────────────────────────────────────────────────────── */

/**
 * @brief Raw RGBA pixel buffer produced by a capture backend.
 *
 * Pixel layout: R G B A, 1 byte per channel, row-major.
 * stride = width * 4  (no padding required, but backends may set larger strides).
 */
struct SnapxImage {
    uint8_t  *data;    /**< Heap-allocated pixel data (RGBA, 8bpc)              */
    int       width;   /**< Image width in pixels                               */
    int       height;  /**< Image height in pixels                              */
    int       stride;  /**< Row stride in bytes (>= width * 4)                  */
    int       scale;   /**< HiDPI scale factor (1 = normal, 2 = retina, …)      */
};

/**
 * @brief Allocate a new SnapxImage with zeroed pixel buffer.
 * @param width   Image width.
 * @param height  Image height.
 * @return Pointer to allocated image, or NULL on failure.
 */
SnapxImage *snapx_image_alloc(int width, int height);

/**
 * @brief Free a SnapxImage and its pixel data.
 */
void snapx_image_free(SnapxImage *img);

/**
 * @brief Save image to disk.
 * @param img      Source image.
 * @param path     Destination file path.
 * @param format   Output format.
 * @param quality  JPEG quality (1–100); ignored for PNG/WebP.
 * @return 0 on success.
 */
int snapx_image_save(const SnapxImage *img, const char *path,
                     SnapxOutputFormat format, int quality);

/**
 * @brief Copy image to system clipboard.
 */
void snapx_clipboard_copy_image(const SnapxImage *img);

/* ─── Monitor info ───────────────────────────────────────────────────────── */

/**
 * @brief Describes a single physical monitor.
 */
struct SnapxMonitorInfo {
    int  index;          /**< 0-based monitor index                             */
    int  x, y;           /**< Position relative to the virtual desktop origin   */
    int  width, height;  /**< Resolution in physical pixels                     */
    int  scale;          /**< HiDPI scale factor                                */
    char name[128];      /**< Human-readable monitor name / connector           */
    int  is_primary;     /**< Non-zero if this is the primary monitor           */
};

/* ─── Capture request ────────────────────────────────────────────────────── */

/**
 * @brief Parameters for a single capture operation.
 */
struct SnapxCaptureRequest {
    SnapxCaptureMode mode;
    int  delay_sec;       /**< Seconds to wait before capturing                 */
    int  monitor_index;   /**< For SNAPX_CAPTURE_MONITOR: which monitor         */
    int  include_cursor;  /**< Non-zero to composite the cursor into image      */

    /* For SNAPX_CAPTURE_REGION: rectangle set by overlay UI                   */
    int  region_x, region_y;
    int  region_w, region_h;
};

/* ─── Backend vtable ─────────────────────────────────────────────────────── */

/**
 * @brief Platform capture backend.
 *
 * Backends fill the function pointers during init; callers must not NULL-check
 * individual pointers — either all are set or init fails.
 */
struct SnapxCaptureBackend {
    SnapxBackendType type;
    void            *priv;  /**< Backend-private state                         */

    /** Capture the screen according to @p req. Returns NULL on failure. */
    SnapxImage *(*capture)(SnapxCaptureBackend *backend,
                            const SnapxCaptureRequest *req);

    /** Enumerate connected monitors into @p out_monitors (caller-allocated).
     *  @return Number of monitors written, or -1 on error. */
    int (*get_monitors)(SnapxCaptureBackend *backend,
                        SnapxMonitorInfo *out_monitors, int max_monitors);

    /** Release all backend resources. */
    void (*destroy)(SnapxCaptureBackend *backend);
};

/* ─── Public API ─────────────────────────────────────────────────────────── */

/**
 * @brief Initialise a capture backend.
 * @param backend  Output backend structure to fill.
 * @param type     Requested backend type.
 * @return 0 on success, non-zero on failure.
 */
int  snapx_capture_backend_init(SnapxCaptureBackend *backend, SnapxBackendType type);

/* Forward reference to SnapxPlatformInfo (defined in platform.h) */
struct SnapxPlatformInfo;
int  snapx_capture_backend_init_best(SnapxCaptureBackend *backend,
                                      const struct SnapxPlatformInfo *info);

/**
 * @brief Perform a capture.  Wraps @c backend->capture with delay handling.
 * @return Newly allocated SnapxImage, or NULL on failure.
 */
SnapxImage *snapx_capture(SnapxCaptureBackend *backend,
                           const SnapxCaptureRequest *req);

/**
 * @brief Enumerate monitors via the active backend.
 * @return Number of monitors, or -1 on error.
 */
int snapx_get_monitors(SnapxCaptureBackend *backend,
                        SnapxMonitorInfo *out, int max);

/**
 * @brief Release the backend and its resources.
 */
void snapx_capture_backend_destroy(SnapxCaptureBackend *backend);

/**
 * @brief Crop a region out of an existing image.
 *
 * Returns a newly allocated SnapxImage with only the specified rectangle.
 * Coordinates are clamped to the source image boundaries.
 * Returns NULL if the resulting crop would be empty or on allocation failure.
 */
SnapxImage *snapx_image_crop(const SnapxImage *src,
                               int x, int y, int w, int h);

/**
 * @brief Crop a region given in virtual-desktop (logical) coordinates.
 *
 * Maps @p region_* through monitor layout bounds to pixel coordinates on
 * @p src (e.g. a full-desktop PipeWire frame).  Use after region overlay.
 */
SnapxImage *snapx_image_crop_desktop(const SnapxImage *src,
                                      int region_x, int region_y,
                                      int region_w, int region_h,
                                      const SnapxMonitorInfo *mons,
                                      int n_monitors);

/* ─── Backend init declarations (called by snapx_capture_backend_init) ───── */
int snapx_capture_x11_init(SnapxCaptureBackend *backend);
int  snapx_capture_wayland_init(SnapxCaptureBackend *backend);
void snapx_capture_wayland_set_parent_window(SnapxCaptureBackend *backend,
                                              const char *parent_window_str);
/** 0 = Screenshot portal first (default), 1 = ScreenCast + PipeWire first */
void snapx_capture_wayland_set_capture_prefer(SnapxCaptureBackend *backend,
                                               int prefer_screencast);
void snapx_capture_wayland_save_token(SnapxCaptureBackend *backend);

#ifndef SNAPX_HEADLESS
typedef void (*SnapxCaptureDoneFn)(SnapxImage *img, void *user_data);
/** Run snapx_capture on a worker thread; @p done runs on the GTK main thread. */
void snapx_capture_async(SnapxCaptureBackend *backend,
                          const SnapxCaptureRequest *req,
                          SnapxCaptureDoneFn done, void *user_data);
#endif
int snapx_capture_windows_init(SnapxCaptureBackend *backend);
int snapx_capture_macos_init(SnapxCaptureBackend *backend);

#endif /* SNAPX_CAPTURE_H */

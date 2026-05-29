/**
 * @file capture_macos.c
 * @brief macOS capture backend: CoreGraphics (10.13+) and ScreenCaptureKit (12.3+).
 *
 * Runtime version detection:
 *   - macOS < 12.3  → CGWindowListCreateImage via CoreGraphics
 *   - macOS ≥ 12.3  → SCScreenshotManager (ScreenCaptureKit) when compiled with SCK
 *
 * This file uses Objective-C runtime calls via the C interface to avoid
 * requiring a full .m compilation unit in the plain-C build.
 * Where Obj-C types are needed (e.g. SCStreamConfiguration) we use
 * conditionally compiled @import blocks guarded by __OBJC__.
 *
 * Clipboard and global hotkeys are handled in output/clipboard.c and
 * utils/hotkey.c respectively using NSPasteboard / CGEventTap.
 */

#ifdef SNAPX_PLATFORM_MACOS

#include "capture.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* CoreGraphics / CoreFoundation */
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

/* IOKit for display names */
#include <IOKit/graphics/IOGraphicsLib.h>

#ifdef SNAPX_HAVE_SCK
#  import <ScreenCaptureKit/ScreenCaptureKit.h>
#endif

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

/* ─── Version helpers ────────────────────────────────────────────────────── */

typedef struct { int major, minor, patch; } OsVersion;

static OsVersion get_macos_version(void)
{
    OsVersion v = {0};
    /* Use gestalt / NSProcessInfo via CF to stay C-friendly */
    SInt32 major = 0, minor = 0, patch = 0;
    /* Gestalt is deprecated but still works through 12.x; use sysctlbyname fallback */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    Gestalt(gestaltSystemVersionMajor, &major);
    Gestalt(gestaltSystemVersionMinor, &minor);
    Gestalt(gestaltSystemVersionBugFix, &patch);
#pragma clang diagnostic pop
    v.major = (int)major; v.minor = (int)minor; v.patch = (int)patch;
    return v;
}

static int macos_version_gte(int major, int minor, int patch)
{
    OsVersion v = get_macos_version();
    if (v.major != major) return v.major > major;
    if (v.minor != minor) return v.minor > minor;
    return v.patch >= patch;
}

/* ─── Private state ──────────────────────────────────────────────────────── */

typedef struct {
    int use_sck;  /**< Use ScreenCaptureKit (macOS 12.3+)  */
} MacosState;

/* ─── Monitor enumeration ────────────────────────────────────────────────── */

static int macos_get_monitors(SnapxCaptureBackend *backend,
                               SnapxMonitorInfo *out, int max)
{
    (void)backend;
    uint32_t display_count = 0;
    CGGetActiveDisplayList(0, NULL, &display_count);
    if (display_count == 0) return 0;

    CGDirectDisplayID *displays = calloc(display_count, sizeof(CGDirectDisplayID));
    if (!displays) return -1;
    CGGetActiveDisplayList(display_count, displays, &display_count);

    int count = 0;
    for (uint32_t i = 0; i < display_count && count < max; i++) {
        CGDirectDisplayID dpy = displays[i];
        CGRect bounds = CGDisplayBounds(dpy);

        SnapxMonitorInfo *m = &out[count];
        m->index     = count;
        m->x         = (int)bounds.origin.x;
        m->y         = (int)bounds.origin.y;
        m->width     = (int)bounds.size.width;
        m->height    = (int)bounds.size.height;
        m->scale     = (int)CGDisplayScreenSize(dpy).width > 0 ? 1 : 2;
        m->is_primary = CGDisplayIsMain(dpy) ? 1 : 0;

        /* Try to get a human-readable name via IOKit */
        io_service_t service = CGDisplayIOServicePort(dpy);
        CFDictionaryRef info  = IODisplayCreateInfoDictionary(service, kIODisplayOnlyPreferredName);
        if (info) {
            CFDictionaryRef names = CFDictionaryGetValue(info, CFSTR(kDisplayProductName));
            if (names) {
                CFStringRef name_ref = NULL;
                CFDictionaryGetKeysAndValues(names, NULL, (const void **)&name_ref);
                if (name_ref)
                    CFStringGetCString(name_ref, m->name, sizeof(m->name),
                                       kCFStringEncodingUTF8);
            }
            CFRelease(info);
        }
        if (m->name[0] == '\0')
            snprintf(m->name, sizeof(m->name), "Display %u", i);

        count++;
    }
    free(displays);
    return count;
}

/* ─── CoreGraphics capture ───────────────────────────────────────────────── */

/**
 * @brief Convert a CGImage to SnapxImage RGBA buffer.
 */
static SnapxImage *cgimage_to_snapx(CGImageRef cg_img)
{
    if (!cg_img) return NULL;
    size_t w = CGImageGetWidth(cg_img);
    size_t h = CGImageGetHeight(cg_img);
    if (w == 0 || h == 0) return NULL;

    SnapxImage *img = snapx_image_alloc((int)w, (int)h);
    if (!img) return NULL;

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx   = CGBitmapContextCreate(
        img->data, w, h, 8, (size_t)img->stride, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);

    if (!ctx) { snapx_image_free(img); return NULL; }

    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), cg_img);
    CGContextRelease(ctx);

    /* Un-premultiply alpha */
    uint8_t *p = img->data;
    size_t total = (size_t)(img->stride * img->height);
    for (size_t i = 0; i < total; i += 4) {
        uint8_t a = p[i + 3];
        if (a > 0 && a < 255) {
            p[i + 0] = (uint8_t)((p[i + 0] * 255u) / a);
            p[i + 1] = (uint8_t)((p[i + 1] * 255u) / a);
            p[i + 2] = (uint8_t)((p[i + 2] * 255u) / a);
        }
    }
    return img;
}

static SnapxImage *cg_capture_display(CGDirectDisplayID display_id)
{
    CGImageRef cg_img = CGDisplayCreateImage(display_id);
    SnapxImage *img = cgimage_to_snapx(cg_img);
    if (cg_img) CGImageRelease(cg_img);
    return img;
}

static SnapxImage *cg_capture_rect(CGRect rect)
{
    CGImageRef cg_img = CGWindowListCreateImage(
        rect, kCGWindowListOptionOnScreenOnly,
        kCGNullWindowID, kCGWindowImageDefault);
    SnapxImage *img = cgimage_to_snapx(cg_img);
    if (cg_img) CGImageRelease(cg_img);
    return img;
}

static SnapxImage *cg_capture_active_window(void)
{
    /* Get frontmost window from CGWindowList */
    CFArrayRef window_list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!window_list) return NULL;

    CGWindowID front_id = kCGNullWindowID;
    CFIndex count = CFArrayGetCount(window_list);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(window_list, i);
        CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
        CFNumberRef owner_pid = CFDictionaryGetValue(info, kCGWindowOwnerPID);
        int layer = 0;
        if (layer_ref) CFNumberGetValue(layer_ref, kCFNumberIntType, &layer);
        if (layer == 0 && owner_pid) {
            CFNumberRef wid_ref = CFDictionaryGetValue(info, kCGWindowNumber);
            if (wid_ref) {
                uint32_t wid = 0;
                CFNumberGetValue(wid_ref, kCFNumberSInt32Type, &wid);
                front_id = (CGWindowID)wid;
                break;
            }
        }
    }
    CFRelease(window_list);

    if (front_id == kCGNullWindowID) return NULL;
    CGImageRef cg_img = CGWindowListCreateImageFromArray(
        CGRectNull, (__bridge CFArrayRef)@[@(front_id)],
        kCGWindowImageBoundsIgnoreFraming);
    SnapxImage *img = cgimage_to_snapx(cg_img);
    if (cg_img) CGImageRelease(cg_img);
    return img;
}

/* ─── ScreenCaptureKit path (macOS 12.3+) ────────────────────────────────── */

#ifdef SNAPX_HAVE_SCK

/**
 * @brief Synchronous screenshot via SCScreenshotManager (macOS 13+).
 *        For 12.3 – 12.x we use SCStream which requires an async loop.
 *        This implementation uses the simpler iOS-style screenshotOfDisplay API
 *        available on macOS 14+; for 12.3/13 it falls through to CoreGraphics.
 */
static SnapxImage *sck_capture_display(CGDirectDisplayID display_id)
{
    if (@available(macOS 14.0, *)) {
        __block CGImageRef captured = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        SCShareableContent *content = nil;
        __block NSError *err = nil;
        dispatch_semaphore_t content_sem = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:
            ^(SCShareableContent *c, NSError *e) {
            content = c;
            err = e;
            dispatch_semaphore_signal(content_sem);
        }];
        dispatch_semaphore_wait(content_sem,
            dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        if (!content) {
            fprintf(stderr, "[macos/sck] SCShareableContent failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "unknown");
            return NULL;
        }

        SCDisplay *target_display = nil;
        for (SCDisplay *d in content.displays) {
            if (d.displayID == display_id) { target_display = d; break; }
        }
        if (!target_display) return NULL;

        SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
        cfg.width  = (size_t)CGDisplayPixelsWide(display_id);
        cfg.height = (size_t)CGDisplayPixelsHigh(display_id);
        cfg.pixelFormat = kCVPixelFormatType_32BGRA;
        cfg.showsCursor = YES;

        SCContentFilter *filter = [[SCContentFilter alloc]
            initWithDisplay:target_display excludingWindows:@[]];

        [SCScreenshotManager captureImageWithFilter:filter
                                      configuration:cfg
                                  completionHandler:^(CGImageRef img, NSError *e) {
            if (img) CGImageRetain(img);
            captured = img;
            (void)e;
            dispatch_semaphore_signal(sem);
        }];

        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));

        if (!captured) return NULL;
        SnapxImage *result = cgimage_to_snapx(captured);
        CGImageRelease(captured);
        return result;
    } else {
        /* Fall through to CoreGraphics */
        return cg_capture_display(display_id);
    }
}

#endif /* SNAPX_HAVE_SCK */

/* ─── Backend capture dispatch ───────────────────────────────────────────── */

static SnapxImage *macos_capture(SnapxCaptureBackend *backend,
                                  const SnapxCaptureRequest *req)
{
    MacosState *st = (MacosState *)backend->priv;

    /* Retrieve display list */
    uint32_t display_count = 0;
    CGGetActiveDisplayList(0, NULL, &display_count);
    CGDirectDisplayID *displays = calloc(display_count, sizeof(CGDirectDisplayID));
    if (!displays) return NULL;
    CGGetActiveDisplayList(display_count, displays, &display_count);

    SnapxImage *img = NULL;

    switch (req->mode) {
        case SNAPX_CAPTURE_FULLSCREEN: {
            /* Combine all displays into the main display's full bounds */
            CGDirectDisplayID main_dpy = CGMainDisplayID();
#ifdef SNAPX_HAVE_SCK
            if (st->use_sck)
                img = sck_capture_display(main_dpy);
            else
#endif
                img = cg_capture_rect(CGDisplayBounds(main_dpy));
            break;
        }
        case SNAPX_CAPTURE_MONITOR: {
            int idx = (req->monitor_index < 0) ? 0 : req->monitor_index;
            if ((uint32_t)idx >= display_count) { free(displays); return NULL; }
            CGDirectDisplayID dpy = displays[idx];
#ifdef SNAPX_HAVE_SCK
            if (st->use_sck)
                img = sck_capture_display(dpy);
            else
#endif
                img = cg_capture_display(dpy);
            break;
        }
        case SNAPX_CAPTURE_REGION: {
            CGRect r = CGRectMake((CGFloat)req->region_x, (CGFloat)req->region_y,
                                  (CGFloat)req->region_w, (CGFloat)req->region_h);
            img = cg_capture_rect(r);
            break;
        }
        case SNAPX_CAPTURE_ACTIVE_WINDOW:
        case SNAPX_CAPTURE_WINDOW:
            img = cg_capture_active_window();
            break;
        default:
            break;
    }

    free(displays);
    (void)st;
    return img;
}

/* ─── Destroy ────────────────────────────────────────────────────────────── */

static void macos_destroy(SnapxCaptureBackend *backend)
{
    free(backend->priv);
    backend->priv = NULL;
}

/* ─── Init ───────────────────────────────────────────────────────────────── */

int snapx_capture_macos_init(SnapxCaptureBackend *backend)
{
    MacosState *st = calloc(1, sizeof(MacosState));
    if (!st) return -1;

#ifdef SNAPX_HAVE_SCK
    /* ScreenCaptureKit available from macOS 12.3 */
    if (macos_version_gte(12, 3, 0))
        st->use_sck = 1;
#endif

    /* Check / request screen recording permission */
    if (CGPreflightScreenCaptureAccess() == NO) {
        fprintf(stderr, "[macos] Requesting screen recording permission...\n");
        CGRequestScreenCaptureAccess();
        /* App must be restarted after permission grant */
        if (CGPreflightScreenCaptureAccess() == NO) {
            fprintf(stderr, "[macos] Screen recording permission denied. "
                            "Grant it in System Preferences → Privacy → Screen Recording.\n");
            free(st); return -1;
        }
    }

    backend->priv         = st;
    backend->capture      = macos_capture;
    backend->get_monitors = macos_get_monitors;
    backend->destroy      = macos_destroy;
    backend->type         = st->use_sck ? SNAPX_BACKEND_MACOS_SCK
                                        : SNAPX_BACKEND_MACOS_CG;

    OsVersion v = get_macos_version();
    fprintf(stderr, "[macos] Backend initialised (macOS %d.%d.%d, %s)\n",
            v.major, v.minor, v.patch, st->use_sck ? "ScreenCaptureKit" : "CoreGraphics");
    return 0;
}

#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

#endif /* SNAPX_PLATFORM_MACOS */

/**
 * @file capture_x11.c
 * @brief X11/Xorg capture backend using libX11 + libXrandr.
 *
 * Capture flow:
 *   1. Open default Display.
 *   2. For FULLSCREEN: walk XRandR CRTCs to build virtual desktop bounds.
 *   3. For MONITOR: capture a single CRTC's viewport.
 *   4. For REGION: capture the user-supplied rect from the root window.
 *   5. For ACTIVE_WINDOW/WINDOW: query _NET_ACTIVE_WINDOW property.
 *   6. XGetImage → convert XImage (BGR/BGRA) → SnapxImage (RGBA).
 *   7. Optionally composite cursor via XFixes.
 */

#ifdef SNAPX_HAVE_X11

#include "capture.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>

/* Suppress X protocol errors during XGetImage (Xwayland returns BadMatch). */
static int g_x11_had_error = 0;
static int x11_error_suppress(Display *d, XErrorEvent *e)
{
    (void)d; (void)e;
    g_x11_had_error = 1;
    return 0;
}

#ifdef SNAPX_HAVE_XFIXES
#  include <X11/extensions/Xfixes.h>
#endif

/* ─── Private state ──────────────────────────────────────────────────────── */

typedef struct {
    Display *display;
    int      screen;
    Window   root;
    int      xrandr_event_base;
    int      xrandr_error_base;
    int      has_xfixes;
} X11State;

/* ─── Helpers ────────────────────────────────────────────────────────────── */

/**
 * @brief Convert an XImage (platform-native byte order) to RGBA SnapxImage.
 */
static SnapxImage *ximage_to_snapx(XImage *xi, int ox, int oy, int w, int h)
{
    SnapxImage *img = snapx_image_alloc(w, h);
    if (!img) return NULL;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            unsigned long pixel = XGetPixel(xi, col, row);
            uint8_t r = (uint8_t)((pixel & xi->red_mask)   >> __builtin_ctzl(xi->red_mask));
            uint8_t g = (uint8_t)((pixel & xi->green_mask) >> __builtin_ctzl(xi->green_mask));
            uint8_t b = (uint8_t)((pixel & xi->blue_mask)  >> __builtin_ctzl(xi->blue_mask));
            uint8_t *dst = img->data + row * img->stride + col * 4;
            dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = 0xFF;
        }
    }
    (void)ox; (void)oy;
    return img;
}

/**
 * @brief Composite cursor on top of image if XFixes is available.
 */
static void composite_cursor(X11State *st, SnapxImage *img, int sx, int sy)
{
#ifdef SNAPX_HAVE_XFIXES
    if (!st->has_xfixes) return;

    XFixesCursorImage *ci = XFixesGetCursorImage(st->display);
    if (!ci) return;

    /* ci->x,y is the cursor hot-spot position in root coords */
    int cx = (int)ci->x - ci->xhot - sx;
    int cy = (int)ci->y - ci->yhot - sy;

    for (unsigned int row = 0; row < ci->height; row++) {
        for (unsigned int col = 0; col < ci->width; col++) {
            int dx = cx + (int)col;
            int dy = cy + (int)row;
            if (dx < 0 || dy < 0 || dx >= img->width || dy >= img->height) continue;
            unsigned long px = ci->pixels[row * ci->width + col];
            uint8_t ca = (uint8_t)((px >> 24) & 0xFF);
            uint8_t cr = (uint8_t)((px >> 16) & 0xFF);
            uint8_t cg = (uint8_t)((px >>  8) & 0xFF);
            uint8_t cb = (uint8_t)( px         & 0xFF);
            uint8_t *dst = img->data + dy * img->stride + dx * 4;
            float a = ca / 255.0f;
            dst[0] = (uint8_t)(cr * a + dst[0] * (1.0f - a));
            dst[1] = (uint8_t)(cg * a + dst[1] * (1.0f - a));
            dst[2] = (uint8_t)(cb * a + dst[2] * (1.0f - a));
            dst[3] = 0xFF;
        }
    }
    XFree(ci);
#else
    (void)st; (void)img; (void)sx; (void)sy;
#endif
}

/**
 * @brief Get the currently focused window via _NET_ACTIVE_WINDOW.
 */
static Window get_active_window(Display *dpy, Window root)
{
    Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    if (net_active == None) return root;

    Atom actual_type;
    int  actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    Window active = root;

    if (XGetWindowProperty(dpy, root, net_active, 0, 1, False,
                           XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        active = *(Window *)prop;
        XFree(prop);
    }
    return active;
}

/* ─── Backend capture function ───────────────────────────────────────────── */

static SnapxImage *x11_capture(SnapxCaptureBackend *backend,
                                const SnapxCaptureRequest *req)
{
    X11State *st = (X11State *)backend->priv;
    int sx = 0, sy = 0, sw = 0, sh = 0;

    /* Determine capture rectangle */
    switch (req->mode) {
        case SNAPX_CAPTURE_FULLSCREEN: {
            /* Virtual desktop spans all CRTCs */
            XWindowAttributes ra;
            XGetWindowAttributes(st->display, st->root, &ra);
            sx = 0; sy = 0; sw = ra.width; sh = ra.height;
            break;
        }
        case SNAPX_CAPTURE_MONITOR: {
            /* Use XRandR to find the specific CRTC */
            XRRScreenResources *res = XRRGetScreenResources(st->display, st->root);
            if (!res) return NULL;

            int target = (req->monitor_index < 0) ? 0 : req->monitor_index;
            int found  = 0;
            for (int i = 0; i < res->noutput; i++) {
                XRROutputInfo *oi = XRRGetOutputInfo(st->display, res, res->outputs[i]);
                if (!oi) continue;
                if (oi->connection != RR_Connected || oi->crtc == None) {
                    XRRFreeOutputInfo(oi); continue;
                }
                if (found == target) {
                    XRRCrtcInfo *ci = XRRGetCrtcInfo(st->display, res, oi->crtc);
                    if (ci) {
                        sx = (int)ci->x; sy = (int)ci->y;
                        sw = (int)ci->width; sh = (int)ci->height;
                        XRRFreeCrtcInfo(ci);
                        XRRFreeOutputInfo(oi);
                        break;
                    }
                }
                found++;
                XRRFreeOutputInfo(oi);
            }
            XRRFreeScreenResources(res);
            if (!sw || !sh) {
                fprintf(stderr, "[x11] Monitor index %d not found.\n", target);
                return NULL;
            }
            break;
        }
        case SNAPX_CAPTURE_REGION:
            sx = req->region_x; sy = req->region_y;
            sw = req->region_w; sh = req->region_h;
            if (sw <= 0 || sh <= 0) return NULL;
            break;
        case SNAPX_CAPTURE_ACTIVE_WINDOW:
        case SNAPX_CAPTURE_WINDOW: {
            Window target_win = get_active_window(st->display, st->root);
            XWindowAttributes wa;
            if (!XGetWindowAttributes(st->display, target_win, &wa)) return NULL;
            /* Translate to root coordinates */
            Window child;
            XTranslateCoordinates(st->display, target_win, st->root,
                                  0, 0, &sx, &sy, &child);
            sw = wa.width; sh = wa.height;
            break;
        }
        default:
            return NULL;
    }

    g_x11_had_error = 0;
    int (*prev_err)(Display *, XErrorEvent *) = XSetErrorHandler(x11_error_suppress);
    XImage *xi = XGetImage(st->display, st->root, sx, sy, (unsigned)sw, (unsigned)sh,
                           AllPlanes, ZPixmap);
    XSync(st->display, False);
    XSetErrorHandler(prev_err);
    if (!xi || g_x11_had_error) {
        if (xi) XDestroyImage(xi);
        fprintf(stderr, "[x11] XGetImage failed — Xwayland does not expose root pixels.\n");
        return NULL;
    }

    SnapxImage *img = ximage_to_snapx(xi, sx, sy, sw, sh);
    XDestroyImage(xi);

    if (img && req->include_cursor)
        composite_cursor(st, img, sx, sy);

    return img;
}

/* ─── Monitor enumeration ────────────────────────────────────────────────── */

static int x11_get_monitors(SnapxCaptureBackend *backend,
                             SnapxMonitorInfo *out, int max)
{
    X11State *st = (X11State *)backend->priv;
    XRRScreenResources *res = XRRGetScreenResources(st->display, st->root);
    if (!res) return -1;

    int count = 0;
    for (int i = 0; i < res->noutput && count < max; i++) {
        XRROutputInfo *oi = XRRGetOutputInfo(st->display, res, res->outputs[i]);
        if (!oi) continue;
        if (oi->connection != RR_Connected || oi->crtc == None) {
            XRRFreeOutputInfo(oi); continue;
        }
        XRRCrtcInfo *ci = XRRGetCrtcInfo(st->display, res, oi->crtc);
        if (!ci) { XRRFreeOutputInfo(oi); continue; }

        SnapxMonitorInfo *m = &out[count];
        m->index  = count;
        m->x      = (int)ci->x; m->y = (int)ci->y;
        m->width  = (int)ci->width; m->height = (int)ci->height;
        m->scale  = 1;
        snprintf(m->name, sizeof(m->name), "%s", oi->name);
        m->is_primary = (i == 0) ? 1 : 0;

        XRRFreeCrtcInfo(ci);
        XRRFreeOutputInfo(oi);
        count++;
    }
    XRRFreeScreenResources(res);
    return count;
}

/* ─── Destroy ────────────────────────────────────────────────────────────── */

static void x11_destroy(SnapxCaptureBackend *backend)
{
    X11State *st = (X11State *)backend->priv;
    if (st) {
        if (st->display)
            XCloseDisplay(st->display);
        free(st);
    }
    backend->priv = NULL;
}

/* ─── Init ───────────────────────────────────────────────────────────────── */

int snapx_capture_x11_init(SnapxCaptureBackend *backend)
{
    X11State *st = calloc(1, sizeof(X11State));
    if (!st) return -1;

    st->display = XOpenDisplay(NULL);
    if (!st->display) {
        fprintf(stderr, "[x11] XOpenDisplay failed. Is $DISPLAY set?\n");
        free(st); return -1;
    }

    st->screen = DefaultScreen(st->display);
    st->root   = RootWindow(st->display, st->screen);

    int dummy;
    if (!XRRQueryExtension(st->display, &st->xrandr_event_base, &st->xrandr_error_base)) {
        fprintf(stderr, "[x11] XRandR extension not available.\n");
        XCloseDisplay(st->display); free(st); return -1;
    }

#ifdef SNAPX_HAVE_XFIXES
    if (XFixesQueryExtension(st->display, &dummy, &dummy))
        st->has_xfixes = 1;
#else
    (void)dummy;
#endif

    backend->priv        = st;
    backend->capture     = x11_capture;
    backend->get_monitors = x11_get_monitors;
    backend->destroy     = x11_destroy;
    backend->type        = SNAPX_BACKEND_X11;

    fprintf(stderr, "[x11] Backend initialised (display=%s, xfixes=%d)\n",
            DisplayString(st->display), st->has_xfixes);
    return 0;
}

int snapx_x11_list_windows(SnapxWindowInfo *out, int max_out)
{
    if (!out || max_out <= 0) return 0;

    Display *d = XOpenDisplay(NULL);
    if (!d) return 0;

    Window root = DefaultRootWindow(d);
    Atom clients = XInternAtom(d, "_NET_CLIENT_LIST", False);
    Atom frame   = XInternAtom(d, "_NET_FRAME_EXTENTS", False);
    Atom name    = XInternAtom(d, "_NET_WM_NAME", False);
    Atom utf8    = XInternAtom(d, "UTF8_STRING", False);

    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    int count = 0;

    if (XGetWindowProperty(d, root, clients, 0, 1024, False, XA_WINDOW,
                           &type, &fmt, &n, &after, &data) != Success || !data) {
        if (data) XFree(data);
        XCloseDisplay(d);
        return 0;
    }

    Window *wins = (Window *)data;
    for (unsigned long i = 0; i < n && count < max_out; i++) {
        long left = 0, right = 0, top = 0, bottom = 0;
        unsigned char *ext = NULL;
        unsigned long en = 0, eafter = 0;
        if (XGetWindowProperty(d, wins[i], frame, 0, 4, False, XA_CARDINAL,
                               &type, &fmt, &en, &eafter, &ext) == Success && ext) {
            long *v = (long *)ext;
            left = v[0]; right = v[1]; top = v[2]; bottom = v[3];
            XFree(ext);
        }
        XWindowAttributes wa;
        if (!XGetWindowAttributes(d, wins[i], &wa) || wa.map_state != IsViewable)
            continue;

        out[count].x = wa.x - (int)left;
        out[count].y = wa.y - (int)top;
        out[count].w = wa.width + (int)(left + right);
        out[count].h = wa.height + (int)(top + bottom);
        out[count].title[0] = '\0';

        unsigned char *nm = NULL;
        unsigned long nn = 0, nafter = 0;
        if (XGetWindowProperty(d, wins[i], name, 0, 256, False, utf8,
                               &type, &fmt, &nn, &nafter, &nm) == Success && nm) {
            snprintf(out[count].title, sizeof(out[count].title), "%s", (char *)nm);
            XFree(nm);
        }
        count++;
    }
    XFree(data);
    XCloseDisplay(d);
    return count;
}

#endif /* SNAPX_HAVE_X11 */

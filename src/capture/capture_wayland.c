/**
 * @file capture_wayland.c
 * @brief Wayland capture backend via XDG ScreenCast portal + PipeWire.
 *
 * Primary path (used always):
 *   org.freedesktop.portal.ScreenCast  →  PipeWire raw-video stream
 *   We capture one frame ourselves — no GNOME screenshot UI involved.
 *   First call: system "what to share" picker.
 *   Subsequent calls with a restore_token: completely silent (no dialog).
 *
 * Fallback:
 *   org.freedesktop.portal.Screenshot  (legacy, shows GNOME flash UI)
 *   Only used if ScreenCast/PipeWire fails.
 */

#ifdef SNAPX_HAVE_WAYLAND

#include "capture.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

/* GIO / DBus */
#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>

/* GDK for monitor enumeration */
#if (defined(SNAPX_USE_GTK4) || defined(SNAPX_USE_GTK3)) && !defined(SNAPX_HEADLESS)
#  include <gtk/gtk.h>
#  include <gdk/gdk.h>
#endif

/* gdk-pixbuf for the Screenshot-portal fallback path */
#ifdef SNAPX_HAVE_GDK_PIXBUF
#  include <gdk-pixbuf/gdk-pixbuf.h>
#endif
#ifndef SNAPX_HAVE_GDK_PIXBUF
#  include <png.h>
#endif

/* PipeWire (for ScreenCast path) */
#ifdef SNAPX_HAVE_PIPEWIRE
/* spa/utils/string.h requires POSIX locale types */
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#  include <pipewire/pipewire.h>
#  include <spa/param/video/format-utils.h>
#  include <spa/param/video/type-info.h>
#  include <spa/debug/types.h>
#endif

/* ─── Portal constants ───────────────────────────────────────────────────── */

#define PORTAL_BUS        "org.freedesktop.portal.Desktop"
#define PORTAL_PATH       "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE_SC   "org.freedesktop.portal.ScreenCast"
#define PORTAL_IFACE_SS   "org.freedesktop.portal.Screenshot"
#define PORTAL_IFACE_REQ  "org.freedesktop.portal.Request"
#define PORTAL_IFACE_SES  "org.freedesktop.portal.Session"

/* Persist mode 2 = persistent across sessions */
#define SC_PERSIST_TRANSIENT   0
#define SC_PERSIST_SESSION     1
#define SC_PERSIST_PERMANENT   2

/* Source type flags: 1=Monitor, 2=Window, 4=Virtual */
#define SC_SOURCE_MONITOR  1
#define SC_SOURCE_WINDOW   2

/* ─── Private state ──────────────────────────────────────────────────────── */

typedef struct {
    GDBusConnection *dbus;
    char             sender_token[64];
    guint32          request_counter;

    /* ScreenCast restore token — survives across captures, loaded/saved by caller */
    char             restore_token[256];

    /* Parent window handle string ("x11:0x..." or "") */
    char             parent_window[128];
} WaylandState;

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static void sanitise_sender(const char *sender, char *out, size_t outsz)
{
    size_t j = 0;
    /* The portal strips the leading ':' and replaces '.' with '_'.
     * e.g. ":1.440" → "1_440"  (NOT "_1_440") */
    size_t i = (sender[0] == ':') ? 1 : 0;
    for (; sender[i] && j + 1 < outsz; i++) {
        char c = sender[i];
        out[j++] = (c == '.') ? '_' : c;
    }
    out[j] = '\0';
}

static const char *next_handle_token(WaylandState *st, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "snapx_%u", ++st->request_counter);
    return buf;
}

static void build_request_path(WaylandState *st, const char *token,
                                char *path, size_t pathsz)
{
    snprintf(path, pathsz,
             "/org/freedesktop/portal/desktop/request/%s/%s",
             st->sender_token, token);
}

/* ─── Generic portal request/response helper ─────────────────────────────── */

typedef struct {
    GMainLoop   *loop;
    GVariant    *results;   /* owned; NULL on cancel/error */
    gboolean     done;
} PortalResponse;

static void portal_response_cb(GDBusConnection *conn  __attribute__((unused)),
                               const gchar *sender    __attribute__((unused)),
                               const gchar *obj_path  __attribute__((unused)),
                               const gchar *iface     __attribute__((unused)),
                               const gchar *signal    __attribute__((unused)),
                               GVariant    *parameters,
                               gpointer     user_data)
{
    PortalResponse *pr = user_data;
    guint32   response = 2;
    GVariant *results  = NULL;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);
    if (response == 0) {
        pr->results = results;   /* caller must unref */
    } else {
        if (results) g_variant_unref(results);
        fprintf(stderr, "[wayland] Portal request cancelled/failed (response=%u)\n",
                response);
    }
    pr->done = TRUE;
    g_main_loop_quit(pr->loop);
}

/**
 * Call a portal method and wait for its Request.Response signal.
 *
 * @param st         Wayland state (for handle token building)
 * @param iface      Portal interface name
 * @param method     Method name
 * @param params     Parameters for the method (method must end with a{sv})
 * @param timeout_ms Timeout in milliseconds for the Response signal
 * @return GVariant* (a{sv} result dict, caller must unref), or NULL on failure.
 */
static GVariant *portal_call_sync(WaylandState *st,
                                   const char   *iface,
                                   const char   *method,
                                   GVariant     *params,   /* floating ref */
                                   int           timeout_ms)
{
    GError *err = NULL;
    PortalResponse pr = { .loop = g_main_loop_new(NULL, FALSE) };

    /* The last item of params must be an a{sv} options dict.
     * We inject handle_token into it so we know the request object path. */
    char token[64];
    next_handle_token(st, token, sizeof(token));

    char req_path[256];
    build_request_path(st, token, req_path, sizeof(req_path));

    /* Inject handle_token into the options dict (last child of params) */
    GVariantBuilder new_params;
    g_variant_builder_init(&new_params, G_VARIANT_TYPE_TUPLE);
    GVariant *params_sink = g_variant_ref_sink(params);
    gsize n = g_variant_n_children(params_sink);
    for (gsize i = 0; i < n - 1; i++)
        g_variant_builder_add_value(&new_params,
                                    g_variant_get_child_value(params_sink, i));

    /* Last child is the options dict — clone and add handle_token */
    GVariant *last = g_variant_get_child_value(params_sink, n - 1);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    /* copy existing keys */
    GVariantIter iter;
    g_variant_iter_init(&iter, last);
    const gchar *key; GVariant *val;
    while (g_variant_iter_next(&iter, "{sv}", &key, &val)) {
        g_variant_builder_add(&opts, "{sv}", key, val);
        g_variant_unref(val);
    }
    g_variant_unref(last);
    g_variant_builder_add(&opts, "{sv}", "handle_token",
                          g_variant_new_string(token));
    g_variant_builder_add_value(&new_params,
                                 g_variant_builder_end(&opts));
    g_variant_unref(params_sink);

    GVariant *call_params = g_variant_builder_end(&new_params);

    /* Subscribe to the Response signal BEFORE making the call */
    guint sub = g_dbus_connection_signal_subscribe(
        st->dbus, NULL, PORTAL_IFACE_REQ, "Response", req_path, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, portal_response_cb, &pr, NULL);

    GVariant *ret = g_dbus_connection_call_sync(
        st->dbus, PORTAL_BUS, PORTAL_PATH, iface, method,
        call_params,
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!ret) {
        fprintf(stderr, "[wayland] %s.%s failed: %s\n", iface, method,
                err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_dbus_connection_signal_unsubscribe(st->dbus, sub);
        g_main_loop_unref(pr.loop);
        return NULL;
    }
    g_variant_unref(ret);

    /* Run nested loop until Response arrives or timeout */
    GSource *tsrc = g_timeout_source_new(timeout_ms);
    g_source_set_callback(tsrc, G_SOURCE_FUNC(g_main_loop_quit), pr.loop, NULL);
    g_source_attach(tsrc, NULL);
    if (!pr.done) g_main_loop_run(pr.loop);
    g_source_destroy(tsrc);
    g_source_unref(tsrc);

    g_dbus_connection_signal_unsubscribe(st->dbus, sub);
    g_main_loop_unref(pr.loop);
    return pr.results;   /* NULL if cancelled/timeout */
}

/* ─── Screenshot portal fallback ─────────────────────────────────────────── */

static SnapxImage *load_file_uri(const char *uri)
{
    if (!uri || strncmp(uri, "file://", 7) != 0) return NULL;
    const char *path = uri + 7;

#ifdef SNAPX_HAVE_GDK_PIXBUF
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (!pb) {
        if (err) { fprintf(stderr, "[wayland] %s\n", err->message); g_error_free(err); }
        return NULL;
    }
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int src_stride = gdk_pixbuf_get_rowstride(pb);
    int chans = gdk_pixbuf_get_n_channels(pb);
    const guchar *src = gdk_pixbuf_get_pixels(pb);
    SnapxImage *img = snapx_image_alloc(w, h);
    if (!img) { g_object_unref(pb); return NULL; }
    for (int row = 0; row < h; row++) {
        const guchar *s = src + row * src_stride;
        uint8_t *d = img->data + row * img->stride;
        for (int col = 0; col < w; col++) {
            d[col*4+0] = s[col*chans+0];
            d[col*4+1] = s[col*chans+1];
            d[col*4+2] = s[col*chans+2];
            d[col*4+3] = (chans == 4) ? s[col*4+3] : 0xFF;
        }
    }
    g_object_unref(pb);
    return img;
#else
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return NULL; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL;
    }
    png_init_io(png, fp);
    png_read_info(png, info);
    int w = (int)png_get_image_width(png, info);
    int h = (int)png_get_image_height(png, info);
    png_set_expand(png); png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_set_gray_to_rgb(png); png_read_update_info(png, info);
    SnapxImage *img = snapx_image_alloc(w, h);
    if (!img) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    png_bytep *rows = malloc((size_t)h * sizeof(png_bytep));
    if (!rows) { snapx_image_free(img); png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    for (int i = 0; i < h; i++) rows[i] = img->data + i * img->stride;
    png_read_image(png, rows);
    free(rows); png_destroy_read_struct(&png, &info, NULL); fclose(fp);
    return img;
#endif
}

static SnapxImage *portal_screenshot_fallback(WaylandState *st)
{
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "interactive", g_variant_new_boolean(FALSE));

    GVariant *results = portal_call_sync(st, PORTAL_IFACE_SS, "Screenshot",
        g_variant_new("(s@a{sv})", st->parent_window,
                      g_variant_builder_end(&opts)),
        30000);
    if (!results) return NULL;

    const char *uri = NULL;
    GVariant *uri_v = g_variant_lookup_value(results, "uri", G_VARIANT_TYPE_STRING);
    if (uri_v) {
        uri = g_variant_get_string(uri_v, NULL);
        fprintf(stderr,
                "[wayland] Screenshot portal fallback — compositor wrote a file\n");
        SnapxImage *img = load_file_uri(uri);
        if (uri && strncmp(uri, "file://", 7) == 0) {
            GError *err = NULL;
            char *path = g_filename_from_uri(uri, NULL, &err);
            if (path) {
                if (unlink(path) == 0)
                    fprintf(stderr,
                            "[wayland] Removed portal screenshot file: %s\n", path);
                else
                    fprintf(stderr,
                            "[wayland] Could not remove portal file %s: %s\n",
                            path, strerror(errno));
                g_free(path);
            } else if (err) {
                fprintf(stderr, "[wayland] %s\n", err->message);
                g_error_free(err);
            }
        }
        g_variant_unref(uri_v);
        g_variant_unref(results);
        return img;
    }
    g_variant_unref(results);
    return NULL;
}

/* ─── ScreenCast portal + PipeWire ──────────────────────────────────────── */

#ifdef SNAPX_HAVE_PIPEWIRE

/* PipeWire one-shot capture state */
typedef struct {
    struct pw_thread_loop *tloop;
    struct pw_stream      *stream;
    SnapxImage            *result;
    int                    width;
    int                    height;
    enum spa_video_format  fmt;
    gboolean               done;
} PwCapture;

static void pw_on_process(void *data)
{
    PwCapture *pc = data;
    if (pc->done) return;

    struct pw_buffer *pwbuf = pw_stream_dequeue_buffer(pc->stream);
    if (!pwbuf) return;

    struct spa_buffer *sbuf = pwbuf->buffer;
    struct spa_data   *sd   = &sbuf->datas[0];

    if (pc->width <= 0 || pc->height <= 0 || !sd->data) {
        pw_stream_queue_buffer(pc->stream, pwbuf);
        return;
    }

    int w = pc->width, h = pc->height;
    uint32_t src_stride = sd->chunk->stride > 0
                          ? (uint32_t)sd->chunk->stride
                          : (uint32_t)(w * 4);

    SnapxImage *img = snapx_image_alloc(w, h);
    if (img) {
        const uint8_t *src = (const uint8_t *)sd->data;
        for (int y = 0; y < h; y++) {
            const uint8_t *srow = src + (uint32_t)y * src_stride;
            uint8_t       *drow = img->data + y * img->stride;
            if (pc->fmt == SPA_VIDEO_FORMAT_BGRx ||
                pc->fmt == SPA_VIDEO_FORMAT_BGRA) {
                /* BGRA → RGBA swap */
                for (int x = 0; x < w; x++) {
                    drow[x*4+0] = srow[x*4+2]; /* R */
                    drow[x*4+1] = srow[x*4+1]; /* G */
                    drow[x*4+2] = srow[x*4+0]; /* B */
                    drow[x*4+3] = (pc->fmt == SPA_VIDEO_FORMAT_BGRx) ? 0xFF : srow[x*4+3];
                }
            } else {
                /* RGBA/RGBx — copy as-is, force alpha=0xFF for RGBx */
                memcpy(drow, srow, (size_t)w * 4);
                if (pc->fmt == SPA_VIDEO_FORMAT_RGBx) {
                    for (int x = 0; x < w; x++) drow[x*4+3] = 0xFF;
                }
            }
        }
        pc->result = img;
    }

    pw_stream_queue_buffer(pc->stream, pwbuf);
    pc->done = TRUE;
    pw_thread_loop_signal(pc->tloop, FALSE);
}

static void pw_on_param_changed(void *data, uint32_t id,
                                 const struct spa_pod *param)
{
    if (id != SPA_PARAM_Format || !param) return;
    PwCapture *pc = data;

    struct spa_video_info info;
    memset(&info, 0, sizeof(info));
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
    if (info.media_type    != SPA_MEDIA_TYPE_video  ||
        info.media_subtype != SPA_MEDIA_SUBTYPE_raw)  return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;

    pc->width  = (int)info.info.raw.size.width;
    pc->height = (int)info.info.raw.size.height;
    pc->fmt    = info.info.raw.format;

    fprintf(stderr, "[wayland/pw] Format: %dx%d fmt=%u\n",
            pc->width, pc->height, pc->fmt);
}

static const struct pw_stream_events pw_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process       = pw_on_process,
    .param_changed = pw_on_param_changed,
};

static SnapxImage *pw_capture_node(int pw_fd, uint32_t node_id)
{
    PwCapture pc = {0};

    pw_init(NULL, NULL);

    pc.tloop = pw_thread_loop_new("snapx-capture", NULL);
    if (!pc.tloop) { fprintf(stderr, "[pw] thread_loop_new failed\n"); return NULL; }

    struct pw_context *ctx = pw_context_new(
        pw_thread_loop_get_loop(pc.tloop), NULL, 0);
    if (!ctx) { pw_thread_loop_destroy(pc.tloop); return NULL; }

    pw_thread_loop_lock(pc.tloop);

    struct pw_core *core = pw_context_connect_fd(ctx, pw_fd, NULL, 0);
    if (!core) {
        fprintf(stderr, "[pw] connect_fd failed: %s\n", strerror(errno));
        pw_thread_loop_unlock(pc.tloop);
        pw_context_destroy(ctx);
        pw_thread_loop_destroy(pc.tloop);
        return NULL;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,     "Screen",
        NULL);

    pc.stream = pw_stream_new(core, "snapx-screen-capture", props);
    if (!pc.stream) {
        pw_core_disconnect(core);
        pw_context_destroy(ctx);
        pw_thread_loop_unlock(pc.tloop);
        pw_thread_loop_destroy(pc.tloop);
        return NULL;
    }

    pw_stream_add_listener(pc.stream, &(struct spa_hook){0},
                           &pw_stream_events, &pc);

    /* Request RGBA or BGRx video */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4,
            SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_BGRx));

    pw_stream_connect(pc.stream, PW_DIRECTION_INPUT, node_id,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1);

    pw_thread_loop_start(pc.tloop);

    /* Wait for one frame (max 10 seconds).
     * pw_thread_loop_timed_wait(loop, max_sec) returns 0=OK, -ETIMEDOUT. */
    if (!pc.done)
        pw_thread_loop_timed_wait(pc.tloop, 10);

    pw_thread_loop_unlock(pc.tloop);
    pw_stream_destroy(pc.stream);
    pw_core_disconnect(core);
    pw_context_destroy(ctx);
    pw_thread_loop_stop(pc.tloop);
    pw_thread_loop_destroy(pc.tloop);
    pw_deinit();

    return pc.result;
}

static SnapxImage *portal_screencast_capture(WaylandState *st)
{
    /* ── Step 1: Create session ─────────────────────────────────────────── */
    GVariantBuilder sc_opts;
    g_variant_builder_init(&sc_opts, G_VARIANT_TYPE_VARDICT);
    char sess_token[64];
    snprintf(sess_token, sizeof(sess_token), "snapx_sess_%u", st->request_counter + 1);
    g_variant_builder_add(&sc_opts, "{sv}", "session_handle_token",
                          g_variant_new_string(sess_token));

    GVariant *create_res = portal_call_sync(st, PORTAL_IFACE_SC, "CreateSession",
        g_variant_new("(@a{sv})", g_variant_builder_end(&sc_opts)),
        10000);
    if (!create_res) {
        fprintf(stderr, "[wayland/sc] CreateSession failed\n");
        return NULL;
    }

    const char *session_handle_str = NULL;
    GVariant *sh_v = g_variant_lookup_value(create_res, "session_handle",
                                             G_VARIANT_TYPE_OBJECT_PATH);
    if (sh_v) session_handle_str = g_variant_get_string(sh_v, NULL);

    if (!session_handle_str) {
        /* GNOME puts it directly in the results dict as object path string */
        g_variant_unref(create_res);
        fprintf(stderr, "[wayland/sc] No session_handle in CreateSession response\n");
        return NULL;
    }
    char session_handle[256];
    snprintf(session_handle, sizeof(session_handle), "%s", session_handle_str);
    if (sh_v) g_variant_unref(sh_v);
    g_variant_unref(create_res);

    fprintf(stderr, "[wayland/sc] Session: %s\n", session_handle);

    /* ── Step 2: SelectSources ──────────────────────────────────────────── */
    GVariantBuilder sel_opts;
    g_variant_builder_init(&sel_opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&sel_opts, "{sv}", "types",
                          g_variant_new_uint32(SC_SOURCE_MONITOR));
    g_variant_builder_add(&sel_opts, "{sv}", "multiple",
                          g_variant_new_boolean(FALSE));
    g_variant_builder_add(&sel_opts, "{sv}", "persist_mode",
                          g_variant_new_uint32(SC_PERSIST_PERMANENT));
    /* Reuse restore_token to skip the picker on subsequent captures */
    if (st->restore_token[0]) {
        g_variant_builder_add(&sel_opts, "{sv}", "restore_token",
                              g_variant_new_string(st->restore_token));
        fprintf(stderr, "[wayland/sc] Using restore_token (silent capture)\n");
    } else {
        fprintf(stderr, "[wayland/sc] No restore_token — showing picker\n");
    }

    GVariant *sel_res = portal_call_sync(st, PORTAL_IFACE_SC, "SelectSources",
        g_variant_new("(o@a{sv})", session_handle,
                      g_variant_builder_end(&sel_opts)),
        120000);   /* 2 min: user needs time to interact with picker */
    if (!sel_res) {
        fprintf(stderr, "[wayland/sc] SelectSources failed or cancelled\n");
        /* Close session */
        GError *e = NULL;
        g_dbus_connection_call_sync(st->dbus, PORTAL_BUS, session_handle,
            PORTAL_IFACE_SES, "Close", NULL, NULL,
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &e);
        if (e) g_error_free(e);
        return NULL;
    }
    g_variant_unref(sel_res);

    /* ── Step 3: Start ──────────────────────────────────────────────────── */
    GVariantBuilder start_opts;
    g_variant_builder_init(&start_opts, G_VARIANT_TYPE_VARDICT);

    GVariant *start_res = portal_call_sync(st, PORTAL_IFACE_SC, "Start",
        g_variant_new("(os@a{sv})", session_handle, st->parent_window,
                      g_variant_builder_end(&start_opts)),
        30000);
    if (!start_res) {
        fprintf(stderr, "[wayland/sc] Start failed\n");
        GError *e = NULL;
        g_dbus_connection_call_sync(st->dbus, PORTAL_BUS, session_handle,
            PORTAL_IFACE_SES, "Close", NULL, NULL,
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &e);
        if (e) g_error_free(e);
        return NULL;
    }

    /* Extract node_id from streams array */
    uint32_t node_id = 0;
    GVariant *streams_v = g_variant_lookup_value(start_res, "streams",
        G_VARIANT_TYPE("a(ua{sv})"));
    if (streams_v && g_variant_n_children(streams_v) > 0) {
        GVariant *first = g_variant_get_child_value(streams_v, 0);
        g_variant_get_child(first, 0, "u", &node_id);
        g_variant_unref(first);
    }
    if (streams_v) g_variant_unref(streams_v);

    /* Extract and save restore_token */
    GVariant *rt_v = g_variant_lookup_value(start_res, "restore_token",
                                             G_VARIANT_TYPE_STRING);
    if (rt_v) {
        snprintf(st->restore_token, sizeof(st->restore_token), "%s",
                 g_variant_get_string(rt_v, NULL));
        g_variant_unref(rt_v);
        fprintf(stderr, "[wayland/sc] Saved restore_token for future silent captures\n");
    }
    g_variant_unref(start_res);

    if (!node_id) {
        fprintf(stderr, "[wayland/sc] No PipeWire node_id in Start response\n");
        GError *e = NULL;
        g_dbus_connection_call_sync(st->dbus, PORTAL_BUS, session_handle,
            PORTAL_IFACE_SES, "Close", NULL, NULL,
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &e);
        if (e) g_error_free(e);
        return NULL;
    }

    fprintf(stderr, "[wayland/sc] PipeWire node_id=%u\n", node_id);

    /* ── Step 4: Open PipeWire remote ───────────────────────────────────── */
    GError *err = NULL;
    GUnixFDList *fd_list = NULL;
    GVariantBuilder pw_opts2;
    g_variant_builder_init(&pw_opts2, G_VARIANT_TYPE_VARDICT);
    GVariant *pw_ret2 = g_dbus_connection_call_with_unix_fd_list_sync(
        st->dbus, PORTAL_BUS, PORTAL_PATH, PORTAL_IFACE_SC,
        "OpenPipeWireRemote",
        g_variant_new("(o@a{sv})", session_handle,
                      g_variant_builder_end(&pw_opts2)),
        G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &fd_list, NULL, &err);
    /* Close the ScreenCast session after we have what we need */
    GError *ce = NULL;
    g_dbus_connection_call_sync(st->dbus, PORTAL_BUS, session_handle,
        PORTAL_IFACE_SES, "Close", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &ce);
    if (ce) g_error_free(ce);

    if (!pw_ret2 || !fd_list) {
        fprintf(stderr, "[wayland/sc] OpenPipeWireRemote (fd) failed: %s\n",
                err ? err->message : "unknown");
        if (err) g_error_free(err);
        if (pw_ret2) g_variant_unref(pw_ret2);
        if (fd_list) g_object_unref(fd_list);
        return NULL;
    }

    gint32 fd_index = 0;
    g_variant_get(pw_ret2, "(h)", &fd_index);
    g_variant_unref(pw_ret2);

    int pw_fd = g_unix_fd_list_get(fd_list, fd_index, NULL);
    g_object_unref(fd_list);

    if (pw_fd < 0) {
        fprintf(stderr, "[wayland/sc] Failed to get PipeWire fd\n");
        return NULL;
    }

    fprintf(stderr, "[wayland/sc] PipeWire fd=%d — capturing frame...\n", pw_fd);

    /* ── Step 5: Capture one PipeWire frame ─────────────────────────────── */
    SnapxImage *img = pw_capture_node(pw_fd, node_id);
    close(pw_fd);

    if (img)
        fprintf(stderr, "[wayland/sc] Captured %dx%d\n", img->width, img->height);
    else
        fprintf(stderr, "[wayland/sc] PipeWire frame capture failed\n");

    return img;
}

#endif /* SNAPX_HAVE_PIPEWIRE */

/* ─── Backend entry points ───────────────────────────────────────────────── */

static SnapxImage *wayland_capture(SnapxCaptureBackend *backend,
                                    const SnapxCaptureRequest *req)
{
    WaylandState *st = (WaylandState *)backend->priv;
    (void)req;  /* ScreenCast always captures full screen; region cropped above */

    SnapxImage *img = NULL;

    /* ScreenCast: use when we have a parent handle (picker modal) or a saved
     * restore_token (silent re-capture).  Avoid Screenshot portal — it writes a
     * full-desktop PNG to disk before snapx crops the region. */
#ifdef SNAPX_HAVE_PIPEWIRE
    if (st->parent_window[0] != '\0' || st->restore_token[0] != '\0') {
        fprintf(stderr, "[wayland] Attempting ScreenCast portal capture...\n");
        img = portal_screencast_capture(st);
        if (img) return img;
        fprintf(stderr, "[wayland] ScreenCast failed — falling back to Screenshot portal\n");
    } else {
        fprintf(stderr,
                "[wayland] No parent window or restore_token — ScreenCast skipped\n");
    }
#endif

    fprintf(stderr, "[wayland] Using Screenshot portal...\n");
    img = portal_screenshot_fallback(st);
    return img;
}

static int wayland_get_monitors(SnapxCaptureBackend *backend,
                                 SnapxMonitorInfo *out, int max)
{
    (void)backend;

#if defined(SNAPX_USE_GTK4) && !defined(SNAPX_HEADLESS)
    GdkDisplay *dpy = gdk_display_get_default();
    if (!dpy) goto single_fallback;
    GListModel *monitors = gdk_display_get_monitors(dpy);
    guint n = g_list_model_get_n_items(monitors);
    int count = 0;
    for (guint i = 0; i < n && count < max; i++) {
        GdkMonitor *mon = GDK_MONITOR(g_list_model_get_item(monitors, i));
        GdkRectangle geo; gdk_monitor_get_geometry(mon, &geo);
        SnapxMonitorInfo *m = &out[count++];
        m->index = (int)i; m->x = geo.x; m->y = geo.y;
        m->width = geo.width; m->height = geo.height;
        m->scale = gdk_monitor_get_scale_factor(mon);
        const char *nm = gdk_monitor_get_model(mon);
        snprintf(m->name, sizeof(m->name), "%s", nm ? nm : "Monitor");
        m->is_primary = (i == 0) ? 1 : 0;
        g_object_unref(mon);
    }
    return count;
#endif

single_fallback:
    if (max < 1) return 0;
    out[0].index = 0; out[0].x = 0; out[0].y = 0;
    out[0].width = 1920; out[0].height = 1080; out[0].scale = 1;
    snprintf(out[0].name, sizeof(out[0].name), "Default");
    out[0].is_primary = 1;
    return 1;
}

static void wayland_destroy(SnapxCaptureBackend *backend)
{
    WaylandState *st = (WaylandState *)backend->priv;
    if (st) {
        if (st->dbus) g_object_unref(st->dbus);
        free(st);
    }
    backend->priv = NULL;
}

int snapx_capture_wayland_init(SnapxCaptureBackend *backend)
{
    WaylandState *st = calloc(1, sizeof(WaylandState));
    if (!st) return -1;

    GError *err = NULL;
    st->dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!st->dbus) {
        fprintf(stderr, "[wayland] DBus connect failed: %s\n",
                err ? err->message : "unknown");
        if (err) g_error_free(err);
        free(st); return -1;
    }

    const char *uname = g_dbus_connection_get_unique_name(st->dbus);
    sanitise_sender(uname ? uname : "_0", st->sender_token, sizeof(st->sender_token));

    /* Load saved restore_token from state file */
    const char *home = g_get_home_dir();
    char token_path[512];
    snprintf(token_path, sizeof(token_path),
             "%s/.config/snapx/wayland_restore_token", home);
    FILE *tf = fopen(token_path, "r");
    if (tf) {
        if (fgets(st->restore_token, sizeof(st->restore_token), tf)) {
            /* Strip newline */
            size_t len = strlen(st->restore_token);
            if (len > 0 && st->restore_token[len-1] == '\n')
                st->restore_token[len-1] = '\0';
        }
        fclose(tf);
        if (st->restore_token[0])
            fprintf(stderr, "[wayland] Loaded restore_token from %s\n", token_path);
    }

    /* Verify portal available */
    GVariant *ret = g_dbus_connection_call_sync(
        st->dbus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", PORTAL_BUS),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    if (!ret) {
        fprintf(stderr, "[wayland] Portal not available: %s\n",
                err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_object_unref(st->dbus); free(st); return -1;
    }
    g_variant_unref(ret);

    backend->priv         = st;
    backend->capture      = wayland_capture;
    backend->get_monitors = wayland_get_monitors;
    backend->destroy      = wayland_destroy;
    backend->type         = SNAPX_BACKEND_WAYLAND;

    fprintf(stderr, "[wayland] Backend initialised (token=%s)\n", st->sender_token);
    return 0;
}

/* ─── Parent window registration (called from GTK UI layer) ─────────────── */

void snapx_capture_wayland_set_parent_window(SnapxCaptureBackend *backend,
                                              const char *parent_window_str)
{
    if (!backend || !backend->priv) return;
    WaylandState *st = (WaylandState *)backend->priv;
    snprintf(st->parent_window, sizeof(st->parent_window), "%s",
             parent_window_str ? parent_window_str : "");
}

/* ─── Restore token persistence ──────────────────────────────────────────── */

void snapx_capture_wayland_save_token(SnapxCaptureBackend *backend)
{
    if (!backend || !backend->priv) return;
    WaylandState *st = (WaylandState *)backend->priv;
    if (!st->restore_token[0]) return;

    const char *home = g_get_home_dir();
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/.config/snapx", home);
    g_mkdir_with_parents(dir_path, 0700);

    char token_path[600];
    snprintf(token_path, sizeof(token_path), "%s/wayland_restore_token", dir_path);
    FILE *tf = fopen(token_path, "w");
    if (tf) {
        fprintf(tf, "%s\n", st->restore_token);
        fclose(tf);
        fprintf(stderr, "[wayland] Saved restore_token to %s\n", token_path);
    }
}

#endif /* SNAPX_HAVE_WAYLAND */

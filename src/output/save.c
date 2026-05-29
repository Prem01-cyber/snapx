/**
 * @file save.c
 * @brief Save SnapxImage to PNG (libpng), JPEG (libjpeg), or WebP (libwebp).
 *
 * All three formats receive an RGBA pixel buffer.  PNG and WebP accept RGBA
 * directly.  JPEG requires RGB conversion (alpha stripped, assumed opaque).
 */

#include "save.h"
#include "../capture/capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef SNAPX_PLATFORM_WINDOWS
#  include <direct.h>
#endif

/* libpng */
#include <png.h>

/* libjpeg */
#ifdef SNAPX_HAVE_JPEG
#  include <jpeglib.h>
#endif

/* libwebp (optional) */
#ifdef SNAPX_HAVE_WEBP
#  include <webp/encode.h>
#endif

/* ─── Directory helpers ──────────────────────────────────────────────────── */

/**
 * @brief Create all directories in @p path (like mkdir -p).
 */
static void mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/'
#ifdef SNAPX_PLATFORM_WINDOWS
         || *p == '\\'
#endif
        ) {
            char save = *p;
            *p = '\0';
#ifdef SNAPX_PLATFORM_WINDOWS
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = save;
        }
    }
}

/**
 * @brief Ensure the parent directory of @p filepath exists.
 */
static void ensure_parent_dir(const char *filepath)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", filepath);
    char *last = strrchr(dir, '/');
#ifdef SNAPX_PLATFORM_WINDOWS
    char *last2 = strrchr(dir, '\\');
    if (last2 > last) last = last2;
#endif
    if (last) {
        *last = '\0';
        mkdir_p(dir);
    }
}

/* ─── In-memory buffer helpers ───────────────────────────────────────────── */

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} MemBuf;

static int membuf_append(MemBuf *b, const void *src, size_t len)
{
    if (b->size + len > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->size + len) ncap *= 2;
        uint8_t *n = realloc(b->data, ncap);
        if (!n) return -1;
        b->data = n;
        b->cap  = ncap;
    }
    memcpy(b->data + b->size, src, len);
    b->size += len;
    return 0;
}

static void png_write_mem(png_structp png, png_bytep data, png_size_t len)
{
    MemBuf *b = (MemBuf *)png_get_io_ptr(png);
    if (membuf_append(b, data, len) != 0)
        png_error(png, "membuf append failed");
}

static void png_flush_mem(png_structp png) { (void)png; }

static int encode_png_mem(const SnapxImage *img, MemBuf *buf)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                               NULL, NULL, NULL);
    if (!png) return -1;

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return -1;
    }

    png_set_write_fn(png, buf, png_write_mem, png_flush_mem);
    png_set_IHDR(png, info,
                 (png_uint_32)img->width, (png_uint_32)img->height, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    png_bytep *rows = malloc((size_t)img->height * sizeof(png_bytep));
    if (!rows) {
        png_destroy_write_struct(&png, &info);
        return -1;
    }
    for (int i = 0; i < img->height; i++)
        rows[i] = img->data + i * img->stride;

    png_write_image(png, rows);
    png_write_end(png, NULL);
    free(rows);
    png_destroy_write_struct(&png, &info);
    return 0;
}

#ifdef SNAPX_HAVE_JPEG
struct jpeg_mem_dest_mgr {
    struct jpeg_destination_mgr pub;
    MemBuf *buf;
    JOCTET  scratch[4096];
};

static void jpeg_init_mem(j_compress_ptr cinfo)
{
    struct jpeg_mem_dest_mgr *d =
        (struct jpeg_mem_dest_mgr *)cinfo->dest;
    d->pub.next_output_byte = d->scratch;
    d->pub.free_in_buffer   = sizeof(d->scratch);
}

static boolean jpeg_empty_mem(j_compress_ptr cinfo)
{
    struct jpeg_mem_dest_mgr *d =
        (struct jpeg_mem_dest_mgr *)cinfo->dest;
    size_t n = sizeof(d->scratch) - d->pub.free_in_buffer;
    if (n && membuf_append(d->buf, d->scratch, n) != 0)
        return FALSE;
    d->pub.next_output_byte = d->scratch;
    d->pub.free_in_buffer   = sizeof(d->scratch);
    return TRUE;
}

static void jpeg_term_mem(j_compress_ptr cinfo)
{
    struct jpeg_mem_dest_mgr *d =
        (struct jpeg_mem_dest_mgr *)cinfo->dest;
    size_t n = sizeof(d->scratch) - d->pub.free_in_buffer;
    if (n)
        membuf_append(d->buf, d->scratch, n);
}

static void jpeg_set_mem_dest(j_compress_ptr cinfo, MemBuf *buf)
{
    struct jpeg_mem_dest_mgr *d;
    if (cinfo->dest == NULL)
        cinfo->dest = (struct jpeg_destination_mgr *)
            (*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT,
                                       sizeof(struct jpeg_mem_dest_mgr));
    d = (struct jpeg_mem_dest_mgr *)cinfo->dest;
    d->buf = buf;
    d->pub.init_destination    = jpeg_init_mem;
    d->pub.empty_output_buffer = jpeg_empty_mem;
    d->pub.term_destination    = jpeg_term_mem;
}

static int encode_jpeg_mem(const SnapxImage *img, int quality, MemBuf *buf)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr       jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_set_mem_dest(&cinfo, buf);

    cinfo.image_width      = (JDIMENSION)img->width;
    cinfo.image_height     = (JDIMENSION)img->height;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    JSAMPLE *row_buf = malloc((size_t)(img->width * 3));
    if (!row_buf) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t *src = img->data + (int)cinfo.next_scanline * img->stride;
        for (int col = 0; col < img->width; col++) {
            row_buf[col * 3 + 0] = src[col * 4 + 0];
            row_buf[col * 3 + 1] = src[col * 4 + 1];
            row_buf[col * 3 + 2] = src[col * 4 + 2];
        }
        JSAMPROW row = row_buf;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    free(row_buf);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return 0;
}
#endif /* SNAPX_HAVE_JPEG */

static int encode_webp_mem(const SnapxImage *img, int quality, MemBuf *buf)
{
#ifdef SNAPX_HAVE_WEBP
    uint8_t *output = NULL;
    size_t output_size = WebPEncodeRGBA(
        img->data, img->width, img->height, img->stride,
        (float)quality, &output);
    if (output_size == 0 || !output) return -1;
    if (membuf_append(buf, output, output_size) != 0) {
        WebPFree(output);
        return -1;
    }
    WebPFree(output);
    return 0;
#else
    (void)img; (void)quality;
    return -1;
#endif
}

void snapx_encoded_image_free(SnapxEncodedImage *enc)
{
    if (!enc) return;
    free(enc->data);
    enc->data = NULL;
    enc->size = 0;
    enc->mime = NULL;
}

int snapx_image_encode(const SnapxImage *img, SnapxOutputFormat format,
                       int quality, SnapxEncodedImage *out)
{
    if (!img || !out) return -1;
    if (quality < 1 || quality > 100) quality = 90;

    memset(out, 0, sizeof(*out));
    MemBuf buf = {0};

    int rc = -1;
    switch (format) {
        case SNAPX_FORMAT_PNG:
            rc = encode_png_mem(img, &buf);
            out->mime = "image/png";
            break;
        case SNAPX_FORMAT_JPEG:
#ifdef SNAPX_HAVE_JPEG
            rc = encode_jpeg_mem(img, quality, &buf);
            out->mime = "image/jpeg";
#else
            rc = encode_png_mem(img, &buf);
            out->mime = "image/png";
#endif
            break;
        case SNAPX_FORMAT_WEBP:
            rc = encode_webp_mem(img, quality, &buf);
            if (rc != 0) {
                rc = encode_png_mem(img, &buf);
                out->mime = "image/png";
            } else {
                out->mime = "image/webp";
            }
            break;
        default:
            rc = encode_png_mem(img, &buf);
            out->mime = "image/png";
            break;
    }

    if (rc != 0) {
        free(buf.data);
        return -1;
    }
    out->data = buf.data;
    out->size = buf.size;
    return 0;
}

/* ─── PNG (file) ─────────────────────────────────────────────────────────── */

static int save_png(const SnapxImage *img, const char *path)
{
    ensure_parent_dir(path);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "[save] Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                               NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp); return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp); return -1;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info,
                 (png_uint_32)img->width, (png_uint_32)img->height, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    /* Cairo ARGB32 is premultiplied BGRA in native byte order.
     * We stored straight RGBA, so write row pointers directly. */
    png_bytep *rows = malloc((size_t)img->height * sizeof(png_bytep));
    if (!rows) {
        png_destroy_write_struct(&png, &info); fclose(fp); return -1;
    }
    for (int i = 0; i < img->height; i++)
        rows[i] = img->data + i * img->stride;

    png_write_image(png, rows);
    png_write_end(png, NULL);
    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

/* ─── JPEG ───────────────────────────────────────────────────────────────── */

static int save_jpeg(const SnapxImage *img, const char *path, int quality)
{
#ifndef SNAPX_HAVE_JPEG
    fprintf(stderr, "[save] JPEG support not compiled in, saving as PNG.\n");
    char alt[512]; snprintf(alt, sizeof(alt), "%s", path);
    char *dot = strrchr(alt, '.'); if (dot) snprintf(dot, 5, ".png");
    return save_png(img, alt);
#else
    ensure_parent_dir(path);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "[save] Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr       jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width      = (JDIMENSION)img->width;
    cinfo.image_height     = (JDIMENSION)img->height;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    /* Convert RGBA → RGB scanline-by-scanline */
    JSAMPLE *row_buf = malloc((size_t)(img->width * 3));
    if (!row_buf) {
        jpeg_destroy_compress(&cinfo); fclose(fp); return -1;
    }

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t *src = img->data + (int)cinfo.next_scanline * img->stride;
        for (int col = 0; col < img->width; col++) {
            row_buf[col * 3 + 0] = src[col * 4 + 0]; /* R */
            row_buf[col * 3 + 1] = src[col * 4 + 1]; /* G */
            row_buf[col * 3 + 2] = src[col * 4 + 2]; /* B */
        }
        JSAMPROW row = row_buf;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    free(row_buf);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    return 0;
#endif /* SNAPX_HAVE_JPEG */
}

/* ─── WebP ───────────────────────────────────────────────────────────────── */

static int save_webp(const SnapxImage *img, const char *path, int quality)
{
#ifdef SNAPX_HAVE_WEBP
    ensure_parent_dir(path);
    uint8_t *output = NULL;
    size_t   output_size = WebPEncodeRGBA(
        img->data, img->width, img->height, img->stride,
        (float)quality, &output);

    if (output_size == 0 || !output) {
        fprintf(stderr, "[save] WebP encoding failed.\n");
        return -1;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "[save] Cannot open %s: %s\n", path, strerror(errno));
        WebPFree(output); return -1;
    }
    fwrite(output, 1, output_size, fp);
    fclose(fp);
    WebPFree(output);
    return 0;
#else
    fprintf(stderr, "[save] WebP support not compiled in. Saving as PNG instead.\n");
    /* Change extension and fall back */
    char alt_path[512];
    snprintf(alt_path, sizeof(alt_path), "%s", path);
    char *dot = strrchr(alt_path, '.');
    if (dot) snprintf(dot, 5, ".png");
    return save_png(img, alt_path);
#endif
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

int snapx_image_save(const SnapxImage *img, const char *path,
                     SnapxOutputFormat format, int quality)
{
    if (!img || !path) return -1;
    if (quality < 1 || quality > 100) quality = 90;

    switch (format) {
        case SNAPX_FORMAT_PNG:  return save_png(img, path);
        case SNAPX_FORMAT_JPEG: return save_jpeg(img, path, quality);
        case SNAPX_FORMAT_WEBP: return save_webp(img, path, quality);
        default:
            fprintf(stderr, "[save] Unknown format %d, defaulting to PNG.\n", format);
            return save_png(img, path);
    }
}

#ifndef SNAPX_HEADLESS
#  include <gdk-pixbuf/gdk-pixbuf.h>

SnapxImage *snapx_image_load_file(const char *path)
{
    if (!path || !path[0]) return NULL;

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (!pb) {
        if (err) g_error_free(err);
        return NULL;
    }

    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int chans = gdk_pixbuf_get_n_channels(pb);
    int src_stride = gdk_pixbuf_get_rowstride(pb);
    const guchar *src = gdk_pixbuf_get_pixels(pb);

    SnapxImage *img = snapx_image_alloc(w, h);
    if (!img) {
        g_object_unref(pb);
        return NULL;
    }

    for (int y = 0; y < h; y++) {
        const guchar *row = src + y * src_stride;
        uint8_t *dst = img->data + y * img->stride;
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = row[x * chans + 0];
            dst[x * 4 + 1] = row[x * chans + 1];
            dst[x * 4 + 2] = row[x * chans + 2];
            dst[x * 4 + 3] = chans >= 4 ? row[x * chans + 3] : 255;
        }
    }

    g_object_unref(pb);
    return img;
}
#endif

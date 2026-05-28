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

/* ─── PNG ────────────────────────────────────────────────────────────────── */

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

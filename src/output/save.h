/**
 * @file save.h
 * @brief Image saving declarations (PNG, JPEG, WebP).
 */

#ifndef SNAPX_SAVE_H
#define SNAPX_SAVE_H

#include "../capture/capture.h"

/** @brief In-memory encoded image buffer (PNG/JPEG/WebP bytes). */
typedef struct {
    uint8_t *data;
    size_t   size;
    const char *mime;   /**< e.g. "image/png" */
} SnapxEncodedImage;

/**
 * @brief Encode @p img to memory. Caller must free with snapx_encoded_image_free().
 * @return 0 on success.
 */
int snapx_image_encode(const SnapxImage *img, SnapxOutputFormat format,
                       int quality, SnapxEncodedImage *out);

/** Release memory from snapx_image_encode(). */
void snapx_encoded_image_free(SnapxEncodedImage *enc);

/**
 * @brief Save a SnapxImage to a file.
 * @param img      Source image (RGBA, 8bpc).
 * @param path     Destination file path.
 * @param format   Output format.
 * @param quality  JPEG quality 1–100 (ignored for PNG/WebP).
 * @return 0 on success, non-zero on failure.
 */
int snapx_image_save(const SnapxImage *img, const char *path,
                     SnapxOutputFormat format, int quality);

#ifndef SNAPX_HEADLESS
/** Load PNG/JPEG/WebP from disk into a new SnapxImage. */
SnapxImage *snapx_image_load_file(const char *path);
#endif

#endif /* SNAPX_SAVE_H */

/**
 * @file save.h
 * @brief Image saving declarations (PNG, JPEG, WebP).
 */

#ifndef SNAPX_SAVE_H
#define SNAPX_SAVE_H

#include "../capture/capture.h"

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

#endif /* SNAPX_SAVE_H */

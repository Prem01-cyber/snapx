/**
 * @file ocr.h
 * @brief Optional Tesseract OCR — extract text from image region.
 */

#ifndef SNAPX_OCR_H
#define SNAPX_OCR_H

#include "../capture/capture.h"

#define SNAPX_OCR_MAX 65536

/** Return 1 when built with Tesseract. */
int snapx_ocr_available(void);

/**
 * @brief Run OCR on full image or NULL-terminated UTF-8 text in @p out.
 * @return 0 on success.
 */
int snapx_ocr_image(const SnapxImage *img, const char *lang,
                    char *out, size_t outsz);

#endif /* SNAPX_OCR_H */

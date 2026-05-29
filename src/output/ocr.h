/**
 * @file ocr.h
 * @brief Optional Tesseract OCR — extract text from image region.
 */

#ifndef SNAPX_OCR_H
#define SNAPX_OCR_H

#include "../capture/capture.h"
#include "../utils/config.h"

#define SNAPX_OCR_MAX 65536

typedef enum {
    SNAPX_OCR_STATUS_AVAILABLE    = 0,
    SNAPX_OCR_STATUS_NOT_BUILT    = 1,
} SnapxOcrStatus;

typedef void (*SnapxOcrDoneFn)(int success, const char *text, gpointer userdata);

/** Return 1 when built with Tesseract. */
int snapx_ocr_available(void);

SnapxOcrStatus snapx_ocr_get_status(const SnapxConfig *cfg);
void snapx_ocr_status_message(const SnapxConfig *cfg, char *buf, size_t bufsz);

/**
 * @brief Run OCR synchronously (CLI).
 * @return 0 on success.
 */
int snapx_ocr_image(const SnapxImage *img, const char *lang,
                    char *out, size_t outsz);

/** Run OCR on a worker thread; @p done runs on the main thread. */
void snapx_ocr_async(const SnapxImage *img, const char *lang,
                     SnapxOcrDoneFn done, gpointer userdata);

/** Request cancel; in-flight result is discarded when complete. */
void snapx_ocr_cancel(void);

/** Non-zero while an async OCR job is running. */
int snapx_ocr_busy(void);

#endif /* SNAPX_OCR_H */

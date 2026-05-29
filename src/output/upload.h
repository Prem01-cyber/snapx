/**
 * @file upload.h
 * @brief Async image upload to Imgur or a custom HTTP endpoint.
 */

#ifndef SNAPX_UPLOAD_H
#define SNAPX_UPLOAD_H

#include "../utils/config.h"
#include "save.h"

#define SNAPX_UPLOAD_URL_MAX 512
#define SNAPX_UPLOAD_ERR_MAX 256

typedef void (*SnapxUploadDoneFn)(int success, const char *url,
                                   const char *error, gpointer userdata);

typedef enum {
    SNAPX_UPLOAD_STATUS_AVAILABLE      = 0,
    SNAPX_UPLOAD_STATUS_NOT_BUILT      = 1,
    SNAPX_UPLOAD_STATUS_NOT_CONFIGURED = 2,
} SnapxUploadStatus;

/** Return 1 when snapx was built with libcurl upload support. */
int snapx_upload_available(void);

SnapxUploadStatus snapx_upload_get_status(const SnapxConfig *cfg);
void snapx_upload_status_message(const SnapxConfig *cfg, char *buf, size_t bufsz);

/**
 * @brief Upload encoded image bytes asynchronously on a worker thread.
 * @p done is invoked on the main thread (default GLib context).
 */
void snapx_upload_async(const SnapxEncodedImage *enc,
                        const SnapxConfig *config,
                        SnapxUploadDoneFn done,
                        gpointer userdata);

/** Cancel in-flight upload; late result is reported as cancelled. */
void snapx_upload_cancel(void);

/** Non-zero while an async upload is running. */
int snapx_upload_busy(void);

/** Synchronous upload for CLI (blocks until complete). Returns 0 on success. */
int snapx_upload_sync(const SnapxEncodedImage *enc, const SnapxConfig *config,
                      char *url_out, size_t url_sz,
                      char *err_out, size_t err_sz);

#endif /* SNAPX_UPLOAD_H */

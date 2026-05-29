/**
 * @file ocr.c
 * @brief Tesseract OCR integration (optional at build time).
 */

#include "ocr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>

#ifdef SNAPX_HAVE_TESSERACT
#  include <tesseract/capi.h>
#endif

static volatile int g_ocr_busy;
static volatile int g_ocr_cancel;

typedef struct {
    SnapxImage      *img_copy;
    char             lang[16];
    SnapxOcrDoneFn   done;
    gpointer         userdata;
    char             text[SNAPX_OCR_MAX];
    int              ok;
} OcrJob;

SnapxOcrStatus snapx_ocr_get_status(const SnapxConfig *cfg)
{
    (void)cfg;
#ifdef SNAPX_HAVE_TESSERACT
    return SNAPX_OCR_STATUS_AVAILABLE;
#else
    return SNAPX_OCR_STATUS_NOT_BUILT;
#endif
}

void snapx_ocr_status_message(const SnapxConfig *cfg, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return;
    if (snapx_ocr_get_status(cfg) == SNAPX_OCR_STATUS_NOT_BUILT) {
        snprintf(buf, bufsz, "OCR: not installed (rebuild with Tesseract)");
        return;
    }
    const char *lang = (cfg && cfg->ocr_lang[0]) ? cfg->ocr_lang : "eng";
    if (g_ocr_busy)
        snprintf(buf, bufsz, "OCR: running…");
    else
        snprintf(buf, bufsz, "OCR: ready (%s)", lang);
}

int snapx_ocr_available(void)
{
#ifdef SNAPX_HAVE_TESSERACT
    return 1;
#else
    return 0;
#endif
}

int snapx_ocr_busy(void)
{
    return g_ocr_busy ? 1 : 0;
}

void snapx_ocr_cancel(void)
{
    g_ocr_cancel = 1;
}

static int ocr_run_on_image(const SnapxImage *img, const char *lang,
                            char *out, size_t outsz)
{
    if (!img || !out || outsz == 0) return -1;
    out[0] = '\0';
    const char *use_lang = (lang && lang[0]) ? lang : "eng";

#ifdef SNAPX_HAVE_TESSERACT
    TessBaseAPI *api = TessBaseAPICreate();
    if (!api) return -1;

    if (TessBaseAPIInit3(api, NULL, use_lang) != 0) {
        TessBaseAPIDelete(api);
        snprintf(out, outsz, "OCR failed (language '%s' not installed?)", use_lang);
        return -1;
    }

    TessBaseAPISetImage(api, img->data, img->width, img->height,
                        img->stride, 4);
    char *text = TessBaseAPIGetUTF8Text(api);
    int ok = -1;
    if (text) {
        snprintf(out, outsz, "%s", text);
        TessDeleteText(text);
        ok = 0;
    }
    TessBaseAPIDelete(api);
    return ok;
#else
    (void)img; (void)use_lang;
    snprintf(out, outsz, "OCR not available (rebuild with Tesseract)");
    return -1;
#endif
}

int snapx_ocr_image(const SnapxImage *img, const char *lang,
                    char *out, size_t outsz)
{
    return ocr_run_on_image(img, lang, out, outsz);
}

static SnapxImage *image_copy(const SnapxImage *src)
{
    if (!src) return NULL;
    SnapxImage *copy = snapx_image_alloc(src->width, src->height);
    if (!copy) return NULL;
    memcpy(copy->data, src->data, (size_t)(src->stride * src->height));
    copy->scale = src->scale;
    return copy;
}

static gboolean ocr_invoke(gpointer data)
{
    OcrJob *job = data;
    int cancelled = g_ocr_cancel;
    g_ocr_cancel = 0;
    g_ocr_busy   = 0;

    if (job->done) {
        if (cancelled)
            job->done(0, "OCR cancelled", job->userdata);
        else if (job->ok == 0)
            job->done(1, job->text, job->userdata);
        else
            job->done(0, job->text, job->userdata);
    }

    snapx_image_free(job->img_copy);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer ocr_thread(gpointer data)
{
    OcrJob *job = data;
    job->ok = ocr_run_on_image(job->img_copy, job->lang,
                               job->text, sizeof(job->text));
    g_main_context_invoke(g_main_context_default(), ocr_invoke, job);
    return NULL;
}

void snapx_ocr_async(const SnapxImage *img, const char *lang,
                     SnapxOcrDoneFn done, gpointer userdata)
{
    if (!img || !done) return;
    if (g_ocr_busy) return;

    OcrJob *job = g_new0(OcrJob, 1);
    job->img_copy = image_copy(img);
    if (!job->img_copy) {
        done(0, "Out of memory", userdata);
        g_free(job);
        return;
    }
    snprintf(job->lang, sizeof(job->lang), "%s",
             (lang && lang[0]) ? lang : "eng");
    job->done     = done;
    job->userdata = userdata;

    g_ocr_cancel = 0;
    g_ocr_busy   = 1;
    GThread *t = g_thread_new("snapx-ocr", ocr_thread, job);
    g_thread_unref(t);
}

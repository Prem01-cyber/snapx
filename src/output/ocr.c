/**
 * @file ocr.c
 * @brief Tesseract OCR integration (optional at build time).
 */

#include "ocr.h"

#include <stdio.h>
#include <string.h>

#ifdef SNAPX_HAVE_TESSERACT
#  include <tesseract/capi.h>
#endif

int snapx_ocr_available(void)
{
#ifdef SNAPX_HAVE_TESSERACT
    return 1;
#else
    return 0;
#endif
}

int snapx_ocr_image(const SnapxImage *img, const char *lang,
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

/**
 * @file beautify.h
 * @brief "Beautiful screenshot" compositor — wraps a capture in padding, a
 *        background (solid / gradient / transparent), rounded corners and a
 *        soft drop shadow.  Pure Cairo, no extra dependencies.
 */

#ifndef SNAPX_BEAUTIFY_H
#define SNAPX_BEAUTIFY_H

#include "../capture/capture.h"

/** Background style behind the padded screenshot. */
typedef enum {
    SNAPX_BG_SOLID       = 0,  /**< Single flat colour                       */
    SNAPX_BG_GRADIENT    = 1,  /**< Diagonal two-colour gradient             */
    SNAPX_BG_TRANSPARENT = 2,  /**< Transparent padding (PNG/WebP only)      */
} SnapxBeautifyBg;

/** Beautify parameters (persisted in SnapxConfig). */
typedef struct {
    int             enabled;        /**< Master toggle                        */
    int             padding;        /**< Padding around the image, px         */
    SnapxBeautifyBg bg_type;
    double          bg_r,  bg_g,  bg_b;   /**< Solid / gradient start colour  */
    double          bg_r2, bg_g2, bg_b2;  /**< Gradient end colour            */
    int             corner_radius;  /**< Rounded-corner radius on the shot, px */
    int             shadow;         /**< Non-zero to draw a drop shadow       */
    int             shadow_size;    /**< Shadow spread radius, px             */
} SnapxBeautifyConfig;

/** Populate @p cfg with sensible defaults (enabled off). */
void snapx_beautify_defaults(SnapxBeautifyConfig *cfg);

/**
 * @brief Compose @p src onto a beautified canvas.
 *
 * The result is a newly allocated SnapxImage that the caller must free with
 * snapx_image_free().  Returns a plain copy of @p src when @p cfg is NULL,
 * disabled, or padding is zero with no shadow/rounding.
 */
SnapxImage *snapx_beautify_apply(const SnapxImage *src,
                                 const SnapxBeautifyConfig *cfg);

#endif /* SNAPX_BEAUTIFY_H */

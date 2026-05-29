/**
 * @file pattern.h
 * @brief Filename pattern token expansion and counter scanning.
 */

#ifndef SNAPX_PATTERN_H
#define SNAPX_PATTERN_H

#include "config.h"

/**
 * Expand filename_pattern into a basename (no directory, no extension).
 * Counter tokens scan save_dir for the next free number.
 */
void snapx_pattern_expand_basename(const SnapxConfig *config,
                                   SnapxOutputFormat fmt,
                                   char *buf, size_t bufsz);

#endif /* SNAPX_PATTERN_H */

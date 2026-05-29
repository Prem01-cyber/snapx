/**
 * @file pattern.c
 * @brief Filename pattern expansion with date tokens and printf-style counters.
 */

#include "pattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef SNAPX_PLATFORM_WINDOWS
#  include <windows.h>
#endif

typedef struct {
    int    is_counter;
    int    width;       /* 0 = unpadded; >0 zero-pad width */
    int    token_len;   /* bytes consumed from pattern after '%' */
} CounterSpec;

static const char *ext_for_format(SnapxOutputFormat fmt)
{
    if (fmt == SNAPX_FORMAT_JPEG) return ".jpg";
    if (fmt == SNAPX_FORMAT_WEBP) return ".webp";
    return ".png";
}

static int parse_counter_at(const char *s, CounterSpec *out)
{
    memset(out, 0, sizeof(*out));
    if (!s || !*s) return 0;

    if (*s == 'n') {
        out->is_counter = 1;
        out->width      = 4;
        out->token_len  = 1;
        return 1;
    }

    int i = 0;
    int width = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        width = width * 10 + (s[i] - '0');
        i++;
    }
    char t = s[i];
    if (t == 'd' || t == 'i' || t == 'u') {
        out->is_counter = 1;
        out->width      = width;
        out->token_len  = i + 1;
        return 1;
    }

    if ((t == 'd' || t == 'i' || t == 'u') && i == 0) {
        out->is_counter = 1;
        out->width      = 0;
        out->token_len  = 1;
        return 1;
    }
    return 0;
}

static int is_day_token(const char *pattern, const char *at)
{
    if (*at != 'd') return 0;
    const char *p = pattern;
    while (p < at) {
        if (*p == '%' && *(p + 1) == 'm') return 1;
        p++;
    }
    return 0;
}

static void format_counter(int value, int width, char *tmp, size_t tmpsz)
{
    if (width < 0) width = 0;
    if (width > 16) width = 16;
    if (width > 0)
        snprintf(tmp, tmpsz, "%0*d", width, value);
    else
        snprintf(tmp, tmpsz, "%d", value);
}

static void append_str(char *dst, size_t dstsz, const char *src)
{
    size_t cur = strlen(dst);
    if (cur >= dstsz - 1) return;
    snprintf(dst + cur, dstsz - cur, "%s", src);
}

/** Build prefix/suffix around the first counter token (date parts expanded). */
static void build_counter_scan_template(const SnapxConfig *config,
                                          const struct tm *tm,
                                          char *prefix, size_t psz,
                                          char *suffix, size_t ssz,
                                          CounterSpec *cspec)
{
    prefix[0] = suffix[0] = '\0';
    const char *s = config->filename_pattern;
    int prev_was_m = 0;
    int found = 0;

    while (*s) {
        if (*s == '%' && *(s + 1)) {
            CounterSpec cs = {0};
            if (!found && parse_counter_at(s + 1, &cs) &&
                !(cs.token_len == 1 && *(s + 1) == 'd' && is_day_token(config->filename_pattern, s + 1))) {
                *cspec = cs;
                found = 1;
                s += 1 + cs.token_len;
                continue;
            }

            char tmp[32] = {0};
            char c = *(s + 1);
            switch (c) {
                case 'Y': snprintf(tmp, sizeof(tmp), "%04d", tm->tm_year + 1900); prev_was_m = 0; break;
                case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); prev_was_m = 1; break;
                case 'd':
                    if (prev_was_m) {
                        snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday);
                        prev_was_m = 0;
                    } else if (!found) {
                        *cspec = (CounterSpec){ .is_counter = 1, .width = 0, .token_len = 1 };
                        found = 1;
                        s += 2;
                        continue;
                    } else {
                        tmp[0] = '%'; tmp[1] = 'd'; tmp[2] = '\0';
                    }
                    break;
                case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); prev_was_m = 0; break;
                case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); prev_was_m = 0; break;
                case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); prev_was_m = 0; break;
                case '%': snprintf(tmp, sizeof(tmp), "%%"); prev_was_m = 0; break;
                default:
                    tmp[0] = '%'; tmp[1] = c; tmp[2] = '\0';
                    prev_was_m = 0;
                    break;
            }
            if (found)
                append_str(suffix, ssz, tmp);
            else
                append_str(prefix, psz, tmp);
            s += 2;
        } else {
            char ch[2] = { *s, '\0' };
            if (found)
                append_str(suffix, ssz, ch);
            else
                append_str(prefix, psz, ch);
            prev_was_m = 0;
            s++;
        }
    }
}

static int extract_number_from_filename(const char *name,
                                         const char *prefix, const char *suffix,
                                         const char *ext)
{
    size_t plen = strlen(prefix);
    size_t slen = strlen(suffix);
    size_t elen = strlen(ext);
    size_t nlen = strlen(name);

    if (nlen < plen + elen + 1) return -1;
    if (strncmp(name, prefix, plen) != 0) return -1;
    if (elen > 0 && strcmp(name + nlen - elen, ext) != 0) return -1;

    size_t num_start = plen;
    size_t num_end   = nlen - elen;
    if (slen > 0) {
        if (num_end < slen || strcmp(name + num_end - slen, suffix) != 0)
            return -1;
        num_end -= slen;
    }
    if (num_end <= num_start) return -1;

    int value = 0;
    for (size_t i = num_start; i < num_end; i++) {
        if (!isdigit((unsigned char)name[i])) return -1;
        value = value * 10 + (name[i] - '0');
    }
    return value;
}

static int next_counter_from_dir(const char *dir, const char *prefix,
                                  const char *suffix, const char *ext)
{
    int max_val = 0;

#ifdef SNAPX_PLATFORM_WINDOWS
    char search[MAX_PATH * 2];
    snprintf(search, sizeof(search), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int v = extract_number_from_filename(fd.cFileName, prefix, suffix, ext);
        if (v > max_val) max_val = v;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        int v = extract_number_from_filename(ent->d_name, prefix, suffix, ext);
        if (v > max_val) max_val = v;
    }
    closedir(d);
#endif
    return max_val + 1;
}

static int compute_next_counter(const SnapxConfig *config,
                                 SnapxOutputFormat fmt,
                                 const struct tm *tm,
                                 CounterSpec *cspec)
{
    char prefix[SNAPX_CONFIG_MAX_PATTERN * 2];
    char suffix[SNAPX_CONFIG_MAX_PATTERN * 2];
    CounterSpec cs = {0};
    build_counter_scan_template(config, tm, prefix, sizeof(prefix),
                                suffix, sizeof(suffix), &cs);
    *cspec = cs;
    if (!cs.is_counter) return 1;
    return next_counter_from_dir(config->save_dir, prefix, suffix,
                                 ext_for_format(fmt));
}

void snapx_pattern_expand_basename(const SnapxConfig *config,
                                   SnapxOutputFormat fmt,
                                   char *buf, size_t bufsz)
{
    if (!config || !buf || bufsz == 0) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    CounterSpec cspec = {0};
    int counter_val = compute_next_counter(config, fmt, tm, &cspec);
    int counter_used = 0;
    int prev_was_m = 0;

    buf[0] = '\0';
    const char *s = config->filename_pattern;

    while (*s && strlen(buf) < bufsz - 1) {
        if (*s == '%' && *(s + 1)) {
            CounterSpec cs = {0};
            if (!counter_used && parse_counter_at(s + 1, &cs) &&
                !(cs.token_len == 1 && *(s + 1) == 'd' && is_day_token(config->filename_pattern, s + 1))) {
                char tmp[32];
                format_counter(counter_val, cs.width, tmp, sizeof(tmp));
                append_str(buf, bufsz, tmp);
                counter_used = 1;
                s += 1 + cs.token_len;
                prev_was_m = 0;
                continue;
            }

            char tmp[32] = {0};
            char c = *(s + 1);
            switch (c) {
                case 'Y': snprintf(tmp, sizeof(tmp), "%04d", tm->tm_year + 1900); prev_was_m = 0; break;
                case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); prev_was_m = 1; break;
                case 'd':
                    if (prev_was_m) {
                        snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday);
                        prev_was_m = 0;
                    } else if (!counter_used) {
                        format_counter(counter_val, 0, tmp, sizeof(tmp));
                        counter_used = 1;
                        s += 2;
                        continue;
                    } else {
                        tmp[0] = '%'; tmp[1] = 'd'; tmp[2] = '\0';
                    }
                    break;
                case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); prev_was_m = 0; break;
                case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); prev_was_m = 0; break;
                case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); prev_was_m = 0; break;
                case 'n':
                    if (!counter_used) {
                        format_counter(counter_val, 4, tmp, sizeof(tmp));
                        counter_used = 1;
                    }
                    prev_was_m = 0;
                    break;
                case '%': snprintf(tmp, sizeof(tmp), "%%"); prev_was_m = 0; break;
                default:
                    tmp[0] = '%'; tmp[1] = c; tmp[2] = '\0';
                    prev_was_m = 0;
                    break;
            }
            append_str(buf, bufsz, tmp);
            s += 2;
        } else {
            char ch[2] = { *s, '\0' };
            append_str(buf, bufsz, ch);
            prev_was_m = 0;
            s++;
        }
    }
}

/**
 * @file upload.c
 * @brief HTTP upload via libcurl (Imgur anonymous + custom multipart POST).
 */

#include "upload.h"

#include "../utils/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#ifdef SNAPX_HAVE_UPLOAD
#  include <curl/curl.h>
#endif

static volatile int g_upload_busy;
static volatile int g_upload_cancel;

#ifdef SNAPX_HAVE_UPLOAD

typedef struct {
    SnapxEncodedImage  enc;
    SnapxConfig        config;
    SnapxUploadDoneFn  done;
    gpointer           userdata;
    char               url[SNAPX_UPLOAD_URL_MAX];
    char               err[SNAPX_UPLOAD_ERR_MAX];
    int                ok;
} UploadJob;

typedef struct {
    char  *data;
    size_t size;
} CurlBuf;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    CurlBuf *b = userdata;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->size + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

static int extract_json_link(const char *json, char *url, size_t urlsz)
{
    const char *key = "\"link\":\"";
    const char *p = strstr(json, key);
    if (!p) {
        key = "\"link\": \"";
        p = strstr(json, key);
    }
    if (!p) return -1;
    p += strlen(key);
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= urlsz) return -1;
    memcpy(url, p, (size_t)(end - p));
    url[end - p] = '\0';
    return 0;
}

static int extract_json_field(const char *json, const char *field,
                               char *out, size_t outsz)
{
    char pattern[128];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\":\"", field);
    if (n < 0 || (size_t)n >= sizeof(pattern)) return -1;
    const char *p = strstr(json, pattern);
    if (!p) {
        n = snprintf(pattern, sizeof(pattern), "\"%s\": \"", field);
        if (n < 0 || (size_t)n >= sizeof(pattern)) return -1;
        p = strstr(json, pattern);
    }
    if (!p) return -1;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= outsz) return -1;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return 0;
}

static int do_upload(UploadJob *job, char *url, size_t urlsz,
                     char *err, size_t errsz)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, errsz, "curl init failed");
        return -1;
    }

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    const char *field = "image";
    const char *post_url = "https://api.imgur.com/3/image";
    struct curl_slist *headers = NULL;

    if (job->config.upload_service == SNAPX_UPLOAD_CUSTOM) {
        if (!job->config.upload_custom_url[0]) {
            snprintf(err, errsz, "Custom upload URL not configured");
            curl_mime_free(mime);
            curl_easy_cleanup(curl);
            return -1;
        }
        post_url = job->config.upload_custom_url;
        field = job->config.upload_custom_field[0]
                ? job->config.upload_custom_field : "file";
    } else if (job->config.upload_service == SNAPX_UPLOAD_IMGUR) {
        if (!job->config.upload_imgur_client_id[0]) {
            snprintf(err, errsz, "Imgur client ID not configured");
            curl_mime_free(mime);
            curl_easy_cleanup(curl);
            return -1;
        }
        char auth[SNAPX_CONFIG_MAX_UPLOAD + 32];
        snprintf(auth, sizeof(auth), "Authorization: Client-ID %s",
                 job->config.upload_imgur_client_id);
        headers = curl_slist_append(headers, auth);
    } else {
        snprintf(err, errsz, "Upload service disabled");
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_mime_name(part, field);
    curl_mime_data(part, (const char *)job->enc.data,
                   job->enc.size);
    curl_mime_type(part, job->enc.mime ? job->enc.mime : "image/png");
    curl_mime_filename(part, "snapx.png");

    CurlBuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, post_url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "snapx/1.3");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (rc != CURLE_OK) {
        snprintf(err, errsz, "%s", curl_easy_strerror(rc));
        free(resp.data);
        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return -1;
    }

    if (http_code < 200 || http_code >= 300) {
        snprintf(err, errsz, "HTTP %ld: %.120s", http_code,
                 resp.data ? resp.data : "");
        free(resp.data);
        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return -1;
    }

    int ok = -1;
    if (job->config.upload_service == SNAPX_UPLOAD_IMGUR) {
        ok = extract_json_link(resp.data ? resp.data : "", url, urlsz);
        if (ok != 0)
            snprintf(err, errsz, "Could not parse Imgur response");
    } else {
        if (job->config.upload_custom_url_field[0]) {
            ok = extract_json_field(resp.data ? resp.data : "",
                                    job->config.upload_custom_url_field,
                                    url, urlsz);
        } else if (resp.data && resp.data[0]) {
            /* Plain-text URL response */
            size_t len = strlen(resp.data);
            while (len > 0 && (resp.data[len-1] == '\n' || resp.data[len-1] == '\r'))
                resp.data[--len] = '\0';
            snprintf(url, urlsz, "%s", resp.data);
            ok = 0;
        } else {
            snprintf(err, errsz, "Empty upload response");
        }
    }

    free(resp.data);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return ok;
}

static gboolean upload_invoke(gpointer data)
{
    UploadJob *job = data;
    int cancelled = g_upload_cancel;
    g_upload_cancel = 0;
    g_upload_busy   = 0;

    if (job->done) {
        if (cancelled)
            job->done(0, NULL, "Upload cancelled", job->userdata);
        else
            job->done(job->ok, job->ok ? job->url : NULL,
                      job->ok ? NULL : job->err, job->userdata);
    }
    snapx_encoded_image_free(&job->enc);
    free(job);
    return G_SOURCE_REMOVE;
}

static gpointer upload_thread(gpointer data)
{
    UploadJob *job = data;
    job->ok = do_upload(job, job->url, sizeof(job->url),
                        job->err, sizeof(job->err));
    g_main_context_invoke(g_main_context_default(), upload_invoke, job);
    return NULL;
}

#endif /* SNAPX_HAVE_UPLOAD */

SnapxUploadStatus snapx_upload_get_status(const SnapxConfig *cfg)
{
#ifndef SNAPX_HAVE_UPLOAD
    (void)cfg;
    return SNAPX_UPLOAD_STATUS_NOT_BUILT;
#else
    if (!cfg || cfg->upload_service == SNAPX_UPLOAD_NONE)
        return SNAPX_UPLOAD_STATUS_NOT_CONFIGURED;
    if (cfg->upload_service == SNAPX_UPLOAD_IMGUR &&
        !cfg->upload_imgur_client_id[0])
        return SNAPX_UPLOAD_STATUS_NOT_CONFIGURED;
    if (cfg->upload_service == SNAPX_UPLOAD_CUSTOM &&
        !cfg->upload_custom_url[0])
        return SNAPX_UPLOAD_STATUS_NOT_CONFIGURED;
    return SNAPX_UPLOAD_STATUS_AVAILABLE;
#endif
}

void snapx_upload_status_message(const SnapxConfig *cfg, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return;
    switch (snapx_upload_get_status(cfg)) {
    case SNAPX_UPLOAD_STATUS_NOT_BUILT:
        snprintf(buf, bufsz, "Upload: not installed (rebuild with libcurl)");
        break;
    case SNAPX_UPLOAD_STATUS_NOT_CONFIGURED:
        snprintf(buf, bufsz, "Upload: not configured — open Settings");
        break;
    default:
        if (g_upload_busy)
            snprintf(buf, bufsz, "Upload: uploading…");
        else
            snprintf(buf, bufsz, "Upload: ready");
        break;
    }
}

int snapx_upload_busy(void)
{
    return g_upload_busy ? 1 : 0;
}

void snapx_upload_cancel(void)
{
    g_upload_cancel = 1;
}

int snapx_upload_sync(const SnapxEncodedImage *enc, const SnapxConfig *config,
                      char *url_out, size_t url_sz,
                      char *err_out, size_t err_sz)
{
#ifdef SNAPX_HAVE_UPLOAD
    UploadJob job = {0};
    if (!enc || !config) return -1;
    job.enc   = *enc;
    job.config = *config;
    return do_upload(&job, url_out, url_sz, err_out, err_sz);
#else
    (void)enc; (void)config; (void)url_out; (void)url_sz;
    if (err_out && err_sz) snprintf(err_out, err_sz, "Upload not compiled in");
    return -1;
#endif
}

int snapx_upload_available(void)
{
#ifdef SNAPX_HAVE_UPLOAD
    return 1;
#else
    return 0;
#endif
}

void snapx_upload_async(const SnapxEncodedImage *enc,
                        const SnapxConfig *config,
                        SnapxUploadDoneFn done,
                        gpointer userdata)
{
#ifdef SNAPX_HAVE_UPLOAD
    if (!enc || !config || !done) return;

    UploadJob *job = calloc(1, sizeof(*job));
    if (!job) {
        done(0, NULL, "Out of memory", userdata);
        return;
    }

    job->enc.data = malloc(enc->size);
    if (!job->enc.data) {
        free(job);
        done(0, NULL, "Out of memory", userdata);
        return;
    }
    memcpy(job->enc.data, enc->data, enc->size);
    job->enc.size = enc->size;
    job->enc.mime = enc->mime;
    job->config   = *config;
    job->done     = done;
    job->userdata = userdata;

    g_upload_cancel = 0;
    g_upload_busy   = 1;
    GThread *t = g_thread_new("snapx-upload", upload_thread, job);
    g_thread_unref(t);
#else
    (void)enc; (void)config; (void)userdata;
    if (done)
        done(0, NULL, "Upload support not compiled in", userdata);
#endif
}

/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_pal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <curl/curl.h>

/*============================================================================
 * Tier 1: Mandatory primitives
 *==========================================================================*/

void *boat_malloc(size_t size) { return malloc(size); }
void  boat_free(void *ptr)    { free(ptr); }

/*--- Storage (file-based) ---*/
static const char *storage_dir = ".boat_storage";

static void ensure_storage_dir(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", storage_dir);
    system(cmd);
}

static void storage_path(const char *name, char *path, size_t cap)
{
    snprintf(path, cap, "%s/%s", storage_dir, name);
}

BoatResult boat_storage_write(const char *name, const uint8_t *data, size_t len)
{
    if (!name || !data) return BOAT_ERROR_ARG_NULL;
    ensure_storage_dir();
    char path[256];
    storage_path(name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return BOAT_ERROR_STORAGE_WRITE;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return (written == len) ? BOAT_SUCCESS : BOAT_ERROR_STORAGE_WRITE;
}

BoatResult boat_storage_read(const char *name, uint8_t *data, size_t cap, size_t *out_len)
{
    if (!name || !data) return BOAT_ERROR_ARG_NULL;
    char path[256];
    storage_path(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return BOAT_ERROR_STORAGE_NOT_FOUND;
    size_t rd = fread(data, 1, cap, f);
    fclose(f);
    if (out_len) *out_len = rd;
    return BOAT_SUCCESS;
}

BoatResult boat_storage_delete(const char *name)
{
    if (!name) return BOAT_ERROR_ARG_NULL;
    char path[256];
    storage_path(name, path, sizeof(path));
    return (remove(path) == 0) ? BOAT_SUCCESS : BOAT_ERROR_STORAGE_NOT_FOUND;
}

/*--- Time ---*/
uint64_t boat_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void boat_sleep_ms(uint32_t ms)
{
    usleep((useconds_t)ms * 1000);
}

/*--- Random ---*/
BoatResult boat_random(uint8_t *buf, size_t len)
{
    if (!buf) return BOAT_ERROR_ARG_NULL;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return BOAT_ERROR;
    size_t rd = fread(buf, 1, len, f);
    fclose(f);
    return (rd == len) ? BOAT_SUCCESS : BOAT_ERROR;
}

/*--- Mutex (pthread) ---*/
struct BoatMutex {
    pthread_mutex_t mtx;
};

BoatResult boat_mutex_init(BoatMutex **mutex)
{
    if (!mutex) return BOAT_ERROR_ARG_NULL;
    BoatMutex *m = (BoatMutex *)malloc(sizeof(BoatMutex));
    if (!m) return BOAT_ERROR_MEM_ALLOC;
    if (pthread_mutex_init(&m->mtx, NULL) != 0) {
        free(m);
        return BOAT_ERROR;
    }
    *mutex = m;
    return BOAT_SUCCESS;
}

BoatResult boat_mutex_lock(BoatMutex *mutex)
{
    if (!mutex) return BOAT_ERROR_ARG_NULL;
    return (pthread_mutex_lock(&mutex->mtx) == 0) ? BOAT_SUCCESS : BOAT_ERROR;
}

BoatResult boat_mutex_unlock(BoatMutex *mutex)
{
    if (!mutex) return BOAT_ERROR_ARG_NULL;
    return (pthread_mutex_unlock(&mutex->mtx) == 0) ? BOAT_SUCCESS : BOAT_ERROR;
}

void boat_mutex_destroy(BoatMutex *mutex)
{
    if (mutex) {
        pthread_mutex_destroy(&mutex->mtx);
        free(mutex);
    }
}

/*============================================================================
 * Tier 2: HTTP service contract (curl-based)
 *==========================================================================*/

static const BoatHttpOps *g_http_ops = NULL;

void boat_set_http_ops(const BoatHttpOps *ops) { g_http_ops = ops; }
const BoatHttpOps *boat_get_http_ops(void)     { return g_http_ops; }

/*--- curl write callback ---*/
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} CurlBuf;

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    CurlBuf *buf = (CurlBuf *)userp;
    if (buf->len + total >= buf->cap) {
        size_t new_cap = (buf->cap + total) * 2;
        uint8_t *new_data = (uint8_t *)realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* Parse extra_headers string ("Key: Val\r\nKey2: Val2\r\n") into curl_slist.
 * Handles both \r\n and \n line endings, strips trailing \r. */
static struct curl_slist *parse_extra_headers(const char *extra_headers)
{
    struct curl_slist *headers = NULL;
    if (!extra_headers || !extra_headers[0]) return NULL;

    char *dup = strdup(extra_headers);
    char *saveptr = NULL;
    char *line = strtok_r(dup, "\n", &saveptr);
    while (line) {
        /* Strip trailing \r */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';
        if (len > 0)
            headers = curl_slist_append(headers, line);
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(dup);
    return headers;
}

static BoatResult curl_post(const char *url, const char *content_type,
                            const uint8_t *body, size_t body_len,
                            const char *extra_headers,
                            BoatHttpResponse *response)
{
    CURL *curl = curl_easy_init();
    if (!curl) return BOAT_ERROR_HTTP_FAIL;

    CurlBuf buf = { .data = (uint8_t *)malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.data) { curl_easy_cleanup(curl); return BOAT_ERROR_MEM_ALLOC; }

    struct curl_slist *headers = NULL;
    char ct_header[128];
    snprintf(ct_header, sizeof(ct_header), "Content-Type: %s",
             content_type ? content_type : "application/json");
    headers = curl_slist_append(headers, ct_header);

    struct curl_slist *extra = parse_extra_headers(extra_headers);
    if (extra) {
        /* Append extra headers to the list */
        struct curl_slist *p = extra;
        while (p) { headers = curl_slist_append(headers, p->data); p = p->next; }
        curl_slist_free_all(extra);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "localhost,127.0.0.1");

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return BOAT_ERROR_HTTP_FAIL;
    }

    response->data = buf.data;
    response->len = buf.len;
    return BOAT_SUCCESS;
}

static BoatResult curl_get(const char *url, const char *extra_headers,
                           BoatHttpResponse *response)
{
    CURL *curl = curl_easy_init();
    if (!curl) return BOAT_ERROR_HTTP_FAIL;

    CurlBuf buf = { .data = (uint8_t *)malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.data) { curl_easy_cleanup(curl); return BOAT_ERROR_MEM_ALLOC; }

    struct curl_slist *headers = parse_extra_headers(extra_headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "localhost,127.0.0.1");

    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    /* x402 v2: payment requirements may be in PAYMENT-REQUIRED header (base64 JSON)
     * instead of the response body. Decode and use as body for 402 responses. */
    if (res == CURLE_OK && http_code == 402) {
        struct curl_header *h = NULL;
        if (curl_easy_header(curl, "payment-required", 0, CURLH_HEADER, -1, &h) == CURLHE_OK
            && h && h->value && strlen(h->value) > 10) {
            /* Base64 decode the header value into the response buffer */
            const char *b64 = h->value;
            size_t b64_len = strlen(b64);
            /* Estimate decoded size (3/4 of base64 length) */
            size_t dec_cap = (b64_len * 3) / 4 + 4;
            uint8_t *decoded = (uint8_t *)malloc(dec_cap + 1);
            if (decoded) {
                /* Simple base64 decode */
                static const uint8_t b64_lut[256] = {
                    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
                    ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
                    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
                    ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
                    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
                    ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
                    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
                    ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63
                };
                size_t di = 0;
                for (size_t i = 0; i < b64_len; i += 4) {
                    uint32_t a = (i < b64_len) ? b64_lut[(uint8_t)b64[i]] : 0;
                    uint32_t b = (i+1 < b64_len) ? b64_lut[(uint8_t)b64[i+1]] : 0;
                    uint32_t c = (i+2 < b64_len) ? b64_lut[(uint8_t)b64[i+2]] : 0;
                    uint32_t d = (i+3 < b64_len) ? b64_lut[(uint8_t)b64[i+3]] : 0;
                    uint32_t triple = (a << 18) | (b << 12) | (c << 6) | d;
                    if (di < dec_cap) decoded[di++] = (triple >> 16) & 0xFF;
                    if (di < dec_cap && b64[i+2] != '=') decoded[di++] = (triple >> 8) & 0xFF;
                    if (di < dec_cap && b64[i+3] != '=') decoded[di++] = triple & 0xFF;
                }
                decoded[di] = '\0';
                /* Replace body with decoded header content */
                free(buf.data);
                buf.data = decoded;
                buf.len = di;
            }
        }
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return BOAT_ERROR_HTTP_FAIL;
    }

    response->data = buf.data;
    response->len = buf.len;

    if (http_code == 402) return BOAT_ERROR_HTTP_402;
    return BOAT_SUCCESS;
}

static void curl_free_response(BoatHttpResponse *response)
{
    if (response && response->data) {
        free(response->data);
        response->data = NULL;
        response->len = 0;
    }
}

static const BoatHttpOps s_curl_http_ops = {
    .post          = curl_post,
    .get           = curl_get,
    .free_response = curl_free_response
};

const BoatHttpOps *boat_pal_linux_default_http_ops(void)
{
    return &s_curl_http_ops;
}

BoatResult boat_pal_linux_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    boat_set_http_ops(&s_curl_http_ops);
    return BOAT_SUCCESS;
}

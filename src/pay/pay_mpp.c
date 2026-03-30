/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * MPP (Machine Payments Protocol) — Client-side envelope
 * Handles HTTP 402 challenge parsing, credential building, receipt parsing.
 *****************************************************************************/
#include "boat_pay.h"
#include "boat_pal.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#if BOAT_PAY_MPP_ENABLED && BOAT_EVM_ENABLED

/*============================================================================
 * Base64url encode/decode (RFC 4648 §5, no padding)
 *==========================================================================*/

static const char b64url_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t boat_base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    size_t out_len = 4 * ((in_len + 2) / 3);
    /* Strip padding: subtract for incomplete trailing groups */
    size_t mod = in_len % 3;
    if (mod == 1) out_len -= 2;
    else if (mod == 2) out_len -= 1;

    if (out_len + 1 > out_cap) return 0;

    size_t i, j;
    for (i = 0, j = 0; i + 2 < in_len; i += 3) {
        uint32_t triple = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[j++] = b64url_table[(triple >> 18) & 0x3F];
        out[j++] = b64url_table[(triple >> 12) & 0x3F];
        out[j++] = b64url_table[(triple >> 6) & 0x3F];
        out[j++] = b64url_table[triple & 0x3F];
    }
    if (mod == 1) {
        uint32_t triple = (uint32_t)in[i] << 16;
        out[j++] = b64url_table[(triple >> 18) & 0x3F];
        out[j++] = b64url_table[(triple >> 12) & 0x3F];
    } else if (mod == 2) {
        uint32_t triple = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8);
        out[j++] = b64url_table[(triple >> 18) & 0x3F];
        out[j++] = b64url_table[(triple >> 12) & 0x3F];
        out[j++] = b64url_table[(triple >> 6) & 0x3F];
    }
    out[j] = '\0';
    return j;
}

static const uint8_t b64url_lut[256] = {
    ['A']=0, ['B']=1, ['C']=2, ['D']=3, ['E']=4, ['F']=5, ['G']=6, ['H']=7,
    ['I']=8, ['J']=9, ['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
    ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
    ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['-']=62,['_']=63,
    /* Also accept standard base64 chars for interop */
    ['+']=62,['/']=63
};

BoatResult boat_base64url_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!in || !out || !out_len) return BOAT_ERROR_ARG_NULL;

    /* Strip trailing padding if present */
    while (in_len > 0 && in[in_len - 1] == '=') in_len--;

    /* Compute output size */
    size_t dec_len = (in_len * 3) / 4;
    if (dec_len > out_cap) return BOAT_ERROR_MEM_OVERFLOW;

    size_t di = 0;
    size_t i = 0;
    for (; i + 3 < in_len; i += 4) {
        uint32_t a = b64url_lut[(uint8_t)in[i]];
        uint32_t b = b64url_lut[(uint8_t)in[i+1]];
        uint32_t c = b64url_lut[(uint8_t)in[i+2]];
        uint32_t d = b64url_lut[(uint8_t)in[i+3]];
        uint32_t triple = (a << 18) | (b << 12) | (c << 6) | d;
        out[di++] = (triple >> 16) & 0xFF;
        out[di++] = (triple >> 8) & 0xFF;
        out[di++] = triple & 0xFF;
    }
    /* Handle remainder (2 or 3 chars without padding) */
    size_t rem = in_len - i;
    if (rem == 2) {
        uint32_t a = b64url_lut[(uint8_t)in[i]];
        uint32_t b = b64url_lut[(uint8_t)in[i+1]];
        out[di++] = ((a << 2) | (b >> 4)) & 0xFF;
    } else if (rem == 3) {
        uint32_t a = b64url_lut[(uint8_t)in[i]];
        uint32_t b = b64url_lut[(uint8_t)in[i+1]];
        uint32_t c = b64url_lut[(uint8_t)in[i+2]];
        out[di++] = ((a << 2) | (b >> 4)) & 0xFF;
        out[di++] = ((b << 4) | (c >> 2)) & 0xFF;
    }

    *out_len = di;
    return BOAT_SUCCESS;
}

/*============================================================================
 * WWW-Authenticate: Payment header parsing
 *==========================================================================*/

/* Find the next occurrence of a header name in raw headers (case-insensitive).
 * Returns pointer to the value (after ": "), or NULL. Sets *value_len. */
static const char *find_header(const char *headers, size_t headers_len,
                               const char *name, size_t *value_len)
{
    size_t name_len = strlen(name);
    const char *p = headers;
    const char *end = headers + headers_len;

    while (p < end) {
        /* Find end of line */
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;

        size_t line_len = (size_t)(eol - p);
        /* Strip trailing \r */
        size_t stripped = line_len;
        if (stripped > 0 && p[stripped - 1] == '\r') stripped--;

        /* Check if line starts with "name:" (case-insensitive) */
        if (stripped > name_len + 1) {
            bool match = true;
            for (size_t i = 0; i < name_len && match; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) match = false;
            }
            if (match && p[name_len] == ':') {
                const char *val = p + name_len + 1;
                while (val < p + stripped && *val == ' ') val++;
                *value_len = (size_t)((p + stripped) - val);
                return val;
            }
        }
        p = eol + 1;
    }
    return NULL;
}

/* Parse a quoted string parameter from auth-param format.
 * Input: ptr to start of params (after "Payment "), len = remaining length.
 * Looks for: name="value" and copies value (unquoted) into out. */
static bool parse_auth_param(const char *params, size_t params_len,
                             const char *name, char *out, size_t out_cap)
{
    size_t name_len = strlen(name);
    const char *p = params;
    const char *end = params + params_len;

    while (p < end) {
        /* Skip whitespace and commas */
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t')) p++;
        if (p >= end) break;

        /* Check param name */
        if ((size_t)(end - p) > name_len + 2 &&
            memcmp(p, name, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            if (p < end && *p == '"') {
                /* Quoted string */
                p++;
                const char *start = p;
                while (p < end && *p != '"') p++;
                size_t vlen = (size_t)(p - start);
                if (vlen >= out_cap) vlen = out_cap - 1;
                memcpy(out, start, vlen);
                out[vlen] = '\0';
                return true;
            } else {
                /* Unquoted token */
                const char *start = p;
                while (p < end && *p != ',' && *p != ' ' && *p != '\t') p++;
                size_t vlen = (size_t)(p - start);
                if (vlen >= out_cap) vlen = out_cap - 1;
                memcpy(out, start, vlen);
                out[vlen] = '\0';
                return true;
            }
        }
        /* Skip to next param */
        while (p < end && *p != ',') {
            if (*p == '"') { p++; while (p < end && *p != '"') p++; }
            if (p < end) p++;
        }
    }
    return false;
}

BoatResult boat_mpp_parse_challenges(const char *headers, size_t headers_len,
                                     BoatMppChallenge *challenges, size_t max_challenges,
                                     size_t *n_challenges)
{
    if (!headers || !challenges || !n_challenges) return BOAT_ERROR_ARG_NULL;

    *n_challenges = 0;
    const char *search_pos = headers;
    size_t remaining = headers_len;

    while (*n_challenges < max_challenges) {
        size_t val_len = 0;
        const char *val = find_header(search_pos, remaining, "WWW-Authenticate", &val_len);
        if (!val) break;

        /* Check if it starts with "Payment " scheme */
        if (val_len > 8 && strncmp(val, "Payment ", 8) == 0) {
            BoatMppChallenge *ch = &challenges[*n_challenges];
            memset(ch, 0, sizeof(BoatMppChallenge));

            const char *params = val + 8;
            size_t params_len = val_len - 8;

            /* Extract required params */
            parse_auth_param(params, params_len, "id", ch->id, sizeof(ch->id));
            parse_auth_param(params, params_len, "realm", ch->realm, sizeof(ch->realm));
            parse_auth_param(params, params_len, "method", ch->method, sizeof(ch->method));
            parse_auth_param(params, params_len, "intent", ch->intent, sizeof(ch->intent));
            parse_auth_param(params, params_len, "request", ch->request_b64, sizeof(ch->request_b64));

            /* Extract optional params */
            parse_auth_param(params, params_len, "expires", ch->expires, sizeof(ch->expires));
            parse_auth_param(params, params_len, "description", ch->description, sizeof(ch->description));
            parse_auth_param(params, params_len, "digest", ch->digest, sizeof(ch->digest));
            parse_auth_param(params, params_len, "opaque", ch->opaque_b64, sizeof(ch->opaque_b64));

            /* Decode request JSON and extract charge fields */
            if (ch->request_b64[0] != '\0') {
                size_t req_b64_len = strlen(ch->request_b64);
                size_t dec_cap = (req_b64_len * 3) / 4 + 4;
                uint8_t *dec = (uint8_t *)boat_malloc(dec_cap + 1);
                if (dec) {
                    size_t dec_len = 0;
                    if (boat_base64url_decode(ch->request_b64, req_b64_len, dec, dec_cap, &dec_len) == BOAT_SUCCESS) {
                        dec[dec_len] = '\0';
                        cJSON *req_json = cJSON_Parse((const char *)dec);
                        if (req_json) {
                            cJSON *j;
                            j = cJSON_GetObjectItem(req_json, "amount");
                            if (j && cJSON_IsString(j))
                                strncpy(ch->amount, j->valuestring, sizeof(ch->amount) - 1);
                            else if (j && cJSON_IsNumber(j))
                                snprintf(ch->amount, sizeof(ch->amount), "%.0f", j->valuedouble);

                            j = cJSON_GetObjectItem(req_json, "currency");
                            if (j && cJSON_IsString(j))
                                strncpy(ch->currency, j->valuestring, sizeof(ch->currency) - 1);

                            j = cJSON_GetObjectItem(req_json, "recipient");
                            if (j && cJSON_IsString(j))
                                strncpy(ch->recipient, j->valuestring, sizeof(ch->recipient) - 1);

                            cJSON *md = cJSON_GetObjectItem(req_json, "methodDetails");
                            if (md) {
                                j = cJSON_GetObjectItem(md, "chainId");
                                if (j && cJSON_IsNumber(j))
                                    ch->chain_id = (uint64_t)j->valuedouble;
                            }
                            cJSON_Delete(req_json);
                        }
                    }
                    boat_free(dec);
                }
            }

            /* Validate required fields */
            if (ch->id[0] && ch->realm[0] && ch->method[0] && ch->intent[0]) {
                (*n_challenges)++;
            }
        }

        /* Advance past this header line for multi-header search */
        size_t consumed = (size_t)(val + val_len - search_pos);
        if (consumed >= remaining) break;
        search_pos = val + val_len;
        remaining -= consumed;
    }

    return (*n_challenges > 0) ? BOAT_SUCCESS : BOAT_ERROR_RPC_PARSE;
}

/*============================================================================
 * Credential building
 *==========================================================================*/

BoatResult boat_mpp_build_credential(const BoatMppChallenge *challenge,
                                     const char *source,
                                     const char *payload_json,
                                     char **credential_out)
{
    if (!challenge || !payload_json || !credential_out) return BOAT_ERROR_ARG_NULL;

    /* Build the credential JSON:
     * {
     *   "challenge": { "id":"...", "realm":"...", "method":"...", "intent":"...", "request":"..." },
     *   "source": "did:...",  (optional)
     *   "payload": { ... }
     * }
     * Note: challenge.request is echoed as the raw base64url string, not decoded.
     */
    char json_buf[4096];
    int n;

    if (source && source[0]) {
        n = snprintf(json_buf, sizeof(json_buf),
            "{\"challenge\":{\"id\":\"%s\",\"realm\":\"%s\",\"method\":\"%s\","
            "\"intent\":\"%s\",\"request\":\"%s\""
            "%s%s%s"  /* optional expires */
            "%s%s%s"  /* optional digest */
            "%s%s%s"  /* optional opaque */
            "},\"source\":\"%s\",\"payload\":%s}",
            challenge->id, challenge->realm, challenge->method,
            challenge->intent, challenge->request_b64,
            challenge->expires[0] ? ",\"expires\":\"" : "",
            challenge->expires[0] ? challenge->expires : "",
            challenge->expires[0] ? "\"" : "",
            challenge->digest[0] ? ",\"digest\":\"" : "",
            challenge->digest[0] ? challenge->digest : "",
            challenge->digest[0] ? "\"" : "",
            challenge->opaque_b64[0] ? ",\"opaque\":\"" : "",
            challenge->opaque_b64[0] ? challenge->opaque_b64 : "",
            challenge->opaque_b64[0] ? "\"" : "",
            source, payload_json);
    } else {
        n = snprintf(json_buf, sizeof(json_buf),
            "{\"challenge\":{\"id\":\"%s\",\"realm\":\"%s\",\"method\":\"%s\","
            "\"intent\":\"%s\",\"request\":\"%s\""
            "%s%s%s"
            "%s%s%s"
            "%s%s%s"
            "},\"payload\":%s}",
            challenge->id, challenge->realm, challenge->method,
            challenge->intent, challenge->request_b64,
            challenge->expires[0] ? ",\"expires\":\"" : "",
            challenge->expires[0] ? challenge->expires : "",
            challenge->expires[0] ? "\"" : "",
            challenge->digest[0] ? ",\"digest\":\"" : "",
            challenge->digest[0] ? challenge->digest : "",
            challenge->digest[0] ? "\"" : "",
            challenge->opaque_b64[0] ? ",\"opaque\":\"" : "",
            challenge->opaque_b64[0] ? challenge->opaque_b64 : "",
            challenge->opaque_b64[0] ? "\"" : "",
            payload_json);
    }

    if (n < 0 || (size_t)n >= sizeof(json_buf)) return BOAT_ERROR_MEM_OVERFLOW;

    /* Base64url encode */
    size_t b64_cap = (size_t)n * 2 + 16;
    char *b64 = (char *)boat_malloc(b64_cap);
    if (!b64) return BOAT_ERROR_MEM_ALLOC;
    size_t b64_len = boat_base64url_encode((const uint8_t *)json_buf, (size_t)n, b64, b64_cap);
    if (b64_len == 0) { boat_free(b64); return BOAT_ERROR; }

    /* Prepend "Payment " scheme prefix */
    size_t total = 8 + b64_len + 1;
    *credential_out = (char *)boat_malloc(total);
    if (!*credential_out) { boat_free(b64); return BOAT_ERROR_MEM_ALLOC; }
    memcpy(*credential_out, "Payment ", 8);
    memcpy(*credential_out + 8, b64, b64_len);
    (*credential_out)[8 + b64_len] = '\0';
    boat_free(b64);

    return BOAT_SUCCESS;
}

/*============================================================================
 * Receipt parsing
 *==========================================================================*/

BoatResult boat_mpp_parse_receipt(const char *headers, size_t headers_len,
                                  BoatMppReceipt *receipt)
{
    if (!receipt) return BOAT_ERROR_ARG_NULL;
    memset(receipt, 0, sizeof(BoatMppReceipt));

    if (!headers || headers_len == 0) return BOAT_SUCCESS; /* No receipt is not an error */

    size_t val_len = 0;
    const char *val = find_header(headers, headers_len, "Payment-Receipt", &val_len);
    if (!val || val_len == 0) return BOAT_SUCCESS;

    /* Decode base64url value */
    size_t dec_cap = (val_len * 3) / 4 + 4;
    uint8_t *dec = (uint8_t *)boat_malloc(dec_cap + 1);
    if (!dec) return BOAT_ERROR_MEM_ALLOC;

    size_t dec_len = 0;
    BoatResult r = boat_base64url_decode(val, val_len, dec, dec_cap, &dec_len);
    if (r != BOAT_SUCCESS) {
        /* Try parsing as raw JSON (some servers may not base64url-encode) */
        dec_len = val_len;
        if (dec_len > dec_cap) { boat_free(dec); return BOAT_ERROR_MEM_OVERFLOW; }
        memcpy(dec, val, dec_len);
    }
    dec[dec_len] = '\0';

    cJSON *root = cJSON_Parse((const char *)dec);
    boat_free(dec);
    if (!root) return BOAT_ERROR_RPC_PARSE;

    cJSON *j;
    j = cJSON_GetObjectItem(root, "status");
    if (j && cJSON_IsString(j)) strncpy(receipt->status, j->valuestring, sizeof(receipt->status) - 1);
    j = cJSON_GetObjectItem(root, "method");
    if (j && cJSON_IsString(j)) strncpy(receipt->method, j->valuestring, sizeof(receipt->method) - 1);
    j = cJSON_GetObjectItem(root, "reference");
    if (j && cJSON_IsString(j)) strncpy(receipt->reference, j->valuestring, sizeof(receipt->reference) - 1);
    j = cJSON_GetObjectItem(root, "timestamp");
    if (j && cJSON_IsString(j)) strncpy(receipt->timestamp, j->valuestring, sizeof(receipt->timestamp) - 1);

    cJSON_Delete(root);
    return BOAT_SUCCESS;
}

/*============================================================================
 * HTTP request helpers (mirrors x402 pattern)
 *==========================================================================*/

static BoatResult mpp_http_do(const char *url, const BoatPayReqOpts *opts,
                              const char *extra_hdr,
                              BoatHttpResponse *resp)
{
    const BoatHttpOps *http = boat_get_http_ops();
    if (!http) return BOAT_ERROR_HTTP_FAIL;

    /* Merge app headers + extra_hdr */
    char *merged = NULL;
    size_t app_len = (opts && opts->extra_headers) ? strlen(opts->extra_headers) : 0;
    size_t ext_len = extra_hdr ? strlen(extra_hdr) : 0;
    if (app_len + ext_len > 0) {
        merged = (char *)boat_malloc(app_len + ext_len + 4);
        if (!merged) return BOAT_ERROR_MEM_ALLOC;
        merged[0] = '\0';
        if (app_len > 0) memcpy(merged, opts->extra_headers, app_len);
        if (ext_len > 0) memcpy(merged + app_len, extra_hdr, ext_len);
        merged[app_len + ext_len] = '\0';
    }

    BoatResult r;
    bool is_post = opts && opts->method == BOAT_HTTP_POST;

    if (is_post) {
        if (!http->post) { boat_free(merged); return BOAT_ERROR_HTTP_FAIL; }
        const char *ct = (opts && opts->content_type) ? opts->content_type : "application/octet-stream";
        const uint8_t *body = (opts && opts->body) ? opts->body : NULL;
        size_t body_len = (opts && opts->body) ? opts->body_len : 0;
        r = http->post(url, ct, body, body_len, merged, resp);
    } else {
        if (!http->get) { boat_free(merged); return BOAT_ERROR_HTTP_FAIL; }
        r = http->get(url, merged, resp);
    }

    boat_free(merged);
    return r;
}

BoatResult boat_mpp_request(const char *url, const BoatPayReqOpts *opts,
                            BoatMppChallenge *challenge_out,
                            uint8_t **response, size_t *response_len)
{
    if (!url || !challenge_out || !response || !response_len) return BOAT_ERROR_ARG_NULL;

    *response = NULL;
    *response_len = 0;

    BoatHttpResponse resp = {0};
    BoatResult r = mpp_http_do(url, opts, NULL, &resp);

    /* 2xx: resource returned directly */
    if (r == BOAT_SUCCESS) {
        memset(challenge_out, 0, sizeof(BoatMppChallenge));
        *response = resp.data;
        *response_len = resp.len;
        /* Free headers, keep body — caller owns resp.data */
        if (resp.headers) { boat_free(resp.headers); }
        return BOAT_SUCCESS;
    }

    if (r != BOAT_ERROR_HTTP_402) {
        const BoatHttpOps *http = boat_get_http_ops();
        if (http) http->free_response(&resp);
        return r;
    }

    /* Parse MPP challenge from response headers */
    size_t n_challenges = 0;
    memset(challenge_out, 0, sizeof(BoatMppChallenge));

    if (resp.headers && resp.headers_len > 0) {
        r = boat_mpp_parse_challenges(resp.headers, resp.headers_len,
                                      challenge_out, 1, &n_challenges);
    } else {
        r = BOAT_ERROR_RPC_PARSE;
    }

    const BoatHttpOps *http = boat_get_http_ops();
    if (http) http->free_response(&resp);

    if (r != BOAT_SUCCESS || n_challenges == 0) {
        BOAT_LOG(BOAT_LOG_NORMAL, "MPP: 402 received but no valid WWW-Authenticate: Payment header found");
        return BOAT_ERROR_RPC_PARSE;
    }

    return BOAT_ERROR_HTTP_402;
}

BoatResult boat_mpp_pay_and_get(const char *url, const BoatPayReqOpts *opts,
                                const char *credential,
                                uint8_t **response, size_t *response_len,
                                BoatMppReceipt *receipt_out)
{
    if (!url || !credential || !response || !response_len) return BOAT_ERROR_ARG_NULL;

    /* Build Authorization: Payment header */
    size_t cred_len = strlen(credential);
    size_t hdr_cap = 16 + cred_len + 4;
    char *auth_hdr = (char *)boat_malloc(hdr_cap);
    if (!auth_hdr) return BOAT_ERROR_MEM_ALLOC;
    snprintf(auth_hdr, hdr_cap, "Authorization: %s\r\n", credential);

    BoatHttpResponse resp = {0};
    BoatResult r = mpp_http_do(url, opts, auth_hdr, &resp);
    boat_free(auth_hdr);

    if (r != BOAT_SUCCESS) {
        if (r == BOAT_ERROR_HTTP_402 && resp.data && resp.len > 0) {
            BOAT_LOG(BOAT_LOG_NORMAL, "MPP: server rejected payment: %.*s",
                     (int)(resp.len < 512 ? resp.len : 512), resp.data);
            *response = resp.data;
            *response_len = resp.len;
            if (resp.headers) boat_free(resp.headers);
            return r;
        }
        const BoatHttpOps *http = boat_get_http_ops();
        if (http) http->free_response(&resp);
        return r;
    }

    /* Parse receipt from response headers */
    if (receipt_out && resp.headers && resp.headers_len > 0) {
        boat_mpp_parse_receipt(resp.headers, resp.headers_len, receipt_out);
    }

    *response = resp.data;
    *response_len = resp.len;
    if (resp.headers) boat_free(resp.headers);
    return BOAT_SUCCESS;
}

#endif /* BOAT_PAY_MPP_ENABLED && BOAT_EVM_ENABLED */

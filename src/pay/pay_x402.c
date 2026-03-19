/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_pay.h"
#include "boat_pal.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#if BOAT_PAY_X402_ENABLED && BOAT_EVM_ENABLED

/* Base64 encode */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    size_t out_len = 4 * ((in_len + 2) / 3);
    if (out_len + 1 > out_cap) return 0;
    size_t i, j;
    for (i = 0, j = 0; i < in_len;) {
        uint32_t a = i < in_len ? in[i++] : 0;
        uint32_t b = i < in_len ? in[i++] : 0;
        uint32_t c = i < in_len ? in[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }
    size_t mod = in_len % 3;
    if (mod == 1) { out[j - 1] = '='; out[j - 2] = '='; }
    else if (mod == 2) { out[j - 1] = '='; }
    out[j] = '\0';
    return j;
}

/*--- USDC contract addresses per network ---*/
/* No longer hardcoded — asset address comes from the 402 response.
 * Kept for pay_nano.c and pay_gateway.c which still need known addresses. */
static const uint8_t USDC_BASE_MAINNET[20] = {
    0x83, 0x3e, 0x89, 0xb9, 0x07, 0x60, 0x5d, 0xf5, 0x6e, 0x7e,
    0x13, 0x63, 0xb9, 0xb5, 0x00, 0x3f, 0x5c, 0x56, 0x7a, 0x52
};
static const uint8_t USDC_BASE_SEPOLIA[20] = {
    0x03, 0x6C, 0xbD, 0x53, 0x84, 0x2c, 0x54, 0x26, 0x63, 0x4e,
    0x79, 0x29, 0x54, 0x1e, 0xC2, 0x31, 0x8f, 0x3d, 0xCF, 0x7e
};

/* Build EIP-712 domain from 402 response fields.
 * For batch scheme (GatewayWalletBatched), verifyingContract is the Gateway Wallet.
 * For standard exact scheme, verifyingContract is the USDC token (asset). */
static void build_domain_from_req(const BoatX402PaymentReq *req, uint64_t chain_id,
                                  BoatEip712Domain *domain)
{
    memset(domain, 0, sizeof(BoatEip712Domain));
    strncpy(domain->name, req->asset_name, sizeof(domain->name) - 1);
    strncpy(domain->version, req->asset_version, sizeof(domain->version) - 1);
    domain->chain_id = chain_id;
    if (req->has_verifying_contract) {
        memcpy(domain->verifying_contract, req->verifying_contract, 20);
    } else {
        memcpy(domain->verifying_contract, req->asset, 20);
    }
}

/*============================================================================
 * x402 Protocol Implementation
 *==========================================================================*/

/* Issue an HTTP request using the application's method/headers/body.
 * If extra_hdr is non-NULL it is appended after opts->extra_headers. */
static BoatResult x402_http_do(const char *url, const BoatX402ReqOpts *opts,
                               const char *extra_hdr,
                               BoatHttpResponse *resp)
{
    const BoatHttpOps *http = boat_get_http_ops();
    if (!http) return BOAT_ERROR_HTTP_FAIL;

    /* Merge app headers + extra_hdr into one string */
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

BoatResult boat_x402_request(const char *url, const BoatX402ReqOpts *opts,
                             BoatX402PaymentReq *req,
                             uint8_t **response, size_t *response_len)
{
    if (!url || !req || !response || !response_len) return BOAT_ERROR_ARG_NULL;

    *response = NULL;
    *response_len = 0;

    BoatHttpResponse resp = {0};
    BoatResult r = x402_http_do(url, opts, NULL, &resp);

    /* 2xx: resource returned directly, no payment needed */
    if (r == BOAT_SUCCESS) {
        memset(req, 0, sizeof(BoatX402PaymentReq));
        *response = resp.data;
        *response_len = resp.len;
        /* Caller owns resp.data */
        return BOAT_SUCCESS;
    }

    /* Not 402: propagate error */
    if (r != BOAT_ERROR_HTTP_402) {
        const BoatHttpOps *http = boat_get_http_ops();
        if (http) http->free_response(&resp);
        return r;
    }

    /* Parse 402 response JSON */
    cJSON *root = cJSON_Parse((const char *)resp.data);
    const BoatHttpOps *http = boat_get_http_ops();
    if (http) http->free_response(&resp);
    if (!root) return BOAT_ERROR_RPC_PARSE;

    memset(req, 0, sizeof(BoatX402PaymentReq));

    /* Detect x402 version */
    cJSON *ver = cJSON_GetObjectItem(root, "x402Version");
    req->x402_version = (ver && cJSON_IsNumber(ver)) ? ver->valueint : 1;

    cJSON *accepts = cJSON_GetObjectItem(root, "accepts");
    if (!accepts || !cJSON_IsArray(accepts) || cJSON_GetArraySize(accepts) == 0) {
        cJSON_Delete(root);
        return BOAT_ERROR_RPC_PARSE;
    }

    cJSON *first = cJSON_GetArrayItem(accepts, 0);

    cJSON *scheme = cJSON_GetObjectItem(first, "scheme");
    if (scheme && cJSON_IsString(scheme))
        strncpy(req->scheme, scheme->valuestring, sizeof(req->scheme) - 1);

    cJSON *network = cJSON_GetObjectItem(first, "network");
    if (network && cJSON_IsString(network))
        strncpy(req->network, network->valuestring, sizeof(req->network) - 1);

    /* x402 v1 uses "maxAmountRequired", v2 uses "amount" */
    cJSON *amount = cJSON_GetObjectItem(first, "maxAmountRequired");
    if (!amount) amount = cJSON_GetObjectItem(first, "amount");
    if (amount && cJSON_IsString(amount))
        strncpy(req->amount_str, amount->valuestring, sizeof(req->amount_str) - 1);
    else if (amount && cJSON_IsNumber(amount))
        snprintf(req->amount_str, sizeof(req->amount_str), "%lld", (long long)amount->valuedouble);

    cJSON *payTo = cJSON_GetObjectItem(first, "payTo");
    if (payTo && cJSON_IsString(payTo)) {
        size_t dummy;
        boat_hex_to_bin(payTo->valuestring, req->pay_to, 20, &dummy);
        strncpy(req->pay_to_hex, payTo->valuestring, sizeof(req->pay_to_hex) - 1);
    }

    cJSON *asset = cJSON_GetObjectItem(first, "asset");
    if (asset && cJSON_IsString(asset)) {
        size_t dummy;
        boat_hex_to_bin(asset->valuestring, req->asset, 20, &dummy);
        strncpy(req->asset_hex, asset->valuestring, sizeof(req->asset_hex) - 1);
    }

    cJSON *timeout = cJSON_GetObjectItem(first, "maxTimeoutSeconds");
    if (timeout) req->max_timeout = (uint32_t)timeout->valueint;

    cJSON *resource = cJSON_GetObjectItem(first, "resource");
    if (resource && cJSON_IsString(resource))
        strncpy(req->resource_url, resource->valuestring, sizeof(req->resource_url) - 1);

    /* EIP-712 domain parameters from extra field */
    cJSON *extra = cJSON_GetObjectItem(first, "extra");
    if (extra) {
        cJSON *ename = cJSON_GetObjectItem(extra, "name");
        if (ename && cJSON_IsString(ename))
            strncpy(req->asset_name, ename->valuestring, sizeof(req->asset_name) - 1);
        cJSON *eversion = cJSON_GetObjectItem(extra, "version");
        if (eversion && cJSON_IsString(eversion))
            strncpy(req->asset_version, eversion->valuestring, sizeof(req->asset_version) - 1);
        /* verifyingContract: Gateway Wallet for batch scheme, USDC for exact */
        cJSON *econtract = cJSON_GetObjectItem(extra, "verifyingContract");
        if (econtract && cJSON_IsString(econtract)) {
            size_t dummy;
            if (boat_hex_to_bin(econtract->valuestring, req->verifying_contract, 20, &dummy) == BOAT_SUCCESS
                && dummy == 20) {
                req->has_verifying_contract = true;
            }
        }
        /* v2: resource URL may be inside extra */
        if (req->resource_url[0] == '\0') {
            cJSON *eres = cJSON_GetObjectItem(extra, "resource");
            if (eres && cJSON_IsString(eres))
                strncpy(req->resource_url, eres->valuestring, sizeof(req->resource_url) - 1);
        }
    }

    /* If resource_url not in response, use the original URL */
    if (req->resource_url[0] == '\0')
        strncpy(req->resource_url, url, sizeof(req->resource_url) - 1);

    cJSON_Delete(root);
    return BOAT_ERROR_HTTP_402;
}

BoatResult boat_x402_make_payment(const BoatX402PaymentReq *req, const BoatKey *key,
                                  const BoatEvmChainConfig *chain,
                                  char **payment_b64)
{
    if (!req || !key || !chain || !payment_b64) return BOAT_ERROR_ARG_NULL;

    /* Get payer address */
    BoatKeyInfo info;
    BoatResult r = boat_key_get_info(key, &info);
    if (r != BOAT_SUCCESS) return r;

    /* Parse amount */
    uint64_t amount = (uint64_t)strtoull(req->amount_str, NULL, 10);

    /* Time bounds (per x402 SDK: validAfter = now-600, validBefore = now+maxTimeout).
     * For GatewayWalletBatched (nanopayments), validBefore must be >= 3 days per Circle docs. */
    uint64_t now = (uint64_t)time(NULL);
    uint64_t valid_after = now - 600;
    uint64_t valid_before = now + (uint64_t)req->max_timeout;
    /* Enforce 3-day minimum for batch scheme */
    uint64_t three_days = 3 * 24 * 3600;
    if (strcmp(req->asset_name, "GatewayWalletBatched") == 0 && valid_before < now + three_days) {
        valid_before = now + three_days + 3600; /* 3 days + 1 hour margin */
    }

    /* Random nonce */
    uint8_t nonce[32];
    boat_random(nonce, 32);

    /* Build EIP-3009 auth */
    BoatEip3009Auth auth;
    memset(&auth, 0, sizeof(auth));
    memcpy(auth.from, info.address, 20);
    memcpy(auth.to, req->pay_to, 20);
    /* value as uint256 */
    memset(auth.value, 0, 32);
    for (int i = 0; i < 8; i++) {
        auth.value[31 - i] = (uint8_t)(amount & 0xFF);
        amount >>= 8;
    }
    auth.valid_after = valid_after;
    auth.valid_before = valid_before;
    memcpy(auth.nonce, nonce, 32);

    /* EIP-712 domain from 402 response extra fields */
    BoatEip712Domain domain;
    build_domain_from_req(req, chain->chain_id, &domain);

    /* Sign */
    uint8_t sig65[65];
    r = boat_eip3009_sign(&auth, &domain, key, sig65);
    if (r != BOAT_SUCCESS) return r;

    /* x402: non-EIP-155 v values (27/28) */
    uint8_t v = sig65[64] + 27;

    /* Build payment payload JSON */
    char from_hex[43], to_hex[43], nonce_hex[67], sig_hex[133];
    boat_bin_to_hex(info.address, 20, from_hex, sizeof(from_hex), true);
    boat_bin_to_hex(req->pay_to, 20, to_hex, sizeof(to_hex), true);
    boat_bin_to_hex(nonce, 32, nonce_hex, sizeof(nonce_hex), true);

    /* Signature: 0x || r(32) || s(32) || v(1) = 0x + 130 hex chars */
    char sig_r_hex[67], sig_s_hex[67];
    boat_bin_to_hex(sig65, 32, sig_r_hex, sizeof(sig_r_hex), true);
    boat_bin_to_hex(sig65 + 32, 32, sig_s_hex, sizeof(sig_s_hex), true);
    snprintf(sig_hex, sizeof(sig_hex), "0x%s%s%02x", sig_r_hex + 2, sig_s_hex + 2, v);

    char payload[4096];
    int n;

    if (req->x402_version >= 2) {
        /* v2 format: { x402Version, resource, payload: {signature, authorization}, accepted: {<requirements>} } */
        const char *resource = req->resource_url[0] ? req->resource_url : "unknown";
        n = snprintf(payload, sizeof(payload),
            "{"
            "\"x402Version\":2,"
            "\"resource\":{\"url\":\"%s\",\"description\":\"\",\"mimeType\":\"application/json\"},"
            "\"payload\":{"
                "\"signature\":\"%s\","
                "\"authorization\":{"
                    "\"from\":\"%s\","
                    "\"to\":\"%s\","
                    "\"value\":\"%s\","
                    "\"validAfter\":\"%llu\","
                    "\"validBefore\":\"%llu\","
                    "\"nonce\":\"%s\""
                "}"
            "},"
            "\"accepted\":{"
                "\"scheme\":\"%s\","
                "\"network\":\"%s\","
                "\"asset\":\"%s\","
                "\"amount\":\"%s\","
                "\"payTo\":\"%s\","
                "\"maxTimeoutSeconds\":%u,"
                "\"extra\":{\"name\":\"%s\",\"version\":\"%s\"}"
            "}"
            "}",
            resource,
            sig_hex,
            from_hex, to_hex, req->amount_str,
            (unsigned long long)valid_after,
            (unsigned long long)valid_before,
            nonce_hex,
            req->scheme[0] ? req->scheme : "exact",
            req->network,
            req->asset_hex,
            req->amount_str,
            req->pay_to_hex,
            req->max_timeout,
            req->asset_name,
            req->asset_version);
    } else {
        /* v1 format: { x402Version, scheme, network, payload: {signature, authorization} } */
        n = snprintf(payload, sizeof(payload),
            "{"
            "\"x402Version\":1,"
            "\"scheme\":\"exact\","
            "\"network\":\"%s\","
            "\"payload\":{"
                "\"signature\":\"%s\","
                "\"authorization\":{"
                    "\"from\":\"%s\","
                    "\"to\":\"%s\","
                    "\"value\":\"%s\","
                    "\"validAfter\":\"%llu\","
                    "\"validBefore\":\"%llu\","
                    "\"nonce\":\"%s\""
                "}"
            "}"
            "}",
            req->network,
            sig_hex,
            from_hex, to_hex, req->amount_str,
            (unsigned long long)valid_after,
            (unsigned long long)valid_before,
            nonce_hex);
    }

    if (n < 0 || (size_t)n >= sizeof(payload)) return BOAT_ERROR_MEM_OVERFLOW;

    /* Base64 encode */
    size_t b64_cap = (size_t)n * 2 + 4;
    *payment_b64 = (char *)boat_malloc(b64_cap);
    if (!*payment_b64) return BOAT_ERROR_MEM_ALLOC;
    base64_encode((const uint8_t *)payload, (size_t)n, *payment_b64, b64_cap);

    return BOAT_SUCCESS;
}

BoatResult boat_x402_pay_and_get(const char *url, const BoatX402ReqOpts *opts,
                                 const char *payment_b64,
                                 uint8_t **response, size_t *response_len)
{
    if (!url || !payment_b64 || !response || !response_len) return BOAT_ERROR_ARG_NULL;

    /* Build payment headers: X-Payment (v1) + PAYMENT-SIGNATURE (v2) */
    size_t hdr_cap = strlen(payment_b64) * 2 + 80;
    char *xpay_hdr = (char *)boat_malloc(hdr_cap);
    if (!xpay_hdr) return BOAT_ERROR_MEM_ALLOC;
    snprintf(xpay_hdr, hdr_cap,
             "X-Payment: %s\r\nPAYMENT-SIGNATURE: %s\r\n",
             payment_b64, payment_b64);

    BoatHttpResponse resp = {0};
    BoatResult r = x402_http_do(url, opts, xpay_hdr, &resp);
    boat_free(xpay_hdr);

    if (r != BOAT_SUCCESS) {
        /* On 402 retry failure, return the error body so caller can diagnose */
        if (r == BOAT_ERROR_HTTP_402 && resp.data && resp.len > 0) {
            BOAT_LOG(BOAT_LOG_NORMAL, "x402 pay_and_get: server rejected payment: %.*s",
                     (int)(resp.len < 512 ? resp.len : 512), resp.data);
            *response = resp.data;
            *response_len = resp.len;
            /* Caller owns resp.data */
            return r;
        }
        const BoatHttpOps *http = boat_get_http_ops();
        if (http) http->free_response(&resp);
        return r;
    }

    *response = resp.data;
    *response_len = resp.len;
    /* Caller owns resp.data — do not free */
    return BOAT_SUCCESS;
}

BoatResult boat_x402_process(const char *url, const BoatX402ReqOpts *opts,
                             const BoatKey *key, const BoatEvmChainConfig *chain,
                             uint8_t **response, size_t *response_len)
{
    if (!url || !key || !chain || !response || !response_len) return BOAT_ERROR_ARG_NULL;

    /* Step 1: Send the application's original request */
    BoatX402PaymentReq req;
    BoatResult r = boat_x402_request(url, opts, &req, response, response_len);

    /* 2xx: resource already returned, no payment needed */
    if (r == BOAT_SUCCESS) return BOAT_SUCCESS;

    /* Not 402: propagate error */
    if (r != BOAT_ERROR_HTTP_402) return r;

    /* Step 2: Make payment */
    char *payment_b64 = NULL;
    r = boat_x402_make_payment(&req, key, chain, &payment_b64);
    if (r != BOAT_SUCCESS) return r;

    /* Step 3: Replay request with X-Payment header */
    r = boat_x402_pay_and_get(req.resource_url, opts, payment_b64, response, response_len);
    boat_free(payment_b64);
    return r;
}

#endif /* BOAT_PAY_X402_ENABLED && BOAT_EVM_ENABLED */

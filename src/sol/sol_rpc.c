/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_sol.h"
#include "boat_pal.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#if BOAT_SOL_ENABLED

/* Internal RPC call */
typedef struct { char url[256]; int req_id; } BoatRpcCtx;
extern BoatResult boat_rpc_ctx_init(BoatRpcCtx *ctx, const char *url);
extern BoatResult boat_rpc_call(BoatRpcCtx *ctx, const char *method, const char *params_json, char **result_json);

/* Base64 encode (simple implementation for Solana tx submission) */
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

    /* Padding */
    size_t mod = in_len % 3;
    if (mod == 1) { out[j - 1] = '='; out[j - 2] = '='; }
    else if (mod == 2) { out[j - 1] = '='; }
    out[j] = '\0';
    return j;
}

/* Base64 decode */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    if (in_len % 4 != 0) return 0;
    size_t out_len = in_len / 4 * 3;
    if (in_len > 0 && in[in_len - 1] == '=') out_len--;
    if (in_len > 1 && in[in_len - 2] == '=') out_len--;
    if (out_len > out_cap) return 0;

    size_t i, j;
    for (i = 0, j = 0; i < in_len;) {
        int a = b64_val(in[i++]);
        int b = b64_val(in[i++]);
        int c = (in[i] == '=') ? 0 : b64_val(in[i]); i++;
        int d = (in[i] == '=') ? 0 : b64_val(in[i]); i++;
        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        if (j < out_len) out[j++] = (triple >> 16) & 0xFF;
        if (j < out_len) out[j++] = (triple >> 8) & 0xFF;
        if (j < out_len) out[j++] = triple & 0xFF;
    }
    return out_len;
}

/*--- Commitment string ---*/
static const char *commitment_str(BoatSolCommitment c)
{
    switch (c) {
        case BOAT_SOL_COMMITMENT_FINALIZED: return "finalized";
        case BOAT_SOL_COMMITMENT_CONFIRMED: return "confirmed";
        case BOAT_SOL_COMMITMENT_PROCESSED: return "processed";
    }
    return "finalized";
}

/*----------------------------------------------------------------------------
 * RPC context
 *--------------------------------------------------------------------------*/
BoatResult boat_sol_rpc_init(BoatSolRpc *rpc, const char *url)
{
    if (!rpc || !url) return BOAT_ERROR_ARG_NULL;
    memset(rpc, 0, sizeof(BoatSolRpc));
    strncpy(rpc->url, url, sizeof(rpc->url) - 1);
    rpc->req_id = 1;
    return BOAT_SUCCESS;
}

void boat_sol_rpc_free(BoatSolRpc *rpc)
{
    if (rpc) memset(rpc, 0, sizeof(BoatSolRpc));
}

static BoatResult sol_rpc_call(BoatSolRpc *rpc, const char *method, const char *params, char **result)
{
    BoatRpcCtx ctx;
    boat_rpc_ctx_init(&ctx, rpc->url);
    ctx.req_id = rpc->req_id++;
    return boat_rpc_call(&ctx, method, params, result);
}

/*----------------------------------------------------------------------------
 * Typed RPC methods
 *--------------------------------------------------------------------------*/
BoatResult boat_sol_rpc_get_latest_blockhash(BoatSolRpc *rpc, BoatSolCommitment commitment,
                                             uint8_t blockhash[32], uint64_t *last_valid_height)
{
    if (!rpc || !blockhash) return BOAT_ERROR_ARG_NULL;

    char params[128];
    snprintf(params, sizeof(params), "[{\"commitment\":\"%s\"}]", commitment_str(commitment));

    char *result = NULL;
    BoatResult r = sol_rpc_call(rpc, "getLatestBlockhash", params, &result);
    if (r != BOAT_SUCCESS) return r;

    /* Parse JSON: {"value":{"blockhash":"...","lastValidBlockHeight":N}} */
    cJSON *root = cJSON_Parse(result);
    boat_free(result);
    if (!root) return BOAT_ERROR_RPC_PARSE;

    cJSON *value = cJSON_GetObjectItem(root, "value");
    if (!value) { cJSON_Delete(root); return BOAT_ERROR_RPC_PARSE; }

    cJSON *bh = cJSON_GetObjectItem(value, "blockhash");
    if (!bh || !cJSON_IsString(bh)) { cJSON_Delete(root); return BOAT_ERROR_RPC_PARSE; }

    /* Decode base58 blockhash */
    {
        size_t decoded_len = 0;
        if (boat_base58_decode(bh->valuestring, blockhash, 32, &decoded_len) != BOAT_SUCCESS || decoded_len != 32) {
            cJSON_Delete(root);
            return BOAT_ERROR_RPC_PARSE;
        }
    }

    if (last_valid_height) {
        cJSON *lvh = cJSON_GetObjectItem(value, "lastValidBlockHeight");
        *last_valid_height = lvh ? (uint64_t)lvh->valuedouble : 0;
    }

    cJSON_Delete(root);
    return BOAT_SUCCESS;
}

BoatResult boat_sol_rpc_get_balance(BoatSolRpc *rpc, const uint8_t pubkey[32], uint64_t *lamports)
{
    if (!rpc || !pubkey || !lamports) return BOAT_ERROR_ARG_NULL;

    /* Base58 encode the pubkey */
    char b58[64];
    if (boat_base58_encode(pubkey, 32, b58, sizeof(b58)) != BOAT_SUCCESS) return BOAT_ERROR;

    char params[128];
    snprintf(params, sizeof(params), "[\"%s\"]", b58);

    char *result = NULL;
    BoatResult r = sol_rpc_call(rpc, "getBalance", params, &result);
    if (r != BOAT_SUCCESS) return r;

    cJSON *root = cJSON_Parse(result);
    boat_free(result);
    if (!root) return BOAT_ERROR_RPC_PARSE;

    cJSON *value = cJSON_GetObjectItem(root, "value");
    *lamports = value ? (uint64_t)value->valuedouble : 0;
    cJSON_Delete(root);
    return BOAT_SUCCESS;
}

BoatResult boat_sol_rpc_get_token_balance(BoatSolRpc *rpc, const uint8_t ata[32],
                                          uint64_t *amount, uint8_t *decimals)
{
    if (!rpc || !ata || !amount) return BOAT_ERROR_ARG_NULL;

    /* Base58 encode ATA address */
    char b58[64];
    if (boat_base58_encode(ata, 32, b58, sizeof(b58)) != BOAT_SUCCESS) return BOAT_ERROR;

    char params[128];
    snprintf(params, sizeof(params), "[\"%s\"]", b58);

    char *result = NULL;
    BoatResult r = sol_rpc_call(rpc, "getTokenAccountBalance", params, &result);
    if (r != BOAT_SUCCESS) return r;

    cJSON *root = cJSON_Parse(result);
    boat_free(result);
    if (!root) return BOAT_ERROR_RPC_PARSE;

    cJSON *value = cJSON_GetObjectItem(root, "value");
    if (value) {
        cJSON *amt = cJSON_GetObjectItem(value, "amount");
        if (amt && cJSON_IsString(amt)) {
            *amount = (uint64_t)strtoull(amt->valuestring, NULL, 10);
        }
        if (decimals) {
            cJSON *dec = cJSON_GetObjectItem(value, "decimals");
            *decimals = dec ? (uint8_t)dec->valueint : 0;
        }
    }

    cJSON_Delete(root);
    return BOAT_SUCCESS;
}

BoatResult boat_sol_rpc_send_transaction(BoatSolRpc *rpc, const uint8_t *raw, size_t raw_len,
                                         uint8_t signature[64])
{
    if (!rpc || !raw || !signature) return BOAT_ERROR_ARG_NULL;

    /* Base64 encode the raw transaction */
    size_t b64_cap = raw_len * 2 + 4;
    char *b64 = (char *)boat_malloc(b64_cap);
    if (!b64) return BOAT_ERROR_MEM_ALLOC;
    base64_encode(raw, raw_len, b64, b64_cap);

    size_t params_cap = strlen(b64) + 128;
    char *params = (char *)boat_malloc(params_cap);
    if (!params) { boat_free(b64); return BOAT_ERROR_MEM_ALLOC; }
    snprintf(params, params_cap,
             "[\"%s\",{\"encoding\":\"base64\",\"preflightCommitment\":\"confirmed\"}]", b64);
    boat_free(b64);

    char *result = NULL;
    BoatResult r = sol_rpc_call(rpc, "sendTransaction", params, &result);
    boat_free(params);
    if (r != BOAT_SUCCESS) return r;

    /* Result is base58-encoded signature — decode to 64 bytes */
    {
        size_t sig_len = 0;
        if (boat_base58_decode(result, signature, 64, &sig_len) != BOAT_SUCCESS || sig_len != 64) {
            boat_free(result);
            return BOAT_ERROR_RPC_PARSE;
        }
    }

    boat_free(result);
    return BOAT_SUCCESS;
}

BoatResult boat_sol_rpc_get_signature_status(BoatSolRpc *rpc, const uint8_t sig[64],
                                             bool *confirmed, bool *finalized)
{
    if (!rpc || !sig) return BOAT_ERROR_ARG_NULL;

    /* Base58 encode signature */
    char b58[128];
    if (boat_base58_encode(sig, 64, b58, sizeof(b58)) != BOAT_SUCCESS) return BOAT_ERROR;

    char params[256];
    snprintf(params, sizeof(params), "[[\"%s\"],{\"searchTransactionHistory\":true}]", b58);

    char *result = NULL;
    BoatResult r = sol_rpc_call(rpc, "getSignatureStatuses", params, &result);
    if (r != BOAT_SUCCESS) return r;

    cJSON *root = cJSON_Parse(result);
    boat_free(result);
    if (!root) return BOAT_ERROR_RPC_PARSE;

    cJSON *value = cJSON_GetObjectItem(root, "value");
    if (value && cJSON_IsArray(value) && cJSON_GetArraySize(value) > 0) {
        cJSON *item = cJSON_GetArrayItem(value, 0);
        if (item && !cJSON_IsNull(item)) {
            cJSON *cs = cJSON_GetObjectItem(item, "confirmationStatus");
            if (cs && cJSON_IsString(cs)) {
                if (confirmed) *confirmed = (strcmp(cs->valuestring, "confirmed") == 0 ||
                                             strcmp(cs->valuestring, "finalized") == 0);
                if (finalized) *finalized = (strcmp(cs->valuestring, "finalized") == 0);
            }
        }
    }

    cJSON_Delete(root);
    return BOAT_SUCCESS;
}

BoatResult boat_sol_rpc_call(BoatSolRpc *rpc, const char *method, const char *params_json,
                             char **result_json)
{
    return sol_rpc_call(rpc, method, params_json, result_json);
}

#endif /* BOAT_SOL_ENABLED */

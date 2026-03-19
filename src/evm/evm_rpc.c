/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_evm.h"
#include "boat_pal.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#if BOAT_EVM_ENABLED

/* Internal RPC call — defined in boat_rpc.c */
typedef struct { char url[256]; int req_id; } BoatRpcCtx;
extern BoatResult boat_rpc_ctx_init(BoatRpcCtx *ctx, const char *url);
extern void       boat_rpc_ctx_free(BoatRpcCtx *ctx);
extern BoatResult boat_rpc_call(BoatRpcCtx *ctx, const char *method, const char *params_json, char **result_json);

/*----------------------------------------------------------------------------
 * EVM RPC context
 *--------------------------------------------------------------------------*/
BoatResult boat_evm_rpc_init(BoatEvmRpc *rpc, const char *url)
{
    if (!rpc || !url) return BOAT_ERROR_ARG_NULL;
    memset(rpc, 0, sizeof(BoatEvmRpc));
    strncpy(rpc->url, url, sizeof(rpc->url) - 1);
    rpc->req_id = 1;
    return BOAT_SUCCESS;
}

void boat_evm_rpc_free(BoatEvmRpc *rpc)
{
    if (rpc) memset(rpc, 0, sizeof(BoatEvmRpc));
}

/* Helper: call RPC using the BoatEvmRpc as a BoatRpcCtx */
static BoatResult evm_rpc_call(BoatEvmRpc *rpc, const char *method, const char *params, char **result)
{
    BoatRpcCtx ctx;
    boat_rpc_ctx_init(&ctx, rpc->url);
    ctx.req_id = rpc->req_id++;
    return boat_rpc_call(&ctx, method, params, result);
}

/* Helper: parse hex string result to uint64 */
static uint64_t hex_to_u64(const char *hex)
{
    if (!hex) return 0;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    uint64_t val = 0;
    while (*hex) {
        val <<= 4;
        char c = *hex++;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
    }
    return val;
}

/* Helper: parse hex string to bin (big-endian, right-aligned in output) */
static BoatResult hex_to_bin_padded(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex || !out) return BOAT_ERROR_ARG_NULL;
    memset(out, 0, out_len);
    size_t actual;
    /* Temp buffer */
    uint8_t tmp[32];
    BoatResult r = boat_hex_to_bin(hex, tmp, sizeof(tmp), &actual);
    if (r != BOAT_SUCCESS) return r;
    if (actual > out_len) return BOAT_ERROR_MEM_OVERFLOW;
    /* Right-align */
    memcpy(out + (out_len - actual), tmp, actual);
    return BOAT_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Typed RPC methods
 *--------------------------------------------------------------------------*/
BoatResult boat_evm_block_number(BoatEvmRpc *rpc, uint64_t *out)
{
    if (!rpc || !out) return BOAT_ERROR_ARG_NULL;
    char *result = NULL;
    BoatResult r = evm_rpc_call(rpc, "eth_blockNumber", "[]", &result);
    if (r != BOAT_SUCCESS) return r;
    *out = hex_to_u64(result);
    boat_free(result);
    return BOAT_SUCCESS;
}

BoatResult boat_evm_get_balance(BoatEvmRpc *rpc, const uint8_t addr[20], uint8_t out_wei[32])
{
    if (!rpc || !addr || !out_wei) return BOAT_ERROR_ARG_NULL;
    char addr_hex[43];
    boat_bin_to_hex(addr, 20, addr_hex, sizeof(addr_hex), true);
    char params[128];
    snprintf(params, sizeof(params), "[\"%s\",\"latest\"]", addr_hex);

    char *result = NULL;
    BoatResult r = evm_rpc_call(rpc, "eth_getBalance", params, &result);
    if (r != BOAT_SUCCESS) return r;
    r = hex_to_bin_padded(result, out_wei, 32);
    boat_free(result);
    return r;
}

BoatResult boat_evm_get_nonce(BoatEvmRpc *rpc, const uint8_t addr[20], uint64_t *out)
{
    if (!rpc || !addr || !out) return BOAT_ERROR_ARG_NULL;
    char addr_hex[43];
    boat_bin_to_hex(addr, 20, addr_hex, sizeof(addr_hex), true);
    char params[128];
    snprintf(params, sizeof(params), "[\"%s\",\"latest\"]", addr_hex);

    char *result = NULL;
    BoatResult r = evm_rpc_call(rpc, "eth_getTransactionCount", params, &result);
    if (r != BOAT_SUCCESS) return r;
    *out = hex_to_u64(result);
    boat_free(result);
    return BOAT_SUCCESS;
}

BoatResult boat_evm_gas_price(BoatEvmRpc *rpc, uint8_t out_wei[32])
{
    if (!rpc || !out_wei) return BOAT_ERROR_ARG_NULL;
    char *result = NULL;
    BoatResult r = evm_rpc_call(rpc, "eth_gasPrice", "[]", &result);
    if (r != BOAT_SUCCESS) return r;
    r = hex_to_bin_padded(result, out_wei, 32);
    boat_free(result);
    return r;
}

BoatResult boat_evm_send_raw_tx(BoatEvmRpc *rpc, const uint8_t *raw, size_t raw_len, uint8_t txhash[32])
{
    if (!rpc || !raw || !txhash) return BOAT_ERROR_ARG_NULL;
    /* Encode raw tx as 0x-prefixed hex */
    size_t hex_cap = raw_len * 2 + 3;
    char *hex = (char *)boat_malloc(hex_cap);
    if (!hex) return BOAT_ERROR_MEM_ALLOC;
    boat_bin_to_hex(raw, raw_len, hex, hex_cap, true);

    char *params = (char *)boat_malloc(hex_cap + 8);
    if (!params) { boat_free(hex); return BOAT_ERROR_MEM_ALLOC; }
    snprintf(params, hex_cap + 8, "[\"%s\"]", hex);
    boat_free(hex);

    char *result = NULL;
    BoatResult r = evm_rpc_call(rpc, "eth_sendRawTransaction", params, &result);
    boat_free(params);
    if (r != BOAT_SUCCESS) return r;

    /* Parse txhash from result hex string */
    size_t actual;
    r = boat_hex_to_bin(result, txhash, 32, &actual);
    boat_free(result);
    return r;
}

BoatResult boat_evm_eth_call(BoatEvmRpc *rpc, const uint8_t to[20],
                             const uint8_t *data, size_t data_len,
                             uint8_t **result_out, size_t *result_len)
{
    if (!rpc || !to || !data || !result_out || !result_len) return BOAT_ERROR_ARG_NULL;

    char to_hex[43];
    boat_bin_to_hex(to, 20, to_hex, sizeof(to_hex), true);

    size_t data_hex_cap = data_len * 2 + 3;
    char *data_hex = (char *)boat_malloc(data_hex_cap);
    if (!data_hex) return BOAT_ERROR_MEM_ALLOC;
    boat_bin_to_hex(data, data_len, data_hex, data_hex_cap, true);

    size_t params_cap = data_hex_cap + 128;
    char *params = (char *)boat_malloc(params_cap);
    if (!params) { boat_free(data_hex); return BOAT_ERROR_MEM_ALLOC; }
    snprintf(params, params_cap, "[{\"to\":\"%s\",\"data\":\"%s\"},\"latest\"]", to_hex, data_hex);
    boat_free(data_hex);

    char *result = NULL;
    BoatResult r = evm_rpc_call(rpc, "eth_call", params, &result);
    boat_free(params);
    if (r != BOAT_SUCCESS) return r;

    /* Decode hex result */
    size_t hex_len = strlen(result);
    if (hex_len >= 2 && result[0] == '0' && result[1] == 'x') hex_len -= 2;
    size_t bin_len = hex_len / 2;
    *result_out = (uint8_t *)boat_malloc(bin_len);
    if (!*result_out) { boat_free(result); return BOAT_ERROR_MEM_ALLOC; }
    r = boat_hex_to_bin(result, *result_out, bin_len, result_len);
    boat_free(result);
    return r;
}

#endif /* BOAT_EVM_ENABLED */

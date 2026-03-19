/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_evm.h"
#include "boat_pal.h"
#include "sha3.h"

#include <string.h>

#if BOAT_EVM_ENABLED

/*============================================================================
 * Minimal RLP encoder (self-contained, no boatiotsdk.h dependency)
 *==========================================================================*/

static BoatResult rlp_encode_length(size_t len, uint8_t offset, BoatBuf *out)
{
    if (len < 56) {
        return boat_buf_append_byte(out, (uint8_t)(offset + len));
    }
    /* Encode length of length */
    uint8_t len_bytes[8];
    int n = 0;
    size_t tmp = len;
    while (tmp > 0) {
        len_bytes[n++] = (uint8_t)(tmp & 0xFF);
        tmp >>= 8;
    }
    BoatResult r = boat_buf_append_byte(out, (uint8_t)(offset + 55 + n));
    if (r != BOAT_SUCCESS) return r;
    /* Big-endian */
    for (int i = n - 1; i >= 0; i--) {
        r = boat_buf_append_byte(out, len_bytes[i]);
        if (r != BOAT_SUCCESS) return r;
    }
    return BOAT_SUCCESS;
}

/* RLP encode a byte string */
static BoatResult rlp_encode_string(const uint8_t *data, size_t len, BoatBuf *out)
{
    if (len == 1 && data[0] < 0x80) {
        return boat_buf_append_byte(out, data[0]);
    }
    BoatResult r = rlp_encode_length(len, 0x80, out);
    if (r != BOAT_SUCCESS) return r;
    if (len > 0) {
        r = boat_buf_append(out, data, len);
    }
    return r;
}

/* RLP encode an integer (big-endian, no leading zeros) */
static BoatResult rlp_encode_uint(uint64_t val, BoatBuf *out)
{
    if (val == 0) {
        return rlp_encode_string((const uint8_t *)"", 0, out);
    }
    uint8_t buf[8];
    int n = 0;
    uint64_t tmp = val;
    while (tmp > 0) {
        buf[7 - n] = (uint8_t)(tmp & 0xFF);
        tmp >>= 8;
        n++;
    }
    return rlp_encode_string(buf + 8 - n, (size_t)n, out);
}

/* RLP encode a uint256 (32 bytes big-endian, strip leading zeros) */
static BoatResult rlp_encode_uint256(const uint8_t val[32], BoatBuf *out)
{
    /* Find first non-zero byte */
    int start = 0;
    while (start < 32 && val[start] == 0) start++;
    if (start == 32) {
        return rlp_encode_string((const uint8_t *)"", 0, out);
    }
    return rlp_encode_string(val + start, (size_t)(32 - start), out);
}

/* Wrap already-encoded items as an RLP list */
static BoatResult rlp_wrap_list(const uint8_t *items, size_t items_len, BoatBuf *out)
{
    BoatResult r = rlp_encode_length(items_len, 0xC0, out);
    if (r != BOAT_SUCCESS) return r;
    return boat_buf_append(out, items, items_len);
}

/*============================================================================
 * Transaction builder
 *==========================================================================*/

BoatResult boat_evm_tx_init(BoatEvmTx *tx, const BoatEvmChainConfig *chain)
{
    if (!tx || !chain) return BOAT_ERROR_ARG_NULL;
    memset(tx, 0, sizeof(BoatEvmTx));
    tx->chain = *chain;
    tx->gas_limit = 21000; /* default for simple transfer */
    return BOAT_SUCCESS;
}

BoatResult boat_evm_tx_set_nonce(BoatEvmTx *tx, uint64_t nonce)
{
    if (!tx) return BOAT_ERROR_ARG_NULL;
    tx->nonce = nonce;
    tx->nonce_set = true;
    return BOAT_SUCCESS;
}

BoatResult boat_evm_tx_set_gas_price(BoatEvmTx *tx, const uint8_t gas_price[32])
{
    if (!tx || !gas_price) return BOAT_ERROR_ARG_NULL;
    memcpy(tx->gas_price, gas_price, 32);
    tx->gas_price_set = true;
    return BOAT_SUCCESS;
}

BoatResult boat_evm_tx_set_gas_limit(BoatEvmTx *tx, uint64_t gas_limit)
{
    if (!tx) return BOAT_ERROR_ARG_NULL;
    tx->gas_limit = gas_limit;
    return BOAT_SUCCESS;
}

BoatResult boat_evm_tx_set_to(BoatEvmTx *tx, const uint8_t to[20])
{
    if (!tx || !to) return BOAT_ERROR_ARG_NULL;
    memcpy(tx->to, to, 20);
    tx->to_set = true;
    return BOAT_SUCCESS;
}

BoatResult boat_evm_tx_set_value(BoatEvmTx *tx, const uint8_t value[32])
{
    if (!tx || !value) return BOAT_ERROR_ARG_NULL;
    memcpy(tx->value, value, 32);
    return BOAT_SUCCESS;
}

BoatResult boat_evm_tx_set_data(BoatEvmTx *tx, const uint8_t *data, size_t len)
{
    if (!tx) return BOAT_ERROR_ARG_NULL;
    if (tx->data) { boat_free(tx->data); tx->data = NULL; }
    if (data && len > 0) {
        tx->data = (uint8_t *)boat_malloc(len);
        if (!tx->data) return BOAT_ERROR_MEM_ALLOC;
        memcpy(tx->data, data, len);
        tx->data_len = len;
    } else {
        tx->data_len = 0;
    }
    return BOAT_SUCCESS;
}

BoatResult boat_evm_tx_auto_fill(BoatEvmTx *tx, BoatEvmRpc *rpc, const BoatKey *key)
{
    if (!tx || !rpc || !key) return BOAT_ERROR_ARG_NULL;

    BoatKeyInfo info;
    BoatResult r = boat_key_get_info(key, &info);
    if (r != BOAT_SUCCESS) return r;

    if (!tx->nonce_set) {
        uint64_t nonce;
        r = boat_evm_get_nonce(rpc, info.address, &nonce);
        if (r != BOAT_SUCCESS) return r;
        tx->nonce = nonce;
        tx->nonce_set = true;
    }

    if (!tx->gas_price_set && !tx->chain.eip1559) {
        r = boat_evm_gas_price(rpc, tx->gas_price);
        if (r != BOAT_SUCCESS) return r;
        tx->gas_price_set = true;
    }

    return BOAT_SUCCESS;
}

/*--- Sign: RLP encode → keccak256 → sign → re-encode with v,r,s ---*/
BoatResult boat_evm_tx_sign(BoatEvmTx *tx, const BoatKey *key, uint8_t **raw_out, size_t *raw_len)
{
    if (!tx || !key || !raw_out || !raw_len) return BOAT_ERROR_ARG_NULL;

    BoatBuf items;
    boat_buf_init(&items, 512);

    /* Legacy tx RLP fields: nonce, gasprice, gaslimit, to, value, data */
    rlp_encode_uint(tx->nonce, &items);
    rlp_encode_uint256(tx->gas_price, &items);
    rlp_encode_uint(tx->gas_limit, &items);

    if (tx->to_set) {
        rlp_encode_string(tx->to, 20, &items);
    } else {
        rlp_encode_string((const uint8_t *)"", 0, &items); /* contract creation */
    }

    rlp_encode_uint256(tx->value, &items);
    rlp_encode_string(tx->data ? tx->data : (const uint8_t *)"", tx->data_len, &items);

    /* EIP-155: append chain_id, 0, 0 for signing hash */
    rlp_encode_uint(tx->chain.chain_id, &items);
    rlp_encode_string((const uint8_t *)"", 0, &items);
    rlp_encode_string((const uint8_t *)"", 0, &items);

    /* Wrap as list and hash */
    BoatBuf sign_rlp;
    boat_buf_init(&sign_rlp, items.len + 8);
    rlp_wrap_list(items.data, items.len, &sign_rlp);

    uint8_t hash[32];
    keccak_256(sign_rlp.data, sign_rlp.len, hash);
    boat_buf_free(&sign_rlp);

    /* Sign */
    uint8_t sig65[65];
    BoatResult r = boat_key_sign_recoverable(key, hash, sig65);
    if (r != BOAT_SUCCESS) {
        boat_buf_free(&items);
        return r;
    }

    /* Re-encode with v, r, s (EIP-155: v = recovery_id + chain_id*2 + 35) */
    uint64_t v = (uint64_t)sig65[64] + tx->chain.chain_id * 2 + 35;

    /* Rebuild items: nonce, gasprice, gaslimit, to, value, data, v, r, s */
    boat_buf_reset(&items);
    rlp_encode_uint(tx->nonce, &items);
    rlp_encode_uint256(tx->gas_price, &items);
    rlp_encode_uint(tx->gas_limit, &items);
    if (tx->to_set) {
        rlp_encode_string(tx->to, 20, &items);
    } else {
        rlp_encode_string((const uint8_t *)"", 0, &items);
    }
    rlp_encode_uint256(tx->value, &items);
    rlp_encode_string(tx->data ? tx->data : (const uint8_t *)"", tx->data_len, &items);
    rlp_encode_uint(v, &items);
    rlp_encode_string(sig65, 32, &items);       /* r */
    rlp_encode_string(sig65 + 32, 32, &items);  /* s */

    /* Final RLP list */
    BoatBuf final_rlp;
    boat_buf_init(&final_rlp, items.len + 8);
    rlp_wrap_list(items.data, items.len, &final_rlp);
    boat_buf_free(&items);

    *raw_out = final_rlp.data;
    *raw_len = final_rlp.len;
    /* Caller owns the buffer — do not free final_rlp.data */
    return BOAT_SUCCESS;
}

BoatResult boat_evm_tx_send(BoatEvmTx *tx, const BoatKey *key, BoatEvmRpc *rpc, uint8_t txhash[32])
{
    if (!tx || !key || !rpc || !txhash) return BOAT_ERROR_ARG_NULL;

    uint8_t *raw = NULL;
    size_t raw_len = 0;
    BoatResult r = boat_evm_tx_sign(tx, key, &raw, &raw_len);
    if (r != BOAT_SUCCESS) return r;

    r = boat_evm_send_raw_tx(rpc, raw, raw_len, txhash);
    boat_free(raw);
    return r;
}

#endif /* BOAT_EVM_ENABLED */

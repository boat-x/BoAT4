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
 * ABI Encoder — single-slot types
 *==========================================================================*/

void boat_evm_abi_encode_uint256(const uint8_t val[32], uint8_t out[32])
{
    memcpy(out, val, 32);
}

void boat_evm_abi_encode_uint64(uint64_t val, uint8_t out[32])
{
    memset(out, 0, 32);
    for (int i = 0; i < 8; i++) {
        out[31 - i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
}

void boat_evm_abi_encode_address(const uint8_t addr[20], uint8_t out[32])
{
    memset(out, 0, 12);
    memcpy(out + 12, addr, 20);
}

void boat_evm_abi_encode_bool(bool val, uint8_t out[32])
{
    memset(out, 0, 32);
    out[31] = val ? 1 : 0;
}

/*============================================================================
 * ABI Encoder — dynamic types
 *==========================================================================*/

BoatResult boat_evm_abi_encode_bytes(const uint8_t *data, size_t len, BoatBuf *out)
{
    if (!out) return BOAT_ERROR_ARG_NULL;

    /* Length as uint256 */
    uint8_t len_slot[32];
    boat_evm_abi_encode_uint64((uint64_t)len, len_slot);
    BoatResult r = boat_buf_append(out, len_slot, 32);
    if (r != BOAT_SUCCESS) return r;

    /* Data padded to 32-byte boundary */
    if (data && len > 0) {
        r = boat_buf_append(out, data, len);
        if (r != BOAT_SUCCESS) return r;
        size_t pad = (32 - (len % 32)) % 32;
        if (pad > 0) {
            uint8_t zeros[32] = {0};
            r = boat_buf_append(out, zeros, pad);
        }
    }
    return r;
}

BoatResult boat_evm_abi_encode_string(const char *str, BoatBuf *out)
{
    if (!str) return BOAT_ERROR_ARG_NULL;
    return boat_evm_abi_encode_bytes((const uint8_t *)str, strlen(str), out);
}

/*============================================================================
 * ABI Encoder — function call
 *==========================================================================*/

BoatResult boat_evm_abi_encode_func(const char *func_sig,
                                    const uint8_t *args[], const size_t arg_lens[], size_t n_args,
                                    uint8_t **calldata, size_t *calldata_len)
{
    if (!func_sig || !calldata || !calldata_len) return BOAT_ERROR_ARG_NULL;

    /* Function selector: keccak256(sig)[0:4] */
    uint8_t sig_hash[32];
    keccak_256((const uint8_t *)func_sig, strlen(func_sig), sig_hash);

    /* Calculate total size: 4 + n_args * 32 (for static args) */
    /* Note: this simplified version treats all args as static 32-byte slots */
    size_t total = 4 + n_args * 32;
    uint8_t *out = (uint8_t *)boat_malloc(total);
    if (!out) return BOAT_ERROR_MEM_ALLOC;

    /* Copy selector */
    memcpy(out, sig_hash, 4);

    /* Copy args (each must be exactly 32 bytes) */
    for (size_t i = 0; i < n_args; i++) {
        if (!args[i] || arg_lens[i] != 32) {
            boat_free(out);
            return BOAT_ERROR_ARG_INVALID;
        }
        memcpy(out + 4 + i * 32, args[i], 32);
    }

    *calldata = out;
    *calldata_len = total;
    return BOAT_SUCCESS;
}

/*============================================================================
 * ABI Decoder
 *==========================================================================*/

BoatResult boat_evm_abi_decode_uint256(const uint8_t *data, size_t offset, uint8_t out[32])
{
    if (!data || !out) return BOAT_ERROR_ARG_NULL;
    memcpy(out, data + offset, 32);
    return BOAT_SUCCESS;
}

BoatResult boat_evm_abi_decode_address(const uint8_t *data, size_t offset, uint8_t out[20])
{
    if (!data || !out) return BOAT_ERROR_ARG_NULL;
    /* Address is in bytes [offset+12 .. offset+31] */
    memcpy(out, data + offset + 12, 20);
    return BOAT_SUCCESS;
}

BoatResult boat_evm_abi_decode_bool(const uint8_t *data, size_t offset, bool *out)
{
    if (!data || !out) return BOAT_ERROR_ARG_NULL;
    *out = (data[offset + 31] != 0);
    return BOAT_SUCCESS;
}

BoatResult boat_evm_abi_decode_bytes(const uint8_t *data, size_t data_len, size_t offset,
                                     uint8_t **out, size_t *out_len)
{
    if (!data || !out || !out_len) return BOAT_ERROR_ARG_NULL;

    /* Read offset pointer (first 32 bytes at position) */
    uint64_t data_offset = 0;
    for (int i = 24; i < 32; i++) {
        data_offset = (data_offset << 8) | data[offset + i];
    }

    /* Read length at data_offset */
    if (data_offset + 32 > data_len) return BOAT_ERROR_ARG_OUT_OF_RANGE;
    uint64_t len = 0;
    for (int i = 24; i < 32; i++) {
        len = (len << 8) | data[data_offset + i];
    }

    if (data_offset + 32 + len > data_len) return BOAT_ERROR_ARG_OUT_OF_RANGE;

    *out = (uint8_t *)boat_malloc((size_t)len);
    if (!*out) return BOAT_ERROR_MEM_ALLOC;
    memcpy(*out, data + data_offset + 32, (size_t)len);
    *out_len = (size_t)len;
    return BOAT_SUCCESS;
}

void boat_evm_abi_free(void *ptr)
{
    if (ptr) boat_free(ptr);
}

#endif /* BOAT_EVM_ENABLED */

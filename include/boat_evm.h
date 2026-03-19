/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#ifndef BOAT_EVM_H
#define BOAT_EVM_H

#include "boat.h"
#include "boat_key.h"

#ifdef __cplusplus
extern "C" {
#endif

#if BOAT_EVM_ENABLED

/*----------------------------------------------------------------------------
 * Chain configuration
 *--------------------------------------------------------------------------*/
typedef struct {
    uint64_t chain_id;
    char     rpc_url[256];
    bool     eip1559;
} BoatEvmChainConfig;

/* Presets */
#define BOAT_EVM_ETH_MAINNET     { 1,     "", false }
#define BOAT_EVM_BASE_MAINNET    { 8453,  "", false }
#define BOAT_EVM_BASE_SEPOLIA    { 84532, "", false }

/*----------------------------------------------------------------------------
 * RPC context (wraps generic boat_rpc internally)
 *--------------------------------------------------------------------------*/
typedef struct {
    char url[256];
    int  req_id;
} BoatEvmRpc;

BoatResult boat_evm_rpc_init(BoatEvmRpc *rpc, const char *url);
void       boat_evm_rpc_free(BoatEvmRpc *rpc);

/*--- Typed RPC methods ---*/
BoatResult boat_evm_block_number(BoatEvmRpc *rpc, uint64_t *out);
BoatResult boat_evm_get_balance(BoatEvmRpc *rpc, const uint8_t addr[20], uint8_t out_wei[32]);
BoatResult boat_evm_get_nonce(BoatEvmRpc *rpc, const uint8_t addr[20], uint64_t *out);
BoatResult boat_evm_gas_price(BoatEvmRpc *rpc, uint8_t out_wei[32]);
BoatResult boat_evm_send_raw_tx(BoatEvmRpc *rpc, const uint8_t *raw, size_t raw_len, uint8_t txhash[32]);
BoatResult boat_evm_eth_call(BoatEvmRpc *rpc, const uint8_t to[20],
                             const uint8_t *data, size_t data_len,
                             uint8_t **result, size_t *result_len);

/*----------------------------------------------------------------------------
 * Transaction builder
 *--------------------------------------------------------------------------*/
typedef struct {
    BoatEvmChainConfig chain;
    uint64_t nonce;
    uint8_t  gas_price[32];
    uint64_t gas_limit;
    uint8_t  to[20];
    bool     to_set;
    uint8_t  value[32];
    uint8_t *data;
    size_t   data_len;
    /* EIP-1559 fields */
    uint8_t  max_fee_per_gas[32];
    uint8_t  max_priority_fee[32];
    bool     nonce_set;
    bool     gas_price_set;
} BoatEvmTx;

BoatResult boat_evm_tx_init(BoatEvmTx *tx, const BoatEvmChainConfig *chain);
BoatResult boat_evm_tx_set_nonce(BoatEvmTx *tx, uint64_t nonce);
BoatResult boat_evm_tx_set_gas_price(BoatEvmTx *tx, const uint8_t gas_price[32]);
BoatResult boat_evm_tx_set_gas_limit(BoatEvmTx *tx, uint64_t gas_limit);
BoatResult boat_evm_tx_set_to(BoatEvmTx *tx, const uint8_t to[20]);
BoatResult boat_evm_tx_set_value(BoatEvmTx *tx, const uint8_t value[32]);
BoatResult boat_evm_tx_set_data(BoatEvmTx *tx, const uint8_t *data, size_t len);
BoatResult boat_evm_tx_auto_fill(BoatEvmTx *tx, BoatEvmRpc *rpc, const BoatKey *key);
BoatResult boat_evm_tx_sign(BoatEvmTx *tx, const BoatKey *key, uint8_t **raw_out, size_t *raw_len);
BoatResult boat_evm_tx_send(BoatEvmTx *tx, const BoatKey *key, BoatEvmRpc *rpc, uint8_t txhash[32]);

/*----------------------------------------------------------------------------
 * ABI encoder/decoder
 *--------------------------------------------------------------------------*/
BoatResult boat_evm_abi_encode_func(const char *func_sig,
                                    const uint8_t *args[], const size_t arg_lens[], size_t n_args,
                                    uint8_t **calldata, size_t *calldata_len);

/* Encode single ABI values into 32-byte slots */
void boat_evm_abi_encode_uint256(const uint8_t val[32], uint8_t out[32]);
void boat_evm_abi_encode_uint64(uint64_t val, uint8_t out[32]);
void boat_evm_abi_encode_address(const uint8_t addr[20], uint8_t out[32]);
void boat_evm_abi_encode_bool(bool val, uint8_t out[32]);

/* Dynamic types — write to BoatBuf */
BoatResult boat_evm_abi_encode_bytes(const uint8_t *data, size_t len, BoatBuf *out);
BoatResult boat_evm_abi_encode_string(const char *str, BoatBuf *out);

/* Decode from ABI-encoded data at offset */
BoatResult boat_evm_abi_decode_uint256(const uint8_t *data, size_t offset, uint8_t out[32]);
BoatResult boat_evm_abi_decode_address(const uint8_t *data, size_t offset, uint8_t out[20]);
BoatResult boat_evm_abi_decode_bool(const uint8_t *data, size_t offset, bool *out);
BoatResult boat_evm_abi_decode_bytes(const uint8_t *data, size_t data_len, size_t offset,
                                     uint8_t **out, size_t *out_len);

/* Free calldata/result allocated by encode/decode */
void boat_evm_abi_free(void *ptr);

#endif /* BOAT_EVM_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* BOAT_EVM_H */

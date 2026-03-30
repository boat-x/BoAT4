/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#ifndef BOAT_H
#define BOAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*----------------------------------------------------------------------------
 * Configuration macros (override via compiler -D flags or Kconfig)
 *--------------------------------------------------------------------------*/
#ifndef BOAT_EVM_ENABLED
#define BOAT_EVM_ENABLED    1
#endif

#ifndef BOAT_SOL_ENABLED
#define BOAT_SOL_ENABLED    1
#endif

#ifndef BOAT_KEY_SE_ENABLED
#define BOAT_KEY_SE_ENABLED  0
#endif

#ifndef BOAT_KEY_TEE_ENABLED
#define BOAT_KEY_TEE_ENABLED 0
#endif

#ifndef BOAT_PAY_X402_ENABLED
#define BOAT_PAY_X402_ENABLED    0
#endif

#ifndef BOAT_PAY_NANO_ENABLED
#define BOAT_PAY_NANO_ENABLED    0
#endif

#ifndef BOAT_PAY_GATEWAY_ENABLED
#define BOAT_PAY_GATEWAY_ENABLED 0
#endif

#ifndef BOAT_PAY_MPP_ENABLED
#define BOAT_PAY_MPP_ENABLED     0
#endif

/*----------------------------------------------------------------------------
 * Error codes
 *--------------------------------------------------------------------------*/
typedef int32_t BoatResult;

#define BOAT_SUCCESS                     0
#define BOAT_ERROR                      -1

/* Argument errors: -100 .. -199 */
#define BOAT_ERROR_ARG_NULL            -100
#define BOAT_ERROR_ARG_INVALID         -101
#define BOAT_ERROR_ARG_OUT_OF_RANGE    -102

/* Memory errors: -200 .. -299 */
#define BOAT_ERROR_MEM_ALLOC           -200
#define BOAT_ERROR_MEM_OVERFLOW        -201

/* RPC errors: -300 .. -399 */
#define BOAT_ERROR_RPC_FAIL            -300
#define BOAT_ERROR_RPC_PARSE           -301
#define BOAT_ERROR_RPC_TIMEOUT         -302
#define BOAT_ERROR_RPC_SERVER          -303

/* Key errors: -400 .. -499 */
#define BOAT_ERROR_KEY_GEN             -400
#define BOAT_ERROR_KEY_IMPORT          -401
#define BOAT_ERROR_KEY_SIGN            -402
#define BOAT_ERROR_KEY_NOT_FOUND       -403
#define BOAT_ERROR_KEY_TYPE            -404

/* Storage errors: -500 .. -599 */
#define BOAT_ERROR_STORAGE_WRITE       -500
#define BOAT_ERROR_STORAGE_READ        -501
#define BOAT_ERROR_STORAGE_NOT_FOUND   -502

/* EVM errors: -600 .. -699 */
#define BOAT_ERROR_EVM_RLP             -600
#define BOAT_ERROR_EVM_ABI             -601
#define BOAT_ERROR_EVM_TX              -602
#define BOAT_ERROR_EVM_NONCE           -603

/* Solana errors: -700 .. -799 */
#define BOAT_ERROR_SOL_BLOCKHASH_EXPIRED   -700
#define BOAT_ERROR_SOL_INSUFFICIENT_FUNDS  -701
#define BOAT_ERROR_SOL_TX_TOO_LARGE        -702

/* HTTP errors: -800 .. -899 */
#define BOAT_ERROR_HTTP_FAIL           -800
#define BOAT_ERROR_HTTP_402            -802

/*----------------------------------------------------------------------------
 * Logging
 *--------------------------------------------------------------------------*/
typedef enum {
    BOAT_LOG_NONE = 0,
    BOAT_LOG_CRITICAL,
    BOAT_LOG_NORMAL,
    BOAT_LOG_VERBOSE
} BoatLogLevel;

#ifndef BOAT_LOG_LEVEL
#define BOAT_LOG_LEVEL  BOAT_LOG_NORMAL
#endif

extern BoatLogLevel g_boat_log_level;

#define BOAT_LOG(level, fmt, ...) \
    do { \
        if ((level) <= g_boat_log_level) { \
            printf("[BOAT %s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while(0)

/*----------------------------------------------------------------------------
 * Buffer helper
 *--------------------------------------------------------------------------*/
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} BoatBuf;

BoatResult boat_buf_init(BoatBuf *buf, size_t initial_cap);
BoatResult boat_buf_append(BoatBuf *buf, const uint8_t *data, size_t len);
BoatResult boat_buf_append_byte(BoatBuf *buf, uint8_t byte);
void       boat_buf_reset(BoatBuf *buf);
void       boat_buf_free(BoatBuf *buf);

/*----------------------------------------------------------------------------
 * Key types (shared between boat_key.h and chain modules)
 *--------------------------------------------------------------------------*/
typedef enum {
    BOAT_KEY_TYPE_SECP256K1 = 0,
    BOAT_KEY_TYPE_SECP256R1,
    BOAT_KEY_TYPE_ED25519
} BoatKeyType;

/*----------------------------------------------------------------------------
 * Utility: hex/bin conversion
 *--------------------------------------------------------------------------*/
BoatResult boat_hex_to_bin(const char *hex_str, uint8_t *bin, size_t bin_cap, size_t *out_len);
BoatResult boat_bin_to_hex(const uint8_t *bin, size_t len, char *hex_str, size_t hex_cap, bool prefix_0x);

/*----------------------------------------------------------------------------
 * Utility: base58 encode/decode (wraps third-party/crypto/base58.c)
 *--------------------------------------------------------------------------*/
BoatResult boat_base58_encode(const uint8_t *bin, size_t bin_len, char *b58_str, size_t b58_cap);
BoatResult boat_base58_decode(const char *b58_str, uint8_t *bin, size_t bin_cap, size_t *out_len);

/*----------------------------------------------------------------------------
 * Utility: amount conversion (human-readable ↔ minimum unit)
 *
 * Converts between a human-readable amount (e.g. 0.001 ETH, 1.5 SOL) and
 * the integer amount in minimum units (wei, lamports, token base units).
 * `decimals` is the number of decimal places (18 for ETH, 9 for SOL, 6 for USDC).
 *--------------------------------------------------------------------------*/
BoatResult boat_amount_to_uint256(double amount, uint8_t decimals, uint8_t value[32]);
BoatResult boat_uint256_to_amount(const uint8_t value[32], uint8_t decimals, double *amount);
BoatResult boat_amount_to_uint64(double amount, uint8_t decimals, uint64_t *value);
BoatResult boat_uint64_to_amount(uint64_t value, uint8_t decimals, double *amount);

#ifdef __cplusplus
}
#endif

#endif /* BOAT_H */

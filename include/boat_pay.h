/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#ifndef BOAT_PAY_H
#define BOAT_PAY_H

#include "boat.h"
#include "boat_key.h"
#include "boat_evm.h"
#include "boat_sol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Common: EIP-712 / EIP-3009
 *==========================================================================*/

typedef struct {
    char     name[64];
    char     version[16];
    uint64_t chain_id;
    uint8_t  verifying_contract[20];
} BoatEip712Domain;

typedef struct {
    uint8_t  from[20];
    uint8_t  to[20];
    uint8_t  value[32];       /* uint256 big-endian */
    uint64_t valid_after;
    uint64_t valid_before;
    uint8_t  nonce[32];       /* bytes32 */
} BoatEip3009Auth;

/* EIP-712 primitives */
BoatResult boat_eip712_domain_hash(const BoatEip712Domain *domain, uint8_t hash[32]);
BoatResult boat_eip712_hash_struct(const uint8_t *type_hash, const uint8_t *encoded_data,
                                   size_t data_len, uint8_t hash[32]);
BoatResult boat_eip712_sign(const uint8_t domain_hash[32], const uint8_t struct_hash[32],
                            const BoatKey *key, uint8_t sig65[65]);

/* EIP-3009 */
BoatResult boat_eip3009_sign(const BoatEip3009Auth *auth, const BoatEip712Domain *domain,
                             const BoatKey *key, uint8_t sig65[65]);

/*============================================================================
 * x402 Protocol
 *==========================================================================*/
#if BOAT_PAY_X402_ENABLED

/* HTTP method for the original application request */
typedef enum {
    BOAT_HTTP_GET  = 0,
    BOAT_HTTP_POST = 1
} BoatHttpMethod;

/* Options describing the original HTTP request from the application.
 * These are replayed in the second request (with X-Payment header added).
 * Pass NULL to x402 functions for a simple GET with no extra headers. */
typedef struct {
    BoatHttpMethod method;            /* GET or POST */
    const char    *content_type;      /* Content-Type for POST (NULL for GET) */
    const uint8_t *body;              /* Request body for POST (NULL for GET) */
    size_t         body_len;          /* Body length */
    const char    *extra_headers;     /* App headers, "Key: Val\r\n..." (NULL if none) */
} BoatX402ReqOpts;

typedef struct {
    int      x402_version;          /* 1 or 2 */
    char     scheme[16];            /* "exact" */
    char     network[64];           /* v1: "base-sepolia", v2: "eip155:84532" */
    char     amount_str[32];
    uint8_t  pay_to[20];
    char     pay_to_hex[43];        /* "0x..." checksum address for v2 echo-back */
    uint8_t  asset[20];             /* USDC contract address (chain-specific) */
    char     asset_hex[43];         /* "0x..." for v2 echo-back */
    uint32_t max_timeout;
    char     resource_url[512];
    char     asset_name[64];        /* EIP-712 domain name from extra.name (e.g. "USD Coin" or "USDC") */
    char     asset_version[16];     /* EIP-712 domain version from extra.version (e.g. "2") */
    uint8_t  verifying_contract[20]; /* EIP-712 verifyingContract from extra (Gateway Wallet for batch scheme) */
    bool     has_verifying_contract; /* true if extra.verifyingContract was present */
} BoatX402PaymentReq;

/* Send the application's HTTP request. If the server returns 2xx, the resource
 * is returned directly in response/response_len and req is zeroed (no payment needed,
 * returns BOAT_SUCCESS). If 402, req is filled with payment requirements (returns
 * BOAT_ERROR_HTTP_402). Pass NULL for opts for a simple GET. */
BoatResult boat_x402_request(const char *url, const BoatX402ReqOpts *opts,
                             BoatX402PaymentReq *req,
                             uint8_t **response, size_t *response_len);

BoatResult boat_x402_make_payment(const BoatX402PaymentReq *req, const BoatKey *key,
                                  const BoatEvmChainConfig *chain,
                                  char **payment_b64);

/* Replay the original request with X-Payment header appended. */
BoatResult boat_x402_pay_and_get(const char *url, const BoatX402ReqOpts *opts,
                                 const char *payment_b64,
                                 uint8_t **response, size_t *response_len);

/* Convenience: full x402 flow. If 2xx on first try, returns immediately. */
BoatResult boat_x402_process(const char *url, const BoatX402ReqOpts *opts,
                             const BoatKey *key, const BoatEvmChainConfig *chain,
                             uint8_t **response, size_t *response_len);

#endif /* BOAT_PAY_X402_ENABLED */

/*============================================================================
 * Circle Nanopayments (Gateway batched settlement)
 *==========================================================================*/
#if BOAT_PAY_NANO_ENABLED

typedef struct {
    uint8_t           gateway_wallet_addr[20];  /* Gateway Wallet contract (deposit + balance) */
    uint8_t           usdc_addr[20];            /* USDC contract address on this chain */
    BoatEvmChainConfig chain;
} BoatNanoConfig;

BoatResult boat_nano_deposit(const BoatNanoConfig *config, const BoatKey *key,
                             const uint8_t amount[32], BoatEvmRpc *rpc, uint8_t txhash[32]);
BoatResult boat_nano_authorize(const BoatNanoConfig *config, const BoatKey *key,
                               const uint8_t to[20], const uint8_t amount[32],
                               const uint8_t nonce[32],
                               BoatEip3009Auth *auth_out, uint8_t sig65[65]);
BoatResult boat_nano_get_balance(const BoatNanoConfig *config, const uint8_t addr[20],
                                 BoatEvmRpc *rpc, uint8_t balance[32]);

/* Full x402 nanopayment flow (GET → 402 → sign EIP-3009 → retry with PAYMENT-SIGNATURE).
 * The 402 response from a Gateway-enabled seller includes extra.name="GatewayWalletBatched"
 * and extra.verifyingContract=<Gateway Wallet>, which is handled automatically. */
BoatResult boat_nano_pay(const char *url, const BoatX402ReqOpts *opts,
                         const BoatNanoConfig *config, const BoatKey *key,
                         uint8_t **response, size_t *response_len);

#endif /* BOAT_PAY_NANO_ENABLED */

/*============================================================================
 * Circle Gateway — EVM
 *==========================================================================*/
#if BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED

typedef struct {
    uint8_t           gateway_wallet_addr[20];
    uint8_t           gateway_minter_addr[20];  /* gatewayMint contract on this chain */
    uint8_t           usdc_addr[20];            /* USDC contract address on this chain */
    uint32_t          domain;                   /* Circle Gateway domain ID */
    char              gateway_api_url[128];     /* e.g. "https://gateway-api.circle.com/v1"; empty = testnet */
    BoatEvmChainConfig chain;
} BoatGatewayConfig;

typedef struct {
    uint8_t mint_txhash[32];
} BoatGatewayTransferResult;

BoatResult boat_gateway_deposit(const BoatGatewayConfig *config, const BoatKey *key,
                                const uint8_t amount[32], BoatEvmRpc *rpc, uint8_t txhash[32]);
BoatResult boat_gateway_balance(const BoatGatewayConfig *config, const uint8_t addr[20],
                                BoatEvmRpc *rpc, uint8_t balance[32]);
/* Instant withdrawal: same-chain transfer back to wallet (normal path).
 * This is just boat_gateway_transfer() with src == dst. */
BoatResult boat_gateway_transfer(const BoatGatewayConfig *src_config,
                                 const BoatGatewayConfig *dst_config,
                                 const BoatKey *key,
                                 const uint8_t amount[32],
                                 const uint8_t max_fee[32],
                                 BoatEvmRpc *dst_rpc,
                                 BoatGatewayTransferResult *result);

/* Trustless withdrawal: two-phase on-chain withdrawal (emergency path).
 * Only needed when Circle's API is unavailable. 7-day delay between steps. */
BoatResult boat_gateway_trustless_withdraw(const BoatGatewayConfig *config, const BoatKey *key,
                                           const uint8_t amount[32], BoatEvmRpc *rpc,
                                           uint8_t txhash[32]);
BoatResult boat_gateway_trustless_complete(const BoatGatewayConfig *config, const BoatKey *key,
                                           BoatEvmRpc *rpc, uint8_t txhash[32]);

#endif /* BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED */

/*============================================================================
 * Circle Gateway — Solana
 *==========================================================================*/
#if BOAT_PAY_GATEWAY_ENABLED && BOAT_SOL_ENABLED

/* Well-known Solana program IDs (base58-decoded 32-byte pubkeys) */
extern const uint8_t BOAT_GW_SOL_DEVNET_WALLET[32];
extern const uint8_t BOAT_GW_SOL_DEVNET_MINTER[32];
extern const uint8_t BOAT_GW_SOL_MAINNET_WALLET[32];
extern const uint8_t BOAT_GW_SOL_MAINNET_MINTER[32];
extern const uint8_t BOAT_GW_SOL_DEVNET_USDC[32];
extern const uint8_t BOAT_GW_SOL_MAINNET_USDC[32];

typedef struct {
    uint8_t            gateway_wallet_program[32];
    uint8_t            gateway_minter_program[32];
    uint8_t            usdc_mint[32];
    uint32_t           domain;          /* 5 for Solana */
    char               gateway_api_url[128]; /* e.g. "https://gateway-api.circle.com/v1"; empty = testnet */
    BoatSolChainConfig chain;
} BoatGatewaySolConfig;

typedef struct {
    uint8_t signature[64];
} BoatGatewaySolTransferResult;

typedef struct {
    uint64_t available_amount;
    uint64_t withdrawing_amount;
    uint64_t withdrawal_slot;
} BoatGatewaySolDepositInfo;

/* Solana-only Gateway functions */
BoatResult boat_gateway_sol_deposit(const BoatGatewaySolConfig *config, const BoatKey *key,
                                    uint64_t amount, BoatSolRpc *rpc, uint8_t sig[64]);
BoatResult boat_gateway_sol_balance(const BoatGatewaySolConfig *config, const uint8_t owner[32],
                                    BoatSolRpc *rpc, BoatGatewaySolDepositInfo *info);
BoatResult boat_gateway_sol_api_balance(const BoatGatewaySolConfig *config,
                                        const uint8_t owner[32],
                                        uint64_t *available);
BoatResult boat_gateway_sol_transfer(const BoatGatewaySolConfig *src_config,
                                     const BoatGatewaySolConfig *dst_config,
                                     const BoatKey *key,
                                     const uint8_t *recipient,  /* 32-byte pubkey, NULL = self */
                                     uint64_t amount, uint64_t max_fee,
                                     BoatSolRpc *dst_rpc,
                                     BoatGatewaySolTransferResult *result);
BoatResult boat_gateway_sol_trustless_withdraw(const BoatGatewaySolConfig *config,
                                               const BoatKey *key, uint64_t amount,
                                               BoatSolRpc *rpc, uint8_t sig[64]);
BoatResult boat_gateway_sol_trustless_complete(const BoatGatewaySolConfig *config,
                                               const BoatKey *key,
                                               BoatSolRpc *rpc, uint8_t sig[64]);

#endif /* BOAT_PAY_GATEWAY_ENABLED && BOAT_SOL_ENABLED */

/*============================================================================
 * Circle Gateway — Cross-chain (EVM <-> Solana)
 *==========================================================================*/
#if BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED && BOAT_SOL_ENABLED

/* EVM -> Solana: burn on EVM (EIP-712 + secp256k1), mint on Solana */
BoatResult boat_gateway_transfer_evm_to_sol(
    const BoatGatewayConfig    *src_config,
    const BoatGatewaySolConfig *dst_config,
    const BoatKey *evm_key,
    const BoatKey *sol_key,
    const uint8_t amount[32],
    const uint8_t max_fee[32],
    BoatSolRpc *dst_rpc,
    BoatGatewaySolTransferResult *result);

/* Solana -> EVM: burn on Solana (binary + Ed25519), mint on EVM */
BoatResult boat_gateway_transfer_sol_to_evm(
    const BoatGatewaySolConfig *src_config,
    const BoatGatewayConfig    *dst_config,
    const BoatKey *sol_key,
    const BoatKey *evm_key,
    uint64_t amount,
    uint64_t max_fee,
    BoatEvmRpc *dst_rpc,
    BoatGatewayTransferResult *result);

#endif /* BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED && BOAT_SOL_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* BOAT_PAY_H */

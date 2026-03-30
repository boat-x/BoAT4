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
 * Common: HTTP request options (shared by x402, MPP, etc.)
 *==========================================================================*/
#if BOAT_PAY_X402_ENABLED || BOAT_PAY_MPP_ENABLED

/* HTTP method for the original application request */
typedef enum {
    BOAT_HTTP_GET  = 0,
    BOAT_HTTP_POST = 1
} BoatHttpMethod;

/* Options describing the original HTTP request from the application.
 * These are replayed in the second request (with payment header added).
 * Pass NULL to payment functions for a simple GET with no extra headers. */
typedef struct {
    BoatHttpMethod method;            /* GET or POST */
    const char    *content_type;      /* Content-Type for POST (NULL for GET) */
    const uint8_t *body;              /* Request body for POST (NULL for GET) */
    size_t         body_len;          /* Body length */
    const char    *extra_headers;     /* App headers, "Key: Val\r\n..." (NULL if none) */
} BoatPayReqOpts;

/* Backward compatibility alias */
typedef BoatPayReqOpts BoatX402ReqOpts;

#endif /* BOAT_PAY_X402_ENABLED || BOAT_PAY_MPP_ENABLED */

/*============================================================================
 * x402 Protocol
 *==========================================================================*/
#if BOAT_PAY_X402_ENABLED

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
                                 const uint8_t *recipient,  /* 20-byte EVM addr, NULL = self */
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
    const uint8_t *sol_recipient,  /* 32-byte pubkey, NULL = sol_key's address */
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
    const uint8_t *evm_recipient,  /* 20-byte EVM addr, NULL = evm_key's address */
    uint64_t amount,
    uint64_t max_fee,
    BoatEvmRpc *dst_rpc,
    BoatGatewayTransferResult *result);

#endif /* BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED && BOAT_SOL_ENABLED */

/*============================================================================
 * MPP (Machine Payments Protocol) — Client-side
 *==========================================================================*/
#if BOAT_PAY_MPP_ENABLED

/*--- Parsed 402 challenge from WWW-Authenticate: Payment header ---*/
typedef struct {
    char     id[64];            /* Challenge ID (HMAC-bound) */
    char     realm[128];        /* Protection space (hostname) */
    char     method[32];        /* Payment method ("tempo", "stripe", etc.) */
    char     intent[32];        /* Intent type ("charge", "session") */
    char     request_b64[2048]; /* Raw base64url-encoded request (echoed in credential) */
    char     expires[64];       /* Optional RFC 3339 expiration */
    char     description[256];  /* Optional human-readable description */
    char     digest[128];       /* Optional content digest */
    char     opaque_b64[512];   /* Optional server correlation data (base64url) */

    /* Decoded request JSON fields (charge intent) */
    char     amount[64];        /* Amount in token base units (string) */
    char     currency[64];      /* Token address or identifier */
    char     recipient[64];     /* Recipient address (0x-prefixed hex) */
    uint64_t chain_id;          /* From methodDetails.chainId */
} BoatMppChallenge;

/*--- Payment receipt from Payment-Receipt header ---*/
typedef struct {
    char     status[16];        /* "success" */
    char     method[32];        /* Payment method */
    char     reference[128];    /* Transaction hash or reference */
    char     timestamp[64];     /* ISO 8601 timestamp */
} BoatMppReceipt;

/*--- Tempo chain configuration ---*/
typedef struct {
    BoatEvmChainConfig chain;
    uint8_t  token_addr[20];    /* pathUSD or USDC contract address */
    char     rpc_url[256];      /* Tempo RPC endpoint */
} BoatMppTempoConfig;

/* Tempo presets */
#define BOAT_MPP_TEMPO_MAINNET_CHAIN_ID     4217
#define BOAT_MPP_TEMPO_TESTNET_CHAIN_ID     42431  /* Tempo Moderato testnet */

/* pathUSD on Tempo testnet: 0x20c0000000000000000000000000000000000000 */
extern const uint8_t BOAT_MPP_TEMPO_PATHUSD_TESTNET[20];
/* USDC on Tempo mainnet: 0x20c000000000000000000000b9537d11c60e8b50 */
extern const uint8_t BOAT_MPP_TEMPO_USDC_MAINNET[20];

/*--- MPP envelope functions ---*/

/* Base64url encode/decode (RFC 4648 URL-safe, no padding) */
size_t   boat_base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);
BoatResult boat_base64url_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);

/* Parse WWW-Authenticate: Payment challenge(s) from raw response headers.
 * Fills challenges[] array, returns count in *n_challenges.
 * Returns BOAT_SUCCESS if at least one MPP challenge found. */
BoatResult boat_mpp_parse_challenges(const char *headers, size_t headers_len,
                                     BoatMppChallenge *challenges, size_t max_challenges,
                                     size_t *n_challenges);

/* Build the Authorization: Payment credential string (heap-allocated).
 * challenge: parsed challenge (id, realm, method, intent, request_b64 are echoed back)
 * source: optional DID string (NULL to omit)
 * payload_json: method-specific proof JSON string (e.g., {"type":"hash","hash":"0x..."})
 * Returns BOAT_SUCCESS, caller must boat_free(*credential_out). */
BoatResult boat_mpp_build_credential(const BoatMppChallenge *challenge,
                                     const char *source,
                                     const char *payload_json,
                                     char **credential_out);

/* Parse Payment-Receipt header value into receipt struct. */
BoatResult boat_mpp_parse_receipt(const char *headers, size_t headers_len,
                                  BoatMppReceipt *receipt);

/* Send HTTP request; if 402 with MPP challenge, parse and return it.
 * Returns BOAT_SUCCESS if 2xx (response filled, no payment needed).
 * Returns BOAT_ERROR_HTTP_402 if MPP challenge parsed into challenge_out. */
BoatResult boat_mpp_request(const char *url, const BoatPayReqOpts *opts,
                            BoatMppChallenge *challenge_out,
                            uint8_t **response, size_t *response_len);

/* Retry request with Authorization: Payment credential attached.
 * Also parses Payment-Receipt from the response. */
BoatResult boat_mpp_pay_and_get(const char *url, const BoatPayReqOpts *opts,
                                const char *credential,
                                uint8_t **response, size_t *response_len,
                                BoatMppReceipt *receipt_out);

/*--- Tempo Charge method handler ---*/

/* Execute Tempo Charge: transfer TIP-20 token on-chain, return credential string.
 * challenge: parsed MPP challenge with charge intent
 * key: secp256k1 signing key
 * config: Tempo chain + RPC configuration
 * credential_out: heap-allocated "Payment <base64url>" string, caller frees */
BoatResult boat_mpp_tempo_charge(const BoatMppChallenge *challenge,
                                 const BoatKey *key,
                                 const BoatMppTempoConfig *config,
                                 char **credential_out);

/* Convenience: full MPP Tempo Charge flow (request → pay → retry → done).
 * Returns BOAT_SUCCESS with response data and receipt on success. */
BoatResult boat_mpp_tempo_process(const char *url, const BoatPayReqOpts *opts,
                                  const BoatKey *key,
                                  const BoatMppTempoConfig *config,
                                  uint8_t **response, size_t *response_len,
                                  BoatMppReceipt *receipt_out);

#endif /* BOAT_PAY_MPP_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* BOAT_PAY_H */

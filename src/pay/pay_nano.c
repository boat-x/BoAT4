/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Circle Nanopayments — gas-free micro-payments via Gateway batched settlement.
 *
 * Deposit is on-chain (ERC20 approve + Gateway Wallet deposit).
 * Authorization is off-chain (EIP-3009 TransferWithAuthorization signed
 * against the "GatewayWalletBatched" EIP-712 domain).
 * Balance is queried on-chain via Gateway Wallet availableBalance().
 *****************************************************************************/
#include "boat_pay.h"
#include "boat_pal.h"

#include <string.h>
#include <time.h>

#if BOAT_PAY_NANO_ENABLED && BOAT_EVM_ENABLED

/* Function selectors */
static const uint8_t APPROVE_SELECTOR[4]        = { 0x09, 0x5e, 0xa7, 0xb3 }; /* approve(address,uint256) */
static const uint8_t DEPOSIT_SEL[4]             = { 0x47, 0xe7, 0xef, 0x24 }; /* deposit(address,uint256) */
static const uint8_t AVAILABLE_BALANCE_SEL[4]   = { 0x3c, 0xcb, 0x64, 0xae }; /* availableBalance(address,address) */

/*============================================================================
 * Deposit: ERC20 approve + Gateway Wallet deposit (on-chain, one-time)
 *==========================================================================*/

BoatResult boat_nano_deposit(const BoatNanoConfig *config, const BoatKey *key,
                             const uint8_t amount[32], BoatEvmRpc *rpc, uint8_t txhash[32])
{
    if (!config || !key || !amount || !rpc || !txhash) return BOAT_ERROR_ARG_NULL;

    /* Step 1: ERC20 approve(gateway_wallet, amount) */
    uint8_t approve_data[68]; /* 4 + 32 + 32 */
    memcpy(approve_data, APPROVE_SELECTOR, 4);
    memset(approve_data + 4, 0, 12);
    memcpy(approve_data + 16, config->gateway_wallet_addr, 20);
    memcpy(approve_data + 36, amount, 32);

    BoatEvmTx tx;
    boat_evm_tx_init(&tx, &config->chain);
    boat_evm_tx_set_to(&tx, config->usdc_addr);
    boat_evm_tx_set_data(&tx, approve_data, sizeof(approve_data));
    boat_evm_tx_set_gas_limit(&tx, 60000);
    boat_evm_tx_auto_fill(&tx, rpc, key);

    uint8_t approve_hash[32];
    BoatResult r = boat_evm_tx_send(&tx, key, rpc, approve_hash);
    if (tx.data) boat_free(tx.data);
    if (r != BOAT_SUCCESS) return r;

    /* Step 2: Gateway Wallet deposit(address token, uint256 value) */
    uint8_t deposit_data[68]; /* 4 + 32 + 32 */
    memcpy(deposit_data, DEPOSIT_SEL, 4);
    memset(deposit_data + 4, 0, 12);
    memcpy(deposit_data + 16, config->usdc_addr, 20);  /* token address */
    memcpy(deposit_data + 36, amount, 32);              /* value */

    BoatEvmTx tx2;
    boat_evm_tx_init(&tx2, &config->chain);
    boat_evm_tx_set_to(&tx2, config->gateway_wallet_addr);
    boat_evm_tx_set_data(&tx2, deposit_data, sizeof(deposit_data));
    boat_evm_tx_set_gas_limit(&tx2, 100000);
    boat_evm_tx_set_nonce(&tx2, tx.nonce + 1);
    boat_evm_tx_auto_fill(&tx2, rpc, key);

    r = boat_evm_tx_send(&tx2, key, rpc, txhash);
    if (tx2.data) boat_free(tx2.data);
    return r;
}

/*============================================================================
 * Authorize: off-chain EIP-3009 TransferWithAuthorization (gas-free)
 *
 * EIP-712 domain: name="GatewayWalletBatched", version="1"
 * verifyingContract = Gateway Wallet address
 *==========================================================================*/

BoatResult boat_nano_authorize(const BoatNanoConfig *config, const BoatKey *key,
                               const uint8_t to[20], const uint8_t amount[32],
                               const uint8_t nonce[32],
                               BoatEip3009Auth *auth_out, uint8_t sig65[65])
{
    if (!config || !key || !to || !amount || !nonce || !auth_out || !sig65)
        return BOAT_ERROR_ARG_NULL;

    BoatKeyInfo info;
    BoatResult r = boat_key_get_info(key, &info);
    if (r != BOAT_SUCCESS) return r;

    /* Build EIP-3009 authorization */
    memset(auth_out, 0, sizeof(BoatEip3009Auth));
    memcpy(auth_out->from, info.address, 20);
    memcpy(auth_out->to, to, 20);
    memcpy(auth_out->value, amount, 32);
    uint64_t now = (uint64_t)time(NULL);
    auth_out->valid_after = now - 600;              /* 10 min before */
    auth_out->valid_before = now + 3 * 24 * 3600 + 3600; /* 3 days + 1h (Circle requires >= 3 days) */
    memcpy(auth_out->nonce, nonce, 32);

    /* GatewayWalletBatched domain (per Circle SDK: CIRCLE_BATCHING_NAME / VERSION) */
    BoatEip712Domain domain;
    memset(&domain, 0, sizeof(domain));
    strncpy(domain.name, "GatewayWalletBatched", sizeof(domain.name) - 1);
    strncpy(domain.version, "1", sizeof(domain.version) - 1);
    domain.chain_id = config->chain.chain_id;
    memcpy(domain.verifying_contract, config->gateway_wallet_addr, 20);

    return boat_eip3009_sign(auth_out, &domain, key, sig65);
}

/*============================================================================
 * Balance: on-chain availableBalance() query on Gateway Wallet contract
 *==========================================================================*/

BoatResult boat_nano_get_balance(const BoatNanoConfig *config, const uint8_t addr[20],
                                 BoatEvmRpc *rpc, uint8_t balance[32])
{
    if (!config || !addr || !rpc || !balance) return BOAT_ERROR_ARG_NULL;

    /* availableBalance(address token, address depositor) */
    uint8_t calldata[68]; /* 4 + 32 + 32 */
    memcpy(calldata, AVAILABLE_BALANCE_SEL, 4);
    memset(calldata + 4, 0, 12);
    memcpy(calldata + 16, config->usdc_addr, 20);   /* token */
    memset(calldata + 36, 0, 12);
    memcpy(calldata + 48, addr, 20);                 /* depositor */

    uint8_t *result = NULL;
    size_t result_len = 0;
    BoatResult r = boat_evm_eth_call(rpc, config->gateway_wallet_addr,
                                     calldata, sizeof(calldata),
                                     &result, &result_len);
    if (r != BOAT_SUCCESS) return r;

    if (result_len >= 32) {
        memcpy(balance, result, 32);
    } else {
        memset(balance, 0, 32);
    }
    boat_free(result);
    return BOAT_SUCCESS;
}

/*============================================================================
 * Pay: full x402 nanopayment flow (request → 402 → sign → pay → resource)
 *
 * This is the buyer-side convenience function. It delegates to the x402
 * engine which automatically handles GatewayWalletBatched domain when
 * extra.verifyingContract is present in the 402 response.
 *==========================================================================*/

BoatResult boat_nano_pay(const char *url, const BoatX402ReqOpts *opts,
                         const BoatNanoConfig *config, const BoatKey *key,
                         uint8_t **response, size_t *response_len)
{
    if (!url || !config || !key || !response || !response_len) return BOAT_ERROR_ARG_NULL;

#if BOAT_PAY_X402_ENABLED
    /* Nanopayments uses the same x402 protocol — the 402 response from a
     * Gateway-enabled server includes extra.name="GatewayWalletBatched" and
     * extra.verifyingContract=<Gateway Wallet>, which boat_x402_process()
     * now handles automatically. */
    BoatEvmChainConfig chain = config->chain;
    return boat_x402_process(url, opts, key, &chain, response, response_len);
#else
    (void)opts;
    return BOAT_ERROR;
#endif
}

#endif /* BOAT_PAY_NANO_ENABLED && BOAT_EVM_ENABLED */

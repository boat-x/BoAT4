/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * MPP Tempo Charge method handler — client-side
 * Executes a TIP-20 (ERC-20 compatible) token transfer on the Tempo blockchain
 * and returns the transaction hash as the MPP credential payload.
 *****************************************************************************/
#include "boat_pay.h"
#include "boat_pal.h"

#include <string.h>
#include <stdio.h>

#if BOAT_PAY_MPP_ENABLED && BOAT_EVM_ENABLED

/*============================================================================
 * Tempo token address constants
 *==========================================================================*/

/* pathUSD on Tempo testnet (Moderato): 0x20c0000000000000000000000000000000000000 */
const uint8_t BOAT_MPP_TEMPO_PATHUSD_TESTNET[20] = {
    0x20, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* USDC on Tempo mainnet: 0x20c000000000000000000000b9537d11c60e8b50 */
const uint8_t BOAT_MPP_TEMPO_USDC_MAINNET[20] = {
    0x20, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xb9, 0x53, 0x7d, 0x11, 0xc6, 0x0e, 0x8b, 0x50
};

/*============================================================================
 * Tempo Charge handler
 *==========================================================================*/

BoatResult boat_mpp_tempo_charge(const BoatMppChallenge *challenge,
                                 const BoatKey *key,
                                 const BoatMppTempoConfig *config,
                                 char **credential_out)
{
    if (!challenge || !key || !config || !credential_out)
        return BOAT_ERROR_ARG_NULL;

    /* Validate this is a charge intent */
    if (strcmp(challenge->intent, "charge") != 0) {
        BOAT_LOG(BOAT_LOG_NORMAL, "MPP Tempo: expected intent='charge', got '%s'", challenge->intent);
        return BOAT_ERROR_ARG_INVALID;
    }

    /* Validate method is tempo */
    if (strcmp(challenge->method, "tempo") != 0) {
        BOAT_LOG(BOAT_LOG_NORMAL, "MPP Tempo: expected method='tempo', got '%s'", challenge->method);
        return BOAT_ERROR_ARG_INVALID;
    }

    BoatResult r;

    /* --- Parse recipient address --- */
    uint8_t recipient[20];
    size_t addr_len = 0;
    r = boat_hex_to_bin(challenge->recipient, recipient, 20, &addr_len);
    if (r != BOAT_SUCCESS || addr_len != 20) {
        BOAT_LOG(BOAT_LOG_NORMAL, "MPP Tempo: invalid recipient address '%s'", challenge->recipient);
        return BOAT_ERROR_ARG_INVALID;
    }

    /* --- Parse amount (string → uint256) ---
     * Amount is in token base units (e.g., "20000" for 0.02 pathUSD with 6 decimals). */
    uint64_t amount = (uint64_t)strtoull(challenge->amount, NULL, 10);
    if (amount == 0) {
        BOAT_LOG(BOAT_LOG_NORMAL, "MPP Tempo: invalid amount '%s'", challenge->amount);
        return BOAT_ERROR_ARG_INVALID;
    }

    /* --- Resolve token contract address ---
     * Use the currency from the challenge if it matches a known token,
     * otherwise fall back to the config's token address. */
    uint8_t token_addr[20];
    if (challenge->currency[0] != '\0') {
        size_t cur_len = 0;
        r = boat_hex_to_bin(challenge->currency, token_addr, 20, &cur_len);
        if (r != BOAT_SUCCESS || cur_len != 20) {
            /* Fall back to config */
            memcpy(token_addr, config->token_addr, 20);
        }
    } else {
        memcpy(token_addr, config->token_addr, 20);
    }

    /* --- Build ERC-20 transfer(address,uint256) calldata --- */
    uint8_t to_slot[32], amount_slot[32];
    boat_evm_abi_encode_address(recipient, to_slot);
    boat_evm_abi_encode_uint64(amount, amount_slot);

    const uint8_t *args[2] = { to_slot, amount_slot };
    size_t arg_lens[2] = { 32, 32 };
    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    r = boat_evm_abi_encode_func("transfer(address,uint256)", args, arg_lens, 2,
                                 &calldata, &calldata_len);
    if (r != BOAT_SUCCESS) return r;

    /* --- Build and send EVM transaction --- */
    BoatEvmRpc rpc;
    r = boat_evm_rpc_init(&rpc, config->rpc_url);
    if (r != BOAT_SUCCESS) { boat_evm_abi_free(calldata); return r; }

    BoatEvmTx tx;
    r = boat_evm_tx_init(&tx, &config->chain);
    if (r != BOAT_SUCCESS) { boat_evm_abi_free(calldata); return r; }

    r = boat_evm_tx_set_to(&tx, token_addr);
    if (r != BOAT_SUCCESS) { boat_evm_abi_free(calldata); return r; }

    /* Value = 0 (token transfer, not native transfer) */
    uint8_t zero_value[32] = {0};
    r = boat_evm_tx_set_value(&tx, zero_value);
    if (r != BOAT_SUCCESS) { boat_evm_abi_free(calldata); return r; }

    r = boat_evm_tx_set_data(&tx, calldata, calldata_len);
    boat_evm_abi_free(calldata);
    if (r != BOAT_SUCCESS) return r;

    /* Auto-fill nonce and gas price from RPC */
    r = boat_evm_tx_auto_fill(&tx, &rpc, key);
    if (r != BOAT_SUCCESS) return r;

    /* Token transfers need more gas than simple ETH transfers (default 21000).
     * Set 100000 which is sufficient for ERC-20/TIP-20 transfer(). */
    r = boat_evm_tx_set_gas_limit(&tx, 100000);
    if (r != BOAT_SUCCESS) return r;

    /* Sign and send */
    uint8_t txhash[32];
    r = boat_evm_tx_send(&tx, key, &rpc, txhash);
    if (r != BOAT_SUCCESS) {
        BOAT_LOG(BOAT_LOG_NORMAL, "MPP Tempo: tx send failed: %d", r);
        return r;
    }

    /* --- Build payload JSON: { "type": "hash", "hash": "0x..." } --- */
    char hash_hex[67]; /* "0x" + 64 hex chars + NUL */
    boat_bin_to_hex(txhash, 32, hash_hex, sizeof(hash_hex), true);

    char payload_json[128];
    snprintf(payload_json, sizeof(payload_json),
             "{\"type\":\"hash\",\"hash\":\"%s\"}", hash_hex);

    /* --- Build DID source: did:pkh:eip155:<chainId>:0x<address> --- */
    BoatKeyInfo info;
    r = boat_key_get_info(key, &info);
    char source[128] = "";
    if (r == BOAT_SUCCESS) {
        char addr_hex[43];
        boat_bin_to_hex(info.address, 20, addr_hex, sizeof(addr_hex), true);
        snprintf(source, sizeof(source), "did:pkh:eip155:%llu:%s",
                 (unsigned long long)config->chain.chain_id, addr_hex);
    }

    /* --- Build MPP credential --- */
    r = boat_mpp_build_credential(challenge, source[0] ? source : NULL, payload_json, credential_out);
    return r;
}

/*============================================================================
 * Convenience: full MPP Tempo Charge flow
 *==========================================================================*/

BoatResult boat_mpp_tempo_process(const char *url, const BoatPayReqOpts *opts,
                                  const BoatKey *key,
                                  const BoatMppTempoConfig *config,
                                  uint8_t **response, size_t *response_len,
                                  BoatMppReceipt *receipt_out)
{
    if (!url || !key || !config || !response || !response_len) return BOAT_ERROR_ARG_NULL;

    /* Step 1: Send the original request */
    BoatMppChallenge challenge;
    BoatResult r = boat_mpp_request(url, opts, &challenge, response, response_len);

    /* 2xx: resource returned directly, no payment needed */
    if (r == BOAT_SUCCESS) return BOAT_SUCCESS;

    /* Not 402: propagate error */
    if (r != BOAT_ERROR_HTTP_402) return r;

    /* Verify this is a Tempo charge we can handle */
    if (strcmp(challenge.method, "tempo") != 0) {
        BOAT_LOG(BOAT_LOG_NORMAL, "MPP: unsupported method '%s' (only 'tempo' supported)", challenge.method);
        return BOAT_ERROR_ARG_INVALID;
    }

    /* Step 2: Execute Tempo Charge payment */
    char *credential = NULL;
    r = boat_mpp_tempo_charge(&challenge, key, config, &credential);
    if (r != BOAT_SUCCESS) return r;

    /* Step 3: Retry request with credential */
    r = boat_mpp_pay_and_get(url, opts, credential, response, response_len, receipt_out);
    boat_free(credential);
    return r;
}

#endif /* BOAT_PAY_MPP_ENABLED && BOAT_EVM_ENABLED */

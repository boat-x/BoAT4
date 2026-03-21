/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_pay.h"
#include "boat_pal.h"
#include "sha3.h"
#include "cJSON.h"

#include <string.h>
#include <time.h>

#if BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED

/* Function selectors (verified against Circle Gateway SDK) */
static const uint8_t APPROVE_SEL[4]              = { 0x09, 0x5e, 0xa7, 0xb3 }; /* approve(address,uint256) */
static const uint8_t DEPOSIT_SEL[4]              = { 0x47, 0xe7, 0xef, 0x24 }; /* deposit(address,uint256) */
static const uint8_t INITIATE_WITHDRAWAL_SEL[4]  = { 0xc8, 0x39, 0x3b, 0xa9 }; /* initiateWithdrawal(address,uint256) */
static const uint8_t WITHDRAW_SEL[4]             = { 0x51, 0xcf, 0xf8, 0xd9 }; /* withdraw(address) — completes after delay */
static const uint8_t AVAILABLE_BALANCE_SEL[4]    = { 0x3c, 0xcb, 0x64, 0xae }; /* availableBalance(address,address) */
static const uint8_t GATEWAY_MINT_SEL[4]         = { 0x9f, 0xb0, 0x1c, 0xc5 }; /* gatewayMint(bytes,bytes) */

#define GATEWAY_API_FALLBACK "https://gateway-api-testnet.circle.com/v1"

/*============================================================================
 * EIP-712 type hashes for BurnIntent / TransferSpec
 *==========================================================================*/

/* keccak256("EIP712Domain(string name,string version)") */
static const uint8_t GW_DOMAIN_TYPEHASH[32] = {
    0xb0,0x39,0x48,0x44,0x63,0x34,0xeb,0x9b,0x21,0x96,0xd5,0xeb,0x16,0x6f,0x69,0xb9,
    0xd4,0x94,0x03,0xeb,0x4a,0x12,0xf3,0x6d,0xe8,0xd3,0xf9,0xf3,0xcb,0x8e,0x15,0xc3
};

/* keccak256("BurnIntent(uint256 maxBlockHeight,uint256 maxFee,TransferSpec spec)"
 *           "TransferSpec(...)") */
static const uint8_t BURN_INTENT_TYPEHASH[32] = {
    0x8b,0x99,0xd1,0x7a,0x83,0xa2,0xdd,0x1a,0xdd,0x9f,0xc2,0xa4,0x50,0xe2,0x27,0x32,
    0xc7,0xe8,0x56,0x4a,0xa1,0x10,0xab,0x99,0xc2,0x04,0x85,0xa7,0xa1,0x0b,0xa3,0x7c
};

/* keccak256("TransferSpec(uint32 version,uint32 sourceDomain,...)") */
static const uint8_t TRANSFER_SPEC_TYPEHASH[32] = {
    0x44,0x40,0x9c,0x7b,0xa8,0x87,0x27,0x20,0xf5,0xfc,0x29,0x0d,0x27,0x88,0xc2,0xd7,
    0x0a,0x39,0x05,0xb7,0xca,0x1c,0xdb,0x2f,0xfa,0x15,0x27,0x91,0xa6,0x9e,0x08,0x9b
};

/*============================================================================
 * Static helpers
 *==========================================================================*/

static void addr_to_bytes32(const uint8_t addr[20], uint8_t out[32])
{
    memset(out, 0, 12);
    memcpy(out + 12, addr, 20);
}

static void u32_to_slot(uint32_t val, uint8_t out[32])
{
    memset(out, 0, 32);
    out[28] = (uint8_t)(val >> 24);
    out[29] = (uint8_t)(val >> 16);
    out[30] = (uint8_t)(val >> 8);
    out[31] = (uint8_t)(val);
}

static void gw_domain_separator(uint8_t out[32])
{
    uint8_t name_hash[32], ver_hash[32];
    keccak_256((const uint8_t *)"GatewayWallet", 13, name_hash);
    keccak_256((const uint8_t *)"1", 1, ver_hash);

    uint8_t buf[96];
    memcpy(buf, GW_DOMAIN_TYPEHASH, 32);
    memcpy(buf + 32, name_hash, 32);
    memcpy(buf + 64, ver_hash, 32);
    keccak_256(buf, 96, out);
}

static void hash_transfer_spec(
    uint32_t version, uint32_t src_domain, uint32_t dst_domain,
    const uint8_t src_contract[32], const uint8_t dst_contract[32],
    const uint8_t src_token[32], const uint8_t dst_token[32],
    const uint8_t src_depositor[32], const uint8_t dst_recipient[32],
    const uint8_t src_signer[32], const uint8_t dst_caller[32],
    const uint8_t value[32], const uint8_t salt[32],
    const uint8_t *hook_data, size_t hook_data_len,
    uint8_t out[32])
{
    uint8_t buf[15 * 32];
    memcpy(buf, TRANSFER_SPEC_TYPEHASH, 32);

    uint8_t slot[32];
    u32_to_slot(version, slot);       memcpy(buf + 32, slot, 32);
    u32_to_slot(src_domain, slot);    memcpy(buf + 64, slot, 32);
    u32_to_slot(dst_domain, slot);    memcpy(buf + 96, slot, 32);
    memcpy(buf + 128, src_contract, 32);
    memcpy(buf + 160, dst_contract, 32);
    memcpy(buf + 192, src_token, 32);
    memcpy(buf + 224, dst_token, 32);
    memcpy(buf + 256, src_depositor, 32);
    memcpy(buf + 288, dst_recipient, 32);
    memcpy(buf + 320, src_signer, 32);
    memcpy(buf + 352, dst_caller, 32);
    memcpy(buf + 384, value, 32);
    memcpy(buf + 416, salt, 32);

    uint8_t hook_hash[32];
    keccak_256(hook_data ? hook_data : (const uint8_t *)"", hook_data_len, hook_hash);
    memcpy(buf + 448, hook_hash, 32);

    keccak_256(buf, sizeof(buf), out);
}

static void hash_burn_intent(
    const uint8_t max_block_height[32], const uint8_t max_fee[32],
    const uint8_t spec_hash[32], uint8_t out[32])
{
    uint8_t buf[4 * 32];
    memcpy(buf, BURN_INTENT_TYPEHASH, 32);
    memcpy(buf + 32, max_block_height, 32);
    memcpy(buf + 64, max_fee, 32);
    memcpy(buf + 96, spec_hash, 32);
    keccak_256(buf, sizeof(buf), out);
}

static BoatResult sign_burn_intent(
    const uint8_t domain_sep[32], const uint8_t struct_hash[32],
    const BoatKey *key, uint8_t sig65[65])
{
    uint8_t buf[66];
    buf[0] = 0x19;
    buf[1] = 0x01;
    memcpy(buf + 2, domain_sep, 32);
    memcpy(buf + 34, struct_hash, 32);

    uint8_t hash[32];
    keccak_256(buf, 66, hash);
    return boat_key_sign_recoverable(key, hash, sig65);
}

/*============================================================================
 * Circle Gateway — deposit / balance / withdraw
 *==========================================================================*/

BoatResult boat_gateway_deposit(const BoatGatewayConfig *config, const BoatKey *key,
                                const uint8_t amount[32], BoatEvmRpc *rpc, uint8_t txhash[32])
{
    if (!config || !key || !amount || !rpc || !txhash) return BOAT_ERROR_ARG_NULL;

    /* Step 1: ERC20 approve(gateway_wallet, amount) */
    uint8_t approve_data[68]; /* 4 + 32 + 32 */
    memcpy(approve_data, APPROVE_SEL, 4);
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

    /* Step 2: Gateway deposit(address token, uint256 value) */
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

BoatResult boat_gateway_balance(const BoatGatewayConfig *config, const uint8_t addr[20],
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

/* Trustless withdrawal step 1: initiateWithdrawal(token, amount).
 * Starts a 7-day delay. Only needed when Circle's API is unavailable. */
BoatResult boat_gateway_trustless_withdraw(const BoatGatewayConfig *config, const BoatKey *key,
                                           const uint8_t amount[32], BoatEvmRpc *rpc,
                                           uint8_t txhash[32])
{
    if (!config || !key || !amount || !rpc || !txhash) return BOAT_ERROR_ARG_NULL;

    uint8_t calldata[68]; /* 4 + 32 + 32 */
    memcpy(calldata, INITIATE_WITHDRAWAL_SEL, 4);
    memset(calldata + 4, 0, 12);
    memcpy(calldata + 16, config->usdc_addr, 20);
    memcpy(calldata + 36, amount, 32);

    BoatEvmTx tx;
    boat_evm_tx_init(&tx, &config->chain);
    boat_evm_tx_set_to(&tx, config->gateway_wallet_addr);
    boat_evm_tx_set_data(&tx, calldata, sizeof(calldata));
    boat_evm_tx_set_gas_limit(&tx, 150000);
    boat_evm_tx_auto_fill(&tx, rpc, key);

    BoatResult r = boat_evm_tx_send(&tx, key, rpc, txhash);
    if (tx.data) boat_free(tx.data);
    return r;
}

/* Trustless withdrawal step 2: withdraw(address token).
 * Call after the 7-day delay to complete the withdrawal. */
BoatResult boat_gateway_trustless_complete(const BoatGatewayConfig *config, const BoatKey *key,
                                           BoatEvmRpc *rpc, uint8_t txhash[32])
{
    if (!config || !key || !rpc || !txhash) return BOAT_ERROR_ARG_NULL;

    uint8_t calldata[36]; /* 4 + 32 */
    memcpy(calldata, WITHDRAW_SEL, 4);
    memset(calldata + 4, 0, 12);
    memcpy(calldata + 16, config->usdc_addr, 20);

    BoatEvmTx tx;
    boat_evm_tx_init(&tx, &config->chain);
    boat_evm_tx_set_to(&tx, config->gateway_wallet_addr);
    boat_evm_tx_set_data(&tx, calldata, sizeof(calldata));
    boat_evm_tx_set_gas_limit(&tx, 150000);
    boat_evm_tx_auto_fill(&tx, rpc, key);

    BoatResult r = boat_evm_tx_send(&tx, key, rpc, txhash);
    if (tx.data) boat_free(tx.data);
    return r;
}

/*============================================================================
 * Circle Gateway — cross-chain transfer
 *
 * Full flow: build BurnIntent → EIP-712 sign → POST to Gateway API →
 *            parse attestation → ABI-encode gatewayMint(bytes,bytes) →
 *            send tx on destination chain
 *==========================================================================*/

BoatResult boat_gateway_transfer(const BoatGatewayConfig *src_config,
                                 const BoatGatewayConfig *dst_config,
                                 const BoatKey *key,
                                 const uint8_t *recipient,
                                 const uint8_t amount[32],
                                 const uint8_t max_fee[32],
                                 BoatEvmRpc *dst_rpc,
                                 BoatGatewayTransferResult *result)
{
    if (!src_config || !dst_config || !key || !amount || !max_fee || !dst_rpc || !result)
        return BOAT_ERROR_ARG_NULL;

    memset(result, 0, sizeof(*result));

    BoatKeyInfo info;
    BoatResult r = boat_key_get_info(key, &info);
    if (r != BOAT_SUCCESS) return r;

    /* Build TransferSpec fields */
    uint8_t src_contract[32], dst_contract[32];
    uint8_t src_token[32], dst_token[32];
    uint8_t src_depositor[32], dst_recipient[32];
    uint8_t src_signer[32], dst_caller[32];
    uint8_t salt[32];

    addr_to_bytes32(src_config->gateway_wallet_addr, src_contract);
    addr_to_bytes32(dst_config->gateway_minter_addr, dst_contract);
    addr_to_bytes32(src_config->usdc_addr, src_token);
    addr_to_bytes32(dst_config->usdc_addr, dst_token);
    addr_to_bytes32(info.address, src_depositor);
    addr_to_bytes32(recipient ? recipient : info.address, dst_recipient);
    addr_to_bytes32(info.address, src_signer);
    memset(dst_caller, 0, 32); /* zero = anyone can call */
    boat_random(salt, 32);

    /* maxBlockHeight = maxUint256 */
    uint8_t mbh[32];
    memset(mbh, 0xFF, 32);

    /* Hash TransferSpec */
    uint8_t spec_hash[32];
    hash_transfer_spec(1, src_config->domain, dst_config->domain,
                       src_contract, dst_contract, src_token, dst_token,
                       src_depositor, dst_recipient, src_signer, dst_caller,
                       amount, salt, NULL, 0, spec_hash);

    /* Hash BurnIntent */
    uint8_t intent_hash[32];
    hash_burn_intent(mbh, max_fee, spec_hash, intent_hash);

    /* Compute domain separator and sign */
    uint8_t domain_sep[32];
    gw_domain_separator(domain_sep);

    uint8_t sig65[65] = {0};
    r = sign_burn_intent(domain_sep, intent_hash, key, sig65);
    if (r != BOAT_SUCCESS) return r;
    if (sig65[64] != 0 && sig65[64] != 1) return BOAT_ERROR_KEY_SIGN;

    /* Build JSON payload for Gateway API */
    cJSON *arr = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON *bi = cJSON_CreateObject();
    cJSON *spec = cJSON_CreateObject();

    char salt_hex[67], sig_hex[133];
    boat_bin_to_hex(salt, 32, salt_hex, sizeof(salt_hex), true);
    {
        char r_hex[67], s_hex[67];
        boat_bin_to_hex(sig65, 32, r_hex, sizeof(r_hex), true);
        boat_bin_to_hex(sig65 + 32, 32, s_hex, sizeof(s_hex), true);
        uint8_t v = sig65[64] + 27;
        snprintf(sig_hex, sizeof(sig_hex), "0x%s%s%02x", r_hex + 2, s_hex + 2, v);
    }

    char sc_hex[67], dc_hex[67], st_hex[67], dt_hex[67];
    char sd_hex[67], dr_hex[67], ss_hex[67], dcl_hex[67];
    boat_bin_to_hex(src_contract, 32, sc_hex, sizeof(sc_hex), true);
    boat_bin_to_hex(dst_contract, 32, dc_hex, sizeof(dc_hex), true);
    boat_bin_to_hex(src_token, 32, st_hex, sizeof(st_hex), true);
    boat_bin_to_hex(dst_token, 32, dt_hex, sizeof(dt_hex), true);
    boat_bin_to_hex(src_depositor, 32, sd_hex, sizeof(sd_hex), true);
    boat_bin_to_hex(dst_recipient, 32, dr_hex, sizeof(dr_hex), true);
    boat_bin_to_hex(src_signer, 32, ss_hex, sizeof(ss_hex), true);
    boat_bin_to_hex(dst_caller, 32, dcl_hex, sizeof(dcl_hex), true);

    /* Convert amount to decimal string */
    char amount_str[80];
    {
        /* Find first non-zero byte */
        int first = 0;
        while (first < 31 && amount[first] == 0) first++;
        uint64_t val = 0;
        for (int i = first; i < 32; i++) val = (val << 8) | amount[i];
        snprintf(amount_str, sizeof(amount_str), "%llu", (unsigned long long)val);
    }

    char max_fee_str[80];
    {
        int first = 0;
        while (first < 31 && max_fee[first] == 0) first++;
        uint64_t val = 0;
        for (int i = first; i < 32; i++) val = (val << 8) | max_fee[i];
        snprintf(max_fee_str, sizeof(max_fee_str), "%llu", (unsigned long long)val);
    }

    cJSON_AddNumberToObject(spec, "version", 1);
    cJSON_AddNumberToObject(spec, "sourceDomain", src_config->domain);
    cJSON_AddNumberToObject(spec, "destinationDomain", dst_config->domain);
    cJSON_AddStringToObject(spec, "sourceContract", sc_hex);
    cJSON_AddStringToObject(spec, "destinationContract", dc_hex);
    cJSON_AddStringToObject(spec, "sourceToken", st_hex);
    cJSON_AddStringToObject(spec, "destinationToken", dt_hex);
    cJSON_AddStringToObject(spec, "sourceDepositor", sd_hex);
    cJSON_AddStringToObject(spec, "destinationRecipient", dr_hex);
    cJSON_AddStringToObject(spec, "sourceSigner", ss_hex);
    cJSON_AddStringToObject(spec, "destinationCaller", dcl_hex);
    cJSON_AddStringToObject(spec, "value", amount_str);
    cJSON_AddStringToObject(spec, "salt", salt_hex);
    cJSON_AddStringToObject(spec, "hookData", "0x");

    const char *max_bh_str = "115792089237316195423570985008687907853269984665640564039457584007913129639935";
    cJSON_AddStringToObject(bi, "maxBlockHeight", max_bh_str);
    cJSON_AddStringToObject(bi, "maxFee", max_fee_str);
    cJSON_AddItemToObject(bi, "spec", spec);

    cJSON_AddItemToObject(item, "burnIntent", bi);
    cJSON_AddStringToObject(item, "signature", sig_hex);
    cJSON_AddItemToArray(arr, item);

    char *api_body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    /* POST to Gateway API */
    const BoatHttpOps *http = boat_get_http_ops();
    BoatHttpResponse api_resp = {0};
    char api_url[256];
    const char *base = (src_config->gateway_api_url[0] != '\0')
                       ? src_config->gateway_api_url : GATEWAY_API_FALLBACK;
    snprintf(api_url, sizeof(api_url), "%s/transfer", base);

    r = http->post(api_url, "application/json",
                   (const uint8_t *)api_body, strlen(api_body), NULL, &api_resp);
    free(api_body);

    if (r != BOAT_SUCCESS || !api_resp.data) {
        if (api_resp.data) http->free_response(&api_resp);
        return BOAT_ERROR_RPC_FAIL;
    }

    /* Parse API response for attestation + signature */
    char *attestation_payload = NULL;
    char *attestation_sig = NULL;

    cJSON *resp_root = cJSON_Parse((const char *)api_resp.data);
    http->free_response(&api_resp);
    if (!resp_root) return BOAT_ERROR_RPC_PARSE;

    cJSON *resp_obj = cJSON_IsArray(resp_root) ? cJSON_GetArrayItem(resp_root, 0) : resp_root;
    cJSON *att = cJSON_GetObjectItem(resp_obj, "attestation");
    cJSON *asig = cJSON_GetObjectItem(resp_obj, "signature");
    cJSON *err = cJSON_GetObjectItem(resp_obj, "error");

    if (err || !att || !asig) {
        cJSON_Delete(resp_root);
        return BOAT_ERROR_RPC_SERVER;
    }

    attestation_payload = strdup(att->valuestring);
    attestation_sig = strdup(asig->valuestring);
    cJSON_Delete(resp_root);

    /* Decode hex attestation and signature to binary */
    uint8_t att_bin[2048], sig_bin[256];
    size_t att_len = 0, sig_len = 0;
    boat_hex_to_bin(attestation_payload, att_bin, sizeof(att_bin), &att_len);
    boat_hex_to_bin(attestation_sig, sig_bin, sizeof(sig_bin), &sig_len);
    free(attestation_payload);
    free(attestation_sig);

    /* ABI encode: gatewayMint(bytes, bytes)
     * selector(4) + offset1(32) + offset2(32) + len1(32) + data1(padded) + len2(32) + data2(padded) */
    size_t att_padded = ((att_len + 31) / 32) * 32;
    size_t sig_padded = ((sig_len + 31) / 32) * 32;
    size_t calldata_len = 4 + 32 + 32 + 32 + att_padded + 32 + sig_padded;
    uint8_t *calldata = (uint8_t *)boat_malloc(calldata_len);
    if (!calldata) return BOAT_ERROR_MEM_ALLOC;
    memset(calldata, 0, calldata_len);

    /* selector */
    memcpy(calldata, GATEWAY_MINT_SEL, 4);
    /* offset to attestation bytes (0x40 = 64) */
    calldata[4 + 31] = 0x40;
    /* offset to signature bytes */
    size_t sig_offset = 64 + 32 + att_padded;
    calldata[4 + 32 + 24] = (uint8_t)(sig_offset >> 56);
    calldata[4 + 32 + 25] = (uint8_t)(sig_offset >> 48);
    calldata[4 + 32 + 26] = (uint8_t)(sig_offset >> 40);
    calldata[4 + 32 + 27] = (uint8_t)(sig_offset >> 32);
    calldata[4 + 32 + 28] = (uint8_t)(sig_offset >> 24);
    calldata[4 + 32 + 29] = (uint8_t)(sig_offset >> 16);
    calldata[4 + 32 + 30] = (uint8_t)(sig_offset >> 8);
    calldata[4 + 32 + 31] = (uint8_t)(sig_offset);
    /* attestation length */
    calldata[4 + 64 + 28] = (uint8_t)(att_len >> 24);
    calldata[4 + 64 + 29] = (uint8_t)(att_len >> 16);
    calldata[4 + 64 + 30] = (uint8_t)(att_len >> 8);
    calldata[4 + 64 + 31] = (uint8_t)(att_len);
    /* attestation data */
    memcpy(calldata + 4 + 64 + 32, att_bin, att_len);
    /* signature length */
    size_t sig_len_off = 4 + 64 + 32 + att_padded;
    calldata[sig_len_off + 28] = (uint8_t)(sig_len >> 24);
    calldata[sig_len_off + 29] = (uint8_t)(sig_len >> 16);
    calldata[sig_len_off + 30] = (uint8_t)(sig_len >> 8);
    calldata[sig_len_off + 31] = (uint8_t)(sig_len);
    /* signature data */
    memcpy(calldata + sig_len_off + 32, sig_bin, sig_len);

    /* Send gatewayMint tx on destination chain */
    BoatEvmTx mint_tx;
    boat_evm_tx_init(&mint_tx, &dst_config->chain);
    boat_evm_tx_set_to(&mint_tx, dst_config->gateway_minter_addr);
    boat_evm_tx_set_data(&mint_tx, calldata, calldata_len);
    boat_evm_tx_set_gas_limit(&mint_tx, 300000);
    boat_evm_tx_auto_fill(&mint_tx, dst_rpc, key);

    r = boat_evm_tx_send(&mint_tx, key, dst_rpc, result->mint_txhash);
    if (mint_tx.data) boat_free(mint_tx.data);
    boat_free(calldata);

    return r;
}

#endif /* BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED */

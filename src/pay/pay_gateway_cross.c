/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Circle Gateway — Cross-chain EVM <-> Solana
 *****************************************************************************/
#include "boat_pay.h"
#include "boat_pal.h"
#include "sha2.h"
#include "sha3.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#if BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED && BOAT_SOL_ENABLED

#define GATEWAY_API_FALLBACK "https://gateway-api-testnet.circle.com/v1"

/*============================================================================
 * EIP-712 helpers (duplicated from pay_gateway.c to keep files decoupled)
 *==========================================================================*/

/* keccak256("EIP712Domain(string name,string version)") */
static const uint8_t GW_DOMAIN_TYPEHASH[32] = {
    0xb0,0x39,0x48,0x44,0x63,0x34,0xeb,0x9b,
    0x21,0x96,0xd5,0xeb,0x16,0x6f,0x69,0xb9,
    0xd4,0x94,0x03,0xeb,0x4a,0x12,0xf3,0x6d,
    0xe8,0xd3,0xf9,0xf3,0xcb,0x8e,0x15,0xc3
};

static const uint8_t BURN_INTENT_TYPEHASH[32] = {
    0x8b,0x99,0xd1,0x7a,0x83,0xa2,0xdd,0x1a,
    0xdd,0x9f,0xc2,0xa4,0x50,0xe2,0x27,0x32,
    0xc7,0xe8,0x56,0x4a,0xa1,0x10,0xab,0x99,
    0xc2,0x04,0x85,0xa7,0xa1,0x0b,0xa3,0x7c
};

static const uint8_t TRANSFER_SPEC_TYPEHASH[32] = {
    0x44,0x40,0x9c,0x7b,0xa8,0x87,0x27,0x20,
    0xf5,0xfc,0x29,0x0d,0x27,0x88,0xc2,0xd7,
    0x0a,0x39,0x05,0xb7,0xca,0x1c,0xdb,0x2f,
    0xfa,0x15,0x27,0x91,0xa6,0x9e,0x08,0x9b
};

static void cross_addr_to_bytes32(const uint8_t addr[20], uint8_t out[32])
{
    memset(out, 0, 12);
    memcpy(out + 12, addr, 20);
}

static void cross_u32_to_slot(uint32_t val, uint8_t out[32])
{
    memset(out, 0, 32);
    out[28] = (uint8_t)(val >> 24);
    out[29] = (uint8_t)(val >> 16);
    out[30] = (uint8_t)(val >> 8);
    out[31] = (uint8_t)(val);
}

static void cross_gw_domain_separator(uint8_t out[32])
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

/* Solana binary BurnIntent constants (duplicated from pay_gateway_sol.c) */
static const uint8_t CROSS_BURN_MAGIC[4] = { 0x07, 0x0a, 0xfb, 0xc2 };
static const uint8_t CROSS_SPEC_MAGIC[4] = { 0xca, 0x85, 0xde, 0xf7 };
static const uint8_t CROSS_SOL_DOMAIN_HDR[16] = {
    0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/*============================================================================
 * Solana PDA helpers (duplicated to avoid cross-file coupling)
 *==========================================================================*/
static BoatResult cross_find_pda_1(const char *seed, const uint8_t prog[32], uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)seed };
    size_t lens[] = { strlen(seed) };
    return boat_sol_find_pda(seeds, lens, 1, prog, pda);
}

static BoatResult cross_find_pda_2(const char *seed, const uint8_t *arg, size_t arg_len,
                                    const uint8_t prog[32], uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)seed, arg };
    size_t lens[] = { strlen(seed), arg_len };
    return boat_sol_find_pda(seeds, lens, 2, prog, pda);
}

/*============================================================================
 * Helper: uint256 big-endian -> decimal string
 *==========================================================================*/
static void cross_uint256_to_dec(const uint8_t val[32], char *out, size_t cap)
{
    /* Find first non-zero byte */
    int start = 0;
    while (start < 31 && val[start] == 0) start++;

    /* For values that fit in uint64 (most USDC amounts) */
    if (start >= 24) {
        uint64_t v = 0;
        for (int i = start; i < 32; i++)
            v = (v << 8) | val[i];
        snprintf(out, cap, "%llu", (unsigned long long)v);
        return;
    }
    /* Fallback: hex representation with 0x prefix for large values */
    out[0] = '0'; out[1] = 'x';
    boat_bin_to_hex(val, 32, out + 2, cap - 2, false);
}

/* Helper: bytes32 -> 0x-prefixed hex string */
static void cross_bytes32_to_hex(const uint8_t val[32], char *out, size_t cap)
{
    out[0] = '0'; out[1] = 'x';
    boat_bin_to_hex(val, 32, out + 2, cap - 2, false);
}

/* Helper: 20-byte address -> 0x-prefixed hex, zero-padded to 32 bytes */
static void cross_addr20_to_hex32(const uint8_t addr[20], char *out, size_t cap)
{
    uint8_t padded[32];
    memset(padded, 0, 12);
    memcpy(padded + 12, addr, 20);
    cross_bytes32_to_hex(padded, out, cap);
}

/* Build the structured JSON payload for Gateway API /v1/transfer (EVM source) */
static char *cross_build_evm_payload(
    const BoatGatewayConfig *src_config,
    const BoatGatewaySolConfig *dst_config,
    const uint8_t depositor_addr[20],
    const uint8_t recipient[32],
    const uint8_t amount[32],
    const uint8_t max_fee[32],
    const uint8_t salt[32],
    const uint8_t evm_sig[65])
{
    char src_contract[67], dst_contract[67], src_token[67], dst_token[67];
    char depositor_hex[67], recipient_hex[67], signer_hex[67], caller_hex[67];
    char amount_str[80], max_fee_str[80];
    uint8_t zero32[32];
    memset(zero32, 0, 32);

    /* sourceContract = Gateway Wallet on source chain (padded to 32 bytes) */
    cross_addr20_to_hex32(src_config->gateway_wallet_addr, src_contract, sizeof(src_contract));
    /* destinationContract = Gateway Minter program on Solana */
    cross_bytes32_to_hex(dst_config->gateway_minter_program, dst_contract, sizeof(dst_contract));
    /* sourceToken = USDC on source EVM chain (padded to 32 bytes) */
    cross_addr20_to_hex32(src_config->usdc_addr, src_token, sizeof(src_token));
    /* destinationToken = USDC mint on Solana */
    cross_bytes32_to_hex(dst_config->usdc_mint, dst_token, sizeof(dst_token));
    /* depositor = EVM address padded to 32 bytes */
    cross_addr20_to_hex32(depositor_addr, depositor_hex, sizeof(depositor_hex));
    /* recipient = SOL pubkey (already 32 bytes) */
    cross_bytes32_to_hex(recipient, recipient_hex, sizeof(recipient_hex));
    /* signer = same as depositor */
    cross_addr20_to_hex32(depositor_addr, signer_hex, sizeof(signer_hex));
    /* caller = zero (anyone can call) */
    cross_bytes32_to_hex(zero32, caller_hex, sizeof(caller_hex));

    cross_uint256_to_dec(amount, amount_str, sizeof(amount_str));
    cross_uint256_to_dec(max_fee, max_fee_str, sizeof(max_fee_str));

    /* signature: 0x-prefixed hex of 65-byte recoverable sig */
    char sig_hex[133];
    sig_hex[0] = '0'; sig_hex[1] = 'x';
    boat_bin_to_hex(evm_sig, 65, sig_hex + 2, sizeof(sig_hex) - 2, false);

    /* salt */
    char salt_hex[67];
    cross_bytes32_to_hex(salt, salt_hex, sizeof(salt_hex));

    char *payload = (char *)boat_malloc(4096);
    if (!payload) return NULL;

    snprintf(payload, 4096,
        "[{\"burnIntent\":{"
            "\"maxBlockHeight\":\"1000000000000000000\","
            "\"maxFee\":\"%s\","
            "\"spec\":{"
                "\"version\":1,"
                "\"sourceDomain\":%u,"
                "\"destinationDomain\":%u,"
                "\"sourceContract\":\"%s\","
                "\"destinationContract\":\"%s\","
                "\"sourceToken\":\"%s\","
                "\"destinationToken\":\"%s\","
                "\"sourceDepositor\":\"%s\","
                "\"destinationRecipient\":\"%s\","
                "\"sourceSigner\":\"%s\","
                "\"destinationCaller\":\"%s\","
                "\"value\":\"%s\","
                "\"salt\":\"%s\""
            "}"
        "},\"signature\":\"%s\"}]",
        max_fee_str,
        src_config->domain, dst_config->domain,
        src_contract, dst_contract,
        src_token, dst_token,
        depositor_hex, recipient_hex,
        signer_hex, caller_hex,
        amount_str, salt_hex,
        sig_hex);

    return payload;
}

/*============================================================================
 * Public: boat_gateway_transfer_evm_to_sol()
 *
 * Burn on EVM (EIP-712 + secp256k1), mint on Solana (ReducedMintAttestation).
 *==========================================================================*/
BoatResult boat_gateway_transfer_evm_to_sol(
    const BoatGatewayConfig    *src_config,
    const BoatGatewaySolConfig *dst_config,
    const BoatKey *evm_key,
    const BoatKey *sol_key,
    const uint8_t *sol_recipient,
    const uint8_t amount[32],
    const uint8_t max_fee[32],
    BoatSolRpc *dst_rpc,
    BoatGatewaySolTransferResult *result)
{
    if (!src_config || !dst_config || !evm_key || !sol_key || !amount || !max_fee ||
        !dst_rpc || !result)
        return BOAT_ERROR_ARG_NULL;

    memset(result, 0, sizeof(*result));

    BoatKeyInfo evm_info, sol_info;
    BoatResult r = boat_key_get_info(evm_key, &evm_info);
    if (r != BOAT_SUCCESS) return r;
    r = boat_key_get_info(sol_key, &sol_info);
    if (r != BOAT_SUCCESS) return r;

    /*--- 1. Build EIP-712 BurnIntent on EVM side ---*/
    /* Derive recipient ATA first — needed for destinationRecipient */
    const uint8_t *recipient_wallet = sol_recipient ? sol_recipient : sol_info.address;
    uint8_t recipient_ata[32];
    r = boat_sol_ata_address(recipient_wallet, dst_config->usdc_mint, recipient_ata);
    if (r != BOAT_SUCCESS) return r;

    /* TransferSpec struct hash: 15 slots (typehash + 14 fields) */
    uint8_t spec_buf[32 * 15];
    size_t off = 0;
    memcpy(spec_buf + off, TRANSFER_SPEC_TYPEHASH, 32); off += 32;  /* typehash */
    cross_u32_to_slot(1, spec_buf + off); off += 32;                 /* version */
    cross_u32_to_slot(src_config->domain, spec_buf + off); off += 32;/* sourceDomain */
    cross_u32_to_slot(dst_config->domain, spec_buf + off); off += 32;/* destinationDomain */
    /* sourceContract = Gateway Wallet (20-byte addr padded to 32) */
    cross_addr_to_bytes32(src_config->gateway_wallet_addr, spec_buf + off); off += 32;
    /* destinationContract = Gateway Minter program on Solana (32 bytes) */
    memcpy(spec_buf + off, dst_config->gateway_minter_program, 32); off += 32;
    /* sourceToken = USDC on source chain (padded to 32) */
    cross_addr_to_bytes32(src_config->usdc_addr, spec_buf + off); off += 32;
    /* destinationToken = USDC mint on Solana (32 bytes) */
    memcpy(spec_buf + off, dst_config->usdc_mint, 32); off += 32;
    /* sourceDepositor = EVM sender (padded to 32) */
    cross_addr_to_bytes32(evm_info.address, spec_buf + off); off += 32;
    /* destinationRecipient = SOL recipient ATA (token account, 32 bytes) */
    memcpy(spec_buf + off, recipient_ata, 32); off += 32;
    /* sourceSigner = same as depositor */
    cross_addr_to_bytes32(evm_info.address, spec_buf + off); off += 32;
    /* destinationCaller = zero (anyone) */
    memset(spec_buf + off, 0, 32); off += 32;
    /* value = amount */
    memcpy(spec_buf + off, amount, 32); off += 32;
    /* salt = random 32 bytes for uniqueness */
    uint8_t salt[32];
    {
        /* Use time + address as entropy source */
        uint32_t t = (uint32_t)time(NULL);
        memset(salt, 0, 32);
        salt[0] = (uint8_t)(t >> 24);
        salt[1] = (uint8_t)(t >> 16);
        salt[2] = (uint8_t)(t >> 8);
        salt[3] = (uint8_t)(t);
        memcpy(salt + 4, evm_info.address, 20);
        /* Hash it for better distribution */
        uint8_t salt_hash[32];
        keccak_256(salt, 32, salt_hash);
        memcpy(salt, salt_hash, 32);
    }
    memcpy(spec_buf + off, salt, 32); off += 32;
    /* hookData = keccak256("") for empty bytes */
    {
        uint8_t empty_hash[32];
        uint8_t empty_buf = 0;
        keccak_256(&empty_buf, 0, empty_hash);
        memcpy(spec_buf + off, empty_hash, 32); off += 32;
    }

    uint8_t spec_hash[32];
    keccak_256(spec_buf, off, spec_hash);

    BOAT_LOG(BOAT_LOG_NORMAL, "specHash: %02x%02x%02x%02x...",
             spec_hash[0], spec_hash[1], spec_hash[2], spec_hash[3]);

    /* BurnIntent struct hash: typehash + maxBlockHeight + maxFee + specHash */
    uint8_t intent_fields[32 * 4];
    memcpy(intent_fields, BURN_INTENT_TYPEHASH, 32);
    /* maxBlockHeight = 1000000000000000000 (same as API payload) */
    memset(intent_fields + 32, 0, 32);
    {
        /* 1000000000000000000 = 0x0DE0B6B3A7640000 */
        intent_fields[56] = 0x0D;
        intent_fields[57] = 0xE0;
        intent_fields[58] = 0xB6;
        intent_fields[59] = 0xB3;
        intent_fields[60] = 0xA7;
        intent_fields[61] = 0x64;
        intent_fields[62] = 0x00;
        intent_fields[63] = 0x00;
    }
    memcpy(intent_fields + 64, max_fee, 32);
    memcpy(intent_fields + 96, spec_hash, 32);

    uint8_t intent_hash[32];
    keccak_256(intent_fields, 128, intent_hash);

    uint8_t domain_sep[32];
    cross_gw_domain_separator(domain_sep);

    uint8_t eip712_msg[66];
    eip712_msg[0] = 0x19;
    eip712_msg[1] = 0x01;
    memcpy(eip712_msg + 2, domain_sep, 32);
    memcpy(eip712_msg + 34, intent_hash, 32);

    uint8_t msg_hash[32];
    keccak_256(eip712_msg, 66, msg_hash);

    uint8_t evm_sig[65];
    r = boat_key_sign_recoverable(evm_key, msg_hash, evm_sig);
    if (r != BOAT_SUCCESS) return r;

    /* Ensure v is 27/28 (EIP-155 style), not 0/1 */
    if (evm_sig[64] < 27) evm_sig[64] += 27;

    BOAT_LOG(BOAT_LOG_NORMAL, "EIP-712 msgHash: %02x%02x%02x%02x..., sig v=%u",
             msg_hash[0], msg_hash[1], msg_hash[2], msg_hash[3], evm_sig[64]);

    /*--- 2. POST to Gateway API ---*/
    char *payload = cross_build_evm_payload(
        src_config, dst_config,
        evm_info.address, recipient_ata,
        amount, max_fee, salt, evm_sig);
    if (!payload) return BOAT_ERROR_MEM_ALLOC;

    BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API payload: %.1024s", payload);

    char api_url[256];
    const char *base = (src_config->gateway_api_url[0] != '\0')
                       ? src_config->gateway_api_url : GATEWAY_API_FALLBACK;
    snprintf(api_url, sizeof(api_url), "%s/transfer", base);

    const BoatHttpOps *http = boat_get_http_ops();
    if (!http || !http->post) { boat_free(payload); return BOAT_ERROR_HTTP_FAIL; }

    BoatHttpResponse api_resp = {0};
    r = http->post(api_url, "application/json",
                   (const uint8_t *)payload, strlen(payload),
                   NULL, &api_resp);
    boat_free(payload);

    if (r != BOAT_SUCCESS || !api_resp.data) {
        BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API POST failed: r=%d, data=%p", (int)r, api_resp.data);
        if (api_resp.data) http->free_response(&api_resp);
        return BOAT_ERROR_HTTP_FAIL;
    }

    /*--- 3. Parse ReducedMintAttestation ---*/
    BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API response: %.512s", (const char *)api_resp.data);

    cJSON *root = cJSON_Parse((const char *)api_resp.data);
    http->free_response(&api_resp);
    if (!root) return BOAT_ERROR_RPC_PARSE;

    cJSON *att_item = cJSON_GetObjectItem(root, "attestation");
    cJSON *sig_item = cJSON_GetObjectItem(root, "signature");
    if (!att_item || !cJSON_IsString(att_item) ||
        !sig_item || !cJSON_IsString(sig_item)) {
        char *dbg = cJSON_PrintUnformatted(root);
        if (dbg) { BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API unexpected response: %.512s", dbg); free(dbg); }
        cJSON_Delete(root);
        return BOAT_ERROR_RPC_PARSE;
    }

    uint8_t att_bin[512], att_sig_bin[128];
    size_t att_len = 0, att_sig_len = 0;
    r = boat_hex_to_bin(att_item->valuestring, att_bin, sizeof(att_bin), &att_len);
    if (r != BOAT_SUCCESS) { cJSON_Delete(root); return r; }
    r = boat_hex_to_bin(sig_item->valuestring, att_sig_bin, sizeof(att_sig_bin), &att_sig_len);
    cJSON_Delete(root);
    if (r != BOAT_SUCCESS) return r;

    /*--- 4. Build Solana gatewayMint instruction ---*/
    /* Extract transfer_spec_hash from attestation at byte offset 160 */
    if (att_len < 192) return BOAT_ERROR_RPC_PARSE;
    const uint8_t *att_spec_hash = att_bin + 160;

    BOAT_LOG(BOAT_LOG_NORMAL, "att spec_hash: %02x%02x%02x%02x...",
             att_spec_hash[0], att_spec_hash[1], att_spec_hash[2], att_spec_hash[3]);

    uint8_t minter_pda[32], minter_custody[32], used_hash_pda[32], event_auth[32];
    r = cross_find_pda_1("gateway_minter", dst_config->gateway_minter_program, minter_pda);
    if (r != BOAT_SUCCESS) return r;
    r = cross_find_pda_2("gateway_minter_custody", dst_config->usdc_mint, 32,
                          dst_config->gateway_minter_program, minter_custody);
    if (r != BOAT_SUCCESS) return r;
    r = cross_find_pda_2("used_transfer_spec_hash", att_spec_hash, 32,
                          dst_config->gateway_minter_program, used_hash_pda);
    if (r != BOAT_SUCCESS) return r;
    r = cross_find_pda_1("__event_authority", dst_config->gateway_minter_program, event_auth);
    if (r != BOAT_SUCCESS) return r;

    BoatSolInstruction ix;
    /* Instruction discriminator: [12, 0] (custom 2-byte, not Anchor 8-byte) */
    static const uint8_t MINT_DISC[2] = { 12, 0 };
    boat_sol_ix_init(&ix, dst_config->gateway_minter_program);

    /* Fixed accounts (matching real on-chain transactions) */
    boat_sol_ix_add_account(&ix, sol_info.address, true, true);       /* 0: payer (signer, writable) */
    boat_sol_ix_add_account(&ix, sol_info.address, true, false);      /* 1: destination_caller (signer) */
    boat_sol_ix_add_account(&ix, minter_pda, false, false);           /* 2: gateway_minter PDA */
    boat_sol_ix_add_account(&ix, BOAT_SOL_SYSTEM_PROGRAM_ID, false, false); /* 3: system_program */
    boat_sol_ix_add_account(&ix, BOAT_SOL_TOKEN_PROGRAM_ID, false, false);  /* 4: token_program */
    boat_sol_ix_add_account(&ix, event_auth, false, false);           /* 5: event_authority */
    boat_sol_ix_add_account(&ix, dst_config->gateway_minter_program, false, false); /* 6: program (CPI) */
    /* Per-transfer remaining accounts */
    boat_sol_ix_add_account(&ix, minter_custody, false, true);        /* 7: custody_token_account */
    boat_sol_ix_add_account(&ix, recipient_ata, false, true);         /* 8: destination_token_account (ATA) */
    boat_sol_ix_add_account(&ix, used_hash_pda, false, true);         /* 9: transfer_spec_hash_account */

    uint8_t ix_data[512];
    BoatBorshEncoder enc;
    boat_borsh_init(&enc, ix_data, sizeof(ix_data));
    /* Discriminator: raw 2 bytes, no length prefix */
    boat_borsh_write_u8(&enc, MINT_DISC[0]);
    boat_borsh_write_u8(&enc, MINT_DISC[1]);
    /* attestation: Borsh Vec<u8> = u32(len) + data */
    boat_borsh_write_bytes(&enc, att_bin, att_len);
    /* signature: Borsh Vec<u8> = u32(len) + data */
    boat_borsh_write_bytes(&enc, att_sig_bin, att_sig_len);
    BOAT_LOG(BOAT_LOG_NORMAL, "att_len=%zu, sig_len=%zu, ix_data_len=%zu",
             att_len, att_sig_len, boat_borsh_len(&enc));
    boat_sol_ix_set_data(&ix, ix_data, boat_borsh_len(&enc));

    /*--- 4b. Check attestation expiry before submitting ---*/
    /* maxBlockHeight is at attestation offset 76, 8 bytes big-endian */
    if (att_len >= 84) {
        uint64_t max_block_height = 0;
        for (int i = 0; i < 8; i++)
            max_block_height = (max_block_height << 8) | att_bin[76 + i];

        /* Query current Solana slot */
        char *slot_result = NULL;
        r = boat_sol_rpc_call(dst_rpc, "getSlot", "[{\"commitment\":\"confirmed\"}]", &slot_result);
        if (r == BOAT_SUCCESS && slot_result) {
            uint64_t current_slot = (uint64_t)strtoull(slot_result, NULL, 10);
            boat_free(slot_result);
            BOAT_LOG(BOAT_LOG_NORMAL, "Attestation maxBlockHeight=%llu, current slot=%llu, margin=%lld",
                     (unsigned long long)max_block_height,
                     (unsigned long long)current_slot,
                     (long long)(max_block_height - current_slot));
            if (current_slot > max_block_height) {
                BOAT_LOG(BOAT_LOG_NORMAL, "ERROR: Attestation already expired! "
                         "maxBlockHeight=%llu < currentSlot=%llu (behind by %llu slots)",
                         (unsigned long long)max_block_height,
                         (unsigned long long)current_slot,
                         (unsigned long long)(current_slot - max_block_height));
                return BOAT_ERROR_RPC_SERVER;
            }
        } else {
            if (slot_result) boat_free(slot_result);
            BOAT_LOG(BOAT_LOG_NORMAL, "Warning: could not query current slot, proceeding anyway");
        }
    }

    /*--- 5. Build Solana tx, sign with sol_key, send ---*/
    uint8_t blockhash[32];
    uint64_t last_valid;
    r = boat_sol_rpc_get_latest_blockhash(dst_rpc, dst_config->chain.commitment,
                                           blockhash, &last_valid);
    if (r != BOAT_SUCCESS) return r;

    /* Create recipient ATA if it doesn't exist (idempotent) */
    BoatSolInstruction create_ata_ix;
    r = boat_sol_spl_create_ata_idempotent(sol_info.address, sol_info.address,
                                            dst_config->usdc_mint, &create_ata_ix);
    if (r != BOAT_SUCCESS) return r;

    BoatSolTx tx;
    boat_sol_tx_init(&tx);
    boat_sol_tx_set_fee_payer(&tx, sol_info.address);
    boat_sol_tx_set_blockhash(&tx, blockhash);
    boat_sol_tx_add_instruction(&tx, &create_ata_ix);
    boat_sol_tx_add_instruction(&tx, &ix);

    return boat_sol_tx_send(&tx, sol_key, dst_rpc, result->signature);
}

/*============================================================================
 * Static helper: encode Solana binary BurnIntent (for sol_to_evm)
 *==========================================================================*/
static void cross_encode_u64_be(uint64_t val, uint8_t out[8])
{
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
}

static void cross_encode_u32_be(uint32_t val, uint8_t out[4])
{
    out[0] = (uint8_t)(val >> 24);
    out[1] = (uint8_t)(val >> 16);
    out[2] = (uint8_t)(val >> 8);
    out[3] = (uint8_t)(val);
}

/*
 * Encode full Solana binary BurnIntent for sol_to_evm.
 *
 * Layout (all big-endian):
 *   [4]   BurnIntent magic (0x070afbc2)
 *   [32]  maxBlockHeight (uint256be)
 *   [32]  maxFee (uint256be)
 *   [4]   transferSpecLength (u32be)
 *   --- embedded TransferSpec ---
 *   [4]   TransferSpec magic (0xca85def7)
 *   [4]   version (1)
 *   [4]   sourceDomain
 *   [4]   destinationDomain
 *   [32]  sourceContract
 *   [32]  destinationContract
 *   [32]  sourceToken
 *   [32]  destinationToken
 *   [32]  sourceDepositor
 *   [32]  destinationRecipient
 *   [32]  sourceSigner
 *   [32]  destinationCaller
 *   [32]  value (uint256be)
 *   [32]  salt
 *   [4]   hookDataLength (0)
 *
 * Total = 4 + 32 + 32 + 4 + (4+4+4+4 + 32*10 + 4) = 408 bytes
 */
static size_t cross_encode_burn_intent_full(
    uint8_t *buf, size_t cap,
    uint64_t max_block_height, uint64_t max_fee,
    uint32_t src_domain, uint32_t dst_domain,
    const uint8_t src_contract[32], const uint8_t dst_contract[32],
    const uint8_t src_token[32], const uint8_t dst_token[32],
    const uint8_t sender[32], const uint8_t recipient[32],
    uint64_t amount, const uint8_t salt[32])
{
    size_t off = 0;
    if (cap < 408) return 0;

    /* BurnIntent header */
    memcpy(buf + off, CROSS_BURN_MAGIC, 4); off += 4;

    /* maxBlockHeight as uint256be (32 bytes, value in last 8) */
    memset(buf + off, 0, 24);
    cross_encode_u64_be(max_block_height, buf + off + 24); off += 32;

    /* maxFee as uint256be (32 bytes, value in last 8) */
    memset(buf + off, 0, 24);
    cross_encode_u64_be(max_fee, buf + off + 24); off += 32;

    /* transferSpecLength: size of the TransferSpec that follows */
    /* TransferSpec = 4+4+4+4 + 32*10 + 4 = 340 bytes */
    cross_encode_u32_be(340, buf + off); off += 4;

    /* TransferSpec */
    memcpy(buf + off, CROSS_SPEC_MAGIC, 4); off += 4;
    cross_encode_u32_be(1, buf + off); off += 4;  /* version */
    cross_encode_u32_be(src_domain, buf + off); off += 4;
    cross_encode_u32_be(dst_domain, buf + off); off += 4;
    memcpy(buf + off, src_contract, 32); off += 32;
    memcpy(buf + off, dst_contract, 32); off += 32;
    memcpy(buf + off, src_token, 32); off += 32;
    memcpy(buf + off, dst_token, 32); off += 32;
    memcpy(buf + off, sender, 32); off += 32;       /* sourceDepositor */
    memcpy(buf + off, recipient, 32); off += 32;     /* destinationRecipient */
    memcpy(buf + off, sender, 32); off += 32;        /* sourceSigner */
    memset(buf + off, 0, 32); off += 32;             /* destinationCaller = zero */
    /* value: u64 amount as uint256be (last 8 bytes) */
    memset(buf + off, 0, 24);
    cross_encode_u64_be(amount, buf + off + 24); off += 32;
    memcpy(buf + off, salt, 32); off += 32;          /* salt */
    /* hookDataLength = 0 */
    cross_encode_u32_be(0, buf + off); off += 4;

    return off;
}

/* EVM gatewayMint selector */
static const uint8_t GATEWAY_MINT_SEL[4] = { 0x9f, 0xb0, 0x1c, 0xc5 };

/*============================================================================
 * Public: boat_gateway_transfer_sol_to_evm()
 *
 * Burn on Solana (binary + Ed25519), mint on EVM (gatewayMint).
 *==========================================================================*/
BoatResult boat_gateway_transfer_sol_to_evm(
    const BoatGatewaySolConfig *src_config,
    const BoatGatewayConfig    *dst_config,
    const BoatKey *sol_key,
    const BoatKey *evm_key,
    const uint8_t *evm_recipient,
    uint64_t amount,
    uint64_t max_fee,
    BoatEvmRpc *dst_rpc,
    BoatGatewayTransferResult *result)
{
    if (!src_config || !dst_config || !sol_key || !evm_key || !dst_rpc || !result)
        return BOAT_ERROR_ARG_NULL;

    memset(result, 0, sizeof(*result));

    BoatKeyInfo sol_info, evm_info;
    BoatResult r = boat_key_get_info(sol_key, &sol_info);
    if (r != BOAT_SUCCESS) return r;
    r = boat_key_get_info(evm_key, &evm_info);
    if (r != BOAT_SUCCESS) return r;

    /*--- 1. Encode Solana binary BurnIntent ---*/
    const uint8_t *recipient_addr = evm_recipient ? evm_recipient : evm_info.address;
    uint8_t evm_recipient_b32[32];
    cross_addr_to_bytes32(recipient_addr, evm_recipient_b32);

    /* Pad 20-byte EVM addresses to 32 bytes for binary encoding */
    uint8_t dst_contract_b32[32], dst_token_b32[32];
    cross_addr_to_bytes32(dst_config->gateway_minter_addr, dst_contract_b32);
    cross_addr_to_bytes32(dst_config->usdc_addr, dst_token_b32);

    /* Generate random salt */
    uint8_t salt[32];
    boat_random(salt, 32);

    uint8_t burn_buf[512];
    size_t burn_len = cross_encode_burn_intent_full(burn_buf, sizeof(burn_buf),
        1000000000000000000ULL, max_fee,  /* maxBlockHeight, maxFee */
        src_config->domain, dst_config->domain,
        src_config->gateway_wallet_program, dst_contract_b32,
        src_config->usdc_mint, dst_token_b32,
        sol_info.address, evm_recipient_b32,
        amount, salt);
    if (burn_len == 0) return BOAT_ERROR;

    /*--- 2. Prepend domain header, sign with Ed25519 ---*/
    uint8_t sign_buf[512];
    memcpy(sign_buf, CROSS_SOL_DOMAIN_HDR, 16);
    memcpy(sign_buf + 16, burn_buf, burn_len);
    size_t sign_len = 16 + burn_len;

    /* Ed25519 signs the raw message (internally uses SHA-512) */
    uint8_t sol_sig[64];
    size_t sol_sig_len = 0;
    r = boat_key_sign(sol_key, sign_buf, sign_len, sol_sig, &sol_sig_len);
    if (r != BOAT_SUCCESS) return r;

    /*--- 3. POST to Gateway API ---*/
    /* Build structured JSON payload for SOL source */
    char sender_hex[67], recipient_hex[67], zero_hex[67], sig_hex_str[133];
    char src_contract_hex[67], dst_contract_hex[67], src_token_hex[67], dst_token_hex[67];
    char salt_hex[67];
    char amount_str[32], max_fee_str[32];
    uint8_t zero32[32];
    memset(zero32, 0, 32);

    cross_bytes32_to_hex(sol_info.address, sender_hex, sizeof(sender_hex));
    cross_bytes32_to_hex(evm_recipient_b32, recipient_hex, sizeof(recipient_hex));
    cross_bytes32_to_hex(zero32, zero_hex, sizeof(zero_hex));
    cross_bytes32_to_hex(salt, salt_hex, sizeof(salt_hex));
    /* sourceContract = Gateway Wallet program on Solana */
    cross_bytes32_to_hex(src_config->gateway_wallet_program, src_contract_hex, sizeof(src_contract_hex));
    /* destinationContract = Gateway Minter on EVM (padded to 32 bytes) */
    cross_addr20_to_hex32(dst_config->gateway_minter_addr, dst_contract_hex, sizeof(dst_contract_hex));
    /* sourceToken = USDC mint on Solana */
    cross_bytes32_to_hex(src_config->usdc_mint, src_token_hex, sizeof(src_token_hex));
    /* destinationToken = USDC on EVM (padded to 32 bytes) */
    cross_addr20_to_hex32(dst_config->usdc_addr, dst_token_hex, sizeof(dst_token_hex));

    snprintf(amount_str, sizeof(amount_str), "%llu", (unsigned long long)amount);
    snprintf(max_fee_str, sizeof(max_fee_str), "%llu", (unsigned long long)max_fee);

    sig_hex_str[0] = '0'; sig_hex_str[1] = 'x';
    boat_bin_to_hex(sol_sig, 64, sig_hex_str + 2, sizeof(sig_hex_str) - 2, false);

    char *payload = (char *)boat_malloc(4096);
    if (!payload) return BOAT_ERROR_MEM_ALLOC;
    snprintf(payload, 4096,
        "[{\"burnIntent\":{"
            "\"maxBlockHeight\":\"1000000000000000000\","
            "\"maxFee\":\"%s\","
            "\"spec\":{"
                "\"version\":1,"
                "\"sourceDomain\":%u,"
                "\"destinationDomain\":%u,"
                "\"sourceContract\":\"%s\","
                "\"destinationContract\":\"%s\","
                "\"sourceToken\":\"%s\","
                "\"destinationToken\":\"%s\","
                "\"sourceDepositor\":\"%s\","
                "\"destinationRecipient\":\"%s\","
                "\"sourceSigner\":\"%s\","
                "\"destinationCaller\":\"%s\","
                "\"value\":\"%s\","
                "\"salt\":\"%s\""
            "}"
        "},\"signature\":\"%s\"}]",
        max_fee_str,
        src_config->domain, dst_config->domain,
        src_contract_hex, dst_contract_hex,
        src_token_hex, dst_token_hex,
        sender_hex, recipient_hex,
        sender_hex, zero_hex,
        amount_str, salt_hex,
        sig_hex_str);

    BOAT_LOG(BOAT_LOG_VERBOSE, "Gateway API payload: %.1024s", payload);

    char api_url[256];
    const char *base2 = (src_config->gateway_api_url[0] != '\0')
                        ? src_config->gateway_api_url : GATEWAY_API_FALLBACK;
    snprintf(api_url, sizeof(api_url), "%s/transfer", base2);

    const BoatHttpOps *http2 = boat_get_http_ops();
    if (!http2 || !http2->post) { boat_free(payload); return BOAT_ERROR_HTTP_FAIL; }

    BoatHttpResponse api_resp2 = {0};
    r = http2->post(api_url, "application/json",
                    (const uint8_t *)payload, strlen(payload),
                    NULL, &api_resp2);
    boat_free(payload);

    if (r != BOAT_SUCCESS || !api_resp2.data) {
        BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API POST failed: r=%d, data=%p", (int)r, api_resp2.data);
        if (api_resp2.data) http2->free_response(&api_resp2);
        return BOAT_ERROR_HTTP_FAIL;
    }

    /*--- 4. Parse attestation + signature ---*/
    BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API response: %.512s", (const char *)api_resp2.data);

    cJSON *root = cJSON_Parse((const char *)api_resp2.data);
    http2->free_response(&api_resp2);
    if (!root) return BOAT_ERROR_RPC_PARSE;

    cJSON *att_item = cJSON_GetObjectItem(root, "attestation");
    cJSON *sig_item = cJSON_GetObjectItem(root, "signature");
    if (!att_item || !cJSON_IsString(att_item) ||
        !sig_item || !cJSON_IsString(sig_item)) {
        char *dbg = cJSON_PrintUnformatted(root);
        if (dbg) { BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API unexpected response: %.512s", dbg); free(dbg); }
        cJSON_Delete(root);
        return BOAT_ERROR_RPC_PARSE;
    }

    uint8_t att_bin[512], att_sig_raw[65];
    size_t att_len = 0, att_sig_len = 0;
    r = boat_hex_to_bin(att_item->valuestring, att_bin, sizeof(att_bin), &att_len);
    if (r != BOAT_SUCCESS) { cJSON_Delete(root); return r; }
    r = boat_hex_to_bin(sig_item->valuestring, att_sig_raw, sizeof(att_sig_raw), &att_sig_len);
    cJSON_Delete(root);
    if (r != BOAT_SUCCESS) return r;

    /*--- 5. Build EVM gatewayMint(bytes,bytes) calldata ---*/
    size_t att_padded = ((att_len + 31) / 32) * 32;
    size_t sig_padded = ((att_sig_len + 31) / 32) * 32;
    size_t calldata_len = 4 + 64 + 32 + att_padded + 32 + sig_padded;

    uint8_t *calldata = (uint8_t *)boat_malloc(calldata_len);
    if (!calldata) return BOAT_ERROR_MEM_ALLOC;
    memset(calldata, 0, calldata_len);

    memcpy(calldata, GATEWAY_MINT_SEL, 4);
    /* offset to attestation data = 64 */
    calldata[4 + 31] = 0x40;
    /* offset to signature data */
    size_t sig_offset = 64 + 32 + att_padded;
    calldata[4 + 32 + 28] = (uint8_t)(sig_offset >> 24);
    calldata[4 + 32 + 29] = (uint8_t)(sig_offset >> 16);
    calldata[4 + 32 + 30] = (uint8_t)(sig_offset >> 8);
    calldata[4 + 32 + 31] = (uint8_t)(sig_offset);
    /* attestation length + data */
    calldata[4 + 64 + 28] = (uint8_t)(att_len >> 24);
    calldata[4 + 64 + 29] = (uint8_t)(att_len >> 16);
    calldata[4 + 64 + 30] = (uint8_t)(att_len >> 8);
    calldata[4 + 64 + 31] = (uint8_t)(att_len);
    memcpy(calldata + 4 + 64 + 32, att_bin, att_len);
    /* signature length + data */
    size_t sig_len_off = 4 + 64 + 32 + att_padded;
    calldata[sig_len_off + 28] = (uint8_t)(att_sig_len >> 24);
    calldata[sig_len_off + 29] = (uint8_t)(att_sig_len >> 16);
    calldata[sig_len_off + 30] = (uint8_t)(att_sig_len >> 8);
    calldata[sig_len_off + 31] = (uint8_t)(att_sig_len);
    memcpy(calldata + sig_len_off + 32, att_sig_raw, att_sig_len);

    /*--- 6. Send EVM tx signed with evm_key ---*/
    BoatEvmTx mint_tx;
    boat_evm_tx_init(&mint_tx, &dst_config->chain);
    boat_evm_tx_set_to(&mint_tx, dst_config->gateway_minter_addr);
    boat_evm_tx_set_data(&mint_tx, calldata, calldata_len);
    boat_evm_tx_set_gas_limit(&mint_tx, 300000);
    boat_evm_tx_auto_fill(&mint_tx, dst_rpc, evm_key);

    r = boat_evm_tx_send(&mint_tx, evm_key, dst_rpc, result->mint_txhash);
    if (mint_tx.data) boat_free(mint_tx.data);
    boat_free(calldata);

    return r;
}

#endif /* BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED && BOAT_SOL_ENABLED */

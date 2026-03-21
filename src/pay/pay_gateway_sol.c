/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Circle Gateway — Solana implementation
 *****************************************************************************/
#include "boat_pay.h"
#include "boat_pal.h"
#include "sha2.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#if BOAT_PAY_GATEWAY_ENABLED && BOAT_SOL_ENABLED

/*============================================================================
 * Program IDs (base58-decoded)
 *==========================================================================*/

/* GATEwdfmYNELfp5wDmmR6noSr2vHnAfBPMm2PvCzX5vu */
const uint8_t BOAT_GW_SOL_DEVNET_WALLET[32] = {
    0xe1,0x4b,0x33,0x86,0x3e,0xb1,0x2c,0x01,
    0x43,0x08,0xf4,0x55,0x11,0x78,0x58,0x72,
    0xc2,0xa3,0x2c,0x65,0x73,0x20,0x99,0xc8,
    0xb9,0xb1,0x63,0x96,0x58,0x08,0xf3,0x26
};

/* GATEmKK2ECL1brEngQZWCgMWPbvrEYqsV6u29dAaHavr */
const uint8_t BOAT_GW_SOL_DEVNET_MINTER[32] = {
    0xe1,0x4b,0x32,0xa2,0xfe,0x76,0x25,0xfc,
    0xbc,0x39,0x87,0x66,0x4d,0x11,0xcb,0x74,
    0x6e,0xfb,0x32,0x5a,0x13,0xaf,0x91,0xa3,
    0xe2,0xd4,0xf7,0xfc,0x60,0x15,0xa4,0x07
};

/* GATEwy4YxeiEbRJLwB6dXgg7q61e6zBPrMzYj5h1pRXQ */
const uint8_t BOAT_GW_SOL_MAINNET_WALLET[32] = {
    0xe1,0x4b,0x33,0x8d,0x9c,0x2f,0xfd,0x77,
    0x5d,0x50,0x0b,0x7b,0x37,0x9b,0x0a,0xe2,
    0x55,0xaa,0x98,0x79,0x69,0x35,0x78,0x9c,
    0x05,0x44,0xd1,0x94,0x7f,0xb3,0xa3,0x9b
};

/* GATEm5SoBJiSw1v2Pz1iPBgUYkXzCUJ27XSXhDfSyzVZ */
const uint8_t BOAT_GW_SOL_MAINNET_MINTER[32] = {
    0xe1,0x4b,0x32,0x9d,0xba,0x52,0xe3,0x24,
    0xa7,0xa8,0x5f,0x52,0xd1,0x6d,0x7e,0xf3,
    0x09,0xff,0x24,0xaa,0x5d,0x74,0x21,0xb0,
    0x31,0x6b,0x56,0x8c,0x0e,0x27,0xd7,0x8c
};

/* 4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU */
const uint8_t BOAT_GW_SOL_DEVNET_USDC[32] = {
    0x3b,0x44,0x2c,0xb3,0x91,0x21,0x57,0xf1,
    0x3a,0x93,0x3d,0x01,0x34,0x28,0x2d,0x03,
    0x2b,0x5f,0xfe,0xcd,0x01,0xa2,0xdb,0xf1,
    0xb7,0x79,0x06,0x08,0xdf,0x00,0x2e,0xa7
};

/* EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v (Solana mainnet USDC) */
const uint8_t BOAT_GW_SOL_MAINNET_USDC[32] = {
    0xc6,0xfa,0x7a,0xf3,0xbe,0xdb,0xad,0x3a,
    0x3d,0x65,0xf3,0x6a,0xab,0xc9,0x74,0x31,
    0xb1,0xbb,0xe4,0xc2,0xd2,0xf6,0xe0,0xe4,
    0x7c,0xa6,0x02,0x03,0x45,0x2f,0x5d,0x61
};

/*============================================================================
 * Instruction discriminators and constants
 *==========================================================================*/
/* Instruction discriminators (custom 2-byte, not Anchor 8-byte) */
static const uint8_t DEPOSIT_DISC[2]  = { 22, 0 };
static const uint8_t MINT_DISC[2]     = { 12, 0 };

/* BurnIntent magic: 0x070afbc2 */
static const uint8_t BURN_INTENT_MAGIC[4] = { 0x07, 0x0a, 0xfb, 0xc2 };

/* Solana domain header: 0xff followed by 15 zero bytes */
static const uint8_t SOL_DOMAIN_HEADER[16] = {
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

#define SOLANA_DOMAIN_ID  5

#define GATEWAY_SOL_API_FALLBACK "https://gateway-api-testnet.circle.com/v1"

/*============================================================================
 * PDA seed constants
 *==========================================================================*/
static const char *SEED_WALLET     = "gateway_wallet";
static const char *SEED_CUSTODY    = "gateway_wallet_custody";
static const char *SEED_DEPOSIT    = "gateway_deposit";
static const char *SEED_DENYLIST   = "denylist";
static const char *SEED_MINTER     = "gateway_minter";
static const char *SEED_MINTER_CUSTODY = "gateway_minter_custody";
static const char *SEED_USED_HASH  = "used_transfer_spec_hash";
static const char *SEED_EVENT_AUTH = "__event_authority";

/*============================================================================
 * Static helpers: PDA derivation
 *==========================================================================*/

static BoatResult gw_sol_find_wallet_pda(const uint8_t program[32], uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)SEED_WALLET };
    size_t lens[] = { strlen(SEED_WALLET) };
    return boat_sol_find_pda(seeds, lens, 1, program, pda);
}

static BoatResult gw_sol_find_custody_pda(const uint8_t program[32], const uint8_t mint[32],
                                          uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)SEED_CUSTODY, mint };
    size_t lens[] = { strlen(SEED_CUSTODY), 32 };
    return boat_sol_find_pda(seeds, lens, 2, program, pda);
}

static BoatResult gw_sol_find_deposit_pda(const uint8_t program[32], const uint8_t owner[32],
                                          const uint8_t mint[32], uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)SEED_DEPOSIT, mint, owner };
    size_t lens[] = { strlen(SEED_DEPOSIT), 32, 32 };
    return boat_sol_find_pda(seeds, lens, 3, program, pda);
}

static BoatResult gw_sol_find_denylist_pda(const uint8_t program[32],
                                           const uint8_t owner[32], uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)SEED_DENYLIST, owner };
    size_t lens[] = { strlen(SEED_DENYLIST), 32 };
    return boat_sol_find_pda(seeds, lens, 2, program, pda);
}

static BoatResult gw_sol_find_minter_pda(const uint8_t program[32], uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)SEED_MINTER };
    size_t lens[] = { strlen(SEED_MINTER) };
    return boat_sol_find_pda(seeds, lens, 1, program, pda);
}

static BoatResult gw_sol_find_minter_custody_pda(const uint8_t program[32],
                                                  const uint8_t mint[32], uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)SEED_MINTER_CUSTODY, mint };
    size_t lens[] = { strlen(SEED_MINTER_CUSTODY), 32 };
    return boat_sol_find_pda(seeds, lens, 2, program, pda);
}

static BoatResult gw_sol_find_used_hash_pda(const uint8_t program[32],
                                             const uint8_t hash[32], uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)SEED_USED_HASH, hash };
    size_t lens[] = { strlen(SEED_USED_HASH), 32 };
    return boat_sol_find_pda(seeds, lens, 2, program, pda);
}

static BoatResult gw_sol_find_event_authority_pda(const uint8_t program[32], uint8_t pda[32])
{
    const uint8_t *seeds[] = { (const uint8_t *)SEED_EVENT_AUTH };
    size_t lens[] = { strlen(SEED_EVENT_AUTH) };
    return boat_sol_find_pda(seeds, lens, 1, program, pda);
}

/*============================================================================
 * Static helpers: BurnIntent encoding (Solana binary format)
 *==========================================================================*/

/* Encode a uint64 as 8 bytes big-endian */
static void encode_u64_be(uint64_t val, uint8_t out[8])
{
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
}

/* Encode a uint32 as 4 bytes big-endian */
static void encode_u32_be(uint32_t val, uint8_t out[4])
{
    out[0] = (uint8_t)(val >> 24);
    out[1] = (uint8_t)(val >> 16);
    out[2] = (uint8_t)(val >> 8);
    out[3] = (uint8_t)(val);
}

/*
 * Encode BurnIntent in Solana binary layout.
 * Layout (big-endian):
 *   [4]  magic (0x070afbc2)
 *   [4]  version (1)
 *   [4]  sourceDomain
 *   [4]  destinationDomain
 *   [8]  amount
 *   [8]  maxFee
 *   [32] sender (owner pubkey)
 *   [32] mintRecipient (destination pubkey, 32-byte)
 *   [32] destinationCaller (zero = any)
 *   ... total ~128 bytes core fields
 *
 * Returns number of bytes written to buf.
 */
static size_t encode_burn_intent(uint8_t *buf, size_t cap,
                                 uint32_t src_domain, uint32_t dst_domain,
                                 uint64_t amount, uint64_t max_fee,
                                 const uint8_t sender[32],
                                 const uint8_t mint_recipient[32],
                                 const uint8_t destination_caller[32])
{
    size_t off = 0;
    if (cap < 128) return 0;

    /* Magic */
    memcpy(buf + off, BURN_INTENT_MAGIC, 4); off += 4;
    /* Version = 1 */
    encode_u32_be(1, buf + off); off += 4;
    /* Source domain */
    encode_u32_be(src_domain, buf + off); off += 4;
    /* Destination domain */
    encode_u32_be(dst_domain, buf + off); off += 4;
    /* Amount (u64 big-endian) */
    encode_u64_be(amount, buf + off); off += 8;
    /* Max fee (u64 big-endian) */
    encode_u64_be(max_fee, buf + off); off += 8;
    /* Sender (32 bytes) */
    memcpy(buf + off, sender, 32); off += 32;
    /* Mint recipient (32 bytes) */
    memcpy(buf + off, mint_recipient, 32); off += 32;
    /* Destination caller (32 bytes, zero = permissionless) */
    if (destination_caller) {
        memcpy(buf + off, destination_caller, 32);
    } else {
        memset(buf + off, 0, 32);
    }
    off += 32;

    return off;
}

/* TransferSpec magic: 0xca85def7 */
static const uint8_t TRANSFER_SPEC_MAGIC[4] = { 0xca, 0x85, 0xde, 0xf7 };

/*
 * Encode full Solana binary BurnIntent with embedded TransferSpec.
 *
 * Layout (all big-endian):
 *   [4]   BurnIntent magic (0x070afbc2)
 *   [32]  maxBlockHeight (uint256be)
 *   [32]  maxFee (uint256be)
 *   [4]   transferSpecLength (u32be)
 *   --- embedded TransferSpec (340 bytes) ---
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
 * Total = 408 bytes
 */
static size_t encode_burn_intent_full(
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

    memcpy(buf + off, BURN_INTENT_MAGIC, 4); off += 4;
    memset(buf + off, 0, 24);
    encode_u64_be(max_block_height, buf + off + 24); off += 32;
    memset(buf + off, 0, 24);
    encode_u64_be(max_fee, buf + off + 24); off += 32;
    encode_u32_be(340, buf + off); off += 4;  /* transferSpecLength */

    memcpy(buf + off, TRANSFER_SPEC_MAGIC, 4); off += 4;
    encode_u32_be(1, buf + off); off += 4;
    encode_u32_be(src_domain, buf + off); off += 4;
    encode_u32_be(dst_domain, buf + off); off += 4;
    memcpy(buf + off, src_contract, 32); off += 32;
    memcpy(buf + off, dst_contract, 32); off += 32;
    memcpy(buf + off, src_token, 32); off += 32;
    memcpy(buf + off, dst_token, 32); off += 32;
    memcpy(buf + off, sender, 32); off += 32;
    memcpy(buf + off, recipient, 32); off += 32;
    memcpy(buf + off, sender, 32); off += 32;  /* sourceSigner */
    memset(buf + off, 0, 32); off += 32;       /* destinationCaller */
    memset(buf + off, 0, 24);
    encode_u64_be(amount, buf + off + 24); off += 32;
    memcpy(buf + off, salt, 32); off += 32;
    encode_u32_be(0, buf + off); off += 4;     /* hookDataLength */

    return off;
}

/*============================================================================
 * Static helpers: ReducedMintAttestation decoding
 *==========================================================================*/

typedef struct {
    uint8_t  message_hash[32];
    uint64_t amount;
    uint32_t source_domain;
    uint32_t destination_domain;
    uint8_t  sender[32];
    uint8_t  recipient[32];
    uint8_t  attestation_sig[64];
} ReducedMintAttestation;

static BoatResult decode_reduced_attestation(const uint8_t *data, size_t len,
                                              ReducedMintAttestation *att)
{
    /* Minimum size: 32 + 8 + 4 + 4 + 32 + 32 + 64 = 176 bytes */
    if (!data || !att || len < 176) return BOAT_ERROR_ARG_INVALID;

    size_t off = 0;
    memcpy(att->message_hash, data + off, 32); off += 32;

    att->amount = 0;
    for (int i = 0; i < 8; i++) {
        att->amount = (att->amount << 8) | data[off + i];
    }
    off += 8;

    att->source_domain = 0;
    for (int i = 0; i < 4; i++) {
        att->source_domain = (att->source_domain << 8) | data[off + i];
    }
    off += 4;

    att->destination_domain = 0;
    for (int i = 0; i < 4; i++) {
        att->destination_domain = (att->destination_domain << 8) | data[off + i];
    }
    off += 4;

    memcpy(att->sender, data + off, 32); off += 32;
    memcpy(att->recipient, data + off, 32); off += 32;
    memcpy(att->attestation_sig, data + off, 64); off += 64;

    (void)off;
    return BOAT_SUCCESS;
}


/*============================================================================
 * Public: boat_gateway_sol_deposit()
 *==========================================================================*/
BoatResult boat_gateway_sol_deposit(const BoatGatewaySolConfig *config, const BoatKey *key,
                                    uint64_t amount, BoatSolRpc *rpc, uint8_t sig[64])
{
    if (!config || !key || !rpc || !sig) return BOAT_ERROR_ARG_NULL;

    BoatKeyInfo info;
    BoatResult r = boat_key_get_info(key, &info);
    if (r != BOAT_SUCCESS) return r;

    /* Derive PDAs */
    uint8_t wallet_pda[32], custody_pda[32], deposit_pda[32], denylist_pda[32], event_auth[32];
    r = gw_sol_find_wallet_pda(config->gateway_wallet_program, wallet_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_custody_pda(config->gateway_wallet_program, config->usdc_mint, custody_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_deposit_pda(config->gateway_wallet_program, info.address, config->usdc_mint, deposit_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_denylist_pda(config->gateway_wallet_program, info.address, denylist_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_event_authority_pda(config->gateway_wallet_program, event_auth);
    if (r != BOAT_SUCCESS) return r;

    /* Compute owner's USDC ATA */
    uint8_t owner_ata[32];
    r = boat_sol_ata_address(info.address, config->usdc_mint, owner_ata);
    if (r != BOAT_SUCCESS) return r;

    /* Build deposit instruction: discriminator [22,0] + Borsh u64 amount = 10 bytes */
    BoatSolInstruction ix;
    boat_sol_ix_init(&ix, config->gateway_wallet_program);

    /* Accounts (11): payer, owner, walletPDA, ownerATA, custodyPDA, depositPDA,
     *                denylistPDA, tokenProgram, systemProgram, eventAuthority, program(self) */
    boat_sol_ix_add_account(&ix, info.address, true, true);     /* 0: payer (signer, writable) */
    boat_sol_ix_add_account(&ix, info.address, true, false);    /* 1: owner (signer) */
    boat_sol_ix_add_account(&ix, wallet_pda, false, false);     /* 2: wallet PDA */
    boat_sol_ix_add_account(&ix, owner_ata, false, true);       /* 3: owner ATA (writable) */
    boat_sol_ix_add_account(&ix, custody_pda, false, true);     /* 4: custody PDA (writable) */
    boat_sol_ix_add_account(&ix, deposit_pda, false, true);     /* 5: deposit PDA (writable) */
    boat_sol_ix_add_account(&ix, denylist_pda, false, false);   /* 6: denylist PDA */
    boat_sol_ix_add_account(&ix, BOAT_SOL_TOKEN_PROGRAM_ID, false, false); /* 7: token program */
    boat_sol_ix_add_account(&ix, BOAT_SOL_SYSTEM_PROGRAM_ID, false, false); /* 8: system program */
    boat_sol_ix_add_account(&ix, event_auth, false, false);     /* 9: event authority */
    boat_sol_ix_add_account(&ix, config->gateway_wallet_program, false, false); /* 10: program (self) */

    /* Instruction data: discriminator(2) + u64 LE amount */
    uint8_t ix_data[10];
    memcpy(ix_data, DEPOSIT_DISC, 2);
    uint64_t amt = amount;
    for (int i = 0; i < 8; i++) {
        ix_data[2 + i] = (uint8_t)(amt & 0xFF);
        amt >>= 8;
    }
    boat_sol_ix_set_data(&ix, ix_data, 10);

    /* Get blockhash, build tx, sign, send */
    uint8_t blockhash[32];
    uint64_t last_valid;
    r = boat_sol_rpc_get_latest_blockhash(rpc, config->chain.commitment, blockhash, &last_valid);
    if (r != BOAT_SUCCESS) return r;

    BoatSolTx tx;
    boat_sol_tx_init(&tx);
    boat_sol_tx_set_fee_payer(&tx, info.address);
    boat_sol_tx_set_blockhash(&tx, blockhash);
    boat_sol_tx_add_instruction(&tx, &ix);

    r = boat_sol_tx_send(&tx, key, rpc, sig);
    return r;
}


/*============================================================================
 * Public: boat_gateway_sol_balance()
 *==========================================================================*/
BoatResult boat_gateway_sol_balance(const BoatGatewaySolConfig *config, const uint8_t owner[32],
                                    BoatSolRpc *rpc, BoatGatewaySolDepositInfo *info)
{
    if (!config || !owner || !rpc || !info) return BOAT_ERROR_ARG_NULL;

    memset(info, 0, sizeof(*info));

    /* Derive deposit PDA */
    uint8_t deposit_pda[32];
    BoatResult r = gw_sol_find_deposit_pda(config->gateway_wallet_program, owner,
                                            config->usdc_mint, deposit_pda);
    if (r != BOAT_SUCCESS) return r;

    /* Convert deposit PDA to base58 for RPC call */
    char pda_b58[64];
    r = boat_base58_encode(deposit_pda, 32, pda_b58, sizeof(pda_b58));
    if (r != BOAT_SUCCESS) return r;

    /* Call getAccountInfo — use base64 encoding */
    char params[256];
    snprintf(params, sizeof(params),
             "[\"%s\",{\"encoding\":\"base64\",\"commitment\":\"%s\"}]",
             pda_b58,
             config->chain.commitment == BOAT_SOL_COMMITMENT_FINALIZED ? "finalized" :
             config->chain.commitment == BOAT_SOL_COMMITMENT_CONFIRMED ? "confirmed" : "processed");

    char *result_json = NULL;
    r = boat_sol_rpc_call(rpc, "getAccountInfo", params, &result_json);
    if (r != BOAT_SUCCESS) {
        BOAT_LOG(BOAT_LOG_NORMAL, "gateway_sol_balance: rpc_call failed: %d", (int)r);
        return r;
    }

    if (!result_json) {
        BOAT_LOG(BOAT_LOG_NORMAL, "gateway_sol_balance: result_json is NULL");
        return BOAT_ERROR_RPC_PARSE;
    }

    BOAT_LOG(BOAT_LOG_VERBOSE, "gateway_sol_balance: result_json=%.256s", result_json);

    /* Parse JSON response — boat_rpc_call already extracts the "result" field,
     * so result_json is: {"context":{...},"value":{...}} or {"context":{...},"value":null} */
    cJSON *root = cJSON_Parse(result_json);
    if (!root) {
        BOAT_LOG(BOAT_LOG_NORMAL, "gateway_sol_balance: failed to parse result_json: %.256s", result_json);
        boat_free(result_json);
        return BOAT_ERROR_RPC_PARSE;
    }
    boat_free(result_json);

    cJSON *value = cJSON_GetObjectItem(root, "value");
    if (!value) {
        /* No "value" key — might be the raw result; treat as no deposit */
        BOAT_LOG(BOAT_LOG_VERBOSE, "gateway_sol_balance: no 'value' key in result, treating as zero");
        cJSON_Delete(root);
        return BOAT_SUCCESS;
    }
    if (!value || cJSON_IsNull(value)) {
        /* Account doesn't exist — no deposit yet */
        cJSON_Delete(root);
        return BOAT_SUCCESS;
    }

    cJSON *data_arr = cJSON_GetObjectItem(value, "data");
    if (!data_arr || !cJSON_IsArray(data_arr)) {
        BOAT_LOG(BOAT_LOG_NORMAL, "gateway_sol_balance: no 'data' array in value");
        cJSON_Delete(root);
        return BOAT_ERROR_RPC_PARSE;
    }

    cJSON *b58_item = cJSON_GetArrayItem(data_arr, 0);
    if (!b58_item || !cJSON_IsString(b58_item)) {
        BOAT_LOG(BOAT_LOG_NORMAL, "gateway_sol_balance: data[0] not a string");
        cJSON_Delete(root);
        return BOAT_ERROR_RPC_PARSE;
    }

    /* Decode base64 account data */
    const char *b64 = b58_item->valuestring;
    size_t b64_len = strlen(b64);
    BOAT_LOG(BOAT_LOG_VERBOSE, "gateway_sol_balance: b64_len=%zu, b64=%.64s...", b64_len, b64);
    uint8_t acct_data[256];
    size_t decoded_len = 0;

    /* Simple base64 decode */
    {
        static const int b64v[256] = {
            ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
            ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
            ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
            ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
            ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
            ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
            ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
            ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63
        };
        if (b64_len % 4 != 0) { cJSON_Delete(root); return BOAT_ERROR_RPC_PARSE; }
        decoded_len = b64_len / 4 * 3;
        if (b64_len > 0 && b64[b64_len - 1] == '=') decoded_len--;
        if (b64_len > 1 && b64[b64_len - 2] == '=') decoded_len--;
        if (decoded_len > sizeof(acct_data)) { cJSON_Delete(root); return BOAT_ERROR_RPC_PARSE; }
        size_t i, j;
        for (i = 0, j = 0; i < b64_len; i += 4) {
            int a = b64v[(unsigned char)b64[i]];
            int b = b64v[(unsigned char)b64[i+1]];
            int c = (b64[i+2] == '=') ? 0 : b64v[(unsigned char)b64[i+2]];
            int d = (b64[i+3] == '=') ? 0 : b64v[(unsigned char)b64[i+3]];
            uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
            if (j < decoded_len) acct_data[j++] = (triple >> 16) & 0xFF;
            if (j < decoded_len) acct_data[j++] = (triple >> 8) & 0xFF;
            if (j < decoded_len) acct_data[j++] = triple & 0xFF;
        }
    }
    cJSON_Delete(root);

    /*
     * GatewayDeposit account layout:
     *   [2]  discriminator
     *   [1]  bump
     *   [32] depositor
     *   [32] mint
     *   [8]  available_amount (u64 LE)
     *   [8]  withdrawing_amount (u64 LE)
     *   [8]  withdrawal_slot (u64 LE)
     * Total: 2 + 1 + 32 + 32 + 8 + 8 + 8 = 91 bytes
     * Minimum needed: 2 + 1 + 32 + 32 + 8 = 75 bytes (for available_amount)
     */
    if (decoded_len < 75) {
        BOAT_LOG(BOAT_LOG_NORMAL, "gateway_sol_balance: decoded_len=%zu, expected >= 75", decoded_len);
        return BOAT_ERROR_RPC_PARSE;
    }

    size_t off = 2 + 1 + 32 + 32; /* skip discriminator + bump + depositor + mint */

    info->available_amount = 0;
    for (int i = 7; i >= 0; i--) {
        info->available_amount = (info->available_amount << 8) | acct_data[off + i];
    }
    off += 8;

    if (off + 8 <= decoded_len) {
        info->withdrawing_amount = 0;
        for (int i = 7; i >= 0; i--) {
            info->withdrawing_amount = (info->withdrawing_amount << 8) | acct_data[off + i];
        }
        off += 8;
    }

    if (off + 8 <= decoded_len) {
        info->withdrawal_slot = 0;
        for (int i = 7; i >= 0; i--) {
            info->withdrawal_slot = (info->withdrawal_slot << 8) | acct_data[off + i];
        }
    }

    return BOAT_SUCCESS;
}


/*============================================================================
 * Public: boat_gateway_sol_api_balance()
 *
 * Query the Gateway API for the depositor's available balance.
 * This reflects the Gateway's off-chain view (accounts for pending transfers).
 *==========================================================================*/
BoatResult boat_gateway_sol_api_balance(const BoatGatewaySolConfig *config,
                                        const uint8_t owner[32],
                                        uint64_t *available)
{
    if (!config || !owner || !available) return BOAT_ERROR_ARG_NULL;
    *available = 0;

    /* Encode owner as base58 for the API */
    char owner_b58[64];
    BoatResult r = boat_base58_encode(owner, 32, owner_b58, sizeof(owner_b58));
    if (r != BOAT_SUCCESS) return r;

    /* Build JSON payload */
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"token\":\"USDC\",\"sources\":[{\"domain\":%u,\"depositor\":\"%s\"}]}",
             config->domain, owner_b58);

    char url[256];
    const char *base = (config->gateway_api_url[0] != '\0')
                       ? config->gateway_api_url : GATEWAY_SOL_API_FALLBACK;
    snprintf(url, sizeof(url), "%s/balances", base);

    const BoatHttpOps *http = boat_get_http_ops();
    if (!http || !http->post) return BOAT_ERROR_HTTP_FAIL;

    BoatHttpResponse resp = {0};
    r = http->post(url, "application/json",
                   (const uint8_t *)payload, strlen(payload),
                   NULL, &resp);
    if (r != BOAT_SUCCESS || !resp.data) {
        if (resp.data) http->free_response(&resp);
        return BOAT_ERROR_HTTP_FAIL;
    }

    /* Parse response: {"balances":[{"domain":5,"balance":"0.165"}]} */
    cJSON *root = cJSON_Parse((const char *)resp.data);
    http->free_response(&resp);
    if (!root) return BOAT_ERROR_RPC_PARSE;

    cJSON *balances = cJSON_GetObjectItem(root, "balances");
    if (balances && cJSON_IsArray(balances) && cJSON_GetArraySize(balances) > 0) {
        cJSON *item = cJSON_GetArrayItem(balances, 0);
        cJSON *bal = cJSON_GetObjectItem(item, "balance");
        if (bal && cJSON_IsString(bal)) {
            /* Parse decimal string to u64 (6 decimals) */
            double val = atof(bal->valuestring);
            *available = (uint64_t)(val * 1000000.0 + 0.5);
        }
    }

    cJSON_Delete(root);
    return BOAT_SUCCESS;
}


/*============================================================================
 * Public: boat_gateway_sol_transfer()
 *==========================================================================*/
BoatResult boat_gateway_sol_transfer(const BoatGatewaySolConfig *src_config,
                                     const BoatGatewaySolConfig *dst_config,
                                     const BoatKey *key,
                                     const uint8_t *recipient,
                                     uint64_t amount, uint64_t max_fee,
                                     BoatSolRpc *dst_rpc,
                                     BoatGatewaySolTransferResult *result)
{
    if (!src_config || !dst_config || !key || !dst_rpc || !result)
        return BOAT_ERROR_ARG_NULL;

    memset(result, 0, sizeof(*result));

    BoatKeyInfo info;
    BoatResult r = boat_key_get_info(key, &info);
    if (r != BOAT_SUCCESS) return r;

    /*--- 1. Encode full BurnIntent (binary, big-endian) ---*/
    const uint8_t *mint_recipient = recipient ? recipient : info.address;

    /* BurnIntent recipient must be the USDC ATA, not the wallet pubkey.
     * The on-chain program checks attestation.recipient == destination_token_account. */
    uint8_t recipient_ata[32];
    r = boat_sol_ata_address(mint_recipient, dst_config->usdc_mint, recipient_ata);
    if (r != BOAT_SUCCESS) return r;

    /* Generate random salt */
    uint8_t salt[32];
    boat_random(salt, 32);

    uint8_t burn_buf[512];
    size_t burn_len = encode_burn_intent_full(burn_buf, sizeof(burn_buf),
        1000000000000000000ULL, max_fee,
        src_config->domain, dst_config->domain,
        src_config->gateway_wallet_program, dst_config->gateway_minter_program,
        src_config->usdc_mint, dst_config->usdc_mint,
        info.address, recipient_ata,
        amount, salt);
    if (burn_len == 0) return BOAT_ERROR;

    /*--- 2. Prepend domain header + sign with Ed25519 (raw, no pre-hash) ---*/
    uint8_t sign_buf[512];
    memcpy(sign_buf, SOL_DOMAIN_HEADER, 16);
    memcpy(sign_buf + 16, burn_buf, burn_len);
    size_t sign_len = 16 + burn_len;

    uint8_t signature[64];
    size_t sig_len = 0;
    r = boat_key_sign(key, sign_buf, sign_len, signature, &sig_len);
    if (r != BOAT_SUCCESS) return r;

    /*--- 3. POST to Circle Gateway API ---*/
    /* Build structured JSON payload */
    char sender_hex[67], recip_hex[67], zero_hex[67], sig_hex_str[133];
    char src_contract_hex[67], dst_contract_hex[67], src_token_hex[67], dst_token_hex[67];
    char salt_hex[67];
    char amount_str[32], max_fee_str[32];
    uint8_t zero32[32];
    memset(zero32, 0, 32);

    /* 0x-prefixed hex for 32-byte values */
    {
        sender_hex[0] = '0'; sender_hex[1] = 'x';
        boat_bin_to_hex(info.address, 32, sender_hex + 2, sizeof(sender_hex) - 2, false);
        recip_hex[0] = '0'; recip_hex[1] = 'x';
        boat_bin_to_hex(recipient_ata, 32, recip_hex + 2, sizeof(recip_hex) - 2, false);
        zero_hex[0] = '0'; zero_hex[1] = 'x';
        boat_bin_to_hex(zero32, 32, zero_hex + 2, sizeof(zero_hex) - 2, false);
        salt_hex[0] = '0'; salt_hex[1] = 'x';
        boat_bin_to_hex(salt, 32, salt_hex + 2, sizeof(salt_hex) - 2, false);
        sig_hex_str[0] = '0'; sig_hex_str[1] = 'x';
        boat_bin_to_hex(signature, 64, sig_hex_str + 2, sizeof(sig_hex_str) - 2, false);
        /* sourceContract = Gateway Wallet program on source Solana */
        src_contract_hex[0] = '0'; src_contract_hex[1] = 'x';
        boat_bin_to_hex(src_config->gateway_wallet_program, 32, src_contract_hex + 2, sizeof(src_contract_hex) - 2, false);
        /* destinationContract = Gateway Minter program on destination Solana */
        dst_contract_hex[0] = '0'; dst_contract_hex[1] = 'x';
        boat_bin_to_hex(dst_config->gateway_minter_program, 32, dst_contract_hex + 2, sizeof(dst_contract_hex) - 2, false);
        /* sourceToken = USDC mint on source */
        src_token_hex[0] = '0'; src_token_hex[1] = 'x';
        boat_bin_to_hex(src_config->usdc_mint, 32, src_token_hex + 2, sizeof(src_token_hex) - 2, false);
        /* destinationToken = USDC mint on destination */
        dst_token_hex[0] = '0'; dst_token_hex[1] = 'x';
        boat_bin_to_hex(dst_config->usdc_mint, 32, dst_token_hex + 2, sizeof(dst_token_hex) - 2, false);
    }
    snprintf(amount_str, sizeof(amount_str), "%llu", (unsigned long long)amount);
    snprintf(max_fee_str, sizeof(max_fee_str), "%llu", (unsigned long long)max_fee);

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
        sender_hex, recip_hex,
        sender_hex, zero_hex,
        amount_str, salt_hex,
        sig_hex_str);

    BOAT_LOG(BOAT_LOG_VERBOSE, "Gateway API payload: %.1024s", payload);

    char url[256];
    const char *base = (src_config->gateway_api_url[0] != '\0')
                       ? src_config->gateway_api_url : GATEWAY_SOL_API_FALLBACK;
    snprintf(url, sizeof(url), "%s/transfer", base);

    const BoatHttpOps *http = boat_get_http_ops();
    if (!http || !http->post) {
        boat_free(payload);
        return BOAT_ERROR_HTTP_FAIL;
    }

    BoatHttpResponse api_resp = {0};
    r = http->post(url, "application/json",
                   (const uint8_t *)payload, strlen(payload),
                   NULL, &api_resp);
    boat_free(payload);

    if (r != BOAT_SUCCESS || !api_resp.data) {
        BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API POST failed: r=%d", (int)r);
        if (api_resp.data) http->free_response(&api_resp);
        return BOAT_ERROR_HTTP_FAIL;
    }

    /*--- 4. Parse attestation + signature from response ---*/
    BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API response: %.512s", (const char *)api_resp.data);

    cJSON *resp_root = cJSON_Parse((const char *)api_resp.data);
    http->free_response(&api_resp);
    if (!resp_root) return BOAT_ERROR_RPC_PARSE;

    cJSON *att_hex_item = cJSON_GetObjectItem(resp_root, "attestation");
    cJSON *att_sig_item = cJSON_GetObjectItem(resp_root, "signature");
    if (!att_hex_item || !cJSON_IsString(att_hex_item) ||
        !att_sig_item || !cJSON_IsString(att_sig_item)) {
        char *dbg = cJSON_PrintUnformatted(resp_root);
        if (dbg) { BOAT_LOG(BOAT_LOG_NORMAL, "Gateway API unexpected: %.512s", dbg); free(dbg); }
        cJSON_Delete(resp_root);
        return BOAT_ERROR_RPC_PARSE;
    }

    /* Decode attestation bytes */
    uint8_t att_bin[512];
    size_t att_len = 0;
    r = boat_hex_to_bin(att_hex_item->valuestring, att_bin, sizeof(att_bin), &att_len);
    if (r != BOAT_SUCCESS) { cJSON_Delete(resp_root); return r; }

    uint8_t att_sig_bin[128];
    size_t att_sig_len = 0;
    r = boat_hex_to_bin(att_sig_item->valuestring, att_sig_bin, sizeof(att_sig_bin), &att_sig_len);
    cJSON_Delete(resp_root);
    if (r != BOAT_SUCCESS) return r;

    /*--- 5. Extract spec_hash from attestation at offset 160 ---*/
    if (att_len < 192) return BOAT_ERROR_RPC_PARSE;
    const uint8_t *att_spec_hash = att_bin + 160;

    BOAT_LOG(BOAT_LOG_NORMAL, "att spec_hash: %02x%02x%02x%02x...",
             att_spec_hash[0], att_spec_hash[1], att_spec_hash[2], att_spec_hash[3]);

    /*--- 6. Build gatewayMint instruction on destination chain ---*/
    /* Derive minter PDAs */
    uint8_t minter_pda[32], minter_custody[32], used_hash_pda[32], event_auth[32];
    r = gw_sol_find_minter_pda(dst_config->gateway_minter_program, minter_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_minter_custody_pda(dst_config->gateway_minter_program,
                                        dst_config->usdc_mint, minter_custody);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_used_hash_pda(dst_config->gateway_minter_program,
                                   att_spec_hash, used_hash_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_event_authority_pda(dst_config->gateway_minter_program, event_auth);
    if (r != BOAT_SUCCESS) return r;

    /* Build instruction: discriminator [12,0] + Borsh(attestation, signature) */
    BoatSolInstruction ix;
    boat_sol_ix_init(&ix, dst_config->gateway_minter_program);

    /* Fixed accounts (matching real on-chain transactions) */
    boat_sol_ix_add_account(&ix, info.address, true, true);           /* 0: payer (signer, writable) */
    boat_sol_ix_add_account(&ix, info.address, true, false);          /* 1: destination_caller (signer) */
    boat_sol_ix_add_account(&ix, minter_pda, false, false);           /* 2: gateway_minter PDA */
    boat_sol_ix_add_account(&ix, BOAT_SOL_SYSTEM_PROGRAM_ID, false, false); /* 3: system_program */
    boat_sol_ix_add_account(&ix, BOAT_SOL_TOKEN_PROGRAM_ID, false, false);  /* 4: token_program */
    boat_sol_ix_add_account(&ix, event_auth, false, false);           /* 5: event_authority */
    boat_sol_ix_add_account(&ix, dst_config->gateway_minter_program, false, false); /* 6: program (CPI) */
    /* Per-transfer remaining accounts */
    boat_sol_ix_add_account(&ix, minter_custody, false, true);        /* 7: custody_token_account */
    boat_sol_ix_add_account(&ix, recipient_ata, false, true);         /* 8: destination_token_account (ATA) */
    boat_sol_ix_add_account(&ix, used_hash_pda, false, true);         /* 9: transfer_spec_hash_account */

    /* Instruction data: disc(2 raw) + Borsh Vec<u8> attestation + Borsh Vec<u8> signature */
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

    boat_sol_ix_set_data(&ix, ix_data, boat_borsh_len(&enc));

    /*--- 6b. Check attestation expiry before submitting ---*/
    if (att_len >= 84) {
        uint64_t max_block_height = 0;
        for (int i = 0; i < 8; i++)
            max_block_height = (max_block_height << 8) | att_bin[76 + i];

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
        }
    }

    /*--- 7. Get blockhash, build tx, sign, send ---*/
    uint8_t blockhash[32];
    uint64_t last_valid;
    r = boat_sol_rpc_get_latest_blockhash(dst_rpc, dst_config->chain.commitment,
                                           blockhash, &last_valid);
    if (r != BOAT_SUCCESS) return r;

    /* Create recipient ATA if it doesn't exist (idempotent) */
    BoatSolInstruction create_ata_ix;
    r = boat_sol_spl_create_ata_idempotent(info.address, mint_recipient,
                                            dst_config->usdc_mint, &create_ata_ix);
    if (r != BOAT_SUCCESS) return r;

    BoatSolTx tx;
    boat_sol_tx_init(&tx);
    boat_sol_tx_set_fee_payer(&tx, info.address);
    boat_sol_tx_set_blockhash(&tx, blockhash);
    boat_sol_tx_add_instruction(&tx, &create_ata_ix);
    boat_sol_tx_add_instruction(&tx, &ix);

    r = boat_sol_tx_send(&tx, key, dst_rpc, result->signature);
    return r;
}


/*============================================================================
 * Public: boat_gateway_sol_trustless_withdraw()
 *
 * Two-phase on-chain withdrawal (emergency path when Circle API unavailable).
 * Phase 1: initiateWithdrawal — locks funds, starts delay timer.
 *==========================================================================*/
BoatResult boat_gateway_sol_trustless_withdraw(const BoatGatewaySolConfig *config,
                                               const BoatKey *key, uint64_t amount,
                                               BoatSolRpc *rpc, uint8_t sig[64])
{
    if (!config || !key || !rpc || !sig) return BOAT_ERROR_ARG_NULL;

    BoatKeyInfo info;
    BoatResult r = boat_key_get_info(key, &info);
    if (r != BOAT_SUCCESS) return r;

    /* Derive PDAs */
    uint8_t wallet_pda[32], deposit_pda[32], event_auth[32];
    r = gw_sol_find_wallet_pda(config->gateway_wallet_program, wallet_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_deposit_pda(config->gateway_wallet_program, info.address,
                                 config->usdc_mint, deposit_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_event_authority_pda(config->gateway_wallet_program, event_auth);
    if (r != BOAT_SUCCESS) return r;

    /* Build initiateWithdrawal instruction
     * Discriminator: we use a placeholder [23, 0] — to be confirmed from on-chain IDL */
    static const uint8_t INITIATE_WD_DISC[2] = { 23, 0 };

    BoatSolInstruction ix;
    boat_sol_ix_init(&ix, config->gateway_wallet_program);

    boat_sol_ix_add_account(&ix, info.address, true, true);     /* 0: payer/owner (signer, writable) */
    boat_sol_ix_add_account(&ix, wallet_pda, false, false);     /* 1: wallet PDA */
    boat_sol_ix_add_account(&ix, deposit_pda, false, true);     /* 2: deposit PDA (writable) */
    boat_sol_ix_add_account(&ix, config->usdc_mint, false, false); /* 3: USDC mint */
    boat_sol_ix_add_account(&ix, BOAT_SOL_SYSTEM_PROGRAM_ID, false, false); /* 4: system program */
    boat_sol_ix_add_account(&ix, event_auth, false, false);     /* 5: event authority */

    /* Instruction data: disc(2) + u64 LE amount */
    uint8_t ix_data[10];
    memcpy(ix_data, INITIATE_WD_DISC, 2);
    uint64_t amt = amount;
    for (int i = 0; i < 8; i++) {
        ix_data[2 + i] = (uint8_t)(amt & 0xFF);
        amt >>= 8;
    }
    boat_sol_ix_set_data(&ix, ix_data, 10);

    /* Get blockhash, build tx, sign, send */
    uint8_t blockhash[32];
    uint64_t last_valid;
    r = boat_sol_rpc_get_latest_blockhash(rpc, config->chain.commitment, blockhash, &last_valid);
    if (r != BOAT_SUCCESS) return r;

    BoatSolTx tx;
    boat_sol_tx_init(&tx);
    boat_sol_tx_set_fee_payer(&tx, info.address);
    boat_sol_tx_set_blockhash(&tx, blockhash);
    boat_sol_tx_add_instruction(&tx, &ix);

    return boat_sol_tx_send(&tx, key, rpc, sig);
}

/*============================================================================
 * Public: boat_gateway_sol_trustless_complete()
 *
 * Phase 2: completeWithdrawal — after delay period, releases funds to owner.
 *==========================================================================*/
BoatResult boat_gateway_sol_trustless_complete(const BoatGatewaySolConfig *config,
                                               const BoatKey *key,
                                               BoatSolRpc *rpc, uint8_t sig[64])
{
    if (!config || !key || !rpc || !sig) return BOAT_ERROR_ARG_NULL;

    BoatKeyInfo info;
    BoatResult r = boat_key_get_info(key, &info);
    if (r != BOAT_SUCCESS) return r;

    /* Derive PDAs */
    uint8_t wallet_pda[32], custody_pda[32], deposit_pda[32], event_auth[32];
    r = gw_sol_find_wallet_pda(config->gateway_wallet_program, wallet_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_custody_pda(config->gateway_wallet_program, config->usdc_mint, custody_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_deposit_pda(config->gateway_wallet_program, info.address,
                                 config->usdc_mint, deposit_pda);
    if (r != BOAT_SUCCESS) return r;
    r = gw_sol_find_event_authority_pda(config->gateway_wallet_program, event_auth);
    if (r != BOAT_SUCCESS) return r;

    /* Owner's USDC ATA (destination for withdrawn funds) */
    uint8_t owner_ata[32];
    r = boat_sol_ata_address(info.address, config->usdc_mint, owner_ata);
    if (r != BOAT_SUCCESS) return r;

    /* Build completeWithdrawal instruction
     * Discriminator: placeholder [24, 0] — to be confirmed from on-chain IDL */
    static const uint8_t COMPLETE_WD_DISC[2] = { 24, 0 };

    BoatSolInstruction ix;
    boat_sol_ix_init(&ix, config->gateway_wallet_program);

    boat_sol_ix_add_account(&ix, info.address, true, true);     /* 0: payer/owner (signer, writable) */
    boat_sol_ix_add_account(&ix, wallet_pda, false, false);     /* 1: wallet PDA */
    boat_sol_ix_add_account(&ix, custody_pda, false, true);     /* 2: custody PDA (writable) */
    boat_sol_ix_add_account(&ix, deposit_pda, false, true);     /* 3: deposit PDA (writable) */
    boat_sol_ix_add_account(&ix, owner_ata, false, true);       /* 4: owner ATA (writable) */
    boat_sol_ix_add_account(&ix, config->usdc_mint, false, false); /* 5: USDC mint */
    boat_sol_ix_add_account(&ix, BOAT_SOL_TOKEN_PROGRAM_ID, false, false); /* 6: token program */
    boat_sol_ix_add_account(&ix, BOAT_SOL_SYSTEM_PROGRAM_ID, false, false); /* 7: system program */
    boat_sol_ix_add_account(&ix, event_auth, false, false);     /* 8: event authority */

    /* No instruction data beyond discriminator */
    boat_sol_ix_set_data(&ix, COMPLETE_WD_DISC, 2);

    /* Get blockhash, build tx, sign, send */
    uint8_t blockhash[32];
    uint64_t last_valid;
    r = boat_sol_rpc_get_latest_blockhash(rpc, config->chain.commitment, blockhash, &last_valid);
    if (r != BOAT_SUCCESS) return r;

    BoatSolTx tx;
    boat_sol_tx_init(&tx);
    boat_sol_tx_set_fee_payer(&tx, info.address);
    boat_sol_tx_set_blockhash(&tx, blockhash);
    boat_sol_tx_add_instruction(&tx, &ix);

    return boat_sol_tx_send(&tx, key, rpc, sig);
}

#endif /* BOAT_PAY_GATEWAY_ENABLED && BOAT_SOL_ENABLED */

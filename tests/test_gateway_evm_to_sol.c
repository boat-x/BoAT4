/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration test: Circle Gateway EVM-to-SOL cross-chain transfer
 *
 * Required env vars:
 *   BOAT_TEST_EVM_PRIVKEY  — secp256k1 key (hex, 64 chars, no 0x prefix)
 *   BOAT_TEST_SOL_PRIVKEY  — Ed25519 key (base58/hex/JSON array)
 *
 * The EVM key must have:
 *   - USDC deposited into Gateway Wallet on Polygon mainnet
 *   - MATIC for gas on Polygon
 *
 * The SOL key must have:
 *   - Mainnet SOL for gas (to submit the mint tx)
 *   - A USDC ATA on mainnet (will receive minted USDC)
 *
 * Flow:
 *   1. Import both keys
 *   2. Build EIP-712 BurnIntent on EVM side, sign with secp256k1
 *   3. POST to Circle Gateway API (mainnet)
 *   4. Parse ReducedMintAttestation
 *   5. Build Solana gatewayMint instruction, sign with Ed25519, send
 *****************************************************************************/
#include "test_common.h"
#include "boat_evm.h"
#include "boat_sol.h"
#include "boat_pay.h"
#include "boat_pal.h"

#if BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED && BOAT_SOL_ENABLED

#define POLYGON_RPC_DEFAULT     "https://polygon.drpc.org"
#define POLYGON_CHAIN_ID        137
#define POLYGON_DOMAIN          7
#define SOL_MAINNET_RPC_DEFAULT "https://api.mainnet.solana.com"
#define SOL_DOMAIN              5
#define GATEWAY_API_MAINNET     "https://gateway-api.circle.com/v1"

/* Polygon mainnet USDC: 0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359 */
static const uint8_t POLYGON_USDC[20] = {
    0x3c,0x49,0x9c,0x54,0x2c,0xEF,0x5E,0x38,0x11,0xe1,
    0x19,0x2c,0xe7,0x0d,0x8c,0xC0,0x3d,0x5c,0x33,0x59
};
/* Gateway Wallet (Polygon mainnet): 0x77777777Dcc4d5A8B6E418Fd04D8997ef11000eE */
static const uint8_t GATEWAY_WALLET[20] = {
    0x77,0x77,0x77,0x77,0xDc,0xc4,0xd5,0xA8,0xB6,0xE4,
    0x18,0xFd,0x04,0xD8,0x99,0x7e,0xf1,0x10,0x00,0xeE
};
/* Gateway Minter (Polygon mainnet): 0x2222222d7164433c4C09B0b0D809a9b52C04C205 */
static const uint8_t GATEWAY_MINTER[20] = {
    0x22,0x22,0x22,0x2d,0x71,0x64,0x43,0x3c,0x4C,0x09,
    0xB0,0xb0,0xD8,0x09,0xa9,0xb5,0x2C,0x04,0xC2,0x05
};

/* 0.01 USDC = 10000 (uint256 big-endian) */
static const uint8_t SMALL_USDC[32] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0x27,0x10
};
/* maxFee: 0.005 USDC = 5000 */
static const uint8_t MAX_FEE[32] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0x13,0x88
};

/* Deposit amount: 0.10 USDC = 100000 (enough for multiple test runs) */
static const uint8_t DEPOSIT_USDC[32] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0x01,0x86,0xA0
};

int main(int argc, char **argv)
{
    test_init("test_gateway_evm_to_sol (Polygon mainnet -> Solana mainnet)");

    /* RPC URLs: env var override or defaults */
    const char *polygon_rpc = getenv("BOAT_TEST_POLYGON_RPC");
    if (!polygon_rpc || !polygon_rpc[0]) polygon_rpc = POLYGON_RPC_DEFAULT;
    const char *sol_rpc_url = getenv("BOAT_TEST_SOL_RPC");
    if (!sol_rpc_url || !sol_rpc_url[0]) sol_rpc_url = SOL_MAINNET_RPC_DEFAULT;
    printf("  Polygon RPC: %s\n", polygon_rpc);
    printf("  Solana  RPC: %s\n", sol_rpc_url);

    /*--- 1. Import EVM key ---*/
    const char *evm_env = getenv("BOAT_TEST_EVM_PRIVKEY");
    if (!evm_env || strlen(evm_env) == 0) {
        printf("  ERROR: BOAT_TEST_EVM_PRIVKEY not set\n");
        return 1;
    }
    BoatKey *evm_key = boat_key_import_string(BOAT_KEY_TYPE_SECP256K1, evm_env);
    TEST_ASSERT("evm_key_import", evm_key != NULL, "failed to import EVM key");
    if (!evm_key) return test_summary();

    BoatKeyInfo evm_info;
    boat_key_get_info(evm_key, &evm_info);
    printf("  EVM address: ");
    test_print_address(&evm_info);

    /*--- 2. Import SOL key ---*/
    const char *sol_env = getenv("BOAT_TEST_SOL_PRIVKEY");
    if (!sol_env || strlen(sol_env) == 0) {
        printf("  ERROR: BOAT_TEST_SOL_PRIVKEY not set\n");
        boat_key_free(evm_key);
        return 1;
    }
    BoatKey *sol_key = boat_key_import_string(BOAT_KEY_TYPE_ED25519, sol_env);
    TEST_ASSERT("sol_key_import", sol_key != NULL, "failed to import SOL key");
    if (!sol_key) { boat_key_free(evm_key); return test_summary(); }

    BoatKeyInfo sol_info;
    boat_key_get_info(sol_key, &sol_info);
    printf("  SOL address: ");
    test_print_address(&sol_info);

    /*--- 3. Configure EVM source (Polygon mainnet) ---*/
    BoatGatewayConfig evm_config;
    memset(&evm_config, 0, sizeof(evm_config));
    memcpy(evm_config.gateway_wallet_addr, GATEWAY_WALLET, 20);
    memcpy(evm_config.gateway_minter_addr, GATEWAY_MINTER, 20);
    memcpy(evm_config.usdc_addr, POLYGON_USDC, 20);
    evm_config.domain = POLYGON_DOMAIN;
    strncpy(evm_config.gateway_api_url, GATEWAY_API_MAINNET,
            sizeof(evm_config.gateway_api_url) - 1);
    evm_config.chain.chain_id = POLYGON_CHAIN_ID;
    strncpy(evm_config.chain.rpc_url, polygon_rpc,
            sizeof(evm_config.chain.rpc_url) - 1);

    /*--- 4. Configure SOL destination (mainnet) ---*/
    BoatGatewaySolConfig sol_config;
    memset(&sol_config, 0, sizeof(sol_config));
    memcpy(sol_config.gateway_wallet_program, BOAT_GW_SOL_MAINNET_WALLET, 32);
    memcpy(sol_config.gateway_minter_program, BOAT_GW_SOL_MAINNET_MINTER, 32);
    memcpy(sol_config.usdc_mint, BOAT_GW_SOL_MAINNET_USDC, 32);
    sol_config.domain = SOL_DOMAIN;
    strncpy(sol_config.gateway_api_url, GATEWAY_API_MAINNET,
            sizeof(sol_config.gateway_api_url) - 1);
    strncpy(sol_config.chain.rpc_url, sol_rpc_url,
            sizeof(sol_config.chain.rpc_url) - 1);
    sol_config.chain.commitment = BOAT_SOL_COMMITMENT_CONFIRMED;

    /*--- 5. Init EVM RPC (for balance check + deposit) ---*/
    BoatEvmRpc evm_rpc;
    BoatResult r = boat_evm_rpc_init(&evm_rpc, polygon_rpc);
    TEST_ASSERT("evm_rpc_init", r == BOAT_SUCCESS, "Polygon RPC init failed");
    if (r != BOAT_SUCCESS) goto cleanup;

    /*--- 6. Check USDC wallet balance on Polygon ---*/
    {
        /* balanceOf(address) selector: 0x70a08231 */
        uint8_t bal_call[36];
        bal_call[0]=0x70; bal_call[1]=0xa0; bal_call[2]=0x82; bal_call[3]=0x31;
        memset(bal_call + 4, 0, 12);
        memcpy(bal_call + 16, evm_info.address, 20);
        uint8_t *bal_result = NULL;
        size_t bal_result_len = 0;
        r = boat_evm_eth_call(&evm_rpc, POLYGON_USDC, bal_call, 36, &bal_result, &bal_result_len);
        if (r == BOAT_SUCCESS && bal_result_len >= 32) {
            uint64_t wallet_bal = 0;
            for (int i = 24; i < 32; i++) wallet_bal = (wallet_bal << 8) | bal_result[i];
            printf("  USDC wallet balance: %llu raw (%.6f USDC)\n",
                   (unsigned long long)wallet_bal, (double)wallet_bal / 1e6);
            boat_free(bal_result);
        } else {
            printf("  WARNING: could not query USDC wallet balance (error %d)\n", (int)r);
            if (bal_result) boat_free(bal_result);
        }
    }

    /*--- 7. Check Gateway balance on Polygon ---*/
    uint8_t balance[32];
    r = boat_gateway_balance(&evm_config, evm_info.address, &evm_rpc, balance);
    if (r != BOAT_SUCCESS) {
        char msg[128];
        snprintf(msg, sizeof(msg), "balance query failed (error %d) — check RPC URL", (int)r);
        TEST_FAIL("gateway_balance", msg);
        boat_evm_rpc_free(&evm_rpc);
        goto cleanup;
    }
    TEST_PASS("gateway_balance");

    /* Print balance (last 8 bytes as u64, USDC has 6 decimals) */
    uint64_t bal_raw = 0;
    for (int i = 24; i < 32; i++) bal_raw = (bal_raw << 8) | balance[i];
    printf("  Gateway balance: %llu raw (%.6f USDC)\n",
           (unsigned long long)bal_raw, (double)bal_raw / 1e6);

    /* Need at least transfer amount + max fee = 10000 + 5000 = 15000 raw */
    uint64_t needed = 15000;
    if (bal_raw < needed) {
        printf("  Balance insufficient (%llu < %llu), depositing 0.10 USDC...\n",
               (unsigned long long)bal_raw, (unsigned long long)needed);

        /* Check wallet has enough USDC to deposit */
        uint8_t bal_call2[36];
        bal_call2[0]=0x70; bal_call2[1]=0xa0; bal_call2[2]=0x82; bal_call2[3]=0x31;
        memset(bal_call2 + 4, 0, 12);
        memcpy(bal_call2 + 16, evm_info.address, 20);
        uint8_t *wr = NULL; size_t wrl = 0;
        r = boat_evm_eth_call(&evm_rpc, POLYGON_USDC, bal_call2, 36, &wr, &wrl);
        if (r == BOAT_SUCCESS && wrl >= 32) {
            uint64_t wb = 0;
            for (int i = 24; i < 32; i++) wb = (wb << 8) | wr[i];
            boat_free(wr);
            if (wb < 100000) {
                printf("  ERROR: wallet USDC balance too low (%llu raw) to deposit. "
                       "Fund 0x023399de... with USDC on Polygon first.\n",
                       (unsigned long long)wb);
                boat_evm_rpc_free(&evm_rpc);
                goto cleanup;
            }
        } else {
            if (wr) boat_free(wr);
        }

        uint8_t dep_txhash[32];
        r = boat_gateway_deposit(&evm_config, evm_key, DEPOSIT_USDC, &evm_rpc, dep_txhash);
        if (r == BOAT_SUCCESS) {
            TEST_PASS("gateway_deposit");
            printf("  Deposit txhash: ");
            for (int i = 0; i < 16; i++) printf("%02x", dep_txhash[i]);
            printf("...\n");
            /* Wait for deposit to be indexed by Gateway */
            printf("  Waiting 20s for deposit confirmation...\n");
            boat_sleep_ms(20000);
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "deposit failed (error %d)", (int)r);
            TEST_FAIL("gateway_deposit", msg);
            boat_evm_rpc_free(&evm_rpc);
            goto cleanup;
        }

        /* Re-check balance after deposit */
        r = boat_gateway_balance(&evm_config, evm_info.address, &evm_rpc, balance);
        bal_raw = 0;
        for (int i = 24; i < 32; i++) bal_raw = (bal_raw << 8) | balance[i];
        printf("  Gateway balance after deposit: %llu raw (%.6f USDC)\n",
               (unsigned long long)bal_raw, (double)bal_raw / 1e6);
    } else {
        printf("  Balance sufficient, skipping deposit.\n");
    }

    boat_evm_rpc_free(&evm_rpc);

    /*--- 7. Init Solana RPC (destination) ---*/
    BoatSolRpc sol_rpc;
    r = boat_sol_rpc_init(&sol_rpc, sol_rpc_url);
    TEST_ASSERT("sol_rpc_init", r == BOAT_SUCCESS, "Solana RPC init failed");
    if (r != BOAT_SUCCESS) goto cleanup;

    /*--- 8. EVM -> SOL transfer ---*/
    printf("\n--- EVM-to-SOL transfer: 0.01 USDC (Polygon mainnet -> Solana mainnet) ---\n");
    printf("  Burning on Polygon, minting on Solana...\n");

    BoatGatewaySolTransferResult result;
    memset(&result, 0, sizeof(result));
    r = boat_gateway_transfer_evm_to_sol(&evm_config, &sol_config,
                                          evm_key, sol_key,
                                          SMALL_USDC, MAX_FEE,
                                          &sol_rpc, &result);
    if (r == BOAT_SUCCESS && !test_is_zero(result.signature, 64)) {
        TEST_PASS("evm_to_sol_transfer");
        printf("  Solana mint sig: ");
        for (int i = 0; i < 16; i++) printf("%02x", result.signature[i]);
        printf("...\n");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "transfer failed (error %d)", (int)r);
        TEST_FAIL("evm_to_sol_transfer", msg);
    }

    boat_sol_rpc_free(&sol_rpc);
cleanup:
    boat_key_free(evm_key);
    boat_key_free(sol_key);
    return test_summary();
}

#else

int main(void)
{
    printf("Circle Gateway cross-chain (EVM+SOL) not enabled.\n");
    printf("Rebuild with: -DBOAT_PAY_GATEWAY=ON -DBOAT_EVM=ON -DBOAT_SOL=ON\n");
    return 0;
}

#endif

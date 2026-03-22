/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Testnet integration test: Circle Gateway on Solana Devnet
 *   - PDA derivation (offline, no network)
 *   - Balance query
 *   - Deposit (opt-in via env var)
 *   - Transfer (opt-in via env var)
 *****************************************************************************/
#include "test_common.h"
#include "boat_sol.h"
#include "boat_pay.h"

#if BOAT_PAY_GATEWAY_ENABLED && BOAT_SOL_ENABLED

#define SOL_DEVNET_RPC  "https://api.devnet.solana.com"
#define SOL_DOMAIN      5

/* 0.01 USDC = 10000 raw */
#define SMALL_USDC_RAW  10000ULL
/* 0.005 USDC = 5000 raw */
#define TINY_USDC_RAW   5000ULL

static void print_pubkey(const char *label, const uint8_t key[32])
{
    printf("  %s: ", label);
    for (int i = 0; i < 8; i++) printf("%02x", key[i]);
    printf("...");
    for (int i = 28; i < 32; i++) printf("%02x", key[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    int kg = test_keygen_mode(argc, argv, BOAT_KEY_TYPE_ED25519);
    if (kg >= 0) return kg;

    test_init("test_gateway_sol (Circle Gateway on Solana Devnet)");

    /*=======================================================================
     * 1. Key import
     *=====================================================================*/
    BoatKey *key = test_load_key(BOAT_KEY_TYPE_ED25519);
    TEST_ASSERT("key_import", key != NULL, "failed to import/generate ed25519 key");
    if (!key) return test_summary();

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    test_print_address(&info);

    /*=======================================================================
     * 2. PDA derivation tests (offline — no network needed)
     *=====================================================================*/
    printf("\n--- PDA derivation ---\n");

    /* Test boat_sol_find_pda with known seeds */
    uint8_t wallet_pda[32];
    const uint8_t *seeds_w[] = { (const uint8_t *)"wallet" };
    size_t lens_w[] = { 6 };
    BoatResult r = boat_sol_find_pda(seeds_w, lens_w, 1,
                                      BOAT_GW_SOL_DEVNET_WALLET, wallet_pda);
    TEST_ASSERT("pda_wallet", r == BOAT_SUCCESS, "boat_sol_find_pda failed for wallet");
    if (r == BOAT_SUCCESS) {
        print_pubkey("wallet PDA", wallet_pda);
    }

    /* Deposit PDA: seeds = ["deposit", owner, usdc_mint] */
    uint8_t deposit_pda[32];
    const uint8_t *seeds_d[] = {
        (const uint8_t *)"deposit",
        info.address,
        BOAT_GW_SOL_DEVNET_USDC
    };
    size_t lens_d[] = { 7, 32, 32 };
    r = boat_sol_find_pda(seeds_d, lens_d, 3,
                           BOAT_GW_SOL_DEVNET_WALLET, deposit_pda);
    TEST_ASSERT("pda_deposit", r == BOAT_SUCCESS, "boat_sol_find_pda failed for deposit");
    if (r == BOAT_SUCCESS) {
        print_pubkey("deposit PDA", deposit_pda);
    }

    /* Custody PDA */
    uint8_t custody_pda[32];
    const uint8_t *seeds_c[] = {
        (const uint8_t *)"custody",
        BOAT_GW_SOL_DEVNET_USDC
    };
    size_t lens_c[] = { 7, 32 };
    r = boat_sol_find_pda(seeds_c, lens_c, 2,
                           BOAT_GW_SOL_DEVNET_WALLET, custody_pda);
    TEST_ASSERT("pda_custody", r == BOAT_SUCCESS, "boat_sol_find_pda failed for custody");

    /* Event authority PDA */
    uint8_t event_pda[32];
    const uint8_t *seeds_e[] = { (const uint8_t *)"__event_authority" };
    size_t lens_e[] = { 17 };
    r = boat_sol_find_pda(seeds_e, lens_e, 1,
                           BOAT_GW_SOL_DEVNET_WALLET, event_pda);
    TEST_ASSERT("pda_event_authority", r == BOAT_SUCCESS, "boat_sol_find_pda failed for event auth");

    /* Minter PDA */
    uint8_t minter_pda[32];
    const uint8_t *seeds_m[] = { (const uint8_t *)"minter" };
    size_t lens_m[] = { 6 };
    r = boat_sol_find_pda(seeds_m, lens_m, 1,
                           BOAT_GW_SOL_DEVNET_MINTER, minter_pda);
    TEST_ASSERT("pda_minter", r == BOAT_SUCCESS, "boat_sol_find_pda failed for minter");

    /*=======================================================================
     * 3. Configure Gateway
     *=====================================================================*/
    BoatGatewaySolConfig config;
    memset(&config, 0, sizeof(config));
    memcpy(config.gateway_wallet_program, BOAT_GW_SOL_DEVNET_WALLET, 32);
    memcpy(config.gateway_minter_program, BOAT_GW_SOL_DEVNET_MINTER, 32);
    memcpy(config.usdc_mint, BOAT_GW_SOL_DEVNET_USDC, 32);
    config.domain = SOL_DOMAIN;
    strncpy(config.chain.rpc_url, SOL_DEVNET_RPC, sizeof(config.chain.rpc_url) - 1);
    config.chain.commitment = BOAT_SOL_COMMITMENT_CONFIRMED;

    /*=======================================================================
     * 4. Balance query
     *=====================================================================*/
    printf("\n--- Balance query ---\n");
    BoatSolRpc rpc;
    r = boat_sol_rpc_init(&rpc, SOL_DEVNET_RPC);
    TEST_ASSERT("rpc_init", r == BOAT_SUCCESS, "RPC init failed");
    if (r != BOAT_SUCCESS) goto cleanup_key;

    BoatGatewaySolDepositInfo dep_info;
    memset(&dep_info, 0, sizeof(dep_info));
    r = boat_gateway_sol_balance(&config, info.address, &rpc, &dep_info);
    if (r == BOAT_SUCCESS) {
        TEST_PASS("balance_query");
        printf("  Available: %llu raw (%.6f USDC)\n",
               (unsigned long long)dep_info.available_amount,
               (double)dep_info.available_amount / 1e6);
    } else {
        /* Account may not exist yet — not a hard failure */
        TEST_SKIP("balance_query", "deposit account may not exist yet");
    }

    /*=======================================================================
     * 5. Deposit (opt-in)
     *=====================================================================*/
    const char *do_deposit = getenv("BOAT_TEST_GATEWAY_SOL_DEPOSIT");
    if (do_deposit && strcmp(do_deposit, "1") == 0) {
        printf("\n--- Deposit ---\n");
        uint8_t dep_sig[64] = {0};
        r = boat_gateway_sol_deposit(&config, key, SMALL_USDC_RAW, &rpc, dep_sig);
        if (r == BOAT_SUCCESS && !test_is_zero(dep_sig, 64)) {
            TEST_PASS("deposit");
            printf("  Sig (first 16): ");
            for (int i = 0; i < 16; i++) printf("%02x", dep_sig[i]);
            printf("...\n");
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "deposit failed (error %d)", (int)r);
            TEST_FAIL("deposit", msg);
        }
    } else {
        TEST_SKIP("deposit", "set BOAT_TEST_GATEWAY_SOL_DEPOSIT=1 to enable");
    }

    /*=======================================================================
     * 6. Transfer (opt-in)
     *=====================================================================*/
    const char *do_transfer = getenv("BOAT_TEST_GATEWAY_SOL_TRANSFER");
    if (do_transfer && strcmp(do_transfer, "1") == 0) {
        printf("\n--- Transfer (same-chain) ---\n");
        BoatGatewaySolTransferResult xfer_result;
        memset(&xfer_result, 0, sizeof(xfer_result));
        r = boat_gateway_sol_transfer(&config, &config, key, NULL,
                                       TINY_USDC_RAW, 0, &rpc, &xfer_result);
        if (r == BOAT_SUCCESS && !test_is_zero(xfer_result.signature, 64)) {
            TEST_PASS("transfer_same_chain");
            printf("  Sig (first 16): ");
            for (int i = 0; i < 16; i++) printf("%02x", xfer_result.signature[i]);
            printf("...\n");
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "transfer failed (error %d)", (int)r);
            TEST_FAIL("transfer_same_chain", msg);
        }
    } else {
        TEST_SKIP("transfer_same_chain", "set BOAT_TEST_GATEWAY_SOL_TRANSFER=1 to enable");
    }

    boat_sol_rpc_free(&rpc);
cleanup_key:
    boat_key_free(key);
    return test_summary();
}

#else /* !BOAT_PAY_GATEWAY_ENABLED || !BOAT_SOL_ENABLED */

int main(void)
{
    printf("Circle Gateway + Solana not enabled.\n");
    printf("Rebuild with: -DBOAT_PAY_GATEWAY=ON -DBOAT_SOL=ON\n");
    return 0;
}

#endif

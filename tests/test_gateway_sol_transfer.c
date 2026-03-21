/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration test: Circle Gateway SOL-to-SOL transfer on Solana mainnet
 *
 * Required env vars:
 *   BOAT_TEST_SOL_PRIVKEY   — Ed25519 sender key (base58/hex/JSON array)
 *
 * Optional env vars:
 *   BOAT_TEST_SOL_PRIVKEY2  — Ed25519 recipient key (if not set, self-transfer)
 *   BOAT_TEST_SOL_RPC       — Solana RPC URL override
 *
 * The sender must have:
 *   - Mainnet SOL for gas
 *   - Mainnet USDC deposited into Gateway Wallet (auto-deposits if needed)
 *
 * Flow:
 *   1. Import keys, derive addresses
 *   2. Check sender Gateway balance, deposit if needed
 *   3. Transfer from sender to recipient via Gateway
 *****************************************************************************/
#include "test_common.h"
#include "boat_sol.h"
#include "boat_pay.h"
#include "boat_pal.h"

#if BOAT_PAY_GATEWAY_ENABLED && BOAT_SOL_ENABLED

#define SOL_MAINNET_RPC_DEFAULT "https://api.mainnet.solana.com"
#define SOL_DOMAIN              5
#define GATEWAY_API_MAINNET     "https://gateway-api.circle.com/v1"

/* 0.005 USDC = 5000 raw */
#define TRANSFER_USDC_RAW 5000ULL
/* maxFee: 0.16 USDC = 160000 raw (Gateway requires >= 0.15) */
#define MAX_FEE_RAW       160000ULL

static BoatGatewaySolConfig make_mainnet_config(const char *rpc_url)
{
    BoatGatewaySolConfig config;
    memset(&config, 0, sizeof(config));
    memcpy(config.gateway_wallet_program, BOAT_GW_SOL_MAINNET_WALLET, 32);
    memcpy(config.gateway_minter_program, BOAT_GW_SOL_MAINNET_MINTER, 32);
    memcpy(config.usdc_mint, BOAT_GW_SOL_MAINNET_USDC, 32);
    config.domain = SOL_DOMAIN;
    strncpy(config.gateway_api_url, GATEWAY_API_MAINNET,
            sizeof(config.gateway_api_url) - 1);
    strncpy(config.chain.rpc_url, rpc_url, sizeof(config.chain.rpc_url) - 1);
    config.chain.commitment = BOAT_SOL_COMMITMENT_CONFIRMED;
    return config;
}

int main(int argc, char **argv)
{
    int kg = test_keygen_mode(argc, argv, BOAT_KEY_TYPE_ED25519);
    if (kg >= 0) return kg;

    test_init("test_gateway_sol_transfer (SOL-to-SOL on mainnet)");

    const char *sol_rpc_url = getenv("BOAT_TEST_SOL_RPC");
    if (!sol_rpc_url || !sol_rpc_url[0]) sol_rpc_url = SOL_MAINNET_RPC_DEFAULT;
    printf("  Solana RPC: %s\n", sol_rpc_url);

    /*--- 1. Import sender key ---*/
    const char *sender_env = getenv("BOAT_TEST_SOL_PRIVKEY");
    if (!sender_env || strlen(sender_env) == 0) {
        printf("  ERROR: BOAT_TEST_SOL_PRIVKEY not set\n");
        return 1;
    }
    BoatKey *sender_key = boat_key_import_string(BOAT_KEY_TYPE_ED25519, sender_env);
    TEST_ASSERT("sender_key_import", sender_key != NULL, "failed to import sender key");
    if (!sender_key) return test_summary();

    BoatKeyInfo sender_info;
    boat_key_get_info(sender_key, &sender_info);
    printf("  Sender: ");
    test_print_address(&sender_info);

    /*--- 2. Recipient: second key or self-transfer ---*/
    const char *recip_env = getenv("BOAT_TEST_SOL_PRIVKEY2");
    BoatKey *recip_key = NULL;
    BoatKeyInfo recip_info;
    const uint8_t *recipient_addr = NULL;

    if (recip_env && strlen(recip_env) > 0) {
        recip_key = boat_key_import_string(BOAT_KEY_TYPE_ED25519, recip_env);
        if (recip_key) {
            boat_key_get_info(recip_key, &recip_info);
            recipient_addr = recip_info.address;
            printf("  Recipient: ");
            test_print_address(&recip_info);
        }
    }
    if (!recipient_addr) {
        recipient_addr = sender_info.address;
        printf("  Recipient: self-transfer\n");
    }

    /*--- 3. Config + RPC ---*/
    BoatGatewaySolConfig config = make_mainnet_config(sol_rpc_url);

    BoatSolRpc rpc;
    BoatResult r = boat_sol_rpc_init(&rpc, sol_rpc_url);
    TEST_ASSERT("rpc_init", r == BOAT_SUCCESS, "RPC init failed");
    if (r != BOAT_SUCCESS) goto cleanup;

    /*--- 4. Check Gateway API balance, deposit if needed ---*/
    {
        uint64_t gw_balance = 0;
        r = boat_gateway_sol_api_balance(&config, sender_info.address, &gw_balance);
        if (r == BOAT_SUCCESS) {
            TEST_PASS("gateway_balance");
            printf("  Gateway balance: %llu raw (%.6f USDC)\n",
                   (unsigned long long)gw_balance,
                   (double)gw_balance / 1000000.0);

            uint64_t needed = TRANSFER_USDC_RAW + MAX_FEE_RAW;
            if (gw_balance < needed) {
                uint64_t deposit_amt = needed - gw_balance;
                printf("  Balance insufficient (need %llu), depositing %.6f USDC...\n",
                       (unsigned long long)needed,
                       (double)deposit_amt / 1000000.0);
                uint8_t dep_sig[64];
                r = boat_gateway_sol_deposit(&config, sender_key, deposit_amt, &rpc, dep_sig);
                if (r == BOAT_SUCCESS) {
                    TEST_PASS("gateway_deposit");
                    printf("  Deposit sig: ");
                    for (int i = 0; i < 16; i++) printf("%02x", dep_sig[i]);
                    printf("...\n");
                    printf("  Waiting 20s for confirmation...\n");
                    boat_sleep_ms(20000);
                } else {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "deposit failed (error %d)", (int)r);
                    TEST_FAIL("gateway_deposit", msg);
                }
            } else {
                printf("  Balance sufficient, skipping deposit.\n");
            }
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "balance query failed (error %d)", (int)r);
            TEST_FAIL("gateway_balance", msg);
            printf("  Proceeding with transfer anyway...\n");
        }
    }

    /*--- 5. SOL-to-SOL transfer ---*/
    printf("\n--- SOL-to-SOL transfer: %.6f USDC ---\n",
           (double)TRANSFER_USDC_RAW / 1000000.0);

    BoatGatewaySolTransferResult result;
    memset(&result, 0, sizeof(result));
    r = boat_gateway_sol_transfer(&config, &config, sender_key, recipient_addr,
                                   TRANSFER_USDC_RAW, MAX_FEE_RAW,
                                   &rpc, &result);
    if (r == BOAT_SUCCESS && !test_is_zero(result.signature, 64)) {
        TEST_PASS("sol_to_sol_transfer");
        printf("  Mint sig: ");
        for (int i = 0; i < 16; i++) printf("%02x", result.signature[i]);
        printf("...\n");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "transfer failed (error %d)", (int)r);
        TEST_FAIL("sol_to_sol_transfer", msg);
    }

    /*--- 6. Post-transfer balances (Gateway API) ---*/
    printf("\n--- Post-transfer balances ---\n");
    {
        uint64_t post_bal = 0;
        r = boat_gateway_sol_api_balance(&config, sender_info.address, &post_bal);
        if (r == BOAT_SUCCESS) {
            printf("  Sender Gateway balance: %llu raw (%.6f USDC)\n",
                   (unsigned long long)post_bal, (double)post_bal / 1e6);
        } else {
            printf("  WARNING: could not query post-transfer Gateway balance (error %d)\n", (int)r);
        }
    }

    boat_sol_rpc_free(&rpc);
cleanup:
    boat_key_free(sender_key);
    if (recip_key) boat_key_free(recip_key);
    return test_summary();
}

#else

int main(void)
{
    printf("Circle Gateway + Solana not enabled.\n");
    printf("Rebuild with: -DBOAT_PAY_GATEWAY=ON -DBOAT_SOL=ON\n");
    return 0;
}

#endif

/******************************************************************************
 * BoAT v4 Example: Circle Gateway on Solana
 *
 * Demonstrates the Circle Gateway flow on Solana Devnet:
 *   1. Check Gateway Wallet balance
 *   2. Deposit USDC (skipped if balance already sufficient)
 *   3. Same-chain transfer (instant withdrawal via Gateway API)
 *
 * Addresses are base58 strings, amounts as plain doubles.
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"
#include "boat_key.h"
#include "boat_sol.h"
#include "boat_pay.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#if BOAT_PAY_GATEWAY_ENABLED && BOAT_SOL_ENABLED

/*===========================================================================
 * Configuration — edit these for your environment
 *=========================================================================*/
static const char *PRIVATE_KEY_B58 = "your_ed25519_private_key_base58";
static const char *RPC_URL = "https://api.devnet.solana.com";

/* Circle Gateway domain ID for Solana */
#define SOL_DOMAIN  5

/*===========================================================================
 * Helpers
 *=========================================================================*/
static double usdc_from_u64(uint64_t raw)
{
    return (double)raw / 1e6;
}

static uint64_t usdc_to_u64(double usdc)
{
    return (uint64_t)round(usdc * 1e6);
}

/*===========================================================================
 * Main
 *=========================================================================*/
int main(void)
{
    boat_pal_linux_init();

    /*--- 1. Import Ed25519 key ---*/
    BoatKey *key = boat_key_import_string(BOAT_KEY_TYPE_ED25519, PRIVATE_KEY_B58);
    if (!key) {
        printf("Key import failed\n");
        return -1;
    }

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    char addr_str[64];
    boat_address_to_string(&info, addr_str, sizeof(addr_str));
    printf("Wallet: %s\n\n", addr_str);

    /*--- 2. Configure Gateway ---*/
    BoatGatewaySolConfig config;
    memset(&config, 0, sizeof(config));
    memcpy(config.gateway_wallet_program, BOAT_GW_SOL_DEVNET_WALLET, 32);
    memcpy(config.gateway_minter_program, BOAT_GW_SOL_DEVNET_MINTER, 32);
    memcpy(config.usdc_mint, BOAT_GW_SOL_DEVNET_USDC, 32);
    config.domain = SOL_DOMAIN;
    strncpy(config.chain.rpc_url, RPC_URL, sizeof(config.chain.rpc_url) - 1);
    config.chain.commitment = BOAT_SOL_COMMITMENT_CONFIRMED;

    /*--- 3. Init RPC ---*/
    BoatSolRpc rpc;
    BoatResult r = boat_sol_rpc_init(&rpc, RPC_URL);
    if (r != BOAT_SUCCESS) {
        printf("RPC init failed: %d\n", (int)r);
        boat_key_free(key);
        return -1;
    }

    /*--- 4. Check Gateway balance ---*/
    printf("=== Step 1: Check Gateway Balance ===\n");
    BoatGatewaySolDepositInfo dep_info;
    r = boat_gateway_sol_balance(&config, info.address, &rpc, &dep_info);
    if (r == BOAT_SUCCESS) {
        printf("Available: %.6f USDC\n", usdc_from_u64(dep_info.available_amount));
        printf("Withdrawing: %.6f USDC\n", usdc_from_u64(dep_info.withdrawing_amount));
    } else {
        printf("Balance query failed: %d (account may not exist yet)\n", (int)r);
    }

    /*--- 5. Deposit USDC ---*/
    double deposit_amount = 0.01;
    uint64_t deposit_raw = usdc_to_u64(deposit_amount);

    if (dep_info.available_amount < deposit_raw) {
        printf("\n=== Step 2: Deposit %.6f USDC ===\n", deposit_amount);
        uint8_t dep_sig[64];
        r = boat_gateway_sol_deposit(&config, key, deposit_raw, &rpc, dep_sig);
        if (r == BOAT_SUCCESS) {
            printf("Deposit TX signature (first 16 bytes): ");
            for (int i = 0; i < 16; i++) printf("%02x", dep_sig[i]);
            printf("...\n");
        } else {
            printf("Deposit failed: %d\n", (int)r);
            goto cleanup;
        }
    } else {
        printf("\n=== Step 2: Deposit skipped (sufficient balance) ===\n");
    }

    /*--- 6. Same-chain transfer (instant withdrawal) ---*/
    printf("\n=== Step 3: Instant Withdrawal (same-chain transfer) ===\n");
    double withdraw_amount = 0.005;
    uint64_t wd_raw = usdc_to_u64(withdraw_amount);
    uint64_t wd_fee = 0; /* same-chain: no transfer fee */

    printf("Withdrawing %.6f USDC from Gateway...\n", withdraw_amount);

    BoatGatewaySolTransferResult xfer_result;
    r = boat_gateway_sol_transfer(&config, &config, key, NULL, wd_raw, wd_fee, &rpc, &xfer_result);
    if (r == BOAT_SUCCESS) {
        printf("Transfer TX signature (first 16 bytes): ");
        for (int i = 0; i < 16; i++) printf("%02x", xfer_result.signature[i]);
        printf("...\n");
        printf("Done — %.6f USDC withdrawn from Gateway\n", withdraw_amount);
    } else {
        printf("Transfer failed: %d\n", (int)r);
    }

cleanup:
    boat_sol_rpc_free(&rpc);
    boat_key_free(key);
    return (r == BOAT_SUCCESS) ? 0 : -1;

#else
    printf("Circle Gateway + Solana not enabled.\n");
    printf("Rebuild with: -DBOAT_PAY_GATEWAY=ON -DBOAT_SOL=ON\n");
    return -1;
#endif
}

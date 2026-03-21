/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration test: Circle Gateway SOL-to-EVM cross-chain transfer
 *
 * Required env vars:
 *   BOAT_TEST_SOL_PRIVKEY  — Ed25519 key (base58/hex/JSON array)
 *   BOAT_TEST_EVM_PRIVKEY  — secp256k1 key (hex, 64 chars, no 0x prefix)
 *
 * The SOL key must have:
 *   - Mainnet SOL for gas
 *   - Mainnet USDC deposited into Gateway Wallet
 *
 * The EVM key must have:
 *   - MATIC for gas on Polygon mainnet
 *     to submit the gatewayMint tx
 *
 * Flow:
 *   1. Import both keys
 *   2. Encode Solana binary BurnIntent, sign with Ed25519
 *   3. POST to Circle Gateway API (mainnet)
 *   4. Parse attestation
 *   5. Build EVM gatewayMint(bytes,bytes) calldata, sign with secp256k1, send
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

/* 0.01 USDC = 10000 raw (Solana u64) */
#define TRANSFER_USDC_RAW  10000ULL
/* maxFee: 0.16 USDC = 160000 raw (Gateway requires >= 0.150001 for cross-chain) */
#define MAX_FEE_RAW        160000ULL

int main(int argc, char **argv)
{
    test_init("test_gateway_sol_to_evm (Solana mainnet -> Polygon mainnet)");

    /* RPC URLs: env var override or defaults */
    const char *polygon_rpc = getenv("BOAT_TEST_POLYGON_RPC");
    if (!polygon_rpc || !polygon_rpc[0]) polygon_rpc = POLYGON_RPC_DEFAULT;
    const char *sol_rpc_url = getenv("BOAT_TEST_SOL_RPC");
    if (!sol_rpc_url || !sol_rpc_url[0]) sol_rpc_url = SOL_MAINNET_RPC_DEFAULT;
    printf("  Polygon RPC: %s\n", polygon_rpc);
    printf("  Solana  RPC: %s\n", sol_rpc_url);

    /*--- 1. Import SOL key ---*/
    const char *sol_env = getenv("BOAT_TEST_SOL_PRIVKEY");
    if (!sol_env || strlen(sol_env) == 0) {
        printf("  ERROR: BOAT_TEST_SOL_PRIVKEY not set\n");
        return 1;
    }
    BoatKey *sol_key = boat_key_import_string(BOAT_KEY_TYPE_ED25519, sol_env);
    TEST_ASSERT("sol_key_import", sol_key != NULL, "failed to import SOL key");
    if (!sol_key) return test_summary();

    BoatKeyInfo sol_info;
    boat_key_get_info(sol_key, &sol_info);
    printf("  SOL address: ");
    test_print_address(&sol_info);

    /*--- 2. Import EVM key ---*/
    const char *evm_env = getenv("BOAT_TEST_EVM_PRIVKEY");
    if (!evm_env || strlen(evm_env) == 0) {
        printf("  ERROR: BOAT_TEST_EVM_PRIVKEY not set\n");
        boat_key_free(sol_key);
        return 1;
    }
    BoatKey *evm_key = boat_key_import_string(BOAT_KEY_TYPE_SECP256K1, evm_env);
    TEST_ASSERT("evm_key_import", evm_key != NULL, "failed to import EVM key");
    if (!evm_key) { boat_key_free(sol_key); return test_summary(); }

    BoatKeyInfo evm_info;
    boat_key_get_info(evm_key, &evm_info);
    printf("  EVM address: ");
    test_print_address(&evm_info);

    /*--- 3. Configure SOL source (mainnet) ---*/
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

    /*--- 4. Configure EVM destination (Polygon mainnet) ---*/
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

    /*--- 5. Init EVM RPC (destination) ---*/
    BoatEvmRpc evm_rpc;
    BoatResult r = boat_evm_rpc_init(&evm_rpc, polygon_rpc);
    TEST_ASSERT("evm_rpc_init", r == BOAT_SUCCESS, "EVM RPC init failed");
    if (r != BOAT_SUCCESS) goto cleanup;

    /*--- 5b. Init SOL RPC (source) for balance check + deposit ---*/
    BoatSolRpc sol_rpc;
    r = boat_sol_rpc_init(&sol_rpc, sol_rpc_url);
    TEST_ASSERT("sol_rpc_init", r == BOAT_SUCCESS, "SOL RPC init failed");
    if (r != BOAT_SUCCESS) goto cleanup;

    /*--- 5c. Check Solana Gateway API balance ---*/
    {
        uint64_t gw_balance = 0;
        r = boat_gateway_sol_api_balance(&sol_config, sol_info.address, &gw_balance);
        if (r == BOAT_SUCCESS) {
            TEST_PASS("sol_gateway_balance");
            printf("  SOL Gateway balance: %llu raw (%.6f USDC)\n",
                   (unsigned long long)gw_balance,
                   (double)gw_balance / 1000000.0);

            uint64_t needed = TRANSFER_USDC_RAW + MAX_FEE_RAW;
            if (gw_balance < needed) {
                uint64_t deposit_amt = needed - gw_balance;
                printf("  Balance insufficient (need %llu), depositing %.6f USDC...\n",
                       (unsigned long long)needed,
                       (double)deposit_amt / 1000000.0);
                uint8_t dep_sig[64];
                r = boat_gateway_sol_deposit(&sol_config, sol_key, deposit_amt, &sol_rpc, dep_sig);
                if (r == BOAT_SUCCESS) {
                    TEST_PASS("sol_gateway_deposit");
                    printf("  Deposit sig: ");
                    for (int i = 0; i < 16; i++) printf("%02x", dep_sig[i]);
                    printf("...\n");
                    printf("  Waiting 20s for confirmation...\n");
                    boat_sleep_ms(20000);
                } else {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "deposit failed (error %d)", (int)r);
                    TEST_FAIL("sol_gateway_deposit", msg);
                }
            } else {
                printf("  Balance sufficient, skipping deposit.\n");
            }
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "balance query failed (error %d)", (int)r);
            TEST_FAIL("sol_gateway_balance", msg);
            printf("  Proceeding with transfer anyway...\n");
        }
    }

    /*--- 6. SOL -> EVM transfer ---*/
    printf("\n--- SOL-to-EVM transfer: 0.01 USDC (Solana mainnet -> Polygon mainnet) ---\n");
    printf("  Burning on Solana, minting on Polygon...\n");

    BoatGatewayTransferResult result;
    memset(&result, 0, sizeof(result));
    r = boat_gateway_transfer_sol_to_evm(&sol_config, &evm_config,
                                          sol_key, evm_key,
                                          TRANSFER_USDC_RAW, MAX_FEE_RAW,
                                          &evm_rpc, &result);
    if (r == BOAT_SUCCESS && !test_is_zero(result.mint_txhash, 32)) {
        TEST_PASS("sol_to_evm_transfer");
        printf("  EVM mint txhash: ");
        test_print_txhash(result.mint_txhash);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "transfer failed (error %d)", (int)r);
        TEST_FAIL("sol_to_evm_transfer", msg);
    }

    boat_evm_rpc_free(&evm_rpc);
    boat_sol_rpc_free(&sol_rpc);
cleanup:
    boat_key_free(sol_key);
    boat_key_free(evm_key);
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

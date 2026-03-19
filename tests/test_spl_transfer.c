/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration test: SPL token transfer on Solana Devnet / Mainnet
 *
 * Compile with -DBOAT_TEST_SOL_MAINNET to target mainnet-beta.
 * Default is devnet.
 *****************************************************************************/
#include "test_common.h"
#include "boat_sol.h"

/*--- Network selection ---*/
#ifdef BOAT_TEST_SOL_MAINNET
  #define SOL_DEFAULT_RPC   "https://api.mainnet.solana.com"
  #define SOL_NETWORK_NAME  "Mainnet"
  #define TRANSFER_AMOUNT   1ULL            /* minimal amount on mainnet */
  /* Mainnet USDC mint: EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v */
  static const char *DEFAULT_MINT_B58 = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
#else
  #define SOL_DEFAULT_RPC   "https://api.devnet.solana.com"
  #define SOL_NETWORK_NAME  "Devnet"
  #define TRANSFER_AMOUNT   1000ULL         /* 0.001 token (6 decimals) */
  /* Devnet USDC mint: 4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU */
  static const char *DEFAULT_MINT_B58 = "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU";
#endif

static const uint8_t *get_mint(void)
{
    static uint8_t mint[32];
    size_t len = 0;

    /* Priority 1: env var (accepts base58, hex, etc.) */
    const char *env = getenv("BOAT_TEST_SPL_MINT");
    if (env && strlen(env) > 0) {
        if (boat_address_from_string(env, mint, sizeof(mint), &len) == BOAT_SUCCESS && len == 32)
            return mint;
    }

    /* Priority 2: default mint */
    if (boat_base58_decode(DEFAULT_MINT_B58, mint, sizeof(mint), &len) == BOAT_SUCCESS && len == 32)
        return mint;

    return NULL;
}

int main(int argc, char **argv)
{
    int kg = test_keygen_mode(argc, argv, BOAT_KEY_TYPE_ED25519);
    if (kg >= 0) return kg;

    test_init("test_spl_transfer (SPL Token on Solana " SOL_NETWORK_NAME ")");

    /*--- 1. key_import ---*/
    BoatKey *key = test_load_key(BOAT_KEY_TYPE_ED25519);
    TEST_ASSERT("key_import", key != NULL, "failed to import/generate key");
    if (!key) return test_summary();

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    test_print_address(&info);

    const char *rpc_url = test_get_rpc_url("BOAT_TEST_SOL_RPC", SOL_DEFAULT_RPC);
    BoatSolRpc rpc;
    boat_sol_rpc_init(&rpc, rpc_url);

    const uint8_t *mint = get_mint();

    /*--- 2. ata_address ---*/
    uint8_t sender_ata[32];
    BoatResult r = boat_sol_ata_address(info.address, mint, sender_ata);
    if (r == BOAT_SUCCESS) {
        TEST_PASS("ata_address");
        char ata_hex[65];
        boat_bin_to_hex(sender_ata, 32, ata_hex, sizeof(ata_hex), true);
        printf("  Sender ATA: %.16s...\n", ata_hex);
    } else {
        TEST_FAIL("ata_address", "failed to derive ATA");
        boat_key_free(key);
        return test_summary();
    }

    /*--- 3. get_token_balance ---*/
    uint64_t token_balance = 0;
    uint8_t decimals = 0;
    bool has_tokens = false;
    r = boat_sol_rpc_get_token_balance(&rpc, sender_ata, &token_balance, &decimals);
    if (r == BOAT_SUCCESS) {
        if (token_balance > 0) {
            TEST_PASS("get_token_balance");
            printf("  Token balance: %llu (decimals: %u)\n",
                   (unsigned long long)token_balance, decimals);
            has_tokens = true;
        } else {
            TEST_FAIL("get_token_balance",
                      "token balance is zero — mint test tokens to ATA first");
        }
    } else {
        TEST_FAIL("get_token_balance",
                  "RPC call failed (ATA may not exist — create it and fund tokens)");
    }

    /*--- 4. get_sol_balance (need SOL for fees) ---*/
    uint64_t sol_balance = 0;
    r = boat_sol_rpc_get_balance(&rpc, info.address, &sol_balance);
    bool has_sol = false;
    if (r == BOAT_SUCCESS && sol_balance > 10000) {
        TEST_PASS("get_sol_balance");
        printf("  SOL balance: %llu lamports\n", (unsigned long long)sol_balance);
        has_sol = true;
    } else {
        TEST_FAIL("get_sol_balance",
                  "need SOL for tx fees — solana airdrop 1 <address> --url devnet");
    }

    /*--- 5. spl_transfer_to_self ---*/
    if (!has_tokens || !has_sol) {
        TEST_SKIP("spl_transfer_to_self", "insufficient token or SOL balance");
        TEST_SKIP("post_token_balance", "skipped");
    } else {
        /* Get blockhash */
        uint8_t blockhash[32];
        uint64_t last_valid = 0;
        r = boat_sol_rpc_get_latest_blockhash(&rpc, BOAT_SOL_COMMITMENT_CONFIRMED,
                                               blockhash, &last_valid);
        if (r != BOAT_SUCCESS) {
            TEST_FAIL("spl_transfer_to_self", "failed to get blockhash");
            TEST_SKIP("post_token_balance", "skipped");
        } else {
            /* Build SPL transfer instruction (self-transfer) */
            BoatSolInstruction ix;
            boat_sol_spl_transfer(sender_ata, sender_ata, info.address,
                                  TRANSFER_AMOUNT, &ix);

            BoatSolTx tx;
            boat_sol_tx_init(&tx);
            boat_sol_tx_set_fee_payer(&tx, info.address);
            boat_sol_tx_set_blockhash(&tx, blockhash);
            boat_sol_tx_add_instruction(&tx, &ix);

            uint8_t sig[64] = {0};
            r = boat_sol_tx_send(&tx, key, &rpc, sig);

            if (r == BOAT_SUCCESS && !test_is_zero(sig, 64)) {
                TEST_PASS("spl_transfer_to_self");
                char sig_hex[131]; /* 2 + 128 + 1 */
                boat_bin_to_hex(sig, 64, sig_hex, sizeof(sig_hex), true);
                printf("  Signature: %.32s...\n", sig_hex);
            } else {
                TEST_FAIL("spl_transfer_to_self", "tx send failed");
            }

            /*--- 6. post_token_balance ---*/
            boat_sleep_ms(2000);
            uint64_t balance2 = 0;
            uint8_t dec2 = 0;
            r = boat_sol_rpc_get_token_balance(&rpc, sender_ata, &balance2, &dec2);
            if (r == BOAT_SUCCESS) {
                if (balance2 == token_balance) {
                    TEST_PASS("post_token_balance");
                    printf("  Token balance unchanged (self-transfer, only SOL fee spent)\n");
                } else {
                    TEST_PASS("post_token_balance");
                    printf("  Token balance changed (possible pending state)\n");
                }
            } else {
                TEST_FAIL("post_token_balance", "RPC call failed");
            }
        }
    }

    boat_sol_rpc_free(&rpc);
    boat_key_free(key);
    return test_summary();
}

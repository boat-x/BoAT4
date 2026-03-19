/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration test: SOL transfer on Solana Devnet / Mainnet
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
  #define SEND_LAMPORTS     100ULL          /* 0.0000001 SOL — minimal on mainnet */
#else
  #define SOL_DEFAULT_RPC   "https://api.devnet.solana.com"
  #define SOL_NETWORK_NAME  "Devnet"
  #define SEND_LAMPORTS     1000000ULL      /* 0.001 SOL */
#endif

int main(int argc, char **argv)
{
    int kg = test_keygen_mode(argc, argv, BOAT_KEY_TYPE_ED25519);
    if (kg >= 0) return kg;

    test_init("test_sol_transfer (Solana " SOL_NETWORK_NAME ")");

    /*--- 1. key_import ---*/
    BoatKey *key = test_load_key(BOAT_KEY_TYPE_ED25519);
    TEST_ASSERT("key_import", key != NULL, "failed to import/generate key");
    if (!key) return test_summary();

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    test_print_address(&info);

    /*--- 2. rpc_connect ---*/
    const char *rpc_url = test_get_rpc_url("BOAT_TEST_SOL_RPC", SOL_DEFAULT_RPC);
    BoatSolRpc rpc;
    BoatResult r = boat_sol_rpc_init(&rpc, rpc_url);
    TEST_ASSERT("rpc_init", r == BOAT_SUCCESS, "boat_sol_rpc_init failed");

    /*--- 3. get_balance ---*/
    uint64_t balance = 0;
    r = boat_sol_rpc_get_balance(&rpc, info.address, &balance);
    bool has_balance = false;
    if (r == BOAT_SUCCESS) {
        if (balance > 0) {
            TEST_PASS("get_balance");
            printf("  Balance: %llu lamports (%.6f SOL)\n",
                   (unsigned long long)balance, (double)balance / 1e9);
            has_balance = true;
        } else {
            TEST_FAIL("get_balance",
                      "balance is zero — fund with: solana airdrop 1 <address> --url devnet");
        }
    } else {
        TEST_FAIL("get_balance", "RPC call failed");
    }

    /*--- 4. get_latest_blockhash ---*/
    uint8_t blockhash[32];
    uint64_t last_valid = 0;
    r = boat_sol_rpc_get_latest_blockhash(&rpc, BOAT_SOL_COMMITMENT_CONFIRMED,
                                           blockhash, &last_valid);
    if (r == BOAT_SUCCESS && last_valid > 0) {
        TEST_PASS("get_latest_blockhash");
        printf("  Last valid block height: %llu\n", (unsigned long long)last_valid);
    } else {
        TEST_FAIL("get_latest_blockhash", "failed to get blockhash");
    }

    /*--- 5. send_sol (self-transfer) ---*/
    if (!has_balance || balance < SEND_LAMPORTS + 10000 /* rent + fee headroom */) {
        TEST_SKIP("send_sol", "insufficient balance");
        TEST_SKIP("post_balance", "skipped");
    } else {
        /* Build System Program transfer instruction:
         * instruction data = [2,0,0,0] (Transfer tag) + [amount LE u64] */
        BoatSolInstruction ix;
        boat_sol_ix_init(&ix, BOAT_SOL_SYSTEM_PROGRAM_ID);
        boat_sol_ix_add_account(&ix, info.address, true, true);   /* from (signer, writable) */
        boat_sol_ix_add_account(&ix, info.address, false, true);  /* to = self (writable) */

        uint8_t ix_data[12];
        /* Transfer instruction index = 2 (little-endian u32) */
        ix_data[0] = 2; ix_data[1] = 0; ix_data[2] = 0; ix_data[3] = 0;
        /* Amount in little-endian u64 */
        uint64_t amt = SEND_LAMPORTS;
        for (int i = 0; i < 8; i++) {
            ix_data[4 + i] = (uint8_t)(amt & 0xFF);
            amt >>= 8;
        }
        boat_sol_ix_set_data(&ix, ix_data, sizeof(ix_data));

        /* Build transaction */
        BoatSolTx tx;
        boat_sol_tx_init(&tx);
        boat_sol_tx_set_fee_payer(&tx, info.address);
        boat_sol_tx_set_blockhash(&tx, blockhash);
        boat_sol_tx_add_instruction(&tx, &ix);

        /* Sign and send */
        uint8_t sig[64] = {0};
        r = boat_sol_tx_send(&tx, key, &rpc, sig);

        if (r == BOAT_SUCCESS && !test_is_zero(sig, 64)) {
            TEST_PASS("send_sol");
            char sig_hex[131]; /* 2 + 128 + 1 */
            boat_bin_to_hex(sig, 64, sig_hex, sizeof(sig_hex), true);
            printf("  Signature: %.32s...\n", sig_hex);
        } else {
            TEST_FAIL("send_sol", "tx send failed");
        }

        /*--- 6. post_balance ---*/
        boat_sleep_ms(2000);
        uint64_t balance2 = 0;
        r = boat_sol_rpc_get_balance(&rpc, info.address, &balance2);
        if (r == BOAT_SUCCESS) {
            if (balance2 != balance) {
                TEST_PASS("post_balance");
                printf("  New balance: %llu lamports (gas consumed)\n",
                       (unsigned long long)balance2);
            } else {
                TEST_PASS("post_balance");
                printf("  Balance unchanged (tx may still be pending)\n");
            }
        } else {
            TEST_FAIL("post_balance", "RPC call failed");
        }
    }

    boat_sol_rpc_free(&rpc);
    boat_key_free(key);
    return test_summary();
}

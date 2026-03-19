/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Testnet integration test: Circle Nanopayments on Arc Testnet
 *
 * Uses the same address as both buyer and seller (loopback) so the test
 * can run with a single funded wallet.  Flow:
 *   1. Import key, connect to Arc Testnet RPC
 *   2. Check on-chain Gateway balance (before)
 *   3. Deposit USDC into Gateway Wallet (on-chain)
 *   4. Check on-chain Gateway balance (after deposit)
 *   5. Authorize a nanopayment to self (off-chain EIP-3009, loopback)
 *   6. Verify authorization fields
 *   7. Check balance again
 *****************************************************************************/
#include "test_common.h"
#include "boat_evm.h"
#include "boat_pay.h"
#include <time.h>

/* Arc Testnet */
#define ARC_TESTNET_CHAIN_ID    5042002
#define ARC_TESTNET_RPC         "https://rpc.testnet.arc.network"

/* Arc Testnet USDC (native gas token address) */
static const uint8_t ARC_USDC[20] = {
    0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* Gateway Wallet (same on all testnet chains) */
static const uint8_t GATEWAY_WALLET[20] = {
    0x00,0x77,0x77,0x7d,0x7E,0xBA,0x46,0x88,0xBD,0xeF,
    0x3E,0x31,0x1b,0x84,0x6F,0x25,0x87,0x0A,0x19,0xB9
};

/* 1 USDC = 1000000 */
static const uint8_t ONE_USDC[32] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0x0F,0x42,0x40
};

/* 0.001 USDC = 1000 = 0x3E8 */
static const uint8_t NANO_AMOUNT[32] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0x03,0xE8
};

static uint64_t balance_to_u64(const uint8_t bal[32])
{
    uint64_t v = 0;
    for (int i = 24; i < 32; i++) v = (v << 8) | bal[i];
    return v;
}

int main(int argc, char **argv)
{
    int kg = test_keygen_mode(argc, argv, BOAT_KEY_TYPE_SECP256K1);
    if (kg >= 0) return kg;

    test_init("test_nano (Circle Nanopayments on Arc Testnet)");

    /*--- 1. key_import ---*/
    BoatKey *key = test_load_key(BOAT_KEY_TYPE_SECP256K1);
    TEST_ASSERT("key_import", key != NULL, "failed to import/generate key");
    if (!key) return test_summary();

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    test_print_address(&info);

    /*--- 2. rpc_connect ---*/
    const char *rpc_url = test_get_rpc_url("BOAT_TEST_ARC_RPC", ARC_TESTNET_RPC);
    BoatEvmRpc rpc;
    BoatResult r = boat_evm_rpc_init(&rpc, rpc_url);
    TEST_ASSERT("rpc_init", r == BOAT_SUCCESS, "boat_evm_rpc_init failed");

    uint64_t block = 0;
    r = boat_evm_block_number(&rpc, &block);
    if (r == BOAT_SUCCESS && block > 0) {
        TEST_PASS("rpc_connect");
        printf("  Arc Testnet block: %llu\n", (unsigned long long)block);
    } else {
        TEST_FAIL("rpc_connect", "Arc Testnet RPC may be down");
        boat_key_free(key);
        return test_summary();
    }

    /* Nano config — uses same Gateway Wallet as Circle Gateway */
    BoatNanoConfig nano_config;
    memset(&nano_config, 0, sizeof(nano_config));
    memcpy(nano_config.gateway_wallet_addr, GATEWAY_WALLET, 20);
    memcpy(nano_config.usdc_addr, ARC_USDC, 20);
    nano_config.chain.chain_id = ARC_TESTNET_CHAIN_ID;
    strncpy(nano_config.chain.rpc_url, rpc_url, sizeof(nano_config.chain.rpc_url) - 1);
    nano_config.chain.eip1559 = false;

    /*--- 3. check_balance_before ---*/
    uint8_t bal_before[32] = {0};
    r = boat_nano_get_balance(&nano_config, info.address, &rpc, bal_before);
    uint64_t bal_before_u64 = 0;
    if (r == BOAT_SUCCESS) {
        bal_before_u64 = balance_to_u64(bal_before);
        TEST_PASS("check_balance_before");
        printf("  Gateway balance before: %llu USDC units\n", (unsigned long long)bal_before_u64);
    } else {
        TEST_FAIL("check_balance_before", "on-chain balance query failed");
    }

    /*--- 4. deposit ---*/
    uint8_t onchain_bal[32] = {0};
    r = boat_evm_get_balance(&rpc, info.address, onchain_bal);
    bool has_funds = (r == BOAT_SUCCESS && !test_is_zero(onchain_bal, 32));

    if (!has_funds) {
        TEST_SKIP("deposit", "no funds on Arc Testnet — fund from https://faucet.circle.com/");
    } else {
        /* Deposit 0.001 USDC if balance > 1 USDC, else 1 USDC */
        const uint8_t *deposit_amount = (bal_before_u64 > 1000000) ? NANO_AMOUNT : ONE_USDC;
        const char *amt_str = (bal_before_u64 > 1000000) ? "0.001" : "1";
        printf("  Depositing %s USDC (gateway balance %s 1 USDC)\n", amt_str,
               bal_before_u64 > 1000000 ? ">" : "<=");

        uint8_t dep_txhash[32] = {0};
        r = boat_nano_deposit(&nano_config, key, deposit_amount, &rpc, dep_txhash);
        if (r == BOAT_SUCCESS && !test_is_zero(dep_txhash, 32)) {
            TEST_PASS("deposit");
            test_print_txhash(dep_txhash);
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "deposit failed (error %d)", (int)r);
            TEST_FAIL("deposit", msg);
        }

        /*--- 5. check_balance_after_deposit ---*/
        boat_sleep_ms(3000);
        uint8_t bal_after[32] = {0};
        r = boat_nano_get_balance(&nano_config, info.address, &rpc, bal_after);
        if (r == BOAT_SUCCESS) {
            uint64_t after = balance_to_u64(bal_after);
            printf("  Gateway balance after deposit: %llu USDC units\n", (unsigned long long)after);
            TEST_PASS("check_balance_after_deposit");
        } else {
            TEST_FAIL("check_balance_after_deposit", "balance query failed");
        }
    }

    /*--- 6. authorize (loopback: pay self) ---*/
    printf("  Authorizing 0.001 USDC nanopayment to self (loopback)...\n");

    uint8_t nonce[32];
    boat_random(nonce, 32);

    BoatEip3009Auth auth;
    uint8_t sig[65];
    r = boat_nano_authorize(&nano_config, key, info.address, NANO_AMOUNT, nonce, &auth, sig);
    if (r == BOAT_SUCCESS) {
        TEST_PASS("authorize");
        printf("  Signature v=%d, payload signed off-chain (zero gas)\n", sig[64]);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "authorize failed (error %d)", (int)r);
        TEST_FAIL("authorize", msg);
    }

    /*--- 7. verify_auth_fields ---*/
    if (r == BOAT_SUCCESS) {
        bool from_ok = (memcmp(auth.from, info.address, 20) == 0);
        bool to_ok   = (memcmp(auth.to, info.address, 20) == 0);  /* loopback: to == from */
        bool value_ok = (memcmp(auth.value, NANO_AMOUNT, 32) == 0);
        uint64_t now = (uint64_t)time(NULL);
        bool time_ok = (auth.valid_after <= now && auth.valid_before > now);
        bool nonce_ok = (memcmp(auth.nonce, nonce, 32) == 0);

        if (from_ok && to_ok && value_ok && time_ok && nonce_ok) {
            TEST_PASS("verify_auth_fields");
            printf("  from=self, to=self (loopback), value=0.001 USDC, time window OK\n");
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "from=%d to=%d value=%d time=%d nonce=%d",
                     from_ok, to_ok, value_ok, time_ok, nonce_ok);
            TEST_FAIL("verify_auth_fields", msg);
        }
    } else {
        TEST_SKIP("verify_auth_fields", "authorize failed");
    }

    /*--- 8. nano_pay (full x402 flow: GET → 402 → sign → PAYMENT-SIGNATURE → resource) ---*/
    {
        const char *seller_url = getenv("BOAT_TEST_NANO_SELLER_URL");
        if (seller_url && strlen(seller_url) > 0) {
            printf("  x402 nanopayment to: %s\n", seller_url);
            uint8_t *pay_resp = NULL;
            size_t pay_resp_len = 0;
            BoatResult pr = boat_nano_pay(seller_url, NULL, &nano_config, key,
                                          &pay_resp, &pay_resp_len);
            if (pr == BOAT_SUCCESS && pay_resp) {
                TEST_PASS("nano_pay");
                printf("  Resource (%zu bytes): %.*s\n",
                       pay_resp_len, (int)(pay_resp_len < 256 ? pay_resp_len : 256),
                       (char *)pay_resp);
                boat_free(pay_resp);
            } else {
                char msg[128];
                snprintf(msg, sizeof(msg), "nano_pay failed (error %d)", (int)pr);
                TEST_FAIL("nano_pay", msg);
            }
        } else {
            TEST_SKIP("nano_pay",
                      "set BOAT_TEST_NANO_SELLER_URL (e.g. http://localhost:3000/premium-data)");
        }
    }

    /*--- 9. check_balance_final ---*/
    uint8_t bal_final[32] = {0};
    r = boat_nano_get_balance(&nano_config, info.address, &rpc, bal_final);
    if (r == BOAT_SUCCESS) {
        uint64_t final_bal = balance_to_u64(bal_final);
        TEST_PASS("check_balance_final");
        printf("  Gateway balance: %llu USDC units\n", (unsigned long long)final_bal);
    } else {
        TEST_FAIL("check_balance_final", "balance query failed");
    }

    boat_evm_rpc_free(&rpc);
    boat_key_free(key);
    return test_summary();
}

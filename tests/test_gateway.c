/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Testnet integration test: Circle Gateway on Arc Testnet
 *   - Deposit USDC into Gateway Wallet
 *   - Cross-chain transfer: burn on Arc → attest via Gateway API → mint on Base Sepolia
 *   - Withdrawal (opt-in, disabled by default — requires ~7 day delay)
 *****************************************************************************/
#include "test_common.h"
#include "boat_evm.h"
#include "boat_pay.h"

/* Arc Testnet: USDC is native gas token */
#define ARC_TESTNET_CHAIN_ID    5042002
#define ARC_TESTNET_RPC         "https://rpc.testnet.arc.network"
#define ARC_DOMAIN              26

/* Base Sepolia */
#define BASE_SEPOLIA_CHAIN_ID   84532
#define BASE_SEPOLIA_RPC        "https://sepolia.base.org"
#define BASE_SEPOLIA_DOMAIN     6

/* Arc Testnet USDC */
static const uint8_t ARC_USDC[20] = {
    0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
/* Base Sepolia USDC */
static const uint8_t BASE_USDC[20] = {
    0x03,0x6C,0xbD,0x53,0x84,0x2c,0x54,0x26,0x63,0x4e,
    0x79,0x29,0x54,0x1e,0xC2,0x31,0x8f,0x3d,0xCF,0x7e
};
/* Gateway Wallet (same on all testnet chains) */
static const uint8_t GATEWAY_WALLET[20] = {
    0x00,0x77,0x77,0x7d,0x7E,0xBA,0x46,0x88,0xBD,0xeF,
    0x3E,0x31,0x1b,0x84,0x6F,0x25,0x87,0x0A,0x19,0xB9
};
/* Gateway Minter (same on all testnet chains) */
static const uint8_t GATEWAY_MINTER[20] = {
    0x00,0x22,0x22,0x2A,0xBE,0x23,0x8C,0xc2,0xC7,0xBb,
    0x1f,0x21,0x00,0x3F,0x0a,0x26,0x00,0x52,0x47,0x5B
};

/* 1 USDC = 1000000 */
static const uint8_t ONE_USDC[32] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0x0F,0x42,0x40
};
/* 0.01 USDC = 10000 */
static const uint8_t SMALL_USDC[32] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0x27,0x10
};
/* maxFee: 2.01 USDC = 2010000 = 0x1EAB90 */
static const uint8_t MAX_FEE[32] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0x1E,0xAB,0x90
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

    test_init("test_gateway (Circle Gateway on Arc Testnet)");

    /*--- 1. key_import ---*/
    BoatKey *key = test_load_key(BOAT_KEY_TYPE_SECP256K1);
    TEST_ASSERT("key_import", key != NULL, "failed to import/generate key");
    if (!key) return test_summary();

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    test_print_address(&info);

    /*--- 2. rpc_connect (Arc Testnet) ---*/
    const char *arc_rpc_url = test_get_rpc_url("BOAT_TEST_ARC_RPC", ARC_TESTNET_RPC);
    BoatEvmRpc arc_rpc;
    BoatResult r = boat_evm_rpc_init(&arc_rpc, arc_rpc_url);
    TEST_ASSERT("rpc_init", r == BOAT_SUCCESS, "boat_evm_rpc_init failed");

    uint64_t block = 0;
    r = boat_evm_block_number(&arc_rpc, &block);
    if (r == BOAT_SUCCESS && block > 0) {
        TEST_PASS("rpc_connect");
        printf("  Arc Testnet block: %llu\n", (unsigned long long)block);
    } else {
        TEST_FAIL("rpc_connect", "Arc Testnet RPC may be down");
        boat_key_free(key);
        return test_summary();
    }

    /* Gateway configs */
    BoatGatewayConfig arc_config;
    memcpy(arc_config.gateway_wallet_addr, GATEWAY_WALLET, 20);
    memcpy(arc_config.gateway_minter_addr, GATEWAY_MINTER, 20);
    memcpy(arc_config.usdc_addr, ARC_USDC, 20);
    arc_config.domain = ARC_DOMAIN;
    arc_config.chain.chain_id = ARC_TESTNET_CHAIN_ID;
    strncpy(arc_config.chain.rpc_url, arc_rpc_url, sizeof(arc_config.chain.rpc_url) - 1);
    arc_config.chain.eip1559 = false;

    BoatGatewayConfig base_config;
    memcpy(base_config.gateway_wallet_addr, GATEWAY_WALLET, 20);
    memcpy(base_config.gateway_minter_addr, GATEWAY_MINTER, 20);
    memcpy(base_config.usdc_addr, BASE_USDC, 20);
    base_config.domain = BASE_SEPOLIA_DOMAIN;
    base_config.chain.chain_id = BASE_SEPOLIA_CHAIN_ID;
    const char *base_rpc_url = test_get_rpc_url("BOAT_TEST_RPC", BASE_SEPOLIA_RPC);
    strncpy(base_config.chain.rpc_url, base_rpc_url, sizeof(base_config.chain.rpc_url) - 1);
    base_config.chain.eip1559 = false;

    /*--- 3. check_balance_before ---*/
    uint8_t bal_before[32] = {0};
    r = boat_gateway_balance(&arc_config, info.address, &arc_rpc, bal_before);
    uint64_t bal_before_u64 = 0;
    if (r == BOAT_SUCCESS) {
        bal_before_u64 = balance_to_u64(bal_before);
        TEST_PASS("check_balance_before");
        printf("  Gateway balance before: %llu USDC units\n", (unsigned long long)bal_before_u64);
    } else {
        TEST_FAIL("check_balance_before", "gateway balance query failed");
    }

    /*--- 4. deposit (adaptive amount) ---*/
    uint8_t onchain_bal[32] = {0};
    r = boat_evm_get_balance(&arc_rpc, info.address, onchain_bal);
    bool has_funds = (r == BOAT_SUCCESS && !test_is_zero(onchain_bal, 32));

    if (!has_funds) {
        TEST_SKIP("deposit", "no funds on Arc Testnet — fund from https://faucet.circle.com/");
    } else {
        /* Deposit 0.01 USDC if balance > 1 USDC, else 1 USDC */
        const uint8_t *deposit_amount = (bal_before_u64 > 1000000) ? SMALL_USDC : ONE_USDC;
        const char *amt_str = (bal_before_u64 > 1000000) ? "0.01" : "1";
        printf("  Depositing %s USDC (balance %s 1 USDC)\n", amt_str,
               bal_before_u64 > 1000000 ? ">" : "<=");

        uint8_t dep_txhash[32] = {0};
        r = boat_gateway_deposit(&arc_config, key, deposit_amount, &arc_rpc, dep_txhash);
        if (r == BOAT_SUCCESS && !test_is_zero(dep_txhash, 32)) {
            TEST_PASS("deposit");
            test_print_txhash(dep_txhash);
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "deposit failed (error %d)", (int)r);
            TEST_FAIL("deposit", msg);
        }

        /*--- 5. check_balance_after ---*/
        boat_sleep_ms(3000);
        uint8_t bal_after[32] = {0};
        r = boat_gateway_balance(&arc_config, info.address, &arc_rpc, bal_after);
        if (r == BOAT_SUCCESS) {
            uint64_t after = balance_to_u64(bal_after);
            printf("  Gateway balance after deposit: %llu USDC units\n", (unsigned long long)after);
            TEST_PASS("check_balance_after");
        } else {
            TEST_FAIL("check_balance_after", "balance query failed");
        }
    }

    /*=======================================================================
     * 6. Cross-chain transfer: Arc Testnet -> Base Sepolia (0.01 USDC)
     *    Single SDK call: sign BurnIntent, POST Gateway API, mint on dst
     *=====================================================================*/
    uint8_t xfer_bal[32] = {0};
    r = boat_gateway_balance(&arc_config, info.address, &arc_rpc, xfer_bal);
    uint64_t xfer_bal_u64 = (r == BOAT_SUCCESS) ? balance_to_u64(xfer_bal) : 0;

    if (xfer_bal_u64 < 10000) {
        TEST_SKIP("cross_chain_transfer", "insufficient gateway balance for cross-chain transfer");
    } else {
        printf("  Cross-chain: Arc (domain %d) -> Base Sepolia (domain %d), 0.01 USDC\n",
               ARC_DOMAIN, BASE_SEPOLIA_DOMAIN);

        BoatEvmRpc base_rpc;
        boat_evm_rpc_init(&base_rpc, base_rpc_url);

        BoatGatewayTransferResult xfer_result;
        r = boat_gateway_transfer(&arc_config, &base_config, key,
                                  SMALL_USDC, MAX_FEE, &base_rpc, &xfer_result);
        if (r == BOAT_SUCCESS && !test_is_zero(xfer_result.mint_txhash, 32)) {
            TEST_PASS("cross_chain_transfer");
            test_print_txhash(xfer_result.mint_txhash);
            printf("  Minted 0.01 USDC on Base Sepolia via gatewayMint\n");
        } else {
            char emsg[128];
            snprintf(emsg, sizeof(emsg), "cross-chain transfer failed (error %d)", (int)r);
            TEST_FAIL("cross_chain_transfer", emsg);
        }

        boat_evm_rpc_free(&base_rpc);
    }

    /*=======================================================================
     * 7. Withdrawal (opt-in, disabled by default)
     *    Set BOAT_TEST_GATEWAY_WITHDRAW=1 to enable.
     *    Note: initiateWithdrawal locks funds for ~7 days.
     *=====================================================================*/
    const char *do_withdraw = getenv("BOAT_TEST_GATEWAY_WITHDRAW");
    if (do_withdraw && strcmp(do_withdraw, "1") == 0) {
        uint8_t wd_txhash[32] = {0};
        r = boat_gateway_trustless_withdraw(&arc_config, key, SMALL_USDC, &arc_rpc, wd_txhash);
        if (r == BOAT_SUCCESS && !test_is_zero(wd_txhash, 32)) {
            TEST_PASS("initiate_withdrawal");
            test_print_txhash(wd_txhash);
            printf("  Trustless withdrawal initiated. Completes after ~7 day delay.\n");
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "initiateWithdrawal failed (error %d)", (int)r);
            TEST_FAIL("initiate_withdrawal", msg);
        }
    } else {
        TEST_SKIP("initiate_withdrawal", "disabled by default (set BOAT_TEST_GATEWAY_WITHDRAW=1)");
    }

cleanup:
    boat_evm_rpc_free(&arc_rpc);
    boat_key_free(key);
    return test_summary();
}

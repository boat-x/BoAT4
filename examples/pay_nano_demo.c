/******************************************************************************
 * BoAT v4 Example: Circle Nanopayments on Arc Testnet
 *
 * Demonstrates the full nanopayment flow:
 *   1. Deposit USDC into Gateway Wallet (on-chain, one-time)
 *   2. Authorize a gas-free nanopayment (off-chain EIP-3009 signature)
 *   3. Check Gateway balance
 *
 * Uses the same address as buyer and seller (loopback) for simplicity.
 * All addresses are human-readable "0x..." strings and USDC amounts are
 * plain doubles — helpers convert them to the raw byte arrays the SDK expects.
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"
#include "boat_key.h"
#include "boat_evm.h"
#include "boat_pay.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/*===========================================================================
 * Configuration — edit these for your environment
 *=========================================================================*/
static const char *PRIVATE_KEY_HEX =
    "your_private_key_hex_here";                       /* 64 hex chars, no 0x prefix */

static const char *ARC_RPC_URL = "https://rpc.testnet.arc.network";

/* Gateway Wallet contract (same on all testnet chains) */
static const char *GATEWAY_WALLET_HEX = "0x0077777d7EBA4688BDeF3E311b846F25870A19B9";

/* Arc Testnet USDC */
static const char *ARC_USDC_HEX = "0x3600000000000000000000000000000000000000";

#define ARC_CHAIN_ID  5042002

/*===========================================================================
 * Helpers: human-readable → raw bytes
 *=========================================================================*/

/* Convert a USDC amount (e.g. 0.001) to a uint256 big-endian byte array.
 * USDC has 6 decimals, so 0.001 USDC = 1000 raw units. */
static void usdc_to_uint256(double usdc, uint8_t out[32])
{
    memset(out, 0, 32);
    uint64_t raw = (uint64_t)round(usdc * 1e6);
    for (int i = 31; i >= 24 && raw > 0; i--) {
        out[i] = (uint8_t)(raw & 0xFF);
        raw >>= 8;
    }
}

/* Convert a uint256 big-endian balance back to a double for display. */
static double uint256_to_usdc(const uint8_t bal[32])
{
    uint64_t v = 0;
    for (int i = 24; i < 32; i++)
        v = (v << 8) | bal[i];
    return (double)v / 1e6;
}

/* Parse a "0x..." hex address string into a 20-byte array. */
static int parse_address(const char *hex, uint8_t out[20])
{
    size_t len;
    BoatResult r = boat_hex_to_bin(hex, out, 20, &len);
    return (r == BOAT_SUCCESS && len == 20) ? 0 : -1;
}

/*===========================================================================
 * Main
 *=========================================================================*/
int main(void)
{
#if BOAT_PAY_NANO_ENABLED

    BoatResult r;

    /*--- 1. Platform init + key import ---*/
    boat_pal_linux_init();

    uint8_t privkey[32];
    size_t privkey_len;
    boat_hex_to_bin(PRIVATE_KEY_HEX, privkey, 32, &privkey_len);

    BoatKey *key = boat_key_import_raw(BOAT_KEY_TYPE_SECP256K1, privkey, 32);
    if (!key) {
        printf("Key import failed\n");
        return -1;
    }

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    char addr_hex[43];
    boat_bin_to_hex(info.address, 20, addr_hex, sizeof(addr_hex), true);
    printf("Wallet: %s\n\n", addr_hex);

    /*--- 2. Init Arc RPC + verify connectivity ---*/
    BoatEvmRpc rpc;
    boat_evm_rpc_init(&rpc, ARC_RPC_URL);

    uint64_t block = 0;
    r = boat_evm_block_number(&rpc, &block);
    if (r != BOAT_SUCCESS) {
        printf("Arc RPC unreachable\n");
        boat_key_free(key);
        return -1;
    }
    printf("Arc Testnet block: %llu\n", (unsigned long long)block);

    /*--- 3. Build nano config ---*/
    BoatNanoConfig nano_config;
    memset(&nano_config, 0, sizeof(nano_config));
    parse_address(GATEWAY_WALLET_HEX, nano_config.gateway_wallet_addr);
    parse_address(ARC_USDC_HEX, nano_config.usdc_addr);
    nano_config.chain.chain_id = ARC_CHAIN_ID;
    strncpy(nano_config.chain.rpc_url, ARC_RPC_URL, sizeof(nano_config.chain.rpc_url) - 1);

    /*--- 4. Check Gateway balance ---*/
    uint8_t balance[32];
    r = boat_nano_get_balance(&nano_config, info.address, &rpc, balance);
    if (r != BOAT_SUCCESS) {
        printf("Balance query failed: %d\n", (int)r);
        goto cleanup;
    }
    double bal_usdc = uint256_to_usdc(balance);
    printf("Gateway balance: %.6f USDC\n\n", bal_usdc);

    /*--- 5. Deposit 0.01 USDC (skip if balance already sufficient) ---*/
    double deposit_amount = 0.01;
    if (bal_usdc < deposit_amount) {
        printf("Balance %.6f < %.6f — depositing %.6f USDC...\n",
               bal_usdc, deposit_amount, deposit_amount);

        uint8_t dep_amount[32];
        usdc_to_uint256(deposit_amount, dep_amount);

        uint8_t dep_txhash[32] = {0};
        r = boat_nano_deposit(&nano_config, key, dep_amount, &rpc, dep_txhash);
        if (r != BOAT_SUCCESS) {
            printf("Deposit failed: %d\n", (int)r);
            goto cleanup;
        }

        char tx_hex[67];
        boat_bin_to_hex(dep_txhash, 32, tx_hex, sizeof(tx_hex), true);
        printf("Deposit TX: %s\n\n", tx_hex);
    } else {
        printf("Balance sufficient (%.6f >= %.6f) — skipping deposit\n\n",
               bal_usdc, deposit_amount);
    }

    /*--- 6. Authorize a nanopayment to self (loopback, gas-free) ---*/
    printf("Authorizing 0.001 USDC nanopayment to self (loopback)...\n");

    uint8_t pay_amount[32];
    usdc_to_uint256(0.001, pay_amount);

    uint8_t nonce[32];
    boat_random(nonce, 32);

    BoatEip3009Auth auth;
    uint8_t sig[65];
    r = boat_nano_authorize(&nano_config, key, info.address, pay_amount, nonce, &auth, sig);
    if (r == BOAT_SUCCESS) {
        printf("  Authorization signed (gas-free, off-chain)\n");
        printf("  Signature v=%d, payload ready for x402 facilitator\n\n", sig[64]);
    } else {
        printf("  Authorization failed: %d\n", (int)r);
        goto cleanup;
    }

    /*--- 7. Pay for an x402-protected resource via nanopayments ---*/
    printf("Paying for x402-protected resource via nanopayments...\n");
    {
        /* x402 flow: GET → 402 → sign EIP-3009 → retry with PAYMENT-SIGNATURE header.
         * For local testing, run: node tests/nano_seller.js
         * For production, use any Gateway-enabled x402 seller endpoint. */
        const char *seller_url = "http://localhost:3000/premium-data";

        uint8_t *pay_resp = NULL;
        size_t pay_resp_len = 0;
        r = boat_nano_pay(seller_url, NULL, &nano_config, key, &pay_resp, &pay_resp_len);
        if (r == BOAT_SUCCESS && pay_resp) {
            printf("  Payment accepted! Resource received.\n");
            printf("  Response: %.*s\n\n", (int)(pay_resp_len < 512 ? pay_resp_len : 512),
                   (char *)pay_resp);
            boat_free(pay_resp);
        } else {
            printf("  Payment failed (error %d)\n", (int)r);
            printf("  (Expected if no seller server running — start with: node tests/nano_seller.js)\n\n");
        }
    }

    /*--- 8. Check balance again ---*/
    r = boat_nano_get_balance(&nano_config, info.address, &rpc, balance);
    if (r == BOAT_SUCCESS) {
        printf("Gateway balance: %.6f USDC\n", uint256_to_usdc(balance));
        printf("(Note: balance unchanged until facilitator settles the batch on-chain)\n");
    }

    printf("\nDone — nanopayment authorized via Circle Gateway batched settlement\n");

cleanup:
    boat_evm_rpc_free(&rpc);
    boat_key_free(key);
    return (r == BOAT_SUCCESS) ? 0 : -1;

#else
    printf("Nanopayments not enabled. Rebuild with -DBOAT_PAY_NANO=ON\n");
    return -1;
#endif
}

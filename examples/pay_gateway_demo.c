/******************************************************************************
 * BoAT v4 Example: Circle Gateway cross-chain transfer
 *
 * Demonstrates the full Circle Gateway flow on Arc Testnet:
 *   1. Check Gateway Wallet balance
 *   2. Deposit USDC (skipped if balance already sufficient)
 *   3. Cross-chain transfer: Arc Testnet → Base Sepolia
 *   4. Instant withdrawal: same-chain transfer (Gateway → wallet on Arc)
 *
 * All addresses are written as human-readable "0x..." strings and USDC
 * amounts as plain doubles (e.g. 0.01) — the helpers below convert them
 * to the raw byte arrays the SDK expects.
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

static const char *ARC_RPC_URL  = "https://rpc.testnet.arc.network";
static const char *BASE_RPC_URL = "https://sepolia.base.org";

/* Contract addresses (same on all Gateway testnets) */
static const char *GATEWAY_WALLET_HEX  = "0x0077777d7EBA4688BDef3E311b846F25870A19B9";
static const char *GATEWAY_MINTER_HEX  = "0x0022222ABE238Cc2C7Bb1f21003F0a260052475B";

/* USDC token addresses */
static const char *ARC_USDC_HEX  = "0x3600000000000000000000000000000000000000";
static const char *BASE_USDC_HEX = "0x036CbD53842c5426634e7929541eC2318f3dCF7e";

/* Circle Gateway domain IDs */
#define ARC_DOMAIN   26
#define BASE_DOMAIN   6

/* Chain IDs */
#define ARC_CHAIN_ID   5042002
#define BASE_CHAIN_ID  84532

/*===========================================================================
 * Helpers: human-readable → raw bytes
 *=========================================================================*/

/* Convert a USDC amount (e.g. 0.01) to a uint256 big-endian byte array.
 * USDC has 6 decimals, so 0.01 USDC = 10000 raw units. */
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
#if BOAT_PAY_GATEWAY_ENABLED

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
    BoatEvmRpc arc_rpc;
    boat_evm_rpc_init(&arc_rpc, ARC_RPC_URL);

    uint64_t block = 0;
    r = boat_evm_block_number(&arc_rpc, &block);
    if (r != BOAT_SUCCESS) {
        printf("Arc RPC unreachable\n");
        boat_key_free(key);
        return -1;
    }
    printf("Arc Testnet block: %llu\n", (unsigned long long)block);

    /*--- 3. Build Gateway configs for Arc and Base ---*/
    BoatGatewayConfig arc_config;
    memset(&arc_config, 0, sizeof(arc_config));
    parse_address(GATEWAY_WALLET_HEX, arc_config.gateway_wallet_addr);
    parse_address(GATEWAY_MINTER_HEX, arc_config.gateway_minter_addr);
    parse_address(ARC_USDC_HEX,       arc_config.usdc_addr);
    arc_config.domain = ARC_DOMAIN;
    arc_config.chain.chain_id = ARC_CHAIN_ID;
    strncpy(arc_config.chain.rpc_url, ARC_RPC_URL, sizeof(arc_config.chain.rpc_url) - 1);

    BoatGatewayConfig base_config;
    memset(&base_config, 0, sizeof(base_config));
    parse_address(GATEWAY_WALLET_HEX, base_config.gateway_wallet_addr);
    parse_address(GATEWAY_MINTER_HEX, base_config.gateway_minter_addr);
    parse_address(BASE_USDC_HEX,      base_config.usdc_addr);
    base_config.domain = BASE_DOMAIN;
    base_config.chain.chain_id = BASE_CHAIN_ID;
    strncpy(base_config.chain.rpc_url, BASE_RPC_URL, sizeof(base_config.chain.rpc_url) - 1);

    /*--- 4. Check Gateway balance ---*/
    uint8_t balance[32];
    r = boat_gateway_balance(&arc_config, info.address, &arc_rpc, balance);
    if (r != BOAT_SUCCESS) {
        printf("Balance query failed: %d\n", (int)r);
        goto cleanup;
    }
    double bal_usdc = uint256_to_usdc(balance);
    printf("Gateway balance: %.6f USDC\n\n", bal_usdc);

    /*--- 5. Deposit 0.01 USDC (skip if balance already covers transfer + maxFee) ---*/
    double transfer_amount = 0.01;
    double max_fee_amount  = 0.1;
    double needed          = transfer_amount + max_fee_amount;  /* 0.11 USDC */

    if (bal_usdc < needed) {
        printf("Balance %.6f < %.6f needed — depositing %.6f USDC...\n",
               bal_usdc, needed, transfer_amount);

        uint8_t dep_amount[32];
        usdc_to_uint256(transfer_amount, dep_amount);

        uint8_t dep_txhash[32] = {0};
        r = boat_gateway_deposit(&arc_config, key, dep_amount, &arc_rpc, dep_txhash);
        if (r != BOAT_SUCCESS) {
            printf("Deposit failed: %d\n", (int)r);
            goto cleanup;
        }

        char tx_hex[67];
        boat_bin_to_hex(dep_txhash, 32, tx_hex, sizeof(tx_hex), true);
        printf("Deposit TX: %s\n\n", tx_hex);
    } else {
        printf("Balance sufficient (%.6f >= %.6f) — skipping deposit\n\n",
               bal_usdc, needed);
    }

    /*--- 6. Cross-chain transfer: Arc → Base Sepolia ---*/
    printf("Transferring %.6f USDC  Arc → Base Sepolia  (maxFee %.6f)...\n",
           transfer_amount, max_fee_amount);

    uint8_t xfer_amount[32], xfer_max_fee[32];
    usdc_to_uint256(transfer_amount, xfer_amount);
    usdc_to_uint256(max_fee_amount,  xfer_max_fee);

    BoatEvmRpc base_rpc;
    boat_evm_rpc_init(&base_rpc, BASE_RPC_URL);

    BoatGatewayTransferResult xfer_result;
    memset(&xfer_result, 0, sizeof(xfer_result));

    r = boat_gateway_transfer(&arc_config, &base_config, key,
                              NULL,  /* NULL = self-transfer */
                              xfer_amount, xfer_max_fee,
                              &base_rpc, &xfer_result);
    if (r != BOAT_SUCCESS) {
        printf("Cross-chain transfer failed: %d\n", (int)r);
        boat_evm_rpc_free(&base_rpc);
        goto cleanup;
    }

    /*--- 7. Print mint txhash ---*/
    char mint_hex[67];
    boat_bin_to_hex(xfer_result.mint_txhash, 32, mint_hex, sizeof(mint_hex), true);
    printf("Mint TX (Base Sepolia): %s\n", mint_hex);
    printf("Done — 0.01 USDC minted on Base Sepolia via Circle Gateway\n\n");

    boat_evm_rpc_free(&base_rpc);

    /*--- 8. Instant withdrawal: same-chain transfer (Gateway → wallet) ---*/
    /* Withdrawal from Gateway is just a transfer where src == dst chain.
     * This uses the Circle Gateway API (instant), not the 7-day trustless path. */
    printf("=== Step 4: Instant Withdrawal (Arc Gateway → Arc wallet) ===\n");

    double withdraw_amount = 0.005;
    double withdraw_fee    = 0.0;  /* same-chain: no transfer fee, only gas */
    printf("Withdrawing %.6f USDC from Gateway to wallet...\n", withdraw_amount);

    uint8_t wd_amount[32], wd_fee[32];
    usdc_to_uint256(withdraw_amount, wd_amount);
    usdc_to_uint256(withdraw_fee, wd_fee);

    BoatGatewayTransferResult wd_result;
    memset(&wd_result, 0, sizeof(wd_result));

    r = boat_gateway_transfer(&arc_config, &arc_config, key,
                              NULL,  /* NULL = self-transfer */
                              wd_amount, wd_fee,
                              &arc_rpc, &wd_result);
    if (r == BOAT_SUCCESS) {
        char wd_hex[67];
        boat_bin_to_hex(wd_result.mint_txhash, 32, wd_hex, sizeof(wd_hex), true);
        printf("Withdrawal TX (Arc): %s\n", wd_hex);
        printf("Done — %.6f USDC withdrawn from Gateway to wallet\n", withdraw_amount);
    } else {
        printf("Instant withdrawal failed: %d\n", (int)r);
    }

cleanup:
    boat_evm_rpc_free(&arc_rpc);
    boat_key_free(key);
    return (r == BOAT_SUCCESS) ? 0 : -1;

#else
    printf("Circle Gateway not enabled. Rebuild with -DBOAT_PAY_GATEWAY=ON\n");
    return -1;
#endif
}

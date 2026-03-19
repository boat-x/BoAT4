/******************************************************************************
 * BoAT v4 Example: x402 payment for HTTP resource
 *
 * Demonstrates the full x402 protocol flow on Base Sepolia:
 *   1. Request a paid resource → receive 402 with payment requirements
 *   2. Inspect the payment requirements (payTo, amount, asset)
 *   3. Sign an EIP-712 payment authorization
 *   4. Replay the request with X-Payment header → receive the resource
 *
 * Also shows the one-liner convenience function boat_x402_process().
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"
#include "boat_key.h"
#include "boat_evm.h"
#include "boat_pay.h"

#include <stdio.h>
#include <string.h>

/*===========================================================================
 * Configuration — edit these for your environment
 *=========================================================================*/
static const char *PRIVATE_KEY_HEX =
    "your_private_key_hex_here";                       /* 64 hex chars, no 0x prefix */

/* Public x402 test endpoint (echo server, auto-refund on Base Sepolia) */
static const char *X402_URL = "https://x402.payai.network/api/base-sepolia/paid-content";

/* Base Sepolia */
#define BASE_SEPOLIA_CHAIN_ID  84532
#define BASE_SEPOLIA_RPC       "https://sepolia.base.org"

/*===========================================================================
 * Main
 *=========================================================================*/
int main(void)
{
#if BOAT_PAY_X402_ENABLED

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
    printf("Payer: %s\n\n", addr_hex);

    /* Chain config for Base Sepolia */
    BoatEvmChainConfig chain = { .chain_id = BASE_SEPOLIA_CHAIN_ID, .eip1559 = false };
    strncpy(chain.rpc_url, BASE_SEPOLIA_RPC, sizeof(chain.rpc_url) - 1);

    /*--- 2. Request the paid resource → expect HTTP 402 ---*/
    printf("Requesting: %s\n", X402_URL);

    BoatX402PaymentReq req;
    uint8_t *response = NULL;
    size_t response_len = 0;
    memset(&req, 0, sizeof(req));

    r = boat_x402_request(X402_URL, NULL, &req, &response, &response_len);

    if (r == BOAT_SUCCESS) {
        /* Server returned 2xx without payment — resource is free */
        printf("Resource returned without payment (%zu bytes):\n", response_len);
        printf("%.*s\n", (int)response_len, (char *)response);
        boat_free(response);
        boat_key_free(key);
        return 0;
    }

    if (r != BOAT_ERROR_HTTP_402) {
        printf("Request failed (error %d) — endpoint may be down\n", (int)r);
        boat_key_free(key);
        return -1;
    }

    /*--- 3. Inspect the 402 payment requirements ---*/
    char pay_to_hex[43];
    boat_bin_to_hex(req.pay_to, 20, pay_to_hex, sizeof(pay_to_hex), true);

    printf("Got 402 — payment required:\n");
    printf("  pay_to:  %s\n", pay_to_hex);
    printf("  amount:  %s\n", req.amount_str);
    printf("  asset:   %s (version %s)\n", req.asset_name, req.asset_version);
    printf("  network: %s\n\n", req.network);

    /*--- 4. Sign the EIP-712 payment authorization ---*/
    printf("Signing payment...\n");

    char *payment_b64 = NULL;
    r = boat_x402_make_payment(&req, key, &chain, &payment_b64);
    if (r != BOAT_SUCCESS || !payment_b64) {
        printf("Payment signing failed: %d\n", (int)r);
        boat_key_free(key);
        return -1;
    }
    printf("  Payment payload: %zu bytes (base64)\n\n", strlen(payment_b64));

    /*--- 5. Replay request with X-Payment header → get the resource ---*/
    printf("Replaying request with payment...\n");

    response = NULL;
    response_len = 0;
    r = boat_x402_pay_and_get(X402_URL, NULL, payment_b64, &response, &response_len);
    boat_free(payment_b64);

    if (r == BOAT_SUCCESS && response && response_len > 0) {
        size_t print_len = response_len < 500 ? response_len : 500;
        printf("Resource received (%zu bytes):\n", response_len);
        printf("%.*s\n\n", (int)print_len, (char *)response);
        boat_free(response);
    } else {
        printf("Pay-and-get failed: %d\n", (int)r);
        boat_key_free(key);
        return -1;
    }

    /*--- 6. (Alternative) One-liner convenience flow ---*/
    printf("--- One-liner flow via boat_x402_process() ---\n");

    response = NULL;
    response_len = 0;
    r = boat_x402_process(X402_URL, NULL, key, &chain, &response, &response_len);
    if (r == BOAT_SUCCESS && response) {
        size_t print_len = response_len < 500 ? response_len : 500;
        printf("Resource (%zu bytes): %.*s\n", response_len, (int)print_len, (char *)response);
        boat_free(response);
    } else {
        printf("boat_x402_process failed: %d\n", (int)r);
    }

    boat_key_free(key);
    return (r == BOAT_SUCCESS) ? 0 : -1;

#else
    printf("x402 not enabled. Rebuild with -DBOAT_PAY_X402=ON\n");
    return -1;
#endif
}

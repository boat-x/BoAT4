/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Testnet integration test: x402 payment protocol on Base Sepolia
 *****************************************************************************/
#include "test_common.h"
#include "boat_evm.h"
#include "boat_pay.h"

#define BASE_SEPOLIA_CHAIN_ID   84532
#define BASE_SEPOLIA_RPC        "https://sepolia.base.org"

/* Primary x402 test endpoint (echo server, auto-refund) */
#define X402_TEST_URL   "https://x402.payai.network/api/base-sepolia/paid-content"

/* Fallback: local x402 server */
#define X402_LOCAL_URL  "http://localhost:4020/resource"

int main(int argc, char **argv)
{
    int kg = test_keygen_mode(argc, argv, BOAT_KEY_TYPE_SECP256K1);
    if (kg >= 0) return kg;

    test_init("test_x402_payment (Base Sepolia)");

    /*--- 1. key_import ---*/
    BoatKey *key = test_load_key(BOAT_KEY_TYPE_SECP256K1);
    TEST_ASSERT("key_import", key != NULL, "failed to import/generate key");
    if (!key) return test_summary();

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    test_print_address(&info);

    /* Determine x402 endpoint */
    const char *x402_url = getenv("BOAT_TEST_X402_URL");
    if (!x402_url || strlen(x402_url) == 0)
        x402_url = X402_TEST_URL;
    printf("  x402 endpoint: %s\n", x402_url);

    BoatEvmChainConfig chain = { BASE_SEPOLIA_CHAIN_ID, "", false };
    const char *rpc_url = test_get_rpc_url("BOAT_TEST_RPC", BASE_SEPOLIA_RPC);
    strncpy(chain.rpc_url, rpc_url, sizeof(chain.rpc_url) - 1);

    /*--- 2. request_402_resource ---*/
    BoatX402PaymentReq req;
    uint8_t *response = NULL;
    size_t response_len = 0;
    memset(&req, 0, sizeof(req));

    BoatResult r = boat_x402_request(x402_url, NULL, &req, &response, &response_len);

    if (r == BOAT_ERROR_HTTP_402) {
        /* Verify req fields are populated */
        bool req_valid = !test_is_zero(req.pay_to, 20) &&
                         strlen(req.amount_str) > 0 &&
                         strlen(req.asset_name) > 0;
        if (req_valid) {
            TEST_PASS("request_402_resource");
            printf("  pay_to: ");
            char pt[43]; boat_bin_to_hex(req.pay_to, 20, pt, sizeof(pt), true);
            printf("%s\n", pt);
            printf("  amount: %s\n", req.amount_str);
            printf("  asset_name: %s, version: %s\n", req.asset_name, req.asset_version);
            printf("  network: %s\n", req.network);
        } else {
            TEST_FAIL("request_402_resource", "402 received but req fields empty");
        }
    } else if (r == BOAT_SUCCESS) {
        /* Server returned 2xx without payment — unexpected for paid endpoint */
        TEST_FAIL("request_402_resource", "expected 402 but got 2xx (free resource?)");
        if (response) { boat_free(response); response = NULL; }
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "HTTP request failed (error %d) — endpoint may be down", (int)r);
        TEST_FAIL("request_402_resource", msg);
    }

    /*--- 3. make_payment ---*/
    if (r != BOAT_ERROR_HTTP_402) {
        TEST_SKIP("make_payment", "no 402 response to pay for");
        TEST_SKIP("full_flow", "skipped");
    } else {
        char *payment_b64 = NULL;
        r = boat_x402_make_payment(&req, key, &chain, &payment_b64);
        if (r == BOAT_SUCCESS && payment_b64 && strlen(payment_b64) > 0) {
            TEST_PASS("make_payment");
            printf("  Payment payload length: %zu bytes (base64)\n", strlen(payment_b64));
        } else {
            TEST_FAIL("make_payment", "failed to create payment payload");
        }
        if (payment_b64) boat_free(payment_b64);

        /*--- 4. full_flow ---*/
        response = NULL;
        response_len = 0;
        r = boat_x402_process(x402_url, NULL, key, &chain, &response, &response_len);
        if (r == BOAT_SUCCESS && response && response_len > 0) {
            TEST_PASS("full_flow");
            size_t print_len = response_len < 200 ? response_len : 200;
            printf("  Response (%zu bytes): %.*s\n", response_len, (int)print_len, response);
            boat_free(response);
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "x402 full flow failed (error %d)", (int)r);
            TEST_FAIL("full_flow", msg);
            if (response && response_len > 0) {
                size_t print_len = response_len < 500 ? response_len : 500;
                printf("  Server response: %.*s\n", (int)print_len, response);
            }
            if (response) boat_free(response);
        }
    }

    boat_key_free(key);
    return test_summary();
}

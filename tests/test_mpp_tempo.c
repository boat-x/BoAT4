/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Testnet integration test: MPP Tempo Charge on Tempo Moderato testnet
 *
 * Setup:
 *   1. Generate a key:    ./test_mpp_tempo --keygen
 *   2. Fund from faucet:  https://faucet.tempo.xyz
 *   3. Run test:          ./test_mpp_tempo
 *
 * Environment variables:
 *   BOAT_TEST_EVM_PRIVKEY    — hex private key (optional, overrides saved key)
 *   BOAT_TEST_MPP_URL        — MPP endpoint URL (default: https://mpp.dev/api/ping/paid)
 *   BOAT_TEST_TEMPO_RPC      — Tempo RPC URL (default: https://rpc.testnet.tempo.xyz)
 *****************************************************************************/
#include "test_common.h"
#include "boat_evm.h"
#include "boat_pay.h"

#if BOAT_PAY_MPP_ENABLED

/* Default MPP test endpoint — mpp.dev ping endpoint that requires payment */
#define MPP_DEFAULT_URL     "https://mpp.dev/api/ping/paid"

/* Tempo Moderato testnet RPC */
#define TEMPO_TESTNET_RPC   "https://rpc.testnet.tempo.xyz"

int main(int argc, char **argv)
{
    int kg = test_keygen_mode(argc, argv, BOAT_KEY_TYPE_SECP256K1);
    if (kg >= 0) return kg;

    test_init("test_mpp_tempo (Tempo Moderato testnet)");

    /*--- 1. Load key ---*/
    BoatKey *key = test_load_key(BOAT_KEY_TYPE_SECP256K1);
    TEST_ASSERT("key_load", key != NULL, "failed to load/generate key");
    if (!key) return test_summary();

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    test_print_address(&info);

    /*--- 2. Configure Tempo testnet ---*/
    const char *mpp_url = getenv("BOAT_TEST_MPP_URL");
    if (!mpp_url || strlen(mpp_url) == 0) mpp_url = MPP_DEFAULT_URL;

    const char *rpc_url = test_get_rpc_url("BOAT_TEST_TEMPO_RPC", TEMPO_TESTNET_RPC);

    printf("  MPP endpoint: %s\n", mpp_url);
    printf("  Tempo RPC:    %s\n", rpc_url);

    BoatMppTempoConfig config;
    memset(&config, 0, sizeof(config));
    config.chain.chain_id = BOAT_MPP_TEMPO_TESTNET_CHAIN_ID;
    strncpy(config.chain.rpc_url, rpc_url, sizeof(config.chain.rpc_url) - 1);
    config.chain.eip1559 = false;
    memcpy(config.token_addr, BOAT_MPP_TEMPO_PATHUSD_TESTNET, 20);
    strncpy(config.rpc_url, rpc_url, sizeof(config.rpc_url) - 1);

    printf("  Chain ID:     %llu\n", (unsigned long long)config.chain.chain_id);

    /*--- 3. Send initial request (expect 402) ---*/
    printf("\n--- Step 1: Request resource (expect 402) ---\n");

    BoatMppChallenge challenge;
    uint8_t *response = NULL;
    size_t response_len = 0;

    BoatResult r = boat_mpp_request(mpp_url, NULL, &challenge, &response, &response_len);

    if (r == BOAT_ERROR_HTTP_402) {
        TEST_PASS("mpp_request_402");
        printf("  Challenge ID:  %s\n", challenge.id);
        printf("  Realm:         %s\n", challenge.realm);
        printf("  Method:        %s\n", challenge.method);
        printf("  Intent:        %s\n", challenge.intent);
        printf("  Amount:        %s\n", challenge.amount);
        printf("  Currency:      %s\n", challenge.currency);
        printf("  Recipient:     %s\n", challenge.recipient);
        printf("  Chain ID:      %llu\n", (unsigned long long)challenge.chain_id);
        if (challenge.expires[0])
            printf("  Expires:       %s\n", challenge.expires);
        if (challenge.description[0])
            printf("  Description:   %s\n", challenge.description);
    } else if (r == BOAT_SUCCESS) {
        TEST_SKIP("mpp_request_402", "server returned 2xx without payment (free resource)");
        if (response) boat_free(response);
        boat_key_free(key);
        return test_summary();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "request failed (error %d) — endpoint may be down or not MPP", (int)r);
        TEST_FAIL("mpp_request_402", msg);
        boat_key_free(key);
        return test_summary();
    }

    /* Verify it's a Tempo charge */
    if (strcmp(challenge.method, "tempo") != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected method='tempo', got '%s' — endpoint may not support Tempo", challenge.method);
        TEST_FAIL("challenge_method_tempo", msg);
        boat_key_free(key);
        return test_summary();
    }
    TEST_PASS("challenge_method_tempo");

    /*--- 4. Execute Tempo Charge payment ---*/
    printf("\n--- Step 2: Execute on-chain payment ---\n");

    char *credential = NULL;
    r = boat_mpp_tempo_charge(&challenge, key, &config, &credential);

    if (r == BOAT_SUCCESS && credential) {
        TEST_PASS("tempo_charge");
        /* Print the credential (truncated) */
        size_t clen = strlen(credential);
        printf("  Credential (%zu chars): %.80s...\n", clen, credential);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "tempo_charge failed (error %d) — check pathUSD balance", (int)r);
        TEST_FAIL("tempo_charge", msg);
        boat_key_free(key);
        return test_summary();
    }

    /*--- 5. Retry with credential ---*/
    printf("\n--- Step 3: Retry request with payment credential ---\n");

    BoatMppReceipt receipt;
    memset(&receipt, 0, sizeof(receipt));

    r = boat_mpp_pay_and_get(mpp_url, NULL, credential, &response, &response_len, &receipt);
    boat_free(credential);

    if (r == BOAT_SUCCESS) {
        TEST_PASS("mpp_pay_and_get");
        printf("  Response (%zu bytes): %.*s\n",
               response_len,
               (int)(response_len < 200 ? response_len : 200),
               response);

        if (receipt.status[0]) {
            TEST_PASS("receipt_present");
            printf("  Receipt status:    %s\n", receipt.status);
            printf("  Receipt method:    %s\n", receipt.method);
            printf("  Receipt reference: %s\n", receipt.reference);
            printf("  Receipt timestamp: %s\n", receipt.timestamp);
        } else {
            TEST_SKIP("receipt_present", "no Payment-Receipt header in response");
        }
        boat_free(response);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "pay_and_get failed (error %d)", (int)r);
        TEST_FAIL("mpp_pay_and_get", msg);
        if (response) {
            printf("  Server response: %.*s\n",
                   (int)(response_len < 512 ? response_len : 512), response);
            boat_free(response);
        }
    }

    /*--- 6. Full convenience flow (separate test) ---*/
    printf("\n--- Step 4: Full boat_mpp_tempo_process() flow ---\n");

    response = NULL;
    response_len = 0;
    memset(&receipt, 0, sizeof(receipt));

    r = boat_mpp_tempo_process(mpp_url, NULL, key, &config,
                               &response, &response_len, &receipt);

    if (r == BOAT_SUCCESS) {
        TEST_PASS("mpp_tempo_process");
        printf("  Response (%zu bytes): %.*s\n",
               response_len,
               (int)(response_len < 200 ? response_len : 200),
               response);
        boat_free(response);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "process failed (error %d)", (int)r);
        TEST_FAIL("mpp_tempo_process", msg);
        if (response) { boat_free(response); }
    }

    boat_key_free(key);
    return test_summary();
}

#else
int main(void)
{
    printf("MPP not enabled. Rebuild with -DBOAT_PAY_MPP=ON\n");
    return 0;
}
#endif

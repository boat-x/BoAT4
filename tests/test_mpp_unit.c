/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit test: MPP envelope — base64url, challenge parsing, credential building,
 * receipt parsing. Pure logic tests, no network required.
 *****************************************************************************/
#include "test_common.h"
#include "boat_pay.h"

#if BOAT_PAY_MPP_ENABLED

/*============================================================================
 * Test 1: Base64url encode/decode round-trip
 *==========================================================================*/
static void test_base64url(void)
{
    /* Known test vector: "Hello, World!" */
    const uint8_t input[] = "Hello, World!";
    size_t input_len = 13;

    char encoded[64];
    size_t enc_len = boat_base64url_encode(input, input_len, encoded, sizeof(encoded));
    TEST_ASSERT("b64url_encode_len", enc_len > 0, "encode returned 0");

    /* base64url of "Hello, World!" = "SGVsbG8sIFdvcmxkIQ" (no padding) */
    TEST_ASSERT("b64url_encode_value", strcmp(encoded, "SGVsbG8sIFdvcmxkIQ") == 0,
                encoded);

    /* Decode back */
    uint8_t decoded[64];
    size_t dec_len = 0;
    BoatResult r = boat_base64url_decode(encoded, enc_len, decoded, sizeof(decoded), &dec_len);
    TEST_ASSERT("b64url_decode_ok", r == BOAT_SUCCESS, "decode failed");
    TEST_ASSERT("b64url_decode_len", dec_len == input_len, "length mismatch");
    TEST_ASSERT("b64url_decode_value", memcmp(decoded, input, input_len) == 0, "content mismatch");

    /* Test URL-safe characters: input with bytes that produce +/ in standard base64 */
    const uint8_t tricky[] = { 0xfb, 0xff, 0xfe };
    enc_len = boat_base64url_encode(tricky, 3, encoded, sizeof(encoded));
    TEST_ASSERT("b64url_no_plus_slash",
                strchr(encoded, '+') == NULL && strchr(encoded, '/') == NULL,
                "found +/ in base64url output");
    TEST_ASSERT("b64url_no_padding",
                strchr(encoded, '=') == NULL,
                "found = padding in base64url output");

    /* Decode the tricky value back */
    r = boat_base64url_decode(encoded, enc_len, decoded, sizeof(decoded), &dec_len);
    TEST_ASSERT("b64url_tricky_roundtrip",
                r == BOAT_SUCCESS && dec_len == 3 && memcmp(decoded, tricky, 3) == 0,
                "tricky roundtrip failed");

    /* Empty input */
    enc_len = boat_base64url_encode((const uint8_t *)"", 0, encoded, sizeof(encoded));
    TEST_ASSERT("b64url_empty_encode", enc_len == 0 && encoded[0] == '\0', "empty encode failed");
}

/*============================================================================
 * Test 2: Parse WWW-Authenticate: Payment header
 *==========================================================================*/
static void test_parse_challenge(void)
{
    /* Build a realistic request JSON and base64url-encode it */
    const char *request_json = "{\"amount\":\"20000\",\"currency\":\"0x20c0000000000000000000000000000000000000\","
                               "\"recipient\":\"0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266\","
                               "\"methodDetails\":{\"chainId\":4217}}";
    char request_b64[2048];
    boat_base64url_encode((const uint8_t *)request_json, strlen(request_json),
                          request_b64, sizeof(request_b64));

    /* Build a simulated 402 response headers string */
    char headers[4096];
    snprintf(headers, sizeof(headers),
        "HTTP/1.1 402 Payment Required\r\n"
        "Content-Type: application/problem+json\r\n"
        "WWW-Authenticate: Payment id=\"testChallengeId123\", realm=\"api.example.com\", "
        "method=\"tempo\", intent=\"charge\", request=\"%s\", "
        "expires=\"2026-12-31T23:59:59Z\", description=\"Test payment\"\r\n"
        "Content-Length: 42\r\n"
        "\r\n",
        request_b64);

    BoatMppChallenge challenges[2];
    size_t n_challenges = 0;
    BoatResult r = boat_mpp_parse_challenges(headers, strlen(headers),
                                             challenges, 2, &n_challenges);

    TEST_ASSERT("parse_challenge_ok", r == BOAT_SUCCESS, "parse returned error");
    TEST_ASSERT("parse_challenge_count", n_challenges == 1, "expected 1 challenge");

    if (n_challenges >= 1) {
        BoatMppChallenge *ch = &challenges[0];
        TEST_ASSERT("parse_id", strcmp(ch->id, "testChallengeId123") == 0, ch->id);
        TEST_ASSERT("parse_realm", strcmp(ch->realm, "api.example.com") == 0, ch->realm);
        TEST_ASSERT("parse_method", strcmp(ch->method, "tempo") == 0, ch->method);
        TEST_ASSERT("parse_intent", strcmp(ch->intent, "charge") == 0, ch->intent);
        TEST_ASSERT("parse_request_b64", strlen(ch->request_b64) > 0, "request_b64 empty");
        TEST_ASSERT("parse_expires", strcmp(ch->expires, "2026-12-31T23:59:59Z") == 0, ch->expires);
        TEST_ASSERT("parse_description", strcmp(ch->description, "Test payment") == 0, ch->description);

        /* Verify decoded request fields */
        TEST_ASSERT("parse_amount", strcmp(ch->amount, "20000") == 0, ch->amount);
        TEST_ASSERT("parse_currency",
                    strcmp(ch->currency, "0x20c0000000000000000000000000000000000000") == 0,
                    ch->currency);
        TEST_ASSERT("parse_recipient",
                    strcmp(ch->recipient, "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266") == 0,
                    ch->recipient);
        char cid_str[32];
        snprintf(cid_str, sizeof(cid_str), "%llu", (unsigned long long)ch->chain_id);
        TEST_ASSERT("parse_chain_id", ch->chain_id == 4217, cid_str);
    }

    /* Test: no MPP challenge in headers */
    const char *no_mpp = "HTTP/1.1 402 Payment Required\r\nContent-Type: text/plain\r\n\r\n";
    r = boat_mpp_parse_challenges(no_mpp, strlen(no_mpp), challenges, 2, &n_challenges);
    TEST_ASSERT("parse_no_challenge", r != BOAT_SUCCESS, "should fail when no Payment header");
}

/*============================================================================
 * Test 3: Build credential
 *==========================================================================*/
static void test_build_credential(void)
{
    BoatMppChallenge ch;
    memset(&ch, 0, sizeof(ch));
    strncpy(ch.id, "test123", sizeof(ch.id));
    strncpy(ch.realm, "api.example.com", sizeof(ch.realm));
    strncpy(ch.method, "tempo", sizeof(ch.method));
    strncpy(ch.intent, "charge", sizeof(ch.intent));
    strncpy(ch.request_b64, "eyJ0ZXN0IjoxfQ", sizeof(ch.request_b64)); /* {"test":1} */

    const char *payload = "{\"type\":\"hash\",\"hash\":\"0xabcdef\"}";
    const char *source = "did:pkh:eip155:4217:0xDeadBeef";

    char *credential = NULL;
    BoatResult r = boat_mpp_build_credential(&ch, source, payload, &credential);

    TEST_ASSERT("build_credential_ok", r == BOAT_SUCCESS, "build_credential failed");
    TEST_ASSERT("build_credential_not_null", credential != NULL, "credential is NULL");

    if (credential) {
        /* Must start with "Payment " */
        TEST_ASSERT("credential_prefix", strncmp(credential, "Payment ", 8) == 0,
                    "missing 'Payment ' prefix");

        /* Decode and verify the JSON structure */
        const char *b64_part = credential + 8;
        size_t b64_len = strlen(b64_part);
        uint8_t *decoded = (uint8_t *)malloc(b64_len + 4);
        size_t dec_len = 0;
        r = boat_base64url_decode(b64_part, b64_len, decoded, b64_len + 4, &dec_len);
        TEST_ASSERT("credential_decode_ok", r == BOAT_SUCCESS, "credential base64url decode failed");

        if (r == BOAT_SUCCESS) {
            decoded[dec_len] = '\0';
            const char *json = (const char *)decoded;

            /* Check key substrings are present */
            TEST_ASSERT("credential_has_challenge",
                        strstr(json, "\"challenge\"") != NULL, "missing challenge field");
            TEST_ASSERT("credential_has_id",
                        strstr(json, "\"test123\"") != NULL, "missing challenge id");
            TEST_ASSERT("credential_has_realm",
                        strstr(json, "\"api.example.com\"") != NULL, "missing realm");
            TEST_ASSERT("credential_has_method",
                        strstr(json, "\"tempo\"") != NULL, "missing method");
            TEST_ASSERT("credential_has_source",
                        strstr(json, "\"did:pkh:eip155:4217:0xDeadBeef\"") != NULL, "missing source");
            TEST_ASSERT("credential_has_payload",
                        strstr(json, "\"type\":\"hash\"") != NULL, "missing payload type");
            TEST_ASSERT("credential_has_hash",
                        strstr(json, "\"0xabcdef\"") != NULL, "missing hash");
        }
        free(decoded);
        boat_free(credential);
    }

    /* Test without source (NULL) */
    r = boat_mpp_build_credential(&ch, NULL, payload, &credential);
    TEST_ASSERT("build_credential_no_source_ok", r == BOAT_SUCCESS, "build without source failed");
    if (credential) {
        const char *b64_part = credential + 8;
        uint8_t *decoded = (uint8_t *)malloc(strlen(b64_part) + 4);
        size_t dec_len = 0;
        boat_base64url_decode(b64_part, strlen(b64_part), decoded, strlen(b64_part) + 4, &dec_len);
        decoded[dec_len] = '\0';
        TEST_ASSERT("credential_no_source_field",
                    strstr((const char *)decoded, "\"source\"") == NULL, "source should be absent");
        free(decoded);
        boat_free(credential);
    }
}

/*============================================================================
 * Test 4: Parse receipt
 *==========================================================================*/
static void test_parse_receipt(void)
{
    /* Build a receipt JSON and base64url-encode it */
    const char *receipt_json = "{\"status\":\"success\",\"method\":\"tempo\","
                               "\"reference\":\"0xabc123\",\"timestamp\":\"2026-03-27T12:00:00Z\"}";
    char receipt_b64[1024];
    boat_base64url_encode((const uint8_t *)receipt_json, strlen(receipt_json),
                          receipt_b64, sizeof(receipt_b64));

    char headers[2048];
    snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Payment-Receipt: %s\r\n"
        "\r\n",
        receipt_b64);

    BoatMppReceipt receipt;
    BoatResult r = boat_mpp_parse_receipt(headers, strlen(headers), &receipt);

    TEST_ASSERT("parse_receipt_ok", r == BOAT_SUCCESS, "parse_receipt failed");
    TEST_ASSERT("receipt_status", strcmp(receipt.status, "success") == 0, receipt.status);
    TEST_ASSERT("receipt_method", strcmp(receipt.method, "tempo") == 0, receipt.method);
    TEST_ASSERT("receipt_reference", strcmp(receipt.reference, "0xabc123") == 0, receipt.reference);
    TEST_ASSERT("receipt_timestamp", strcmp(receipt.timestamp, "2026-03-27T12:00:00Z") == 0, receipt.timestamp);

    /* Test: no receipt header (not an error — receipts are optional) */
    const char *no_receipt = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n";
    memset(&receipt, 0, sizeof(receipt));
    r = boat_mpp_parse_receipt(no_receipt, strlen(no_receipt), &receipt);
    TEST_ASSERT("parse_no_receipt_ok", r == BOAT_SUCCESS, "should succeed with no receipt");
    TEST_ASSERT("parse_no_receipt_empty", receipt.status[0] == '\0', "status should be empty");
}

/*============================================================================
 * Test 5: Tempo token address constants
 *==========================================================================*/
static void test_tempo_constants(void)
{
    /* pathUSD testnet: 0x20c0000000000000000000000000000000000000 */
    TEST_ASSERT("pathusd_testnet_byte0", BOAT_MPP_TEMPO_PATHUSD_TESTNET[0] == 0x20, "wrong byte 0");
    TEST_ASSERT("pathusd_testnet_byte1", BOAT_MPP_TEMPO_PATHUSD_TESTNET[1] == 0xc0, "wrong byte 1");
    bool rest_zero = true;
    for (int i = 2; i < 20; i++) {
        if (BOAT_MPP_TEMPO_PATHUSD_TESTNET[i] != 0) { rest_zero = false; break; }
    }
    TEST_ASSERT("pathusd_testnet_rest_zero", rest_zero, "bytes 2-19 should be zero");

    /* USDC mainnet: 0x20c000000000000000000000b9537d11c60e8b50 */
    TEST_ASSERT("usdc_mainnet_byte0", BOAT_MPP_TEMPO_USDC_MAINNET[0] == 0x20, "wrong byte 0");
    TEST_ASSERT("usdc_mainnet_byte12", BOAT_MPP_TEMPO_USDC_MAINNET[12] == 0xb9, "wrong byte 12");
    TEST_ASSERT("usdc_mainnet_byte19", BOAT_MPP_TEMPO_USDC_MAINNET[19] == 0x50, "wrong byte 19");

    /* Chain ID */
    TEST_ASSERT("tempo_chain_id", BOAT_MPP_TEMPO_MAINNET_CHAIN_ID == 4217, "wrong chain ID");
}

/*============================================================================
 * Main
 *==========================================================================*/
int main(void)
{
    test_init("test_mpp_unit (MPP envelope — no network)");

    test_base64url();
    test_parse_challenge();
    test_build_credential();
    test_parse_receipt();
    test_tempo_constants();

    return test_summary();
}

#else
int main(void)
{
    printf("MPP not enabled. Rebuild with -DBOAT_PAY_MPP=ON\n");
    return 0;
}
#endif

/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared test utilities for BoAT v4 testnet integration tests.
 *****************************************************************************/
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include "boat.h"
#include "boat_key.h"
#include "boat_pal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*--- Pass/Fail counters ---*/
static int g_test_pass = 0;
static int g_test_fail = 0;
static int g_test_skip = 0;

#define TEST_PASS(name) \
    do { g_test_pass++; printf("  [PASS] %s\n", (name)); } while(0)

#define TEST_FAIL(name, reason) \
    do { g_test_fail++; printf("  [FAIL] %s — %s\n", (name), (reason)); } while(0)

#define TEST_SKIP(name, reason) \
    do { g_test_skip++; printf("  [SKIP] %s — %s\n", (name), (reason)); } while(0)

#define TEST_ASSERT(name, cond, fail_reason) \
    do { if (cond) { TEST_PASS(name); } else { TEST_FAIL(name, fail_reason); } } while(0)

/*--- Summary ---*/
static inline int test_summary(void)
{
    printf("\n=== Results: %d passed, %d failed, %d skipped ===\n",
           g_test_pass, g_test_fail, g_test_skip);
    return g_test_fail > 0 ? 1 : 0;
}

/*--- Init PAL ---*/
static inline void test_init(const char *test_name)
{
    printf("\n=== %s ===\n", test_name);
    boat_pal_linux_init();
}

/* Storage name for persisted test key */
#define TEST_KEY_STORAGE_NAME  "boat_test_key"

/*--- Load key: env var → saved file → generate fresh & save ---*/
static inline BoatKey *test_load_key(BoatKeyType type)
{
    /* Priority 1: Type-specific env var */
    const char *env_name = (type == BOAT_KEY_TYPE_ED25519)
                           ? "BOAT_TEST_SOL_PRIVKEY" : "BOAT_TEST_EVM_PRIVKEY";
    const char *env_val = getenv(env_name);
    if (env_val && strlen(env_val) > 0) {
        BoatKey *key = boat_key_import_string(type, env_val);
        if (key) {
            printf("  Key imported from %s\n", env_name);
            return key;
        }
        printf("  ERROR: %s is set but key import failed. "
               "Supported formats: base58, JSON array [n1,n2,...], hex.\n", env_name);
        return NULL;
    }

    /* Priority 2: Previously saved key file */
    BoatKey *key = boat_key_load(TEST_KEY_STORAGE_NAME, type);
    if (key) {
        printf("  Key loaded from saved file (%s)\n", TEST_KEY_STORAGE_NAME);
        return key;
    }

    /* Priority 3: Generate fresh key and persist it */
    printf("  No saved key found, generating fresh key...\n");
    key = boat_key_generate(type);
    if (key) {
        boat_key_save(key, TEST_KEY_STORAGE_NAME);
        BoatKeyInfo info;
        boat_key_get_info(key, &info);
        char addr_str[69];
        boat_address_to_string(&info, addr_str, sizeof(addr_str));
        printf("  Generated address: %s\n", addr_str);
        printf("  Key saved to file '%s' (will be reused on next run)\n", TEST_KEY_STORAGE_NAME);
        printf("  Fund this address from faucet, then re-run.\n");
    }
    return key;
}

/*--- Get RPC URL from env or default ---*/
static inline const char *test_get_rpc_url(const char *env_name, const char *default_url)
{
    const char *url = getenv(env_name);
    return (url && strlen(url) > 0) ? url : default_url;
}

/*--- Print address from key info ---*/
static inline void test_print_address(const BoatKeyInfo *info)
{
    char str[69];
    if (boat_address_to_string(info, str, sizeof(str)) == BOAT_SUCCESS) {
        printf("  Address: %s\n", str);
    }
}

/*--- Print txhash ---*/
static inline void test_print_txhash(const uint8_t txhash[32])
{
    char hex[67];
    boat_bin_to_hex(txhash, 32, hex, sizeof(hex), true);
    printf("  TxHash: %s\n", hex);
}

/*--- Check if a 32-byte buffer is all zeros ---*/
static inline bool test_is_zero(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0) return false;
    }
    return true;
}

/*--- Keygen mode: generate key, save to file, print address, exit ---*/
static inline int test_keygen_mode(int argc, char **argv, BoatKeyType type)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--keygen") == 0) {
            boat_pal_linux_init();
            BoatKey *key = boat_key_generate(type);
            if (!key) {
                fprintf(stderr, "Key generation failed\n");
                return 1;
            }

            /* Persist key so subsequent runs can load it */
            BoatResult r = boat_key_save(key, TEST_KEY_STORAGE_NAME);
            if (r != BOAT_SUCCESS) {
                fprintf(stderr, "Warning: failed to save key to '%s'\n", TEST_KEY_STORAGE_NAME);
            }

            BoatKeyInfo info;
            boat_key_get_info(key, &info);

            char addr_str[69];
            boat_address_to_string(&info, addr_str, sizeof(addr_str));

            printf("Generated %s key\n", type == BOAT_KEY_TYPE_SECP256K1 ? "secp256k1" : "ed25519");
            printf("Address: %s\n", addr_str);
            printf("Key saved to file '%s'\n", TEST_KEY_STORAGE_NAME);
            printf("\nFund this address from faucet, then re-run: %s\n", argv[0]);

            boat_key_free(key);
            return 0;
        }
    }
    return -1; /* not keygen mode */
}

#endif /* TEST_COMMON_H */

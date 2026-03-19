/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"

#include <string.h>

/* trezor-crypto headers */
#include "ecdsa.h"
#include "secp256k1.h"
#include "nist256p1.h"
#include "bignum.h"
#include "sha3.h"
#include "ed25519-donna/ed25519.h"

/*----------------------------------------------------------------------------
 * Helpers
 *--------------------------------------------------------------------------*/
static const ecdsa_curve *get_curve(BoatKeyType type)
{
    switch (type) {
        case BOAT_KEY_TYPE_SECP256K1: return &secp256k1;
        case BOAT_KEY_TYPE_SECP256R1: return &nist256p1;
        default: return NULL;
    }
}

/* secp256k1 order N for private key validation */
static const uint32_t secp256k1_order_le[8] = {
    0xD0364141, 0xBFD25E8C, 0xAF48A03B, 0xBAAEDCE6,
    0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
};

static bool privkey_is_valid(const uint8_t *key32)
{
    /* Check key is not zero and less than curve order */
    bool all_zero = true;
    for (int i = 0; i < 32; i++) {
        if (key32[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) return false;

    bignum256 key_bn, order_bn;
    bn_read_be(key32, &key_bn);
    bn_read_le((const uint8_t *)secp256k1_order_le, &order_bn);
    return bn_is_less(&key_bn, &order_bn);
}

/*----------------------------------------------------------------------------
 * Key generation
 *--------------------------------------------------------------------------*/
BoatResult boat_key_soft_generate(BoatKeyType type, uint8_t *privkey,
                                  uint8_t *pubkey, size_t *pubkey_len)
{
    if (!privkey || !pubkey || !pubkey_len) return BOAT_ERROR_ARG_NULL;

    if (type == BOAT_KEY_TYPE_ED25519) {
        /* Generate 32-byte random seed */
        BoatResult r = boat_random(privkey, 32);
        if (r != BOAT_SUCCESS) return BOAT_ERROR_KEY_GEN;

        /* Derive public key */
        ed25519_publickey(privkey, pubkey);
        /* Store sk||pk in privkey (64 bytes) for ed25519 signing */
        memcpy(privkey + 32, pubkey, 32);
        *pubkey_len = 32;
        return BOAT_SUCCESS;
    }

    /* ECDSA key generation with validation */
    const ecdsa_curve *curve = get_curve(type);
    if (!curve) return BOAT_ERROR_KEY_TYPE;

    for (int tries = 0; tries < 100; tries++) {
        BoatResult r = boat_random(privkey, 32);
        if (r != BOAT_SUCCESS) return BOAT_ERROR_KEY_GEN;

        if (!privkey_is_valid(privkey)) continue;

        /* Derive uncompressed public key (65 bytes: 04||x||y) */
        ecdsa_get_public_key65(curve, privkey, pubkey);
        *pubkey_len = 65;
        return BOAT_SUCCESS;
    }

    return BOAT_ERROR_KEY_GEN;
}

/*----------------------------------------------------------------------------
 * Public key derivation from private key
 *--------------------------------------------------------------------------*/
BoatResult boat_key_soft_pubkey(BoatKeyType type, const uint8_t *privkey,
                                uint8_t *pubkey, size_t *pubkey_len)
{
    if (!privkey || !pubkey || !pubkey_len) return BOAT_ERROR_ARG_NULL;

    if (type == BOAT_KEY_TYPE_ED25519) {
        ed25519_publickey(privkey, pubkey);
        *pubkey_len = 32;
        return BOAT_SUCCESS;
    }

    const ecdsa_curve *curve = get_curve(type);
    if (!curve) return BOAT_ERROR_KEY_TYPE;

    ecdsa_get_public_key65(curve, privkey, pubkey);
    *pubkey_len = 65;
    return BOAT_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Address derivation
 *--------------------------------------------------------------------------*/
BoatResult boat_key_soft_address(BoatKeyType type, const uint8_t *pubkey, size_t pubkey_len,
                                 uint8_t *address, size_t *address_len)
{
    if (!pubkey || !address || !address_len) return BOAT_ERROR_ARG_NULL;

    if (type == BOAT_KEY_TYPE_ED25519) {
        /* Solana address = raw 32-byte public key */
        if (pubkey_len != 32) return BOAT_ERROR_ARG_INVALID;
        memcpy(address, pubkey, 32);
        *address_len = 32;
        return BOAT_SUCCESS;
    }

    /* EVM address = keccak256(pubkey[1..64])[12..31] */
    if (pubkey_len != 65 || pubkey[0] != 0x04) return BOAT_ERROR_ARG_INVALID;

    uint8_t hash[32];
    keccak_256(pubkey + 1, 64, hash);
    memcpy(address, hash + 12, 20);
    *address_len = 20;
    return BOAT_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Signing (non-recoverable)
 *--------------------------------------------------------------------------*/
BoatResult boat_key_soft_sign(BoatKeyType type, const uint8_t *privkey,
                              const uint8_t *hash, size_t hash_len,
                              uint8_t *sig, size_t *sig_len)
{
    if (!privkey || !hash || !sig || !sig_len) return BOAT_ERROR_ARG_NULL;

    if (type == BOAT_KEY_TYPE_ED25519) {
        /* ed25519 signs the full message, but we accept pre-hashed for consistency */
        /* privkey is sk(32)||pk(32) = 64 bytes */
        ed25519_sign(hash, hash_len, privkey, privkey + 32, sig);
        *sig_len = 64;
        return BOAT_SUCCESS;
    }

    const ecdsa_curve *curve = get_curve(type);
    if (!curve) return BOAT_ERROR_KEY_TYPE;
    if (hash_len != 32) return BOAT_ERROR_ARG_INVALID;

    uint8_t v;
    if (ecdsa_sign_digest(curve, privkey, hash, sig, &v, NULL) != 0) {
        return BOAT_ERROR_KEY_SIGN;
    }
    *sig_len = 64;
    return BOAT_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Recoverable signing (EVM ecrecover: returns r[32] || s[32] || v[1])
 *--------------------------------------------------------------------------*/
BoatResult boat_key_soft_sign_recoverable(BoatKeyType type, const uint8_t *privkey,
                                          const uint8_t *hash32, uint8_t sig65[65])
{
    if (!privkey || !hash32 || !sig65) return BOAT_ERROR_ARG_NULL;

    const ecdsa_curve *curve = get_curve(type);
    if (!curve) return BOAT_ERROR_KEY_TYPE;

    uint8_t v;
    /* ecdsa_sign_digest outputs r||s (64 bytes) and recovery id v (0 or 1) */
    if (ecdsa_sign_digest(curve, privkey, hash32, sig65, &v, NULL) != 0) {
        return BOAT_ERROR_KEY_SIGN;
    }
    sig65[64] = v;  /* recovery id: 0 or 1 */
    return BOAT_SUCCESS;
}

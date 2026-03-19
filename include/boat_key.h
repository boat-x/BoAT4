/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#ifndef BOAT_KEY_H
#define BOAT_KEY_H

#include "boat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque key handle — internal struct defined in boat_key.c */
typedef struct BoatKey BoatKey;

/* Key info returned by boat_key_get_info */
typedef struct {
    BoatKeyType type;
    uint8_t     pubkey[65];     /* uncompressed: 04||x||y (secp256k1/r1) or 32-byte ed25519 */
    size_t      pubkey_len;     /* 65 for secp256k1/r1, 32 for ed25519 */
    uint8_t     address[32];    /* 20 bytes for EVM, 32 bytes for Solana */
    size_t      address_len;    /* 20 or 32 */
} BoatKeyInfo;

/*--- Lifecycle ---*/
BoatKey   *boat_key_generate(BoatKeyType type);
BoatKey   *boat_key_import_raw(BoatKeyType type, const uint8_t *privkey, size_t len);
BoatKey   *boat_key_import_base58(BoatKeyType type, const char *b58_privkey);
BoatKey   *boat_key_import_json_array(BoatKeyType type, const char *json_array);
BoatKey   *boat_key_import_string(BoatKeyType type, const char *key_str);
BoatKey   *boat_key_import_mnemonic(const char *mnemonic, const char *path, BoatKeyType type);
BoatKey   *boat_key_from_se(BoatKeyType type, uint32_t slot);
void       boat_key_free(BoatKey *key);

/*--- Signing ---*/
BoatResult boat_key_sign(const BoatKey *key, const uint8_t *hash, size_t hash_len,
                         uint8_t *sig, size_t *sig_len);
BoatResult boat_key_sign_recoverable(const BoatKey *key, const uint8_t *hash32,
                                     uint8_t sig65[65]);

/*--- Info ---*/
BoatResult boat_key_get_info(const BoatKey *key, BoatKeyInfo *info);

/*--- Address string conversion ---*/
BoatResult boat_address_to_string(const BoatKeyInfo *info, char *str, size_t str_cap);
BoatResult boat_address_from_string(const char *str, uint8_t *address, size_t addr_cap, size_t *out_len);

/*--- Persistent storage ---*/
BoatResult boat_key_save(const BoatKey *key, const char *name);
BoatKey   *boat_key_load(const char *name, BoatKeyType type);
BoatResult boat_key_delete(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* BOAT_KEY_H */

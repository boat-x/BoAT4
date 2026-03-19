/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_key.h"
#include "boat_pal.h"
#include "cJSON.h"

#include <string.h>

/* Forward declarations for software backend (boat_key_soft.c) */
extern BoatResult boat_key_soft_generate(BoatKeyType type, uint8_t *privkey, uint8_t *pubkey, size_t *pubkey_len);
extern BoatResult boat_key_soft_sign(BoatKeyType type, const uint8_t *privkey,
                                     const uint8_t *hash, size_t hash_len,
                                     uint8_t *sig, size_t *sig_len);
extern BoatResult boat_key_soft_sign_recoverable(BoatKeyType type, const uint8_t *privkey,
                                                  const uint8_t *hash32, uint8_t sig65[65]);
extern BoatResult boat_key_soft_pubkey(BoatKeyType type, const uint8_t *privkey,
                                       uint8_t *pubkey, size_t *pubkey_len);
extern BoatResult boat_key_soft_address(BoatKeyType type, const uint8_t *pubkey, size_t pubkey_len,
                                        uint8_t *address, size_t *address_len);

/*----------------------------------------------------------------------------
 * Internal key structure (opaque to callers)
 *--------------------------------------------------------------------------*/
struct BoatKey {
    BoatKeyType type;
    uint8_t     privkey[64];    /* 32 for secp256k1/r1, 64 for ed25519 (sk||pk) */
    size_t      privkey_len;
    uint8_t     pubkey[65];
    size_t      pubkey_len;
    uint8_t     address[32];
    size_t      address_len;
};

/*----------------------------------------------------------------------------
 * Lifecycle
 *--------------------------------------------------------------------------*/
BoatKey *boat_key_generate(BoatKeyType type)
{
    BoatKey *key = (BoatKey *)boat_malloc(sizeof(BoatKey));
    if (!key) return NULL;
    memset(key, 0, sizeof(BoatKey));
    key->type = type;

    if (boat_key_soft_generate(type, key->privkey, key->pubkey, &key->pubkey_len) != BOAT_SUCCESS) {
        boat_free(key);
        return NULL;
    }
    key->privkey_len = (type == BOAT_KEY_TYPE_ED25519) ? 64 : 32;

    if (boat_key_soft_address(type, key->pubkey, key->pubkey_len,
                              key->address, &key->address_len) != BOAT_SUCCESS) {
        memset(key, 0, sizeof(BoatKey));
        boat_free(key);
        return NULL;
    }

    return key;
}

BoatKey *boat_key_import_raw(BoatKeyType type, const uint8_t *privkey, size_t len)
{
    if (!privkey) return NULL;
    if (type == BOAT_KEY_TYPE_ED25519 && len != 32) return NULL;
    if (type != BOAT_KEY_TYPE_ED25519 && len != 32) return NULL;

    BoatKey *key = (BoatKey *)boat_malloc(sizeof(BoatKey));
    if (!key) return NULL;
    memset(key, 0, sizeof(BoatKey));
    key->type = type;
    memcpy(key->privkey, privkey, 32);
    key->privkey_len = 32;

    if (boat_key_soft_pubkey(type, key->privkey, key->pubkey, &key->pubkey_len) != BOAT_SUCCESS) {
        memset(key, 0, sizeof(BoatKey));
        boat_free(key);
        return NULL;
    }

    /* For ed25519, store sk||pk (64 bytes) as trezor-crypto expects */
    if (type == BOAT_KEY_TYPE_ED25519) {
        memcpy(key->privkey + 32, key->pubkey, 32);
        key->privkey_len = 64;
    }

    if (boat_key_soft_address(type, key->pubkey, key->pubkey_len,
                              key->address, &key->address_len) != BOAT_SUCCESS) {
        memset(key, 0, sizeof(BoatKey));
        boat_free(key);
        return NULL;
    }

    return key;
}

BoatKey *boat_key_import_base58(BoatKeyType type, const char *b58_privkey)
{
    if (!b58_privkey) return NULL;
    size_t slen = strlen(b58_privkey);
    if (slen < 32 || slen > 88) {
        BOAT_LOG(BOAT_LOG_NORMAL, "Base58 key rejected: length %zu not in [32..88]", slen);
        return NULL;
    }
    uint8_t raw[64];
    size_t raw_len = 0;
    if (boat_base58_decode(b58_privkey, raw, sizeof(raw), &raw_len) != BOAT_SUCCESS) {
        BOAT_LOG(BOAT_LOG_NORMAL, "Base58 key rejected: decode failed");
        return NULL;
    }
    if (raw_len != 32 && raw_len != 64) {
        BOAT_LOG(BOAT_LOG_NORMAL, "Base58 key rejected: decoded %zu bytes (expected 32 or 64)", raw_len);
        memset(raw, 0, sizeof(raw));
        return NULL;
    }
    /* Solana wallets export 64-byte keys (seed||pubkey) — use first 32 as seed */
    size_t key_len = (raw_len == 64) ? 32 : raw_len;
    BoatKey *key = boat_key_import_raw(type, raw, key_len);
    memset(raw, 0, sizeof(raw));
    return key;
}

BoatKey *boat_key_import_json_array(BoatKeyType type, const char *json_array)
{
    if (!json_array) return NULL;

    cJSON *arr = cJSON_Parse(json_array);
    if (!arr || !cJSON_IsArray(arr)) {
        BOAT_LOG(BOAT_LOG_NORMAL, "JSON array key rejected: not a valid JSON array");
        if (arr) cJSON_Delete(arr);
        return NULL;
    }
    int count = cJSON_GetArraySize(arr);
    if (count != 32 && count != 64) {
        BOAT_LOG(BOAT_LOG_NORMAL, "JSON array key rejected: %d elements (expected 32 or 64)", count);
        cJSON_Delete(arr);
        return NULL;
    }
    uint8_t raw[64];
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item || !cJSON_IsNumber(item)) {
            BOAT_LOG(BOAT_LOG_NORMAL, "JSON array key rejected: element %d is not a number", i);
            cJSON_Delete(arr);
            return NULL;
        }
        double val = item->valuedouble;
        if (val < 0 || val > 255 || val != (int)val) {
            BOAT_LOG(BOAT_LOG_NORMAL, "JSON array key rejected: element %d value %.1f not in [0..255]", i, val);
            cJSON_Delete(arr);
            return NULL;
        }
        raw[i] = (uint8_t)(int)val;
    }
    cJSON_Delete(arr);
    size_t key_len = (count == 64) ? 32 : (size_t)count;
    BoatKey *key = boat_key_import_raw(type, raw, key_len);
    memset(raw, 0, sizeof(raw));
    return key;
}

static bool is_base58_char(char c)
{
    return (c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') ||
           (c >= 'J' && c <= 'N') || (c >= 'P' && c <= 'Z') ||
           (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
}

BoatKey *boat_key_import_string(BoatKeyType type, const char *key_str)
{
    if (!key_str || strlen(key_str) == 0) {
        BOAT_LOG(BOAT_LOG_NORMAL, "Key string is NULL or empty");
        return NULL;
    }
    size_t len = strlen(key_str);

    /* JSON byte array: starts with '[' */
    if (key_str[0] == '[') {
        BoatKey *key = boat_key_import_json_array(type, key_str);
        if (!key) {
            BOAT_LOG(BOAT_LOG_NORMAL, "Key string starts with '[' but JSON array import failed");
        }
        return key;
    }

    /* Base58: all chars in base58 alphabet, length 32-88 */
    if (len >= 32 && len <= 88) {
        bool all_b58 = true;
        for (size_t i = 0; i < len; i++) {
            if (!is_base58_char(key_str[i])) { all_b58 = false; break; }
        }
        if (all_b58) {
            BoatKey *key = boat_key_import_base58(type, key_str);
            if (!key) {
                BOAT_LOG(BOAT_LOG_NORMAL, "Key string looks like base58 but import failed");
            }
            return key;
        }
    }

    /* Hex: length >= 64, optional 0x prefix */
    if (len >= 64) {
        const char *hex = key_str;
        size_t hex_len = len;
        if (hex_len >= 66 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
            hex += 2;
            hex_len -= 2;
        }
        if (hex_len == 64 || hex_len == 128) {
            bool all_hex = true;
            for (size_t i = 0; i < hex_len; i++) {
                char c = hex[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    all_hex = false; break;
                }
            }
            if (all_hex) {
                uint8_t privkey[64];
                size_t key_len = 0;
                if (boat_hex_to_bin(key_str, privkey, sizeof(privkey), &key_len) == BOAT_SUCCESS && key_len >= 32) {
                    BoatKey *key = boat_key_import_raw(type, privkey, key_len > 32 ? 32 : key_len);
                    memset(privkey, 0, sizeof(privkey));
                    if (key) return key;
                }
                BOAT_LOG(BOAT_LOG_NORMAL, "Key string looks like hex but import failed");
                return NULL;
            }
        }
    }

    BOAT_LOG(BOAT_LOG_NORMAL, "Key string format not recognized (supported: base58, JSON array [n1,n2,...], hex)");
    return NULL;
}

BoatKey *boat_key_import_mnemonic(const char *mnemonic, const char *path, BoatKeyType type)
{
    /* BIP-39 mnemonic → seed → BIP-32 derivation → private key */
    /* Uses trezor-crypto bip39/bip32 functions */
    (void)mnemonic; (void)path; (void)type;
    /* TODO: implement with bip39_seed_from_mnemonic + hdnode_from_seed + hdnode_private_ckd_prime */
    BOAT_LOG(BOAT_LOG_NORMAL, "Mnemonic import not yet implemented");
    return NULL;
}

BoatKey *boat_key_from_se(BoatKeyType type, uint32_t slot)
{
    (void)type; (void)slot;
#if BOAT_KEY_SE_ENABLED
    /* TODO: SE backend */
#endif
    BOAT_LOG(BOAT_LOG_NORMAL, "SE key backend not compiled");
    return NULL;
}

void boat_key_free(BoatKey *key)
{
    if (key) {
        /* Secure wipe */
        memset(key, 0, sizeof(BoatKey));
        boat_free(key);
    }
}

/*----------------------------------------------------------------------------
 * Signing
 *--------------------------------------------------------------------------*/
BoatResult boat_key_sign(const BoatKey *key, const uint8_t *hash, size_t hash_len,
                         uint8_t *sig, size_t *sig_len)
{
    if (!key || !hash || !sig || !sig_len) return BOAT_ERROR_ARG_NULL;
    return boat_key_soft_sign(key->type, key->privkey, hash, hash_len, sig, sig_len);
}

BoatResult boat_key_sign_recoverable(const BoatKey *key, const uint8_t *hash32,
                                     uint8_t sig65[65])
{
    if (!key || !hash32 || !sig65) return BOAT_ERROR_ARG_NULL;
    if (key->type == BOAT_KEY_TYPE_ED25519) return BOAT_ERROR_KEY_TYPE;
    return boat_key_soft_sign_recoverable(key->type, key->privkey, hash32, sig65);
}

/*----------------------------------------------------------------------------
 * Info
 *--------------------------------------------------------------------------*/
BoatResult boat_key_get_info(const BoatKey *key, BoatKeyInfo *info)
{
    if (!key || !info) return BOAT_ERROR_ARG_NULL;
    memset(info, 0, sizeof(BoatKeyInfo));
    info->type = key->type;
    memcpy(info->pubkey, key->pubkey, key->pubkey_len);
    info->pubkey_len = key->pubkey_len;
    memcpy(info->address, key->address, key->address_len);
    info->address_len = key->address_len;
    return BOAT_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Address string conversion
 *--------------------------------------------------------------------------*/
BoatResult boat_address_to_string(const BoatKeyInfo *info, char *str, size_t str_cap)
{
    if (!info || !str) return BOAT_ERROR_ARG_NULL;
    if (info->address_len == 32) {
        /* Solana: base58 */
        return boat_base58_encode(info->address, 32, str, str_cap);
    }
    /* EVM: hex with 0x prefix */
    return boat_bin_to_hex(info->address, info->address_len, str, str_cap, true);
}

BoatResult boat_address_from_string(const char *str, uint8_t *address, size_t addr_cap, size_t *out_len)
{
    if (!str || !address) return BOAT_ERROR_ARG_NULL;
    size_t len = strlen(str);
    /* EVM hex: 0x prefix + 40 hex chars, or 40 hex chars */
    if ((len == 42 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) || len == 40) {
        return boat_hex_to_bin(str, address, addr_cap, out_len);
    }
    /* Solana base58 */
    return boat_base58_decode(str, address, addr_cap, out_len);
}

/*----------------------------------------------------------------------------
 * Persistent storage
 *--------------------------------------------------------------------------*/
#define KEY_STORAGE_MAGIC  0x424F4154  /* "BOAT" */

BoatResult boat_key_save(const BoatKey *key, const char *name)
{
    if (!key || !name) return BOAT_ERROR_ARG_NULL;

    /* Format: [magic:4][type:1][privkey_len:1][privkey:N] */
    uint8_t buf[128];
    size_t pos = 0;
    uint32_t magic = KEY_STORAGE_MAGIC;
    memcpy(buf + pos, &magic, 4); pos += 4;
    buf[pos++] = (uint8_t)key->type;
    buf[pos++] = (uint8_t)key->privkey_len;
    memcpy(buf + pos, key->privkey, key->privkey_len);
    pos += key->privkey_len;

    return boat_storage_write(name, buf, pos);
}

BoatKey *boat_key_load(const char *name, BoatKeyType type)
{
    if (!name) return NULL;

    uint8_t buf[128];
    size_t rd = 0;
    if (boat_storage_read(name, buf, sizeof(buf), &rd) != BOAT_SUCCESS) return NULL;
    if (rd < 6) return NULL;

    uint32_t magic;
    memcpy(&magic, buf, 4);
    if (magic != KEY_STORAGE_MAGIC) return NULL;

    BoatKeyType stored_type = (BoatKeyType)buf[4];
    size_t privkey_len = buf[5];
    if (stored_type != type) return NULL;
    if (rd < 6 + privkey_len) return NULL;

    return boat_key_import_raw(type, buf + 6, (privkey_len > 32) ? 32 : privkey_len);
}

BoatResult boat_key_delete(const char *name)
{
    return boat_storage_delete(name);
}

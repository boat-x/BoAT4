/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_pay.h"
#include "boat_pal.h"
#include "sha3.h"

#include <string.h>

/*============================================================================
 * EIP-712 Implementation
 *==========================================================================*/

BoatResult boat_eip712_domain_hash(const BoatEip712Domain *domain, uint8_t hash[32])
{
    if (!domain || !hash) return BOAT_ERROR_ARG_NULL;

    /*
     * domainSeparator = keccak256(
     *   keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
     *   || keccak256(name)
     *   || keccak256(version)
     *   || encode(chainId)
     *   || encode(verifyingContract)
     * )
     */

    /* Type hash */
    const char *type_str = "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    uint8_t type_hash[32];
    keccak_256((const uint8_t *)type_str, strlen(type_str), type_hash);

    /* keccak256(name) */
    uint8_t name_hash[32];
    keccak_256((const uint8_t *)domain->name, strlen(domain->name), name_hash);

    /* keccak256(version) */
    uint8_t version_hash[32];
    keccak_256((const uint8_t *)domain->version, strlen(domain->version), version_hash);

    /* chainId as uint256 */
    uint8_t chain_id_slot[32];
    memset(chain_id_slot, 0, 32);
    uint64_t cid = domain->chain_id;
    for (int i = 0; i < 8; i++) {
        chain_id_slot[31 - i] = (uint8_t)(cid & 0xFF);
        cid >>= 8;
    }

    /* verifyingContract as address (left-padded to 32 bytes) */
    uint8_t contract_slot[32];
    memset(contract_slot, 0, 12);
    memcpy(contract_slot + 12, domain->verifying_contract, 20);

    /* Concatenate and hash */
    uint8_t buf[5 * 32]; /* typeHash + nameHash + versionHash + chainId + contract */
    memcpy(buf,       type_hash, 32);
    memcpy(buf + 32,  name_hash, 32);
    memcpy(buf + 64,  version_hash, 32);
    memcpy(buf + 96,  chain_id_slot, 32);
    memcpy(buf + 128, contract_slot, 32);

    keccak_256(buf, 160, hash);
    return BOAT_SUCCESS;
}

BoatResult boat_eip712_hash_struct(const uint8_t *type_hash, const uint8_t *encoded_data,
                                   size_t data_len, uint8_t hash[32])
{
    if (!type_hash || !hash) return BOAT_ERROR_ARG_NULL;

    /* hashStruct = keccak256(typeHash || encodeData) */
    size_t total = 32 + data_len;
    uint8_t *buf = (uint8_t *)boat_malloc(total);
    if (!buf) return BOAT_ERROR_MEM_ALLOC;

    memcpy(buf, type_hash, 32);
    if (encoded_data && data_len > 0) {
        memcpy(buf + 32, encoded_data, data_len);
    }

    keccak_256(buf, total, hash);
    boat_free(buf);
    return BOAT_SUCCESS;
}

BoatResult boat_eip712_sign(const uint8_t domain_hash[32], const uint8_t struct_hash[32],
                            const BoatKey *key, uint8_t sig65[65])
{
    if (!domain_hash || !struct_hash || !key || !sig65) return BOAT_ERROR_ARG_NULL;

    /* EIP-712 signing hash = keccak256("\x19\x01" || domainSeparator || structHash) */
    uint8_t buf[66]; /* 2 + 32 + 32 */
    buf[0] = 0x19;
    buf[1] = 0x01;
    memcpy(buf + 2, domain_hash, 32);
    memcpy(buf + 34, struct_hash, 32);

    uint8_t hash[32];
    keccak_256(buf, 66, hash);

    return boat_key_sign_recoverable(key, hash, sig65);
}

/*============================================================================
 * EIP-3009 Implementation
 *==========================================================================*/

/* Pre-computed: keccak256("TransferWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)") */
static const uint8_t EIP3009_TYPE_HASH[32] = {
    0x7c, 0x7c, 0x6c, 0xdb, 0x67, 0xa1, 0x87, 0x43,
    0xf4, 0x9e, 0xc6, 0xfa, 0x9b, 0x35, 0xf5, 0x0d,
    0x52, 0xed, 0x05, 0xcb, 0xed, 0x4c, 0xc5, 0x92,
    0xe1, 0x3b, 0x44, 0x50, 0x1c, 0x1a, 0x22, 0x67
};

BoatResult boat_eip3009_sign(const BoatEip3009Auth *auth, const BoatEip712Domain *domain,
                             const BoatKey *key, uint8_t sig65[65])
{
    if (!auth || !domain || !key || !sig65) return BOAT_ERROR_ARG_NULL;

    /* Domain separator */
    uint8_t domain_hash[32];
    BoatResult r = boat_eip712_domain_hash(domain, domain_hash);
    if (r != BOAT_SUCCESS) return r;

    /*
     * encodeData = encode(from) || encode(to) || encode(value)
     *           || encode(validAfter) || encode(validBefore) || encode(nonce)
     */
    uint8_t encoded[6 * 32]; /* 6 fields, each 32 bytes */

    /* from (address → left-padded to 32) */
    memset(encoded, 0, 32);
    memcpy(encoded + 12, auth->from, 20);

    /* to */
    memset(encoded + 32, 0, 32);
    memcpy(encoded + 32 + 12, auth->to, 20);

    /* value (uint256) */
    memcpy(encoded + 64, auth->value, 32);

    /* validAfter (uint256) */
    memset(encoded + 96, 0, 32);
    {
        uint64_t va = auth->valid_after;
        for (int i = 0; i < 8; i++) {
            encoded[96 + 31 - i] = (uint8_t)(va & 0xFF);
            va >>= 8;
        }
    }

    /* validBefore (uint256) */
    memset(encoded + 128, 0, 32);
    {
        uint64_t vb = auth->valid_before;
        for (int i = 0; i < 8; i++) {
            encoded[128 + 31 - i] = (uint8_t)(vb & 0xFF);
            vb >>= 8;
        }
    }

    /* nonce (bytes32) */
    memcpy(encoded + 160, auth->nonce, 32);

    /* hashStruct(message) */
    uint8_t struct_hash[32];
    r = boat_eip712_hash_struct(EIP3009_TYPE_HASH, encoded, sizeof(encoded), struct_hash);
    if (r != BOAT_SUCCESS) return r;

    /* Sign */
    return boat_eip712_sign(domain_hash, struct_hash, key, sig65);
}

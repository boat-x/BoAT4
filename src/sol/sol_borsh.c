/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_sol.h"
#include <string.h>

#if BOAT_SOL_ENABLED

BoatResult boat_borsh_init(BoatBorshEncoder *enc, uint8_t *buf, size_t cap)
{
    if (!enc || !buf) return BOAT_ERROR_ARG_NULL;
    enc->buf = buf;
    enc->cap = cap;
    enc->len = 0;
    return BOAT_SUCCESS;
}

static BoatResult borsh_write(BoatBorshEncoder *enc, const uint8_t *data, size_t len)
{
    if (enc->len + len > enc->cap) return BOAT_ERROR_MEM_OVERFLOW;
    memcpy(enc->buf + enc->len, data, len);
    enc->len += len;
    return BOAT_SUCCESS;
}

BoatResult boat_borsh_write_u8(BoatBorshEncoder *enc, uint8_t val)
{
    return borsh_write(enc, &val, 1);
}

BoatResult boat_borsh_write_u32(BoatBorshEncoder *enc, uint32_t val)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)(val);
    buf[1] = (uint8_t)(val >> 8);
    buf[2] = (uint8_t)(val >> 16);
    buf[3] = (uint8_t)(val >> 24);
    return borsh_write(enc, buf, 4);
}

BoatResult boat_borsh_write_u64(BoatBorshEncoder *enc, uint64_t val)
{
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
    return borsh_write(enc, buf, 8);
}

BoatResult boat_borsh_write_pubkey(BoatBorshEncoder *enc, const uint8_t pubkey[32])
{
    if (!pubkey) return BOAT_ERROR_ARG_NULL;
    return borsh_write(enc, pubkey, 32);
}

BoatResult boat_borsh_write_bytes(BoatBorshEncoder *enc, const uint8_t *data, size_t len)
{
    if (!data && len > 0) return BOAT_ERROR_ARG_NULL;
    BoatResult r = boat_borsh_write_u32(enc, (uint32_t)len);
    if (r != BOAT_SUCCESS) return r;
    if (len > 0) r = borsh_write(enc, data, len);
    return r;
}

BoatResult boat_borsh_write_string(BoatBorshEncoder *enc, const char *str)
{
    if (!str) return BOAT_ERROR_ARG_NULL;
    size_t len = strlen(str);
    BoatResult r = boat_borsh_write_u32(enc, (uint32_t)len);
    if (r != BOAT_SUCCESS) return r;
    return borsh_write(enc, (const uint8_t *)str, len);
}

size_t boat_borsh_len(const BoatBorshEncoder *enc)
{
    return enc ? enc->len : 0;
}

#endif /* BOAT_SOL_ENABLED */

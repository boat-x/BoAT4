/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"

/*----------------------------------------------------------------------------
 * Logging global
 *--------------------------------------------------------------------------*/
BoatLogLevel g_boat_log_level = (BoatLogLevel)BOAT_LOG_LEVEL;

/*----------------------------------------------------------------------------
 * Hex / Bin conversion
 *--------------------------------------------------------------------------*/
static int hex_char_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

BoatResult boat_hex_to_bin(const char *hex_str, uint8_t *bin, size_t bin_cap, size_t *out_len)
{
    if (!hex_str || !bin) return BOAT_ERROR_ARG_NULL;

    /* Skip optional 0x prefix */
    if (hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        hex_str += 2;
    }

    size_t hex_len = strlen(hex_str);
    /* Handle odd-length hex by treating as if leading zero */
    bool odd = (hex_len % 2) != 0;
    size_t byte_len = (hex_len + 1) / 2;

    if (byte_len > bin_cap) return BOAT_ERROR_MEM_OVERFLOW;

    size_t bi = 0;
    size_t hi = 0;

    if (odd) {
        int v = hex_char_val(hex_str[0]);
        if (v < 0) return BOAT_ERROR_ARG_INVALID;
        bin[bi++] = (uint8_t)v;
        hi = 1;
    }

    while (hi < hex_len) {
        int h = hex_char_val(hex_str[hi]);
        int l = hex_char_val(hex_str[hi + 1]);
        if (h < 0 || l < 0) return BOAT_ERROR_ARG_INVALID;
        bin[bi++] = (uint8_t)((h << 4) | l);
        hi += 2;
    }

    if (out_len) *out_len = bi;
    return BOAT_SUCCESS;
}

BoatResult boat_bin_to_hex(const uint8_t *bin, size_t len, char *hex_str, size_t hex_cap, bool prefix_0x)
{
    if (!bin || !hex_str) return BOAT_ERROR_ARG_NULL;

    size_t needed = len * 2 + (prefix_0x ? 2 : 0) + 1;
    if (needed > hex_cap) return BOAT_ERROR_MEM_OVERFLOW;

    static const char hex_chars[] = "0123456789abcdef";
    size_t pos = 0;

    if (prefix_0x) {
        hex_str[pos++] = '0';
        hex_str[pos++] = 'x';
    }

    for (size_t i = 0; i < len; i++) {
        hex_str[pos++] = hex_chars[(bin[i] >> 4) & 0x0F];
        hex_str[pos++] = hex_chars[bin[i] & 0x0F];
    }
    hex_str[pos] = '\0';

    return BOAT_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Base58 encode/decode (wrappers around third-party/crypto/base58.c)
 *--------------------------------------------------------------------------*/
extern bool b58enc(char *b58, size_t *b58sz, const void *data, size_t binsz);
extern bool b58tobin(void *bin, size_t *binszp, const char *b58);

BoatResult boat_base58_encode(const uint8_t *bin, size_t bin_len, char *b58_str, size_t b58_cap)
{
    if (!bin || !b58_str) return BOAT_ERROR_ARG_NULL;
    size_t b58_len = b58_cap;
    if (!b58enc(b58_str, &b58_len, bin, bin_len)) return BOAT_ERROR_MEM_OVERFLOW;
    return BOAT_SUCCESS;
}

BoatResult boat_base58_decode(const char *b58_str, uint8_t *bin, size_t bin_cap, size_t *out_len)
{
    if (!b58_str || !bin) return BOAT_ERROR_ARG_NULL;
    uint8_t tmp[128];
    size_t tmp_len = sizeof(tmp);
    if (!b58tobin(tmp, &tmp_len, b58_str)) return BOAT_ERROR_ARG_INVALID;
    /* b58tobin writes result at the END of tmp buffer */
    if (tmp_len > bin_cap) return BOAT_ERROR_MEM_OVERFLOW;
    memcpy(bin, tmp + sizeof(tmp) - tmp_len, tmp_len);
    if (out_len) *out_len = tmp_len;
    return BOAT_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Amount conversion (human-readable ↔ minimum unit)
 *--------------------------------------------------------------------------*/
#include <math.h>

static uint64_t pow10_table[] = {
    1ULL,                    /* 0 */
    10ULL,                   /* 1 */
    100ULL,                  /* 2 */
    1000ULL,                 /* 3 */
    10000ULL,                /* 4 */
    100000ULL,               /* 5 */
    1000000ULL,              /* 6 */
    10000000ULL,             /* 7 */
    100000000ULL,            /* 8 */
    1000000000ULL,           /* 9 */
    10000000000ULL,          /* 10 */
    100000000000ULL,         /* 11 */
    1000000000000ULL,        /* 12 */
    10000000000000ULL,       /* 13 */
    100000000000000ULL,      /* 14 */
    1000000000000000ULL,     /* 15 */
    10000000000000000ULL,    /* 16 */
    100000000000000000ULL,   /* 17 */
    1000000000000000000ULL,  /* 18 */
    10000000000000000000ULL, /* 19 */
};

BoatResult boat_amount_to_uint64(double amount, uint8_t decimals, uint64_t *value)
{
    if (!value) return BOAT_ERROR_ARG_NULL;
    if (amount < 0 || decimals > 19) return BOAT_ERROR_ARG_INVALID;
    /* Use round() to avoid floating-point truncation issues (e.g. 0.001 * 1e18) */
    double scaled = amount * (double)pow10_table[decimals];
    if (scaled > (double)UINT64_MAX) return BOAT_ERROR_ARG_OUT_OF_RANGE;
    *value = (uint64_t)round(scaled);
    return BOAT_SUCCESS;
}

BoatResult boat_uint64_to_amount(uint64_t value, uint8_t decimals, double *amount)
{
    if (!amount) return BOAT_ERROR_ARG_NULL;
    if (decimals > 19) return BOAT_ERROR_ARG_INVALID;
    *amount = (double)value / (double)pow10_table[decimals];
    return BOAT_SUCCESS;
}

BoatResult boat_amount_to_uint256(double amount, uint8_t decimals, uint8_t value[32])
{
    if (!value) return BOAT_ERROR_ARG_NULL;
    if (amount < 0 || decimals > 19) return BOAT_ERROR_ARG_INVALID;
    memset(value, 0, 32);

    /*
     * Convert double to string to avoid FP multiplication precision loss.
     * Parse whole and fractional digit strings, pad/truncate frac to `decimals`,
     * then convert the combined decimal string to uint256 via multiply-and-add.
     */
    char str[64];
    snprintf(str, sizeof(str), "%.15g", amount);

    /* Split at decimal point */
    char *dot = strchr(str, '.');
    char whole_str[32] = {0};
    char frac_str[32] = {0};
    if (dot) {
        size_t wlen = (size_t)(dot - str);
        memcpy(whole_str, str, wlen);
        whole_str[wlen] = '\0';
        /* Copy frac digits, up to `decimals` chars */
        const char *fp = dot + 1;
        size_t flen = strlen(fp);
        /* Trim trailing zeros from printf artifacts beyond actual precision */
        size_t copy = (flen < (size_t)decimals) ? flen : (size_t)decimals;
        memcpy(frac_str, fp, copy);
        frac_str[copy] = '\0';
    } else {
        strncpy(whole_str, str, sizeof(whole_str) - 1);
    }

    /* Pad frac_str with trailing zeros to exactly `decimals` digits */
    size_t frac_len = strlen(frac_str);
    while (frac_len < (size_t)decimals) {
        frac_str[frac_len++] = '0';
    }
    frac_str[frac_len] = '\0';

    /* Concatenate: whole_str + frac_str = full decimal integer string */
    char full[64];
    snprintf(full, sizeof(full), "%s%s", whole_str, frac_str);

    /* Strip leading zeros */
    const char *p = full;
    while (*p == '0' && *(p + 1) != '\0') p++;

    /* Convert decimal string to big-endian uint256 via multiply-by-10-and-add */
    for (; *p; p++) {
        int digit = *p - '0';
        /* Multiply value by 10 */
        uint32_t carry = 0;
        for (int i = 31; i >= 0; i--) {
            uint32_t prod = (uint32_t)value[i] * 10 + carry;
            value[i] = prod & 0xFF;
            carry = prod >> 8;
        }
        /* Add digit */
        carry = (uint32_t)digit;
        for (int i = 31; i >= 0 && carry; i--) {
            uint32_t sum = (uint32_t)value[i] + carry;
            value[i] = sum & 0xFF;
            carry = sum >> 8;
        }
    }

    return BOAT_SUCCESS;
}

BoatResult boat_uint256_to_amount(const uint8_t value[32], uint8_t decimals, double *amount)
{
    if (!value || !amount) return BOAT_ERROR_ARG_NULL;
    if (decimals > 19) return BOAT_ERROR_ARG_INVALID;

    /* Convert big-endian uint256 to double (loses precision for very large values, acceptable for display) */
    double result = 0.0;
    for (int i = 0; i < 32; i++) {
        result = result * 256.0 + (double)value[i];
    }
    *amount = result / (double)pow10_table[decimals];
    return BOAT_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Dynamic buffer
 *--------------------------------------------------------------------------*/
BoatResult boat_buf_init(BoatBuf *buf, size_t initial_cap)
{
    if (!buf) return BOAT_ERROR_ARG_NULL;
    buf->data = (uint8_t *)boat_malloc(initial_cap > 0 ? initial_cap : 64);
    if (!buf->data) return BOAT_ERROR_MEM_ALLOC;
    buf->len = 0;
    buf->cap = initial_cap > 0 ? initial_cap : 64;
    return BOAT_SUCCESS;
}

static BoatResult buf_grow(BoatBuf *buf, size_t needed)
{
    if (buf->len + needed <= buf->cap) return BOAT_SUCCESS;
    size_t new_cap = buf->cap * 2;
    while (new_cap < buf->len + needed) new_cap *= 2;
    uint8_t *new_data = (uint8_t *)boat_malloc(new_cap);
    if (!new_data) return BOAT_ERROR_MEM_ALLOC;
    memcpy(new_data, buf->data, buf->len);
    boat_free(buf->data);
    buf->data = new_data;
    buf->cap = new_cap;
    return BOAT_SUCCESS;
}

BoatResult boat_buf_append(BoatBuf *buf, const uint8_t *data, size_t len)
{
    if (!buf || !data) return BOAT_ERROR_ARG_NULL;
    BoatResult r = buf_grow(buf, len);
    if (r != BOAT_SUCCESS) return r;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return BOAT_SUCCESS;
}

BoatResult boat_buf_append_byte(BoatBuf *buf, uint8_t byte)
{
    return boat_buf_append(buf, &byte, 1);
}

void boat_buf_reset(BoatBuf *buf)
{
    if (buf) buf->len = 0;
}

void boat_buf_free(BoatBuf *buf)
{
    if (buf && buf->data) {
        boat_free(buf->data);
        buf->data = NULL;
        buf->len = 0;
        buf->cap = 0;
    }
}

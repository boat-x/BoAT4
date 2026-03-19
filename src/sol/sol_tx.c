/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_sol.h"
#include "boat_key.h"
#include "boat_pal.h"

#include <string.h>

#if BOAT_SOL_ENABLED

/* Base64 encode (duplicated from sol_rpc.c for self-containment) */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    size_t out_len = 4 * ((in_len + 2) / 3);
    if (out_len + 1 > out_cap) return 0;
    size_t i, j;
    for (i = 0, j = 0; i < in_len;) {
        uint32_t a = i < in_len ? in[i++] : 0;
        uint32_t b = i < in_len ? in[i++] : 0;
        uint32_t c = i < in_len ? in[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }
    size_t mod = in_len % 3;
    if (mod == 1) { out[j - 1] = '='; out[j - 2] = '='; }
    else if (mod == 2) { out[j - 1] = '='; }
    out[j] = '\0';
    return j;
}

/*============================================================================
 * Compact-array encoding (Solana wire format)
 *==========================================================================*/
static size_t compact_u16_encode(uint16_t val, uint8_t *out)
{
    size_t n = 0;
    while (val >= 0x80) {
        out[n++] = (uint8_t)(val & 0x7F) | 0x80;
        val >>= 7;
    }
    out[n++] = (uint8_t)val;
    return n;
}

/*============================================================================
 * Account deduplication and ordering
 *==========================================================================*/
static int find_account(const BoatSolTx *tx, const uint8_t pubkey[32])
{
    for (size_t i = 0; i < tx->num_accounts; i++) {
        if (memcmp(tx->accounts[i].pubkey, pubkey, 32) == 0) return (int)i;
    }
    return -1;
}

static BoatResult add_or_update_account(BoatSolTx *tx, const uint8_t pubkey[32],
                                        bool is_signer, bool is_writable)
{
    int idx = find_account(tx, pubkey);
    if (idx >= 0) {
        /* Upgrade permissions */
        if (is_signer) tx->accounts[idx].is_signer = true;
        if (is_writable) tx->accounts[idx].is_writable = true;
        return BOAT_SUCCESS;
    }
    if (tx->num_accounts >= BOAT_SOL_MAX_ACCOUNTS) return BOAT_ERROR_MEM_OVERFLOW;
    memcpy(tx->accounts[tx->num_accounts].pubkey, pubkey, 32);
    tx->accounts[tx->num_accounts].is_signer = is_signer;
    tx->accounts[tx->num_accounts].is_writable = is_writable;
    tx->num_accounts++;
    return BOAT_SUCCESS;
}

/* Sort accounts: signers+writable first, then signers+readonly, then non-signer+writable, then non-signer+readonly */
static int account_sort_key(bool is_signer, bool is_writable)
{
    if (is_signer && is_writable) return 0;
    if (is_signer && !is_writable) return 1;
    if (!is_signer && is_writable) return 2;
    return 3;
}

static void sort_accounts(BoatSolTx *tx)
{
    /* Simple insertion sort — small N */
    for (size_t i = 1; i < tx->num_accounts; i++) {
        int ki = account_sort_key(tx->accounts[i].is_signer, tx->accounts[i].is_writable);
        size_t j = i;
        while (j > 0) {
            int kj = account_sort_key(tx->accounts[j-1].is_signer, tx->accounts[j-1].is_writable);
            if (kj <= ki) break;
            /* Swap */
            uint8_t tmp_pk[32];
            bool tmp_s = tx->accounts[j].is_signer;
            bool tmp_w = tx->accounts[j].is_writable;
            memcpy(tmp_pk, tx->accounts[j].pubkey, 32);
            memcpy(tx->accounts[j].pubkey, tx->accounts[j-1].pubkey, 32);
            tx->accounts[j].is_signer = tx->accounts[j-1].is_signer;
            tx->accounts[j].is_writable = tx->accounts[j-1].is_writable;
            memcpy(tx->accounts[j-1].pubkey, tmp_pk, 32);
            tx->accounts[j-1].is_signer = tmp_s;
            tx->accounts[j-1].is_writable = tmp_w;
            j--;
        }
    }

    /* Ensure fee payer is first */
    if (tx->fee_payer_set) {
        int idx = find_account(tx, tx->fee_payer);
        if (idx > 0) {
            /* Swap with position 0 */
            uint8_t tmp_pk[32];
            bool tmp_s = tx->accounts[0].is_signer;
            bool tmp_w = tx->accounts[0].is_writable;
            memcpy(tmp_pk, tx->accounts[0].pubkey, 32);

            memcpy(tx->accounts[0].pubkey, tx->accounts[idx].pubkey, 32);
            tx->accounts[0].is_signer = tx->accounts[idx].is_signer;
            tx->accounts[0].is_writable = tx->accounts[idx].is_writable;

            memcpy(tx->accounts[idx].pubkey, tmp_pk, 32);
            tx->accounts[idx].is_signer = tmp_s;
            tx->accounts[idx].is_writable = tmp_w;
        }
    }
}

/*============================================================================
 * Transaction builder
 *==========================================================================*/

BoatResult boat_sol_tx_init(BoatSolTx *tx)
{
    if (!tx) return BOAT_ERROR_ARG_NULL;
    memset(tx, 0, sizeof(BoatSolTx));
    return BOAT_SUCCESS;
}

BoatResult boat_sol_tx_set_fee_payer(BoatSolTx *tx, const uint8_t pubkey[32])
{
    if (!tx || !pubkey) return BOAT_ERROR_ARG_NULL;
    memcpy(tx->fee_payer, pubkey, 32);
    tx->fee_payer_set = true;
    return add_or_update_account(tx, pubkey, true, true);
}

BoatResult boat_sol_tx_set_blockhash(BoatSolTx *tx, const uint8_t blockhash[32])
{
    if (!tx || !blockhash) return BOAT_ERROR_ARG_NULL;
    memcpy(tx->recent_blockhash, blockhash, 32);
    tx->blockhash_set = true;
    return BOAT_SUCCESS;
}

BoatResult boat_sol_tx_add_instruction(BoatSolTx *tx, const BoatSolInstruction *ix)
{
    if (!tx || !ix) return BOAT_ERROR_ARG_NULL;
    if (tx->num_instructions >= BOAT_SOL_MAX_INSTRUCTIONS) return BOAT_ERROR_MEM_OVERFLOW;

    /* Add program_id to account list (non-signer, non-writable) */
    BoatResult r = add_or_update_account(tx, ix->program_id, false, false);
    if (r != BOAT_SUCCESS) return r;

    /* Add instruction accounts */
    for (size_t i = 0; i < ix->num_accounts; i++) {
        r = add_or_update_account(tx, ix->accounts[i].pubkey,
                                  ix->accounts[i].is_signer, ix->accounts[i].is_writable);
        if (r != BOAT_SUCCESS) return r;
    }

    /* Copy instruction */
    memcpy(&tx->instructions[tx->num_instructions], ix, sizeof(BoatSolInstruction));
    tx->num_instructions++;
    return BOAT_SUCCESS;
}

/*--- Serialize message ---*/
static BoatResult serialize_message(BoatSolTx *tx, uint8_t **msg_out, size_t *msg_len)
{
    sort_accounts(tx);

    /* Count header fields */
    uint8_t num_signers = 0;
    uint8_t num_readonly_signed = 0;
    uint8_t num_readonly_unsigned = 0;

    for (size_t i = 0; i < tx->num_accounts; i++) {
        if (tx->accounts[i].is_signer) {
            num_signers++;
            if (!tx->accounts[i].is_writable) num_readonly_signed++;
        } else {
            if (!tx->accounts[i].is_writable) num_readonly_unsigned++;
        }
    }

    BoatBuf buf;
    boat_buf_init(&buf, 1024);

    /* Header: 3 bytes */
    boat_buf_append_byte(&buf, num_signers);
    boat_buf_append_byte(&buf, num_readonly_signed);
    boat_buf_append_byte(&buf, num_readonly_unsigned);

    /* Account keys (compact-array) */
    uint8_t ca[3];
    size_t ca_len = compact_u16_encode((uint16_t)tx->num_accounts, ca);
    boat_buf_append(&buf, ca, ca_len);
    for (size_t i = 0; i < tx->num_accounts; i++) {
        boat_buf_append(&buf, tx->accounts[i].pubkey, 32);
    }

    /* Recent blockhash */
    boat_buf_append(&buf, tx->recent_blockhash, 32);

    /* Instructions (compact-array) */
    ca_len = compact_u16_encode((uint16_t)tx->num_instructions, ca);
    boat_buf_append(&buf, ca, ca_len);

    for (size_t i = 0; i < tx->num_instructions; i++) {
        BoatSolInstruction *ix = &tx->instructions[i];

        /* Program ID index */
        int pidx = find_account(tx, ix->program_id);
        boat_buf_append_byte(&buf, (uint8_t)pidx);

        /* Account indices (compact-array) */
        ca_len = compact_u16_encode((uint16_t)ix->num_accounts, ca);
        boat_buf_append(&buf, ca, ca_len);
        for (size_t j = 0; j < ix->num_accounts; j++) {
            int aidx = find_account(tx, ix->accounts[j].pubkey);
            boat_buf_append_byte(&buf, (uint8_t)aidx);
        }

        /* Data (compact-array) */
        ca_len = compact_u16_encode((uint16_t)ix->data_len, ca);
        boat_buf_append(&buf, ca, ca_len);
        if (ix->data_len > 0) {
            boat_buf_append(&buf, ix->data, ix->data_len);
        }
    }

    *msg_out = buf.data;
    *msg_len = buf.len;
    return BOAT_SUCCESS;
}

BoatResult boat_sol_tx_sign(BoatSolTx *tx, const BoatKey *key, uint8_t **raw, size_t *raw_len)
{
    if (!tx || !key || !raw || !raw_len) return BOAT_ERROR_ARG_NULL;
    if (!tx->blockhash_set) return BOAT_ERROR_SOL_BLOCKHASH_EXPIRED;

    /* Serialize message */
    uint8_t *msg = NULL;
    size_t msg_len = 0;
    BoatResult r = serialize_message(tx, &msg, &msg_len);
    if (r != BOAT_SUCCESS) return r;

    /* Sign with ed25519 */
    uint8_t sig[64];
    size_t sig_len = 64;
    r = boat_key_sign(key, msg, msg_len, sig, &sig_len);
    if (r != BOAT_SUCCESS) { boat_free(msg); return r; }

    /* Build wire format: compact_u16(num_signatures) || signatures || message */
    BoatBuf wire;
    boat_buf_init(&wire, sig_len + msg_len + 8);

    /* Number of signatures (compact-array: 1 signer) */
    uint8_t ca[3];
    size_t ca_len = compact_u16_encode(1, ca);
    boat_buf_append(&wire, ca, ca_len);
    boat_buf_append(&wire, sig, 64);
    boat_buf_append(&wire, msg, msg_len);
    boat_free(msg);

    if (wire.len > 1232) {
        boat_buf_free(&wire);
        return BOAT_ERROR_SOL_TX_TOO_LARGE;
    }

    *raw = wire.data;
    *raw_len = wire.len;
    return BOAT_SUCCESS;
}

BoatResult boat_sol_tx_send(BoatSolTx *tx, const BoatKey *key, BoatSolRpc *rpc, uint8_t sig[64])
{
    if (!tx || !key || !rpc || !sig) return BOAT_ERROR_ARG_NULL;

    uint8_t *raw = NULL;
    size_t raw_len = 0;
    BoatResult r = boat_sol_tx_sign(tx, key, &raw, &raw_len);
    if (r != BOAT_SUCCESS) return r;

    r = boat_sol_rpc_send_transaction(rpc, raw, raw_len, sig);
    boat_free(raw);
    return r;
}

void boat_sol_tx_free(BoatSolTx *tx)
{
    if (tx) memset(tx, 0, sizeof(BoatSolTx));
}

#endif /* BOAT_SOL_ENABLED */

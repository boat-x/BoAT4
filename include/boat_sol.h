/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#ifndef BOAT_SOL_H
#define BOAT_SOL_H

#include "boat.h"
#include "boat_key.h"

#ifdef __cplusplus
extern "C" {
#endif

#if BOAT_SOL_ENABLED

/*----------------------------------------------------------------------------
 * Well-known program IDs (32-byte pubkeys)
 *--------------------------------------------------------------------------*/
extern const uint8_t BOAT_SOL_SYSTEM_PROGRAM_ID[32];
extern const uint8_t BOAT_SOL_TOKEN_PROGRAM_ID[32];
extern const uint8_t BOAT_SOL_ATA_PROGRAM_ID[32];

/*----------------------------------------------------------------------------
 * Chain configuration
 *--------------------------------------------------------------------------*/
typedef enum {
    BOAT_SOL_COMMITMENT_FINALIZED = 0,
    BOAT_SOL_COMMITMENT_CONFIRMED,
    BOAT_SOL_COMMITMENT_PROCESSED
} BoatSolCommitment;

typedef struct {
    char               rpc_url[256];
    BoatSolCommitment  commitment;
} BoatSolChainConfig;

/*----------------------------------------------------------------------------
 * RPC context
 *--------------------------------------------------------------------------*/
typedef struct {
    char url[256];
    int  req_id;
} BoatSolRpc;

BoatResult boat_sol_rpc_init(BoatSolRpc *rpc, const char *url);
void       boat_sol_rpc_free(BoatSolRpc *rpc);

/*--- Typed RPC methods ---*/
BoatResult boat_sol_rpc_get_latest_blockhash(BoatSolRpc *rpc, BoatSolCommitment commitment,
                                             uint8_t blockhash[32], uint64_t *last_valid_height);
BoatResult boat_sol_rpc_get_balance(BoatSolRpc *rpc, const uint8_t pubkey[32], uint64_t *lamports);
BoatResult boat_sol_rpc_get_token_balance(BoatSolRpc *rpc, const uint8_t ata[32],
                                          uint64_t *amount, uint8_t *decimals);
BoatResult boat_sol_rpc_send_transaction(BoatSolRpc *rpc, const uint8_t *raw, size_t raw_len,
                                         uint8_t signature[64]);
BoatResult boat_sol_rpc_get_signature_status(BoatSolRpc *rpc, const uint8_t sig[64],
                                             bool *confirmed, bool *finalized);
BoatResult boat_sol_rpc_call(BoatSolRpc *rpc, const char *method, const char *params_json,
                             char **result_json);

/*----------------------------------------------------------------------------
 * Instruction
 *--------------------------------------------------------------------------*/
#define BOAT_SOL_MAX_IX_ACCOUNTS  16
#define BOAT_SOL_MAX_IX_DATA      512

typedef struct {
    uint8_t program_id[32];
    struct {
        uint8_t pubkey[32];
        bool    is_signer;
        bool    is_writable;
    } accounts[BOAT_SOL_MAX_IX_ACCOUNTS];
    size_t  num_accounts;
    uint8_t data[BOAT_SOL_MAX_IX_DATA];
    size_t  data_len;
} BoatSolInstruction;

BoatResult boat_sol_ix_init(BoatSolInstruction *ix, const uint8_t program_id[32]);
BoatResult boat_sol_ix_add_account(BoatSolInstruction *ix, const uint8_t pubkey[32],
                                   bool is_signer, bool is_writable);
BoatResult boat_sol_ix_set_data(BoatSolInstruction *ix, const uint8_t *data, size_t len);

/*----------------------------------------------------------------------------
 * Transaction builder
 *--------------------------------------------------------------------------*/
#define BOAT_SOL_MAX_INSTRUCTIONS  8
#define BOAT_SOL_MAX_ACCOUNTS      32
#define BOAT_SOL_MAX_SIGNERS       4

typedef struct {
    uint8_t fee_payer[32];
    uint8_t recent_blockhash[32];
    bool    fee_payer_set;
    bool    blockhash_set;

    BoatSolInstruction instructions[BOAT_SOL_MAX_INSTRUCTIONS];
    size_t num_instructions;

    /* Deduplicated account list (built during serialize) */
    struct {
        uint8_t pubkey[32];
        bool    is_signer;
        bool    is_writable;
    } accounts[BOAT_SOL_MAX_ACCOUNTS];
    size_t num_accounts;
} BoatSolTx;

BoatResult boat_sol_tx_init(BoatSolTx *tx);
BoatResult boat_sol_tx_set_fee_payer(BoatSolTx *tx, const uint8_t pubkey[32]);
BoatResult boat_sol_tx_set_blockhash(BoatSolTx *tx, const uint8_t blockhash[32]);
BoatResult boat_sol_tx_add_instruction(BoatSolTx *tx, const BoatSolInstruction *ix);
BoatResult boat_sol_tx_sign(BoatSolTx *tx, const BoatKey *key, uint8_t **raw, size_t *raw_len);
BoatResult boat_sol_tx_send(BoatSolTx *tx, const BoatKey *key, BoatSolRpc *rpc, uint8_t sig[64]);
void       boat_sol_tx_free(BoatSolTx *tx);

/*----------------------------------------------------------------------------
 * SPL Token helpers
 *--------------------------------------------------------------------------*/
BoatResult boat_sol_ata_address(const uint8_t wallet[32], const uint8_t mint[32], uint8_t ata[32]);
BoatResult boat_sol_spl_transfer(const uint8_t from_ata[32], const uint8_t to_ata[32],
                                 const uint8_t owner[32], uint64_t amount,
                                 BoatSolInstruction *ix);
BoatResult boat_sol_spl_create_ata(const uint8_t payer[32], const uint8_t wallet[32],
                                   const uint8_t mint[32], BoatSolInstruction *ix);
BoatResult boat_sol_spl_create_ata_idempotent(const uint8_t payer[32], const uint8_t wallet[32],
                                               const uint8_t mint[32], BoatSolInstruction *ix);

/* Generic PDA derivation: findProgramAddress(seeds, program_id) */
BoatResult boat_sol_find_pda(const uint8_t *seeds[], const size_t seed_lens[],
                             size_t num_seeds, const uint8_t program_id[32],
                             uint8_t pda[32]);

/*----------------------------------------------------------------------------
 * Borsh encoder (Tier 2)
 *--------------------------------------------------------------------------*/
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
} BoatBorshEncoder;

BoatResult boat_borsh_init(BoatBorshEncoder *enc, uint8_t *buf, size_t cap);
BoatResult boat_borsh_write_u8(BoatBorshEncoder *enc, uint8_t val);
BoatResult boat_borsh_write_u32(BoatBorshEncoder *enc, uint32_t val);
BoatResult boat_borsh_write_u64(BoatBorshEncoder *enc, uint64_t val);
BoatResult boat_borsh_write_pubkey(BoatBorshEncoder *enc, const uint8_t pubkey[32]);
BoatResult boat_borsh_write_bytes(BoatBorshEncoder *enc, const uint8_t *data, size_t len);
BoatResult boat_borsh_write_string(BoatBorshEncoder *enc, const char *str);
size_t     boat_borsh_len(const BoatBorshEncoder *enc);

#endif /* BOAT_SOL_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* BOAT_SOL_H */

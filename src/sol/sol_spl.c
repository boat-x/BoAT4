/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_sol.h"
#include "boat_pal.h"
#include "sha2.h"  /* trezor-crypto SHA256 */
#include "ed25519-donna/ed25519.h"  /* ed25519_point_is_on_curve */

#include <string.h>

#if BOAT_SOL_ENABLED

/*----------------------------------------------------------------------------
 * Well-known program IDs
 *--------------------------------------------------------------------------*/
/* 11111111111111111111111111111111 */
const uint8_t BOAT_SOL_SYSTEM_PROGRAM_ID[32] = {0};

/* TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA */
const uint8_t BOAT_SOL_TOKEN_PROGRAM_ID[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93,
    0xd9, 0xcb, 0xe1, 0x46, 0xce, 0xeb, 0x79, 0xac,
    0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91,
    0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9
};

/* ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL */
const uint8_t BOAT_SOL_ATA_PROGRAM_ID[32] = {
    0x8c, 0x97, 0x25, 0x8f, 0x4e, 0x24, 0x89, 0xf1,
    0xbb, 0x3d, 0x10, 0x29, 0x14, 0x8e, 0x0d, 0x83,
    0x0b, 0x5a, 0x13, 0x99, 0xda, 0xff, 0x10, 0x84,
    0x04, 0x8e, 0x7b, 0xd8, 0xdb, 0xe9, 0xf8, 0x59
};

/*----------------------------------------------------------------------------
 * ATA address derivation (PDA: Program Derived Address)
 *--------------------------------------------------------------------------*/
BoatResult boat_sol_ata_address(const uint8_t wallet[32], const uint8_t mint[32], uint8_t ata[32])
{
    if (!wallet || !mint || !ata) return BOAT_ERROR_ARG_NULL;

    /*
     * ATA = findProgramAddress(
     *   [wallet, TOKEN_PROGRAM_ID, mint],
     *   ATA_PROGRAM_ID
     * )
     *
     * findProgramAddress tries bump seeds 255..0:
     *   candidate = SHA256(seed0 || seed1 || seed2 || bump || program_id)
     *   if candidate is NOT on ed25519 curve, return it
     */

    for (int bump = 255; bump >= 0; bump--) {
        DEFAULT_SHA256_CTX ctx;
        sha256_Init(&ctx);
        sha256_Update(&ctx, wallet, 32);
        sha256_Update(&ctx, BOAT_SOL_TOKEN_PROGRAM_ID, 32);
        sha256_Update(&ctx, mint, 32);
        uint8_t bump_byte = (uint8_t)bump;
        sha256_Update(&ctx, &bump_byte, 1);
        sha256_Update(&ctx, BOAT_SOL_ATA_PROGRAM_ID, 32);
        /* "ProgramDerivedAddress" suffix */
        const char *pda_marker = "ProgramDerivedAddress";
        sha256_Update(&ctx, (const uint8_t *)pda_marker, 21);

        uint8_t hash[32];
        sha256_Final(&ctx, hash);

        /* PDA must NOT be on the ed25519 curve */
        if (!ed25519_point_is_on_curve(hash)) {
            memcpy(ata, hash, 32);
            return BOAT_SUCCESS;
        }
    }

    return BOAT_ERROR;
}

/*----------------------------------------------------------------------------
 * Generic PDA derivation: findProgramAddress(seeds, program_id)
 *--------------------------------------------------------------------------*/
BoatResult boat_sol_find_pda(const uint8_t *seeds[], const size_t seed_lens[],
                             size_t num_seeds, const uint8_t program_id[32],
                             uint8_t pda[32])
{
    if (!seeds || !seed_lens || !program_id || !pda) return BOAT_ERROR_ARG_NULL;

    const char *pda_marker = "ProgramDerivedAddress";

    for (int bump = 255; bump >= 0; bump--) {
        DEFAULT_SHA256_CTX ctx;
        sha256_Init(&ctx);
        for (size_t i = 0; i < num_seeds; i++) {
            sha256_Update(&ctx, seeds[i], seed_lens[i]);
        }
        uint8_t bump_byte = (uint8_t)bump;
        sha256_Update(&ctx, &bump_byte, 1);
        sha256_Update(&ctx, program_id, 32);
        sha256_Update(&ctx, (const uint8_t *)pda_marker, 21);

        uint8_t hash[32];
        sha256_Final(&ctx, hash);

        /* PDA must NOT be on the ed25519 curve */
        if (!ed25519_point_is_on_curve(hash)) {
            memcpy(pda, hash, 32);
            return BOAT_SUCCESS;
        }
    }

    return BOAT_ERROR;
}

/*----------------------------------------------------------------------------
 * SPL Token Transfer instruction
 *--------------------------------------------------------------------------*/
BoatResult boat_sol_spl_transfer(const uint8_t from_ata[32], const uint8_t to_ata[32],
                                 const uint8_t owner[32], uint64_t amount,
                                 BoatSolInstruction *ix)
{
    if (!from_ata || !to_ata || !owner || !ix) return BOAT_ERROR_ARG_NULL;

    boat_sol_ix_init(ix, BOAT_SOL_TOKEN_PROGRAM_ID);
    boat_sol_ix_add_account(ix, from_ata, false, true);   /* source (writable) */
    boat_sol_ix_add_account(ix, to_ata, false, true);     /* destination (writable) */
    boat_sol_ix_add_account(ix, owner, true, false);      /* owner (signer) */

    /* SPL Token Transfer instruction: [3 (u8 discriminator), amount (u64 LE)] */
    uint8_t data[9];
    data[0] = 3; /* Transfer instruction index */
    for (int i = 0; i < 8; i++) {
        data[1 + i] = (uint8_t)(amount & 0xFF);
        amount >>= 8;
    }
    boat_sol_ix_set_data(ix, data, 9);

    return BOAT_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Create Associated Token Account instruction
 *--------------------------------------------------------------------------*/
BoatResult boat_sol_spl_create_ata(const uint8_t payer[32], const uint8_t wallet[32],
                                   const uint8_t mint[32], BoatSolInstruction *ix)
{
    if (!payer || !wallet || !mint || !ix) return BOAT_ERROR_ARG_NULL;

    /* Derive ATA address */
    uint8_t ata[32];
    BoatResult r = boat_sol_ata_address(wallet, mint, ata);
    if (r != BOAT_SUCCESS) return r;

    boat_sol_ix_init(ix, BOAT_SOL_ATA_PROGRAM_ID);
    boat_sol_ix_add_account(ix, payer, true, true);                    /* payer (signer, writable) */
    boat_sol_ix_add_account(ix, ata, false, true);                     /* ATA (writable) */
    boat_sol_ix_add_account(ix, wallet, false, false);                 /* wallet owner */
    boat_sol_ix_add_account(ix, mint, false, false);                   /* mint */
    boat_sol_ix_add_account(ix, BOAT_SOL_SYSTEM_PROGRAM_ID, false, false);  /* system program */
    boat_sol_ix_add_account(ix, BOAT_SOL_TOKEN_PROGRAM_ID, false, false);   /* token program */

    /* CreateAssociatedTokenAccount has no instruction data */
    return BOAT_SUCCESS;
}

BoatResult boat_sol_spl_create_ata_idempotent(const uint8_t payer[32], const uint8_t wallet[32],
                                               const uint8_t mint[32], BoatSolInstruction *ix)
{
    if (!payer || !wallet || !mint || !ix) return BOAT_ERROR_ARG_NULL;

    uint8_t ata[32];
    BoatResult r = boat_sol_ata_address(wallet, mint, ata);
    if (r != BOAT_SUCCESS) return r;

    boat_sol_ix_init(ix, BOAT_SOL_ATA_PROGRAM_ID);
    boat_sol_ix_add_account(ix, payer, true, true);
    boat_sol_ix_add_account(ix, ata, false, true);
    boat_sol_ix_add_account(ix, wallet, false, false);
    boat_sol_ix_add_account(ix, mint, false, false);
    boat_sol_ix_add_account(ix, BOAT_SOL_SYSTEM_PROGRAM_ID, false, false);
    boat_sol_ix_add_account(ix, BOAT_SOL_TOKEN_PROGRAM_ID, false, false);

    /* Idempotent variant: instruction data = [1] */
    static const uint8_t data[1] = { 1 };
    boat_sol_ix_set_data(ix, data, 1);
    return BOAT_SUCCESS;
}

#endif /* BOAT_SOL_ENABLED */

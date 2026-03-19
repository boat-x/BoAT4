/******************************************************************************
 * Copyright (C) TLAY.IO — BoAT v4 SDK
 * SPDX-License-Identifier: Apache-2.0
 *****************************************************************************/
#include "boat_sol.h"
#include <string.h>

#if BOAT_SOL_ENABLED

BoatResult boat_sol_ix_init(BoatSolInstruction *ix, const uint8_t program_id[32])
{
    if (!ix || !program_id) return BOAT_ERROR_ARG_NULL;
    memset(ix, 0, sizeof(BoatSolInstruction));
    memcpy(ix->program_id, program_id, 32);
    return BOAT_SUCCESS;
}

BoatResult boat_sol_ix_add_account(BoatSolInstruction *ix, const uint8_t pubkey[32],
                                   bool is_signer, bool is_writable)
{
    if (!ix || !pubkey) return BOAT_ERROR_ARG_NULL;
    if (ix->num_accounts >= BOAT_SOL_MAX_IX_ACCOUNTS) return BOAT_ERROR_MEM_OVERFLOW;
    memcpy(ix->accounts[ix->num_accounts].pubkey, pubkey, 32);
    ix->accounts[ix->num_accounts].is_signer = is_signer;
    ix->accounts[ix->num_accounts].is_writable = is_writable;
    ix->num_accounts++;
    return BOAT_SUCCESS;
}

BoatResult boat_sol_ix_set_data(BoatSolInstruction *ix, const uint8_t *data, size_t len)
{
    if (!ix) return BOAT_ERROR_ARG_NULL;
    if (len > BOAT_SOL_MAX_IX_DATA) return BOAT_ERROR_MEM_OVERFLOW;
    if (data && len > 0) {
        memcpy(ix->data, data, len);
    }
    ix->data_len = len;
    return BOAT_SUCCESS;
}

#endif /* BOAT_SOL_ENABLED */

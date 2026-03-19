/******************************************************************************
 * BoAT v4 Example: Send SPL token on Solana
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"
#include "boat_key.h"
#include "boat_sol.h"

#include <stdio.h>
#include <string.h>

static const char *PRIVATE_KEY = "your_ed25519_private_key_base58";
static const char *RPC_URL = "https://api.devnet.solana.com";

/* SPL Token mint address — replace with actual mint (base58) */
static const char *TOKEN_MINT_B58 = "11111111111111111111111111111112";

/* Recipient wallet — replace with actual recipient (base58) */
static const char *RECIPIENT_B58 = "DeadBeef111111111111111111111111111111111111";

int main(void)
{
    boat_pal_linux_init();

    /* Import ed25519 key (accepts base58, JSON array, or hex) */
    BoatKey *key = boat_key_import_string(BOAT_KEY_TYPE_ED25519, PRIVATE_KEY);
    if (!key) { printf("Key import failed\n"); return -1; }

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    char addr_str[64];
    boat_address_to_string(&info, addr_str, sizeof(addr_str));
    printf("Sender: %s\n", addr_str);

    /* Init RPC */
    BoatSolRpc rpc;
    boat_sol_rpc_init(&rpc, RPC_URL);

    /* Decode addresses from base58 */
    uint8_t token_mint[32], recipient_wallet[32];
    size_t dec_len;
    boat_address_from_string(TOKEN_MINT_B58, token_mint, sizeof(token_mint), &dec_len);
    boat_address_from_string(RECIPIENT_B58, recipient_wallet, sizeof(recipient_wallet), &dec_len);

    /* Compute ATA addresses */
    uint8_t sender_ata[32], recipient_ata[32];
    boat_sol_ata_address(info.address, token_mint, sender_ata);
    boat_sol_ata_address(recipient_wallet, token_mint, recipient_ata);

    /* Get recent blockhash */
    uint8_t blockhash[32];
    uint64_t last_valid;
    BoatResult r = boat_sol_rpc_get_latest_blockhash(&rpc, BOAT_SOL_COMMITMENT_FINALIZED,
                                                      blockhash, &last_valid);
    if (r != BOAT_SUCCESS) {
        printf("Failed to get blockhash: %d\n", r);
        boat_key_free(key);
        return -1;
    }

    /* Build SPL transfer instruction */
    BoatSolInstruction ix;
    uint64_t amount;
    boat_amount_to_uint64(1.0, 6, &amount); /* 1 token with 6 decimals */
    boat_sol_spl_transfer(sender_ata, recipient_ata, info.address, amount, &ix);

    /* Build transaction */
    BoatSolTx tx;
    boat_sol_tx_init(&tx);
    boat_sol_tx_set_fee_payer(&tx, info.address);
    boat_sol_tx_set_blockhash(&tx, blockhash);
    boat_sol_tx_add_instruction(&tx, &ix);

    /* Sign and send */
    uint8_t sig[64];
    r = boat_sol_tx_send(&tx, key, &rpc, sig);
    if (r == BOAT_SUCCESS) {
        printf("TX signature (first 16 bytes): ");
        for (int i = 0; i < 16; i++) printf("%02x", sig[i]);
        printf("...\n");
    } else {
        printf("TX failed: %d\n", r);
    }

    boat_key_free(key);
    return (r == BOAT_SUCCESS) ? 0 : -1;
}

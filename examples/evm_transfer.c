/******************************************************************************
 * BoAT v4 Example: Send ETH on Base
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"
#include "boat_key.h"
#include "boat_evm.h"

#include <stdio.h>
#include <string.h>

/* Replace with your private key (hex, base58, or JSON array) */
static const char *PRIVATE_KEY = "your_private_key_here";

/* Replace with your RPC endpoint */
static const char *RPC_URL = "https://sepolia.base.org";

/* Recipient address */
static const char *RECIPIENT_ADDR = "0xDeadBeef000000000000000000000000000000001";

int main(void)
{
    /* Initialize PAL */
    boat_pal_linux_init();

    /* Import private key (auto-detects format) */
    BoatKey *key = boat_key_import_string(BOAT_KEY_TYPE_SECP256K1, PRIVATE_KEY);
    if (!key) {
        printf("Failed to import key\n");
        return -1;
    }

    /* Print address */
    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    char addr_str[43];
    boat_address_to_string(&info, addr_str, sizeof(addr_str));
    printf("Sender: %s\n", addr_str);

    /* Decode recipient address */
    uint8_t recipient[20];
    size_t rec_len;
    boat_address_from_string(RECIPIENT_ADDR, recipient, sizeof(recipient), &rec_len);

    /* Init RPC */
    BoatEvmRpc rpc;
    boat_evm_rpc_init(&rpc, RPC_URL);

    /* Build transaction: send 0.001 ETH */
    BoatEvmChainConfig chain = { .chain_id = 84532, .eip1559 = false };
    strncpy(chain.rpc_url, RPC_URL, sizeof(chain.rpc_url) - 1);

    BoatEvmTx tx;
    boat_evm_tx_init(&tx, &chain);
    boat_evm_tx_set_to(&tx, recipient);

    uint8_t value[32];
    boat_amount_to_uint256(0.001, 18, value); /* 0.001 ETH, 18 decimals */
    boat_evm_tx_set_value(&tx, value);
    boat_evm_tx_set_gas_limit(&tx, 21000);

    /* Auto-fill nonce and gas price from chain */
    BoatResult r = boat_evm_tx_auto_fill(&tx, &rpc, key);
    if (r != BOAT_SUCCESS) {
        printf("Auto-fill failed: %d\n", r);
        boat_key_free(key);
        return -1;
    }

    /* Send */
    uint8_t txhash[32];
    r = boat_evm_tx_send(&tx, key, &rpc, txhash);
    if (r == BOAT_SUCCESS) {
        char hash_hex[67];
        boat_bin_to_hex(txhash, 32, hash_hex, sizeof(hash_hex), true);
        printf("TX sent: %s\n", hash_hex);
    } else {
        printf("TX failed: %d\n", r);
    }

    /* Cleanup */
    if (tx.data) boat_free(tx.data);
    boat_key_free(key);
    return (r == BOAT_SUCCESS) ? 0 : -1;
}

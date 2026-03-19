/******************************************************************************
 * BoAT v4 Example: ERC-20 transferFrom using generated codec
 *
 * Demonstrates the full abi2c workflow:
 *   1. ABI JSON → abi2c.py → erc20_codec.h / erc20_codec.c
 *   2. encode_allowance() → eth_call → decode_allowance() to check allowance
 *   3. encode_transferFrom() → build tx → send to execute transferFrom
 *
 * The generated codec eliminates manual ABI slot management.
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"
#include "boat_key.h"
#include "boat_evm.h"
#include "erc20_codec.h"

#include <stdio.h>
#include <string.h>

/* Replace with your test credentials */
static const char *PRIVATE_KEY = "your_private_key_here";
static const char *RPC_URL     = "https://sepolia.base.org";

/* USDC on Base Sepolia (replace with actual address) */
static const char *USDC_CONTRACT_ADDR = "0x036CbD53842c5426fB2E64C12e076b1300D3a52";

/* The token owner who has approved us to spend */
static const char *OWNER_ADDR     = "0x1111111111111111111111111111111111111111";
/* The recipient of the transfer */
static const char *RECIPIENT_ADDR = "0x2222222222222222222222222222222222222222";

int main(void)
{
    boat_pal_linux_init();

    /* --- Import key and derive our address --- */
    BoatKey *key = boat_key_import_string(BOAT_KEY_TYPE_SECP256K1, PRIVATE_KEY);
    if (!key) { printf("Key import failed\n"); return -1; }

    BoatKeyInfo info;
    boat_key_get_info(key, &info);

    /* --- Parse addresses --- */
    uint8_t usdc_contract[20], owner[20], recipient[20];
    size_t dec_len;
    boat_address_from_string(USDC_CONTRACT_ADDR, usdc_contract, sizeof(usdc_contract), &dec_len);
    boat_address_from_string(OWNER_ADDR, owner, sizeof(owner), &dec_len);
    boat_address_from_string(RECIPIENT_ADDR, recipient, sizeof(recipient), &dec_len);

    /* --- Init RPC --- */
    BoatEvmRpc rpc;
    boat_evm_rpc_init(&rpc, RPC_URL);

    /* =======================================================================
     * Step 1: Check allowance(owner, spender) via eth_call
     * The "spender" is us (info.address).
     * =====================================================================*/
    printf("Checking allowance...\n");

    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    BoatResult r = encode_allowance(owner, info.address, &calldata, &calldata_len);
    if (r != BOAT_SUCCESS) {
        printf("encode_allowance failed: %d\n", r);
        boat_key_free(key);
        return -1;
    }

    uint8_t *result = NULL;
    size_t result_len = 0;
    r = boat_evm_eth_call(&rpc, usdc_contract, calldata, calldata_len, &result, &result_len);
    boat_evm_abi_free(calldata);

    if (r != BOAT_SUCCESS || result_len < 32) {
        printf("allowance eth_call failed: %d\n", r);
        if (result) boat_free(result);
        boat_key_free(key);
        return -1;
    }

    uint8_t allowance_val[32];
    decode_allowance(result, result_len, allowance_val);
    boat_free(result);

    char hex_buf[67];
    boat_bin_to_hex(allowance_val, 32, hex_buf, sizeof(hex_buf), true);
    printf("Allowance: %s\n", hex_buf);

    /* Check if allowance is zero (all bytes zero) */
    bool is_zero = true;
    for (int i = 0; i < 32; i++) {
        if (allowance_val[i] != 0) { is_zero = false; break; }
    }
    if (is_zero) {
        printf("Allowance is zero — owner must approve us first.\n");
        boat_key_free(key);
        return -1;
    }

    /* =======================================================================
     * Step 2: Execute transferFrom(owner, recipient, amount)
     * =====================================================================*/
    printf("Sending transferFrom...\n");

    /* 1 USDC = 1_000_000 base units (6 decimals) */
    uint8_t amount[32];
    boat_amount_to_uint256(1.0, 6, amount);

    r = encode_transferFrom(owner, recipient, amount, &calldata, &calldata_len);
    if (r != BOAT_SUCCESS) {
        printf("encode_transferFrom failed: %d\n", r);
        boat_key_free(key);
        return -1;
    }

    /* Build and send transaction */
    BoatEvmChainConfig chain = { .chain_id = 84532, .eip1559 = false };
    strncpy(chain.rpc_url, RPC_URL, sizeof(chain.rpc_url) - 1);

    BoatEvmTx tx;
    boat_evm_tx_init(&tx, &chain);
    boat_evm_tx_set_to(&tx, usdc_contract);
    boat_evm_tx_set_data(&tx, calldata, calldata_len);
    boat_evm_abi_free(calldata);
    boat_evm_tx_set_gas_limit(&tx, 80000);
    boat_evm_tx_auto_fill(&tx, &rpc, key);

    uint8_t txhash[32];
    r = boat_evm_tx_send(&tx, key, &rpc, txhash);
    if (r == BOAT_SUCCESS) {
        boat_bin_to_hex(txhash, 32, hex_buf, sizeof(hex_buf), true);
        printf("transferFrom TX: %s\n", hex_buf);
    } else {
        printf("transferFrom failed: %d\n", r);
    }

    if (tx.data) boat_free(tx.data);
    boat_key_free(key);
    return (r == BOAT_SUCCESS) ? 0 : -1;
}

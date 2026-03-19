/******************************************************************************
 * BoAT v4 Example: ERC20 balanceOf + transfer on Base
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"
#include "boat_key.h"
#include "boat_evm.h"

#include <stdio.h>
#include <string.h>

static const char *PRIVATE_KEY = "your_private_key_here";
static const char *RPC_URL = "https://sepolia.base.org";

/* USDC on Base Sepolia */
static const char *USDC_CONTRACT_ADDR = "0x036CbD53842c5426fB2E64C12e076b1300D3a52";

static const char *RECIPIENT_ADDR = "0xDeadBeef000000000000000000000000000000001";

int main(void)
{
    boat_pal_linux_init();

    BoatKey *key = boat_key_import_string(BOAT_KEY_TYPE_SECP256K1, PRIVATE_KEY);
    if (!key) { printf("Key import failed\n"); return -1; }

    BoatKeyInfo info;
    boat_key_get_info(key, &info);

    /* Decode contract and recipient addresses */
    uint8_t usdc_contract[20], recipient[20];
    size_t dec_len;
    boat_address_from_string(USDC_CONTRACT_ADDR, usdc_contract, sizeof(usdc_contract), &dec_len);
    boat_address_from_string(RECIPIENT_ADDR, recipient, sizeof(recipient), &dec_len);

    BoatEvmRpc rpc;
    boat_evm_rpc_init(&rpc, RPC_URL);

    /* --- balanceOf(address) --- */
    printf("Querying USDC balance...\n");
    uint8_t addr_slot[32];
    boat_evm_abi_encode_address(info.address, addr_slot);

    const uint8_t *args[1] = { addr_slot };
    size_t arg_lens[1] = { 32 };
    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    BoatResult r = boat_evm_abi_encode_func("balanceOf(address)", args, arg_lens, 1,
                                            &calldata, &calldata_len);
    if (r != BOAT_SUCCESS) { printf("ABI encode failed\n"); boat_key_free(key); return -1; }

    uint8_t *result = NULL;
    size_t result_len = 0;
    r = boat_evm_eth_call(&rpc, usdc_contract, calldata, calldata_len, &result, &result_len);
    boat_evm_abi_free(calldata);

    if (r == BOAT_SUCCESS && result_len >= 32) {
        uint8_t balance[32];
        boat_evm_abi_decode_uint256(result, 0, balance);
        char bal_hex[67];
        boat_bin_to_hex(balance, 32, bal_hex, sizeof(bal_hex), true);
        printf("USDC balance: %s\n", bal_hex);
        boat_free(result);
    } else {
        printf("balanceOf failed: %d\n", r);
    }

    /* --- transfer(address, uint256) --- */
    printf("Sending USDC transfer...\n");

    uint8_t to_slot[32], amount_slot[32];
    boat_evm_abi_encode_address(recipient, to_slot);
    /* 1 USDC (6 decimals) */
    uint64_t usdc_amount;
    boat_amount_to_uint64(1.0, 6, &usdc_amount);
    boat_evm_abi_encode_uint64(usdc_amount, amount_slot);

    const uint8_t *tx_args[2] = { to_slot, amount_slot };
    size_t tx_arg_lens[2] = { 32, 32 };
    uint8_t *tx_calldata = NULL;
    size_t tx_calldata_len = 0;
    r = boat_evm_abi_encode_func("transfer(address,uint256)", tx_args, tx_arg_lens, 2,
                                 &tx_calldata, &tx_calldata_len);
    if (r != BOAT_SUCCESS) { printf("ABI encode failed\n"); boat_key_free(key); return -1; }

    BoatEvmChainConfig chain = { .chain_id = 84532, .eip1559 = false };
    strncpy(chain.rpc_url, RPC_URL, sizeof(chain.rpc_url) - 1);

    BoatEvmTx tx;
    boat_evm_tx_init(&tx, &chain);
    boat_evm_tx_set_to(&tx, usdc_contract);
    boat_evm_tx_set_data(&tx, tx_calldata, tx_calldata_len);
    boat_evm_abi_free(tx_calldata);
    boat_evm_tx_set_gas_limit(&tx, 60000);
    boat_evm_tx_auto_fill(&tx, &rpc, key);

    uint8_t txhash[32];
    r = boat_evm_tx_send(&tx, key, &rpc, txhash);
    if (r == BOAT_SUCCESS) {
        char hash_hex[67];
        boat_bin_to_hex(txhash, 32, hash_hex, sizeof(hash_hex), true);
        printf("Transfer TX: %s\n", hash_hex);
    } else {
        printf("Transfer failed: %d\n", r);
    }

    if (tx.data) boat_free(tx.data);
    boat_key_free(key);
    return (r == BOAT_SUCCESS) ? 0 : -1;
}

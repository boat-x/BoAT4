/******************************************************************************
 * BoAT v4 Example: MPP Tempo Charge — pay-per-request via Machine Payments Protocol
 *
 * Demonstrates the full MPP client flow:
 *   1. Send HTTP GET to an MPP-enabled endpoint
 *   2. Receive HTTP 402 with WWW-Authenticate: Payment challenge
 *   3. Execute TIP-20 token transfer on Tempo blockchain
 *   4. Retry request with Authorization: Payment credential
 *   5. Receive 200 OK with resource + Payment-Receipt
 *
 * Prerequisites:
 *   - Private key with pathUSD balance on Tempo Moderato testnet
 *   - Faucet: https://faucet.tempo.xyz
 *   - An MPP server endpoint (e.g., from stripe-samples/machine-payments)
 *****************************************************************************/
#include "boat.h"
#include "boat_pal.h"
#include "boat_key.h"
#include "boat_evm.h"
#include "boat_pay.h"

#include <stdio.h>
#include <string.h>

#if BOAT_PAY_MPP_ENABLED

/* Replace with your own private key (Tempo Moderato testnet) */
static const char *PRIVATE_KEY = "your_private_key_here";

/* Replace with the MPP endpoint URL you want to access */
static const char *MPP_ENDPOINT = "https://mpp.dev/api/ping/paid";

/* Tempo Moderato testnet RPC */
static const char *TEMPO_RPC_URL = "https://rpc.testnet.tempo.xyz";

int main(void)
{
    printf("=== BoAT v4 MPP Tempo Charge Demo ===\n\n");

    /* Initialize platform */
    boat_pal_linux_init();

    /* Import key */
    BoatKey *key = boat_key_import_string(BOAT_KEY_TYPE_SECP256K1, PRIVATE_KEY);
    if (!key) {
        printf("ERROR: Key import failed\n");
        return -1;
    }

    BoatKeyInfo info;
    boat_key_get_info(key, &info);
    char addr_hex[43];
    boat_bin_to_hex(info.address, 20, addr_hex, sizeof(addr_hex), true);
    printf("Payer address: %s\n", addr_hex);

    /* Configure Tempo testnet */
    BoatMppTempoConfig config;
    memset(&config, 0, sizeof(config));
    config.chain.chain_id = BOAT_MPP_TEMPO_TESTNET_CHAIN_ID;
    strncpy(config.chain.rpc_url, TEMPO_RPC_URL, sizeof(config.chain.rpc_url) - 1);
    config.chain.eip1559 = false;
    memcpy(config.token_addr, BOAT_MPP_TEMPO_PATHUSD_TESTNET, 20);
    strncpy(config.rpc_url, TEMPO_RPC_URL, sizeof(config.rpc_url) - 1);

    printf("Target: %s\n", MPP_ENDPOINT);
    printf("Chain:  Tempo Moderato (chain_id=%llu)\n\n",
           (unsigned long long)config.chain.chain_id);

    /* Execute full MPP flow */
    uint8_t *response = NULL;
    size_t response_len = 0;
    BoatMppReceipt receipt;
    memset(&receipt, 0, sizeof(receipt));

    BoatResult r = boat_mpp_tempo_process(
        MPP_ENDPOINT, NULL, key, &config,
        &response, &response_len, &receipt);

    if (r == BOAT_SUCCESS) {
        printf("SUCCESS! Resource received.\n");
        printf("Response (%zu bytes): %.*s\n",
               response_len, (int)(response_len < 200 ? response_len : 200), response);
        if (receipt.status[0]) {
            printf("\nPayment Receipt:\n");
            printf("  Status:    %s\n", receipt.status);
            printf("  Method:    %s\n", receipt.method);
            printf("  Reference: %s\n", receipt.reference);
            printf("  Timestamp: %s\n", receipt.timestamp);
        }
        boat_free(response);
    } else {
        printf("FAILED: error %d\n", r);
        if (response) {
            printf("Server response: %.*s\n",
                   (int)(response_len < 512 ? response_len : 512), response);
            boat_free(response);
        }
    }

    boat_key_free(key);
    return (r == BOAT_SUCCESS) ? 0 : -1;
}

#else
int main(void)
{
    printf("MPP not enabled. Rebuild with -DBOAT_PAY_MPP=ON\n");
    return 0;
}
#endif

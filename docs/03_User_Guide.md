# BoAT v4 User Guide

**Document:** User Guide
**Project:** BoAT v4 SDK
**Author:** TLAY.io
**Date:** March 2026

---

## 1. Introduction

BoAT v4 is a lightweight C-language blockchain SDK for IoT devices. It lets your device:

- Send transactions on EVM chains (Ethereum, Base, Polygon, Arbitrum, etc.)
- Transfer SPL tokens on Solana
- Pay for HTTP resources using the x402 protocol
- Make micropayments via Circle Nanopayments
- Manage USDC across chains via Circle Gateway

This guide walks you through building, integrating, and using BoAT v4.

---

## 2. Getting Started

### 2.1 Prerequisites

- C compiler with C11 support (GCC, Clang, or vendor toolchain)
- libcurl development headers (Linux PAL only)
- CMake 3.12+ (if using CMake build) or GNU Make
- Python 3 (for abi2c.py code generator, optional)

### 2.2 Directory Layout

    BoAT4/
      include/          Public headers -- add to your include path
      src/              Source files -- add to your build
      third-party/      trezor-crypto and cJSON -- add to your build
      tools/            abi2c.py code generator
      examples/         Example programs
      CMakeLists.txt    CMake build
      boat.mk           GNU Make fragment
      sources.txt       Flat source list
      Kconfig           Menuconfig entries

### 2.3 Quick Build (Linux Standalone)

    mkdir build && cd build
    cmake .. -DBOAT_PAL=linux
    make

This produces libboat4.a. To also build examples:

    cmake .. -DBOAT_PAL=linux -DBOAT_BUILD_EXAMPLES=ON
    make

### 2.4 Build Options

| Option | Default | Description |
|--------|---------|-------------|
| BOAT_EVM | ON | Enable EVM chain support |
| BOAT_SOL | ON | Enable Solana support |
| BOAT_PAY_X402 | OFF | Enable x402 payment protocol |
| BOAT_PAY_NANO | OFF | Enable Circle Nanopayments |
| BOAT_PAY_GATEWAY | OFF | Enable Circle Gateway |
| BOAT_PAL | linux | Platform abstraction (linux or custom) |
| BOAT_BUILD_EXAMPLES | OFF | Build example programs |

Example: build with only EVM and x402, no Solana:

    cmake .. -DBOAT_PAL=linux -DBOAT_EVM=ON -DBOAT_SOL=OFF -DBOAT_PAY_X402=ON

---

## 3. Integration with Your Build System

### 3.1 CMake (ESP-IDF, Zephyr, STM32CubeIDE)

Add BoAT4 as a subdirectory in your CMakeLists.txt:

    add_subdirectory(path/to/BoAT4)
    target_link_libraries(your_app boat4)

For ESP-IDF, place BoAT4 in your components/ directory. The CMakeLists.txt auto-detects
ESP_PLATFORM and uses idf_component_register().

### 3.2 GNU Make

In your Makefile:

    BOAT_EVM := 1
    BOAT_SOL := 1
    BOAT_PAL_LINUX := 1
    include path/to/BoAT4/boat.mk

    your_app: your_app.c $(BOAT_SRCS)
        $(CC) $(BOAT_CFLAGS) $(BOAT_INCS) -o $@ $^ -lcurl -lpthread

### 3.3 Vendor Toolchain (raw file list)

Read sources.txt and add the relevant .c files to your build. Lines starting with [TAG]
are conditional -- include them only if you need that feature:

    [EVM] src/evm/evm_rpc.c      -- include if you need EVM support
    [SOL] src/sol/sol_rpc.c      -- include if you need Solana support
    [PAY_X402] src/pay/pay_x402.c -- include if you need x402

Add these to your compiler include path:
- BoAT4/include
- BoAT4/third-party/crypto
- BoAT4/third-party/cJSON

### 3.4 Kconfig (ESP-IDF / Zephyr)

Copy or symlink BoAT4/Kconfig into your project Kconfig tree. Then use menuconfig to
enable/disable chains and payment protocols.

---

## 4. Initialization

Every BoAT v4 application starts with PAL initialization:

    #include "boat.h"
    #include "boat_pal.h"

    int main(void)
    {
        // Initialize platform (registers HTTP ops, inits curl)
        boat_pal_linux_init();

        // ... your application code ...

        return 0;
    }

On custom platforms, implement the Tier 1 PAL functions and register your HTTP ops:

    // Your custom HTTP implementation
    static BoatHttpOps my_http_ops = {
        .post = my_http_post,
        .get = my_http_get,
        .free_response = my_http_free
    };

    void my_platform_init(void)
    {
        boat_set_http_ops(&my_http_ops);
    }

---

## 5. Key Management

### 5.1 Generate a New Key

    #include "boat_key.h"

    // Generate secp256k1 key (for EVM chains)
    BoatKey *key = boat_key_generate(BOAT_KEY_TYPE_SECP256K1);

    // Generate ed25519 key (for Solana)
    BoatKey *key = boat_key_generate(BOAT_KEY_TYPE_ED25519);

### 5.2 Import an Existing Key

    // From raw 32-byte private key
    uint8_t privkey[32];
    boat_hex_to_bin("your_hex_private_key", privkey, 32, NULL);
    BoatKey *key = boat_key_import_raw(BOAT_KEY_TYPE_SECP256K1, privkey, 32);

### 5.3 Get Address

    BoatKeyInfo info;
    boat_key_get_info(key, &info);

    // EVM: info.address is 20 bytes
    char addr_hex[43];
    boat_bin_to_hex(info.address, 20, addr_hex, sizeof(addr_hex), true);
    printf("Address: %s\n", addr_hex);  // 0x...

    // Solana: info.address is 32 bytes (= public key)

### 5.4 Save and Load Keys

    // Save to persistent storage
    boat_key_save(key, "my_device_key");

    // Load later
    BoatKey *loaded = boat_key_load("my_device_key", BOAT_KEY_TYPE_SECP256K1);

    // Delete from storage
    boat_key_delete("my_device_key");

### 5.5 Cleanup

Always free keys when done:

    boat_key_free(key);  // zeroes and frees key material

---

## 6. EVM Chains (Ethereum, Base, Polygon, etc.)

### 6.1 Configure Chain

    BoatEvmChainConfig chain;
    chain.chain_id = 8453;       // Base mainnet
    chain.eip1559 = false;
    strncpy(chain.rpc_url, "https://mainnet.base.org", sizeof(chain.rpc_url) - 1);

### 6.2 Initialize RPC

    BoatEvmRpc rpc;
    boat_evm_rpc_init(&rpc, chain.rpc_url);

### 6.3 Query Chain Data

    // Get block number
    uint64_t block;
    boat_evm_block_number(&rpc, &block);

    // Get balance
    uint8_t balance[32];
    boat_evm_get_balance(&rpc, info.address, balance);

    // Get nonce
    uint64_t nonce;
    boat_evm_get_nonce(&rpc, info.address, &nonce);

### 6.4 Send ETH/Native Token

    BoatEvmTx tx;
    boat_evm_tx_init(&tx, &chain);
    boat_evm_tx_set_to(&tx, recipient_address);    // 20-byte address
    boat_evm_tx_set_value(&tx, amount_wei);         // 32-byte uint256
    boat_evm_tx_set_gas_limit(&tx, 21000);
    boat_evm_tx_auto_fill(&tx, &rpc, key);          // fills nonce + gas price

    uint8_t txhash[32];
    boat_evm_tx_send(&tx, key, &rpc, txhash);

    // Cleanup
    if (tx.data) boat_free(tx.data);

### 6.5 Call Smart Contract (Read)

    // Encode balanceOf(address)
    uint8_t addr_slot[32];
    boat_evm_abi_encode_address(my_address, addr_slot);

    const uint8_t *args[1] = { addr_slot };
    size_t arg_lens[1] = { 32 };
    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    boat_evm_abi_encode_func("balanceOf(address)", args, arg_lens, 1,
                             &calldata, &calldata_len);

    // Call
    uint8_t *result = NULL;
    size_t result_len = 0;
    boat_evm_eth_call(&rpc, contract_address, calldata, calldata_len,
                      &result, &result_len);

    // Decode result
    uint8_t balance[32];
    boat_evm_abi_decode_uint256(result, 0, balance);

    // Cleanup
    boat_evm_abi_free(calldata);
    boat_free(result);

### 6.6 Call Smart Contract (Write / Transaction)

    // Encode transfer(address,uint256)
    uint8_t to_slot[32], amount_slot[32];
    boat_evm_abi_encode_address(recipient, to_slot);
    boat_evm_abi_encode_uint64(1000000, amount_slot);  // 1 USDC (6 decimals)

    const uint8_t *args[2] = { to_slot, amount_slot };
    size_t arg_lens[2] = { 32, 32 };
    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    boat_evm_abi_encode_func("transfer(address,uint256)", args, arg_lens, 2,
                             &calldata, &calldata_len);

    // Build and send transaction
    BoatEvmTx tx;
    boat_evm_tx_init(&tx, &chain);
    boat_evm_tx_set_to(&tx, usdc_contract);
    boat_evm_tx_set_data(&tx, calldata, calldata_len);
    boat_evm_abi_free(calldata);
    boat_evm_tx_set_gas_limit(&tx, 60000);
    boat_evm_tx_auto_fill(&tx, &rpc, key);

    uint8_t txhash[32];
    boat_evm_tx_send(&tx, key, &rpc, txhash);
    if (tx.data) boat_free(tx.data);

### 6.7 Using abi2c.py Code Generator

For contracts with many methods, generate C codec functions from ABI JSON:

    python3 tools/abi2c.py MyContract.abi.json -o my_contract_codec.c

This generates both `my_contract_codec.c` and `my_contract_codec.h` containing
encode_methodName() and decode_methodName() functions that call the
boat_evm_abi_encode_* / boat_evm_abi_decode_* primitives. Include the generated
files in your build and call the generated functions directly.

**Full workflow:**

1. Create or obtain the ABI JSON for your contract (e.g. from Solidity compiler output)
2. Run the generator:

       python3 tools/abi2c.py erc20_abi.json -o erc20_codec.c

3. This produces `erc20_codec.h` (declarations + include guard) and `erc20_codec.c`
   (implementations that include the header)
4. Add both files to your build (see CMakeLists.txt example below)
5. Include the header and call the generated functions:

       #include "erc20_codec.h"

       uint8_t *calldata;
       size_t calldata_len;
       encode_transferFrom(from, to, amount, &calldata, &calldata_len);

**Supported types:** address, bool, uint256 (and all uintN), bytes32, bytes, string.

For functions with only static types (address, bool, uintN, bytes32), the generated
encoder uses `boat_evm_abi_encode_func()`. For functions containing dynamic types
(bytes, string), the encoder builds calldata manually with proper head/tail encoding
per the ABI specification.

**Build integration (CMake):**

    add_executable(my_app
        my_app.c
        my_contract_codec.c)
    target_link_libraries(my_app boat4)

Generated files should be committed to your repository rather than regenerated at
build time, so the build does not depend on Python.

See `examples/evm_erc20_transferfrom.c` for a complete example using the generated
ERC-20 codec to check allowance and execute transferFrom.

---

## 7. Solana

### 7.1 Configure and Initialize

    BoatSolRpc rpc;
    boat_sol_rpc_init(&rpc, "https://api.mainnet-beta.solana.com");

    BoatKey *key = boat_key_import_raw(BOAT_KEY_TYPE_ED25519, privkey, 32);

### 7.2 Check Balance

    BoatKeyInfo info;
    boat_key_get_info(key, &info);

    uint64_t lamports;
    boat_sol_rpc_get_balance(&rpc, info.address, &lamports);
    printf("Balance: %llu lamports\n", (unsigned long long)lamports);

### 7.3 SPL Token Transfer

    // Compute Associated Token Account addresses
    uint8_t sender_ata[32], recipient_ata[32];
    boat_sol_ata_address(sender_pubkey, token_mint, sender_ata);
    boat_sol_ata_address(recipient_pubkey, token_mint, recipient_ata);

    // Build transfer instruction
    BoatSolInstruction ix;
    boat_sol_spl_transfer(sender_ata, recipient_ata, sender_pubkey,
                          1000000, &ix);  // amount in token base units

    // Get recent blockhash
    uint8_t blockhash[32];
    uint64_t last_valid;
    boat_sol_rpc_get_latest_blockhash(&rpc, BOAT_SOL_COMMITMENT_FINALIZED,
                                      blockhash, &last_valid);

    // Build and send transaction
    BoatSolTx tx;
    boat_sol_tx_init(&tx);
    boat_sol_tx_set_fee_payer(&tx, sender_pubkey);
    boat_sol_tx_set_blockhash(&tx, blockhash);
    boat_sol_tx_add_instruction(&tx, &ix);

    uint8_t sig[64];
    boat_sol_tx_send(&tx, key, &rpc, sig);

### 7.4 Create Associated Token Account

If the recipient does not have an ATA for the token, create one first:

    BoatSolInstruction create_ix;
    boat_sol_spl_create_ata(payer_pubkey, recipient_pubkey, token_mint, &create_ix);

    // Add both instructions to the same transaction
    BoatSolTx tx;
    boat_sol_tx_init(&tx);
    boat_sol_tx_set_fee_payer(&tx, payer_pubkey);
    boat_sol_tx_set_blockhash(&tx, blockhash);
    boat_sol_tx_add_instruction(&tx, &create_ix);   // create ATA first
    boat_sol_tx_add_instruction(&tx, &transfer_ix);  // then transfer
    boat_sol_tx_send(&tx, key, &rpc, sig);

### 7.5 Custom Program Interaction

For programs beyond SPL Token, use the generic instruction builder:

    BoatSolInstruction ix;
    boat_sol_ix_init(&ix, my_program_id);
    boat_sol_ix_add_account(&ix, account1, true, true);    // signer + writable
    boat_sol_ix_add_account(&ix, account2, false, true);   // writable
    boat_sol_ix_add_account(&ix, account3, false, false);  // readonly

    // Build instruction data with Borsh encoder
    uint8_t data_buf[128];
    BoatBorshEncoder enc;
    boat_borsh_init(&enc, data_buf, sizeof(data_buf));
    boat_borsh_write_u8(&enc, 2);           // instruction discriminator
    boat_borsh_write_u64(&enc, amount);
    boat_borsh_write_pubkey(&enc, some_key);

    boat_sol_ix_set_data(&ix, data_buf, boat_borsh_len(&enc));

    // Add to transaction and send as usual

### 7.6 Handling Blockhash Expiry

Solana blockhashes expire after ~60 seconds. If you get BOAT_ERROR_SOL_BLOCKHASH_EXPIRED,
fetch a new blockhash and retry:

    BoatResult r = boat_sol_tx_send(&tx, key, &rpc, sig);
    if (r == BOAT_ERROR_SOL_BLOCKHASH_EXPIRED) {
        // Fetch new blockhash
        boat_sol_rpc_get_latest_blockhash(&rpc, BOAT_SOL_COMMITMENT_FINALIZED,
                                          blockhash, &last_valid);
        boat_sol_tx_set_blockhash(&tx, blockhash);
        r = boat_sol_tx_send(&tx, key, &rpc, sig);  // retry
    }

---

## 8. Payment Protocols

All payment modules require EVM support (`-DBOAT_EVM=ON`). They use EIP-712 typed data signing and EIP-3009 `transferWithAuthorization`, which are EVM-specific.

### 8.1 x402: Pay-per-API-Call

Enable with -DBOAT_PAY_X402=ON (requires -DBOAT_EVM=ON) at build time.

Simplest usage — GET with one-call convenience (pass NULL for opts):

    #include "boat_pay.h"

    BoatEvmChainConfig chain = { .chain_id = 8453 };  // Base mainnet

    uint8_t *response = NULL;
    size_t response_len = 0;
    BoatResult r = boat_x402_process("https://api.example.com/data",
                                     NULL, key, &chain, &response, &response_len);
    if (r == BOAT_SUCCESS) {
        printf("Got %zu bytes: %.*s\n", response_len,
               (int)response_len, (char *)response);
        boat_free(response);
    }

POST with custom headers:

    BoatX402ReqOpts opts = {
        .method       = BOAT_HTTP_POST,
        .content_type = "application/json",
        .body         = (const uint8_t *)"{\"query\":\"weather\"}",
        .body_len     = 18,
        .extra_headers = "X-Api-Key: mykey123\r\n"
    };
    BoatResult r = boat_x402_process("https://api.example.com/data",
                                     &opts, key, &chain, &response, &response_len);

Step-by-step usage (for custom flows):

    // Step 1: Send original request. Returns BOAT_SUCCESS if 2xx (no payment),
    //         or BOAT_ERROR_HTTP_402 if payment required.
    BoatX402PaymentReq req;
    uint8_t *response = NULL;
    size_t response_len = 0;
    BoatResult r = boat_x402_request("https://api.example.com/data",
                                     NULL, &req, &response, &response_len);
    if (r == BOAT_SUCCESS) {
        // 2xx — resource returned directly, no payment needed
        printf("Got resource without payment\n");
    } else if (r == BOAT_ERROR_HTTP_402) {
        // Step 2: Sign payment
        char *payment_b64 = NULL;
        boat_x402_make_payment(&req, key, &chain, &payment_b64);

        // Step 3: Replay request with X-Payment header
        boat_x402_pay_and_get(req.resource_url, NULL, payment_b64,
                              &response, &response_len);
        boat_free(payment_b64);
    }

### 8.2 Circle Nanopayments: High-Frequency Micropayments

Enable with -DBOAT_PAY_NANO=ON (requires -DBOAT_EVM=ON) at build time.

    #include "boat_pay.h"

    BoatNanoConfig config;
    memset(&config, 0, sizeof(config));
    strncpy(config.gateway_url, "https://gateway.example.com",
            sizeof(config.gateway_url) - 1);
    memcpy(config.gateway_wallet_addr, gateway_wallet_address, 20);
    config.chain.chain_id = 84532;  // Base Sepolia
    strncpy(config.chain.rpc_url, "https://sepolia.base.org",
            sizeof(config.chain.rpc_url) - 1);

    BoatEvmRpc rpc;
    boat_evm_rpc_init(&rpc, config.chain.rpc_url);

    // One-time: deposit USDC to Gateway
    uint8_t deposit_amount[32] = {0};
    deposit_amount[31] = 100;  // 100 base units
    uint8_t txhash[32];
    boat_nano_deposit(&config, key, deposit_amount, &rpc, txhash);

    // Per-payment: sign authorization (off-chain, instant)
    uint8_t pay_amount[32] = {0};
    pay_amount[31] = 1;  // 1 base unit
    uint8_t nonce[32];
    boat_random(nonce, 32);

    BoatEip3009Auth auth;
    uint8_t sig[65];
    boat_nano_authorize(&config, key, merchant_addr, pay_amount, nonce, &auth, &sig);
    // Send auth + sig to merchant/gateway via your application protocol

    // Check remaining balance
    uint8_t balance[32];
    boat_nano_get_balance(&config, my_address, balance);

### 8.3 Circle Gateway: Cross-Chain USDC

Enable with -DBOAT_PAY_GATEWAY=ON (requires -DBOAT_EVM=ON) at build time.

    #include "boat_pay.h"

    BoatGatewayConfig config;
    memset(&config, 0, sizeof(config));
    // Gateway Wallet testnet: 0x0077777d7EBA4688BDeF3E311b846F25870A19B9
    memcpy(config.gateway_wallet_addr, gateway_wallet_addr, 20);
    // USDC on Base Sepolia: 0x036CbD53842c5426634e7929541eC2318f3dCF7e
    memcpy(config.usdc_addr, usdc_addr, 20);
    config.chain.chain_id = 84532;  // Base Sepolia
    strncpy(config.chain.rpc_url, "https://sepolia.base.org",
            sizeof(config.chain.rpc_url) - 1);

    BoatEvmRpc rpc;
    boat_evm_rpc_init(&rpc, config.chain.rpc_url);

    // Deposit USDC
    uint8_t amount[32] = {0};
    amount[28] = 0x05; amount[29] = 0xF5; amount[30] = 0xE1; amount[31] = 0x00;
    // = 100000000 = 100 USDC (6 decimals)
    uint8_t txhash[32];
    boat_gateway_deposit(&config, key, amount, &rpc, txhash);

    // Check balance
    uint8_t balance[32];
    boat_gateway_balance(&config, my_address, &rpc, balance);

    // Withdraw all available balance (instant, same-chain transfer)
    // Use boat_gateway_transfer() with src == dst config:
    // boat_gateway_transfer(&config, &config, key, amount, zero_fee, &rpc, &result);

    // Trustless withdrawal (emergency, 7-day delay, only if Circle API is down):
    // boat_gateway_trustless_withdraw(&config, key, amount, &rpc, txhash);
    // ... wait 7 days ...
    // boat_gateway_trustless_complete(&config, key, &rpc, txhash);

---

## 9. Porting to a New Platform

### 9.1 Tier 1: Implement Mandatory Primitives

Create a new file, e.g., src/pal/myplatform/pal_myplatform.c, and implement:

    void  *boat_malloc(size_t size);
    void   boat_free(void *ptr);
    BoatResult boat_storage_write(const char *name, const uint8_t *data, size_t len);
    BoatResult boat_storage_read(const char *name, uint8_t *data, size_t cap, size_t *out_len);
    BoatResult boat_storage_delete(const char *name);
    uint64_t boat_time_ms(void);
    void     boat_sleep_ms(uint32_t ms);
    BoatResult boat_random(uint8_t *buf, size_t len);
    BoatResult boat_mutex_init(BoatMutex **mutex);
    BoatResult boat_mutex_lock(BoatMutex *mutex);
    BoatResult boat_mutex_unlock(BoatMutex *mutex);
    void       boat_mutex_destroy(BoatMutex *mutex);

Most of these are trivial wrappers around your RTOS or OS primitives.

### 9.2 Tier 2: Implement HTTP Operations

Implement the BoatHttpOps function pointers using your platform HTTP stack:

    static BoatResult my_http_post(const char *url, const char *content_type,
                                   const uint8_t *body, size_t body_len,
                                   const char *extra_headers,
                                   BoatHttpResponse *response)
    {
        // Use your HTTP library to POST
        // Allocate response->data with malloc, set response->len
        return BOAT_SUCCESS;
    }

    static BoatResult my_http_get(const char *url, const char *extra_headers,
                                  BoatHttpResponse *response)
    {
        // Use your HTTP library to GET
        return BOAT_SUCCESS;
    }

    static void my_http_free(BoatHttpResponse *response)
    {
        if (response->data) { free(response->data); response->data = NULL; }
    }

    static const BoatHttpOps my_ops = {
        .post = my_http_post,
        .get = my_http_get,
        .free_response = my_http_free
    };

Register at initialization:

    boat_set_http_ops(&my_ops);

### 9.3 Platform-Specific Notes

**ESP-IDF:** Use esp_http_client for HTTP. Use esp_random() for boat_random().
Use nvs_flash for boat_storage_*. Use esp_timer for boat_time_ms().

**Zephyr:** Use HTTP client API for HTTP. Use sys_rand_get() for boat_random().
Use settings subsystem for boat_storage_*. Use k_uptime_get() for boat_time_ms().

**FreeRTOS (bare):** Use your TCP/IP stack (lwIP + mbedTLS) for HTTP.
Use hardware RNG for boat_random(). Use filesystem or flash driver for storage.

**Cellular modules (Quectel, Fibocom, etc.):** Use AT command HTTP interface or
vendor HTTP API. Map boat_random() to the modem RNG. Use vendor filesystem API
for storage.

---

## 10. Error Handling

All BoAT v4 functions return BoatResult (int32_t). Check for BOAT_SUCCESS (0):

    BoatResult r = boat_evm_tx_send(&tx, key, &rpc, txhash);
    if (r != BOAT_SUCCESS) {
        printf("Failed: %d\n", r);
        // Handle error based on code range
    }

Error code ranges for category-level handling:

    if (r <= -100 && r > -200) { /* argument error */ }
    if (r <= -200 && r > -300) { /* memory error */ }
    if (r <= -300 && r > -400) { /* RPC/network error -- retry? */ }
    if (r <= -400 && r > -500) { /* key error */ }
    if (r <= -700 && r > -800) { /* Solana-specific error */ }

---

## 11. Security Best Practices

1. **Use hardware RNG** in production. The Linux /dev/urandom is fine for development,
   but production IoT devices should use their hardware RNG via boat_random().

2. **Use secure storage** in production. The Linux file-based storage is unencrypted.
   Map boat_storage_write/read to encrypted flash or secure element storage.

3. **Free keys promptly.** Call boat_key_free() as soon as you no longer need the key.
   This zeroes the private key material in memory.

4. **Use HTTPS for RPC endpoints.** The SDK does not enforce TLS. Always use https://
   URLs in production to prevent man-in-the-middle attacks on RPC calls.

5. **Validate transaction parameters.** The SDK does not validate business logic. Ensure
   your application checks amounts, addresses, and gas limits before sending.

6. **Handle errors.** Never ignore return codes. A failed RPC call might mean network
   issues; a failed sign might mean a corrupted key.

---

## 12. Examples

The examples/ directory contains five complete programs:

| File | Description |
|------|-------------|
| evm_transfer.c | Send ETH on Base Sepolia |
| evm_erc20.c | Query ERC20 balance + send transfer |
| sol_transfer.c | Send SPL token on Solana devnet |
| pay_x402_demo.c | x402 payment for HTTP resource |
| pay_nano_demo.c | Circle Nanopayments deposit + authorize |

To run an example:

1. Edit the example file: replace PRIVATE_KEY_HEX with your test key
2. Build with examples enabled:

       cmake .. -DBOAT_PAL=linux -DBOAT_BUILD_EXAMPLES=ON -DBOAT_PAY_X402=ON
       make

3. Run:

       ./evm_transfer
       ./pay_x402_demo

---

## 13. Troubleshooting

**Build error: curl/curl.h not found**
Install libcurl development headers: sudo apt install libcurl4-openssl-dev

**RPC calls return BOAT_ERROR_RPC_FAIL**
Check your RPC URL. Ensure the endpoint is reachable. Check if you need an API key.

**BOAT_ERROR_KEY_SIGN returned**
Verify your private key is valid (32 bytes, non-zero, less than curve order for secp256k1).

**BOAT_ERROR_SOL_TX_TOO_LARGE**
Solana transactions must be under 1232 bytes. Reduce the number of instructions or accounts.

**BOAT_ERROR_HTTP_402 from x402**
This is expected -- it means the server requires payment. Use boat_x402_process() to
handle the full payment flow automatically.

**Logging:** Increase log verbosity by setting the log level before any SDK calls:

    #include "boat.h"
    extern BoatLogLevel g_boat_log_level;
    g_boat_log_level = BOAT_LOG_VERBOSE;

# BoAT v4 API Manual

**Document:** API Reference Manual
**Project:** BoAT v4 SDK
**Author:** TLAY.io
**Date:** March 2026

---

## 1. Conventions

- All functions return BoatResult (int32_t) unless otherwise noted.
- BOAT_SUCCESS (0) indicates success; negative values indicate errors.
- Pointer parameters marked [in] are read-only; [out] are written by the function.
- Caller is responsible for freeing memory allocated by functions (noted per function).
- All sizes are in bytes unless otherwise noted.

---

## 2. Core: Types and Errors (boat.h)

### 2.1 BoatResult Error Codes

    BOAT_SUCCESS                     0
    BOAT_ERROR                      -1    // Generic error

    // Argument errors (-100..-199)
    BOAT_ERROR_ARG_NULL            -100   // Required argument is NULL
    BOAT_ERROR_ARG_INVALID         -101   // Argument value is invalid
    BOAT_ERROR_ARG_OUT_OF_RANGE    -102   // Argument out of acceptable range

    // Memory errors (-200..-299)
    BOAT_ERROR_MEM_ALLOC           -200   // Memory allocation failed
    BOAT_ERROR_MEM_OVERFLOW        -201   // Buffer overflow

    // RPC errors (-300..-399)
    BOAT_ERROR_RPC_FAIL            -300   // RPC call failed (network)
    BOAT_ERROR_RPC_PARSE           -301   // Failed to parse RPC response
    BOAT_ERROR_RPC_TIMEOUT         -302   // RPC call timed out
    BOAT_ERROR_RPC_SERVER          -303   // RPC server returned error

    // Key errors (-400..-499)
    BOAT_ERROR_KEY_GEN             -400   // Key generation failed
    BOAT_ERROR_KEY_IMPORT          -401   // Key import failed
    BOAT_ERROR_KEY_SIGN            -402   // Signing failed
    BOAT_ERROR_KEY_NOT_FOUND       -403   // Key not found in storage
    BOAT_ERROR_KEY_TYPE            -404   // Key type mismatch

    // Storage errors (-500..-599)
    BOAT_ERROR_STORAGE_WRITE       -500   // Storage write failed
    BOAT_ERROR_STORAGE_READ        -501   // Storage read failed
    BOAT_ERROR_STORAGE_NOT_FOUND   -502   // Storage entry not found

    // EVM errors (-600..-699)
    BOAT_ERROR_EVM_RLP             -600   // RLP encoding error
    BOAT_ERROR_EVM_ABI             -601   // ABI encoding/decoding error
    BOAT_ERROR_EVM_TX              -602   // Transaction error
    BOAT_ERROR_EVM_NONCE           -603   // Nonce error

    // Solana errors (-700..-799)
    BOAT_ERROR_SOL_BLOCKHASH_EXPIRED   -700   // Blockhash expired
    BOAT_ERROR_SOL_INSUFFICIENT_FUNDS  -701   // Insufficient funds
    BOAT_ERROR_SOL_TX_TOO_LARGE        -702   // Transaction exceeds 1232 bytes

    // HTTP errors (-800..-899)
    BOAT_ERROR_HTTP_FAIL           -800   // HTTP request failed
    BOAT_ERROR_HTTP_402            -802   // HTTP 402 Payment Required

### 2.2 BoatKeyType

    typedef enum {
        BOAT_KEY_TYPE_SECP256K1 = 0,   // EVM chains
        BOAT_KEY_TYPE_SECP256R1,        // WebAuthn/FIDO2
        BOAT_KEY_TYPE_ED25519           // Solana
    } BoatKeyType;

### 2.3 BoatBuf

    typedef struct {
        uint8_t *data;   // Buffer data
        size_t   len;    // Current length
        size_t   cap;    // Allocated capacity
    } BoatBuf;

### 2.4 BoatBuf Functions

#### boat_buf_init

    BoatResult boat_buf_init(BoatBuf *buf, size_t initial_cap);

Initialize a dynamic buffer.
- buf [out]: Buffer to initialize
- initial_cap: Initial capacity (0 defaults to 64)
- Returns: BOAT_SUCCESS or BOAT_ERROR_MEM_ALLOC

#### boat_buf_append

    BoatResult boat_buf_append(BoatBuf *buf, const uint8_t *data, size_t len);

Append data to buffer. Grows automatically if needed.
- buf [in/out]: Target buffer
- data [in]: Data to append
- len: Length of data
- Returns: BOAT_SUCCESS, BOAT_ERROR_ARG_NULL, or BOAT_ERROR_MEM_ALLOC

#### boat_buf_append_byte

    BoatResult boat_buf_append_byte(BoatBuf *buf, uint8_t byte);

Append a single byte.

#### boat_buf_reset

    void boat_buf_reset(BoatBuf *buf);

Reset length to 0 without freeing memory.

#### boat_buf_free

    void boat_buf_free(BoatBuf *buf);

Free buffer memory and reset all fields.

### 2.5 Hex/Bin Conversion

#### boat_hex_to_bin

    BoatResult boat_hex_to_bin(const char *hex_str, uint8_t *bin,
                               size_t bin_cap, size_t *out_len);

Convert hex string to binary. Handles optional "0x" prefix and odd-length strings.
- hex_str [in]: Hex string (with or without "0x" prefix)
- bin [out]: Output binary buffer
- bin_cap: Capacity of bin buffer
- out_len [out]: Actual bytes written (may be NULL)
- Returns: BOAT_SUCCESS, BOAT_ERROR_ARG_NULL, BOAT_ERROR_ARG_INVALID, BOAT_ERROR_MEM_OVERFLOW

#### boat_bin_to_hex

    BoatResult boat_bin_to_hex(const uint8_t *bin, size_t len,
                               char *hex_str, size_t hex_cap, bool prefix_0x);

Convert binary to hex string.
- bin [in]: Input binary data
- len: Length of binary data
- hex_str [out]: Output hex string buffer
- hex_cap: Capacity of hex string buffer (must be >= len*2 + prefix + 1)
- prefix_0x: If true, prepend "0x"
- Returns: BOAT_SUCCESS, BOAT_ERROR_ARG_NULL, BOAT_ERROR_MEM_OVERFLOW

---

## 3. Key Management (boat_key.h)

### 3.1 Types

#### BoatKey

    typedef struct BoatKey BoatKey;  // Opaque handle

Internal structure is hidden. Application only uses BoatKey* pointers.

#### BoatKeyInfo

    typedef struct {
        BoatKeyType type;
        uint8_t     pubkey[65];     // 65 bytes for secp256k1/r1, 32 for ed25519
        size_t      pubkey_len;     // 65 or 32
        uint8_t     address[32];    // 20 bytes for EVM, 32 for Solana
        size_t      address_len;    // 20 or 32
    } BoatKeyInfo;

### 3.2 Lifecycle Functions

#### boat_key_generate

    BoatKey *boat_key_generate(BoatKeyType type);

Generate a new random key pair.
- type: BOAT_KEY_TYPE_SECP256K1, BOAT_KEY_TYPE_SECP256R1, or BOAT_KEY_TYPE_ED25519
- Returns: New key handle, or NULL on failure
- Note: Uses boat_random() from PAL for entropy

#### boat_key_import_raw

    BoatKey *boat_key_import_raw(BoatKeyType type, const uint8_t *privkey, size_t len);

Import a key from raw private key bytes.
- type: Key type
- privkey [in]: 32-byte private key
- len: Must be 32
- Returns: New key handle, or NULL on failure
- Note: For secp256k1, validates key is in range [1, n-1]

#### boat_key_import_mnemonic

    BoatKey *boat_key_import_mnemonic(const char *mnemonic, const char *path,
                                      BoatKeyType type);

Import a key from BIP-39 mnemonic phrase.
- mnemonic [in]: Space-separated mnemonic words
- path [in]: BIP-32 derivation path (e.g., "m/44'/60'/0'/0/0")
- type: Key type
- Returns: New key handle, or NULL on failure
- Status: Not yet implemented (returns NULL)

#### boat_key_from_se

    BoatKey *boat_key_from_se(BoatKeyType type, uint32_t slot);

Create a key handle backed by a Secure Element slot.
- type: Key type
- slot: SE slot index
- Returns: New key handle, or NULL if SE not compiled
- Status: Stub (requires BOAT_KEY_SE_ENABLED)

#### boat_key_free

    void boat_key_free(BoatKey *key);

Free a key handle. Zeroes all private key material before deallocation.
- key: Key handle to free (NULL is safe)

### 3.3 Signing Functions

#### boat_key_sign

    BoatResult boat_key_sign(const BoatKey *key, const uint8_t *hash,
                             size_t hash_len, uint8_t *sig, size_t *sig_len);

Sign a hash.
- key [in]: Key handle
- hash [in]: Hash to sign
- hash_len: Length of hash (32 for ECDSA, any for ed25519)
- sig [out]: Signature output (64 bytes for ECDSA, 64 bytes for ed25519)
- sig_len [in/out]: On input, capacity; on output, actual signature length
- Returns: BOAT_SUCCESS or error
- Note: For ed25519, hash is treated as the message (ed25519 hashes internally)

#### boat_key_sign_recoverable

    BoatResult boat_key_sign_recoverable(const BoatKey *key, const uint8_t *hash32,
                                         uint8_t sig65[65]);

Sign with recovery ID (for EVM ecrecover).
- key [in]: Key handle (must be secp256k1 or secp256r1)
- hash32 [in]: 32-byte hash to sign
- sig65 [out]: 65-byte output: r[32] || s[32] || v[1] (v = 0 or 1)
- Returns: BOAT_SUCCESS, BOAT_ERROR_KEY_TYPE (if ed25519), or BOAT_ERROR_KEY_SIGN

### 3.4 Info Function

#### boat_key_get_info

    BoatResult boat_key_get_info(const BoatKey *key, BoatKeyInfo *info);

Get public key and derived address.
- key [in]: Key handle
- info [out]: Filled with type, pubkey, pubkey_len, address, address_len
- Returns: BOAT_SUCCESS or BOAT_ERROR_ARG_NULL

### 3.5 Storage Functions

#### boat_key_save

    BoatResult boat_key_save(const BoatKey *key, const char *name);

Save key to persistent storage via PAL.
- key [in]: Key handle
- name [in]: Storage name (identifier string)
- Returns: BOAT_SUCCESS or storage error
- Format: [magic:4][type:1][privkey_len:1][privkey:N]

#### boat_key_load

    BoatKey *boat_key_load(const char *name, BoatKeyType type);

Load key from persistent storage.
- name [in]: Storage name used in boat_key_save()
- type: Expected key type (must match stored type)
- Returns: Key handle, or NULL on failure

#### boat_key_delete

    BoatResult boat_key_delete(const char *name);

Delete key from persistent storage.
- name [in]: Storage name
- Returns: BOAT_SUCCESS or BOAT_ERROR_STORAGE_NOT_FOUND

---

## 4. Platform Abstraction Layer (boat_pal.h)

### 4.1 Tier 1: Mandatory Primitives

#### boat_malloc / boat_free

    void *boat_malloc(size_t size);
    void  boat_free(void *ptr);

Standard memory allocation. Maps to malloc/free on Linux.

#### boat_storage_write

    BoatResult boat_storage_write(const char *name, const uint8_t *data, size_t len);

Write data to persistent storage.
- name [in]: Storage key name
- data [in]: Data to write
- len: Data length
- Returns: BOAT_SUCCESS or BOAT_ERROR_STORAGE_WRITE

#### boat_storage_read

    BoatResult boat_storage_read(const char *name, uint8_t *data,
                                 size_t cap, size_t *out_len);

Read data from persistent storage.
- name [in]: Storage key name
- data [out]: Buffer to receive data
- cap: Buffer capacity
- out_len [out]: Actual bytes read (may be NULL)
- Returns: BOAT_SUCCESS, BOAT_ERROR_STORAGE_NOT_FOUND

#### boat_storage_delete

    BoatResult boat_storage_delete(const char *name);

Delete a storage entry.

#### boat_time_ms

    uint64_t boat_time_ms(void);

Return monotonic time in milliseconds.

#### boat_sleep_ms

    void boat_sleep_ms(uint32_t ms);

Sleep for the specified number of milliseconds.

#### boat_random

    BoatResult boat_random(uint8_t *buf, size_t len);

Fill buffer with cryptographic random bytes.
- buf [out]: Buffer to fill
- len: Number of random bytes
- Returns: BOAT_SUCCESS or BOAT_ERROR

#### Mutex Functions

    BoatResult boat_mutex_init(BoatMutex **mutex);
    BoatResult boat_mutex_lock(BoatMutex *mutex);
    BoatResult boat_mutex_unlock(BoatMutex *mutex);
    void       boat_mutex_destroy(BoatMutex *mutex);

Standard mutex operations. BoatMutex is an opaque struct defined by the PAL.

### 4.2 Tier 2: HTTP Service Contract

#### BoatHttpResponse

    typedef struct {
        uint8_t *data;   // Response body (allocated by post/get)
        size_t   len;    // Response body length
    } BoatHttpResponse;

#### BoatHttpOps

    typedef struct {
        BoatResult (*post)(const char *url, const char *content_type,
                           const uint8_t *body, size_t body_len,
                           const char *extra_headers,
                           BoatHttpResponse *response);
        BoatResult (*get)(const char *url, const char *extra_headers,
                          BoatHttpResponse *response);
        void (*free_response)(BoatHttpResponse *response);
    } BoatHttpOps;

Function pointer struct for HTTP operations.
- post: Send HTTP POST. extra_headers is newline-separated additional headers (may be NULL).
- get: Send HTTP GET. Returns BOAT_ERROR_HTTP_402 for 402 responses (body still populated).
- free_response: Free response data allocated by post/get.

#### boat_set_http_ops / boat_get_http_ops

    void              boat_set_http_ops(const BoatHttpOps *ops);
    const BoatHttpOps *boat_get_http_ops(void);

Register and retrieve the HTTP implementation. Must be called before any RPC or HTTP
operations.

#### boat_pal_linux_init

    BoatResult boat_pal_linux_init(void);

Initialize Linux PAL: calls curl_global_init() and registers curl-based HTTP ops.

#### boat_pal_linux_default_http_ops

    const BoatHttpOps *boat_pal_linux_default_http_ops(void);

Returns pointer to the curl-based HTTP ops struct.

---

## 5. EVM Module (boat_evm.h)

### 5.1 Types

#### BoatEvmChainConfig

    typedef struct {
        uint64_t chain_id;       // EIP-155 chain ID
        char     rpc_url[256];   // JSON-RPC endpoint URL
        bool     eip1559;        // Enable EIP-1559 type-2 transactions
    } BoatEvmChainConfig;

#### BoatEvmRpc

    typedef struct {
        char url[256];
        int  req_id;
    } BoatEvmRpc;

#### BoatEvmTx

    typedef struct {
        BoatEvmChainConfig chain;
        uint64_t nonce;
        uint8_t  gas_price[32];      // uint256 big-endian
        uint64_t gas_limit;
        uint8_t  to[20];
        bool     to_set;
        uint8_t  value[32];          // uint256 big-endian
        uint8_t *data;               // Calldata (heap-allocated)
        size_t   data_len;
        uint8_t  max_fee_per_gas[32];     // EIP-1559
        uint8_t  max_priority_fee[32];    // EIP-1559
        bool     nonce_set;
        bool     gas_price_set;
    } BoatEvmTx;

### 5.2 RPC Functions

#### boat_evm_rpc_init / boat_evm_rpc_free

    BoatResult boat_evm_rpc_init(BoatEvmRpc *rpc, const char *url);
    void       boat_evm_rpc_free(BoatEvmRpc *rpc);

Initialize/free RPC context.

#### boat_evm_block_number

    BoatResult boat_evm_block_number(BoatEvmRpc *rpc, uint64_t *out);

Get current block number.

#### boat_evm_get_balance

    BoatResult boat_evm_get_balance(BoatEvmRpc *rpc, const uint8_t addr[20],
                                    uint8_t out_wei[32]);

Get account balance in wei (uint256 big-endian).

#### boat_evm_get_nonce

    BoatResult boat_evm_get_nonce(BoatEvmRpc *rpc, const uint8_t addr[20],
                                  uint64_t *out);

Get account transaction count (nonce).

#### boat_evm_gas_price

    BoatResult boat_evm_gas_price(BoatEvmRpc *rpc, uint8_t out_wei[32]);

Get current gas price (uint256 big-endian).

#### boat_evm_send_raw_tx

    BoatResult boat_evm_send_raw_tx(BoatEvmRpc *rpc, const uint8_t *raw,
                                    size_t raw_len, uint8_t txhash[32]);

Send a signed raw transaction.
- raw [in]: RLP-encoded signed transaction
- raw_len: Length of raw data
- txhash [out]: 32-byte transaction hash

#### boat_evm_eth_call

    BoatResult boat_evm_eth_call(BoatEvmRpc *rpc, const uint8_t to[20],
                                 const uint8_t *data, size_t data_len,
                                 uint8_t **result, size_t *result_len);

Call a contract view function (no transaction).
- to [in]: Contract address
- data [in]: ABI-encoded calldata
- result [out]: Decoded result bytes (caller must boat_free)
- result_len [out]: Length of result

### 5.3 Transaction Builder Functions

#### boat_evm_tx_init

    BoatResult boat_evm_tx_init(BoatEvmTx *tx, const BoatEvmChainConfig *chain);

Initialize transaction with chain config. Sets default gas_limit to 21000.

#### boat_evm_tx_set_nonce

    BoatResult boat_evm_tx_set_nonce(BoatEvmTx *tx, uint64_t nonce);

Set nonce manually. Prevents auto_fill from querying chain.

#### boat_evm_tx_set_gas_price

    BoatResult boat_evm_tx_set_gas_price(BoatEvmTx *tx, const uint8_t gas_price[32]);

Set gas price (uint256 big-endian). Prevents auto_fill from querying chain.

#### boat_evm_tx_set_gas_limit

    BoatResult boat_evm_tx_set_gas_limit(BoatEvmTx *tx, uint64_t gas_limit);

Set gas limit.

#### boat_evm_tx_set_to

    BoatResult boat_evm_tx_set_to(BoatEvmTx *tx, const uint8_t to[20]);

Set recipient address. Omit for contract creation.

#### boat_evm_tx_set_value

    BoatResult boat_evm_tx_set_value(BoatEvmTx *tx, const uint8_t value[32]);

Set transfer value in wei (uint256 big-endian).

#### boat_evm_tx_set_data

    BoatResult boat_evm_tx_set_data(BoatEvmTx *tx, const uint8_t *data, size_t len);

Set transaction calldata. Allocates a copy internally.
- data [in]: Calldata bytes (typically ABI-encoded)
- len: Calldata length
- Note: Caller must free tx.data with boat_free() after use

#### boat_evm_tx_auto_fill

    BoatResult boat_evm_tx_auto_fill(BoatEvmTx *tx, BoatEvmRpc *rpc,
                                     const BoatKey *key);

Query chain for nonce and gas price if not already set.
- Derives sender address from key
- Calls boat_evm_get_nonce() if nonce not set
- Calls boat_evm_gas_price() if gas price not set and not EIP-1559

#### boat_evm_tx_sign

    BoatResult boat_evm_tx_sign(BoatEvmTx *tx, const BoatKey *key,
                                uint8_t **raw_out, size_t *raw_len);

Sign transaction and produce RLP-encoded raw transaction.
- raw_out [out]: Heap-allocated signed transaction (caller must boat_free())
- raw_len [out]: Length of raw transaction
- Process: RLP encode fields -> keccak256 -> sign -> re-encode with v,r,s
- EIP-155: v = recovery_id + chain_id * 2 + 35

#### boat_evm_tx_send

    BoatResult boat_evm_tx_send(BoatEvmTx *tx, const BoatKey *key,
                                BoatEvmRpc *rpc, uint8_t txhash[32]);

Sign and send transaction in one call.
- txhash [out]: 32-byte transaction hash on success

### 5.4 ABI Encoder Functions

#### boat_evm_abi_encode_func

    BoatResult boat_evm_abi_encode_func(const char *func_sig,
                                        const uint8_t *args[],
                                        const size_t arg_lens[],
                                        size_t n_args,
                                        uint8_t **calldata,
                                        size_t *calldata_len);

Encode a function call with selector and arguments.
- func_sig [in]: Solidity function signature (e.g., "transfer(address,uint256)")
- args [in]: Array of 32-byte encoded argument slots
- arg_lens [in]: Array of argument lengths (must all be 32)
- n_args: Number of arguments
- calldata [out]: Heap-allocated calldata (caller must boat_evm_abi_free())
- calldata_len [out]: Length of calldata (4 + n_args * 32)

#### boat_evm_abi_encode_uint256

    void boat_evm_abi_encode_uint256(const uint8_t val[32], uint8_t out[32]);

Copy 32-byte big-endian value to ABI slot.

#### boat_evm_abi_encode_uint64

    void boat_evm_abi_encode_uint64(uint64_t val, uint8_t out[32]);

Encode uint64 as 32-byte big-endian ABI slot (zero-padded on left).

#### boat_evm_abi_encode_address

    void boat_evm_abi_encode_address(const uint8_t addr[20], uint8_t out[32]);

Encode 20-byte address as 32-byte ABI slot (12 zero bytes + 20 address bytes).

#### boat_evm_abi_encode_bool

    void boat_evm_abi_encode_bool(bool val, uint8_t out[32]);

Encode boolean as 32-byte ABI slot.

#### boat_evm_abi_encode_bytes

    BoatResult boat_evm_abi_encode_bytes(const uint8_t *data, size_t len,
                                         BoatBuf *out);

Encode dynamic bytes: length (uint256) + data padded to 32-byte boundary.

#### boat_evm_abi_encode_string

    BoatResult boat_evm_abi_encode_string(const char *str, BoatBuf *out);

Encode dynamic string (same encoding as bytes).

### 5.5 ABI Decoder Functions

#### boat_evm_abi_decode_uint256

    BoatResult boat_evm_abi_decode_uint256(const uint8_t *data, size_t offset,
                                           uint8_t out[32]);

Read 32-byte uint256 from ABI data at byte offset.

#### boat_evm_abi_decode_address

    BoatResult boat_evm_abi_decode_address(const uint8_t *data, size_t offset,
                                           uint8_t out[20]);

Read 20-byte address from ABI data at byte offset (skips 12-byte left padding).

#### boat_evm_abi_decode_bool

    BoatResult boat_evm_abi_decode_bool(const uint8_t *data, size_t offset,
                                        bool *out);

Read boolean from ABI data at byte offset.

#### boat_evm_abi_decode_bytes

    BoatResult boat_evm_abi_decode_bytes(const uint8_t *data, size_t data_len,
                                         size_t offset,
                                         uint8_t **out, size_t *out_len);

Decode dynamic bytes from ABI data. Follows offset pointer, reads length, copies data.
- out [out]: Heap-allocated decoded bytes (caller must boat_free())
- out_len [out]: Length of decoded bytes

#### boat_evm_abi_free

    void boat_evm_abi_free(void *ptr);

Free memory allocated by ABI encode/decode functions.

---

## 6. Solana Module (boat_sol.h)

### 6.1 Constants

    extern const uint8_t BOAT_SOL_SYSTEM_PROGRAM_ID[32];  // 1111..1111
    extern const uint8_t BOAT_SOL_TOKEN_PROGRAM_ID[32];   // TokenkegQ...
    extern const uint8_t BOAT_SOL_ATA_PROGRAM_ID[32];     // ATokenGPv...

### 6.2 Types

#### BoatSolCommitment

    typedef enum {
        BOAT_SOL_COMMITMENT_FINALIZED = 0,
        BOAT_SOL_COMMITMENT_CONFIRMED,
        BOAT_SOL_COMMITMENT_PROCESSED
    } BoatSolCommitment;

#### BoatSolChainConfig

    typedef struct {
        char               rpc_url[256];
        BoatSolCommitment  commitment;
    } BoatSolChainConfig;

#### BoatSolInstruction

    typedef struct {
        uint8_t program_id[32];
        struct {
            uint8_t pubkey[32];
            bool    is_signer;
            bool    is_writable;
        } accounts[BOAT_SOL_MAX_IX_ACCOUNTS];   // max 16
        size_t  num_accounts;
        uint8_t data[BOAT_SOL_MAX_IX_DATA];     // max 256
        size_t  data_len;
    } BoatSolInstruction;

#### BoatSolTx

    typedef struct {
        uint8_t fee_payer[32];
        uint8_t recent_blockhash[32];
        bool    fee_payer_set;
        bool    blockhash_set;
        BoatSolInstruction instructions[BOAT_SOL_MAX_INSTRUCTIONS];  // max 8
        size_t num_instructions;
        struct { uint8_t pubkey[32]; bool is_signer; bool is_writable; }
            accounts[BOAT_SOL_MAX_ACCOUNTS];  // max 32, deduplicated
        size_t num_accounts;
    } BoatSolTx;

#### BoatBorshEncoder

    typedef struct {
        uint8_t *buf;
        size_t   cap;
        size_t   len;
    } BoatBorshEncoder;

### 6.3 RPC Functions

#### boat_sol_rpc_init / boat_sol_rpc_free

    BoatResult boat_sol_rpc_init(BoatSolRpc *rpc, const char *url);
    void       boat_sol_rpc_free(BoatSolRpc *rpc);

#### boat_sol_rpc_get_latest_blockhash

    BoatResult boat_sol_rpc_get_latest_blockhash(BoatSolRpc *rpc,
                                                  BoatSolCommitment commitment,
                                                  uint8_t blockhash[32],
                                                  uint64_t *last_valid_height);

Get recent blockhash for transaction signing.
- blockhash [out]: 32-byte blockhash
- last_valid_height [out]: Last block height where this blockhash is valid (may be NULL)

#### boat_sol_rpc_get_balance

    BoatResult boat_sol_rpc_get_balance(BoatSolRpc *rpc, const uint8_t pubkey[32],
                                        uint64_t *lamports);

Get SOL balance in lamports.

#### boat_sol_rpc_get_token_balance

    BoatResult boat_sol_rpc_get_token_balance(BoatSolRpc *rpc, const uint8_t ata[32],
                                              uint64_t *amount, uint8_t *decimals);

Get SPL token balance for an Associated Token Account.
- ata [in]: 32-byte ATA address
- amount [out]: Token amount in base units
- decimals [out]: Token decimals (may be NULL)

#### boat_sol_rpc_send_transaction

    BoatResult boat_sol_rpc_send_transaction(BoatSolRpc *rpc, const uint8_t *raw,
                                             size_t raw_len, uint8_t signature[64]);

Send a signed transaction.
- raw [in]: Serialized signed transaction
- raw_len: Transaction length
- signature [out]: 64-byte transaction signature

#### boat_sol_rpc_get_signature_status

    BoatResult boat_sol_rpc_get_signature_status(BoatSolRpc *rpc,
                                                  const uint8_t sig[64],
                                                  bool *confirmed, bool *finalized);

Check transaction confirmation status.
- confirmed [out]: True if confirmed or finalized (may be NULL)
- finalized [out]: True if finalized (may be NULL)

#### boat_sol_rpc_call

    BoatResult boat_sol_rpc_call(BoatSolRpc *rpc, const char *method,
                                 const char *params_json, char **result_json);

Generic Solana RPC call for methods not covered by typed wrappers.
- result_json [out]: Heap-allocated JSON result string (caller must boat_free())

### 6.4 Instruction Builder

#### boat_sol_ix_init

    BoatResult boat_sol_ix_init(BoatSolInstruction *ix, const uint8_t program_id[32]);

Initialize instruction with program ID. Zeroes all fields.

#### boat_sol_ix_add_account

    BoatResult boat_sol_ix_add_account(BoatSolInstruction *ix,
                                       const uint8_t pubkey[32],
                                       bool is_signer, bool is_writable);

Add an account to the instruction. Max BOAT_SOL_MAX_IX_ACCOUNTS (16).

#### boat_sol_ix_set_data

    BoatResult boat_sol_ix_set_data(BoatSolInstruction *ix,
                                    const uint8_t *data, size_t len);

Set instruction data. Max BOAT_SOL_MAX_IX_DATA (256) bytes.

### 6.5 Transaction Builder

#### boat_sol_tx_init

    BoatResult boat_sol_tx_init(BoatSolTx *tx);

Initialize empty transaction.

#### boat_sol_tx_set_fee_payer

    BoatResult boat_sol_tx_set_fee_payer(BoatSolTx *tx, const uint8_t pubkey[32]);

Set fee payer. Automatically added to account list as signer+writable.

#### boat_sol_tx_set_blockhash

    BoatResult boat_sol_tx_set_blockhash(BoatSolTx *tx, const uint8_t blockhash[32]);

Set recent blockhash. Required before signing.

#### boat_sol_tx_add_instruction

    BoatResult boat_sol_tx_add_instruction(BoatSolTx *tx,
                                           const BoatSolInstruction *ix);

Add instruction to transaction. Accounts are automatically deduplicated and permissions
are upgraded (if same account appears as both signer and non-signer, signer wins).
Max BOAT_SOL_MAX_INSTRUCTIONS (8).

#### boat_sol_tx_sign

    BoatResult boat_sol_tx_sign(BoatSolTx *tx, const BoatKey *key,
                                uint8_t **raw, size_t *raw_len);

Serialize message, sign with ed25519, produce wire-format transaction.
- raw [out]: Heap-allocated serialized transaction (caller must boat_free())
- raw_len [out]: Transaction length
- Returns: BOAT_ERROR_SOL_TX_TOO_LARGE if > 1232 bytes

#### boat_sol_tx_send

    BoatResult boat_sol_tx_send(BoatSolTx *tx, const BoatKey *key,
                                BoatSolRpc *rpc, uint8_t sig[64]);

Sign and send transaction in one call.

#### boat_sol_tx_free

    void boat_sol_tx_free(BoatSolTx *tx);

Zero and reset transaction struct.

### 6.6 SPL Token Functions

#### boat_sol_ata_address

    BoatResult boat_sol_ata_address(const uint8_t wallet[32],
                                    const uint8_t mint[32],
                                    uint8_t ata[32]);

Derive Associated Token Account address (Program Derived Address).
- wallet [in]: Owner wallet public key
- mint [in]: Token mint public key
- ata [out]: 32-byte ATA address

#### boat_sol_spl_transfer

    BoatResult boat_sol_spl_transfer(const uint8_t from_ata[32],
                                     const uint8_t to_ata[32],
                                     const uint8_t owner[32],
                                     uint64_t amount,
                                     BoatSolInstruction *ix);

Build SPL Token Transfer instruction.
- from_ata [in]: Source ATA address
- to_ata [in]: Destination ATA address
- owner [in]: Source account owner (signer)
- amount: Token amount in base units
- ix [out]: Filled instruction (program_id = TOKEN_PROGRAM_ID, data = [3, amount_le])

#### boat_sol_spl_create_ata

    BoatResult boat_sol_spl_create_ata(const uint8_t payer[32],
                                       const uint8_t wallet[32],
                                       const uint8_t mint[32],
                                       BoatSolInstruction *ix);

Build CreateAssociatedTokenAccount instruction.
- payer [in]: Transaction fee payer (signer, writable)
- wallet [in]: Wallet that will own the new ATA
- mint [in]: Token mint
- ix [out]: Filled instruction (program_id = ATA_PROGRAM_ID)

### 6.7 Borsh Encoder

#### boat_borsh_init

    BoatResult boat_borsh_init(BoatBorshEncoder *enc, uint8_t *buf, size_t cap);

Initialize Borsh encoder with a user-provided buffer.

#### boat_borsh_write_u8

    BoatResult boat_borsh_write_u8(BoatBorshEncoder *enc, uint8_t val);

Write 1-byte unsigned integer.

#### boat_borsh_write_u32

    BoatResult boat_borsh_write_u32(BoatBorshEncoder *enc, uint32_t val);

Write 4-byte little-endian unsigned integer.

#### boat_borsh_write_u64

    BoatResult boat_borsh_write_u64(BoatBorshEncoder *enc, uint64_t val);

Write 8-byte little-endian unsigned integer.

#### boat_borsh_write_pubkey

    BoatResult boat_borsh_write_pubkey(BoatBorshEncoder *enc,
                                       const uint8_t pubkey[32]);

Write 32-byte public key.

#### boat_borsh_write_bytes

    BoatResult boat_borsh_write_bytes(BoatBorshEncoder *enc,
                                      const uint8_t *data, size_t len);

Write length-prefixed byte array (u32 length + data).

#### boat_borsh_write_string

    BoatResult boat_borsh_write_string(BoatBorshEncoder *enc, const char *str);

Write length-prefixed UTF-8 string (u32 length + string bytes, no null terminator).

#### boat_borsh_len

    size_t boat_borsh_len(const BoatBorshEncoder *enc);

Return current encoded data length.

---

## 7. Payment Module (boat_pay.h)

### 7.1 Common: EIP-712 Types

#### BoatEip712Domain

    typedef struct {
        char     name[64];              // Domain name (e.g., "USD Coin")
        char     version[16];           // Domain version (e.g., "2")
        uint64_t chain_id;              // EIP-155 chain ID
        uint8_t  verifying_contract[20]; // Contract address
    } BoatEip712Domain;

#### BoatEip3009Auth

    typedef struct {
        uint8_t  from[20];         // Payer address
        uint8_t  to[20];           // Payee address
        uint8_t  value[32];        // uint256 big-endian transfer amount
        uint64_t valid_after;      // UNIX timestamp
        uint64_t valid_before;     // UNIX timestamp
        uint8_t  nonce[32];        // bytes32 random nonce
    } BoatEip3009Auth;

### 7.2 EIP-712 Functions

#### boat_eip712_domain_hash

    BoatResult boat_eip712_domain_hash(const BoatEip712Domain *domain,
                                       uint8_t hash[32]);

Compute EIP-712 domain separator hash.
- domain [in]: Domain parameters
- hash [out]: 32-byte domain separator

#### boat_eip712_hash_struct

    BoatResult boat_eip712_hash_struct(const uint8_t *type_hash,
                                       const uint8_t *encoded_data,
                                       size_t data_len,
                                       uint8_t hash[32]);

Compute hashStruct = keccak256(typeHash || encodeData).
- type_hash [in]: 32-byte keccak256 of type string
- encoded_data [in]: ABI-encoded struct fields
- data_len: Length of encoded_data

#### boat_eip712_sign

    BoatResult boat_eip712_sign(const uint8_t domain_hash[32],
                                const uint8_t struct_hash[32],
                                const BoatKey *key,
                                uint8_t sig65[65]);

Sign EIP-712 typed data: keccak256(0x19 0x01 || domainSeparator || structHash).
- sig65 [out]: 65-byte signature (r[32] || s[32] || v[1])

### 7.3 EIP-3009 Function

#### boat_eip3009_sign

    BoatResult boat_eip3009_sign(const BoatEip3009Auth *auth,
                                 const BoatEip712Domain *domain,
                                 const BoatKey *key,
                                 uint8_t sig65[65]);

Sign an EIP-3009 TransferWithAuthorization.
- auth [in]: Authorization parameters (from, to, value, validAfter, validBefore, nonce)
- domain [in]: EIP-712 domain (typically USDC contract)
- key [in]: Signing key (secp256k1)
- sig65 [out]: 65-byte recoverable signature

### 7.4 x402 Protocol (requires BOAT_PAY_X402_ENABLED)

#### BoatHttpMethod

    typedef enum {
        BOAT_HTTP_GET  = 0,
        BOAT_HTTP_POST = 1
    } BoatHttpMethod;

#### BoatX402ReqOpts

    typedef struct {
        BoatHttpMethod method;            // GET or POST
        const char    *content_type;      // Content-Type for POST (NULL for GET)
        const uint8_t *body;              // Request body for POST (NULL for GET)
        size_t         body_len;          // Body length
        const char    *extra_headers;     // App headers, "Key: Val\r\n..." (NULL if none)
    } BoatX402ReqOpts;

Options describing the original HTTP request from the application. These are replayed
in the second request (with X-Payment header added). Pass NULL for opts in all x402
functions for a simple GET with no extra headers.

#### BoatX402PaymentReq

    typedef struct {
        int      x402_version;          // 1 or 2
        char     scheme[16];            // "exact"
        char     network[64];           // v1: "base-sepolia", v2: "eip155:84532"
        char     amount_str[32];        // Amount as decimal string
        uint8_t  pay_to[20];            // Payee address
        char     pay_to_hex[43];        // "0x..." checksum address for v2 echo-back
        uint8_t  asset[20];             // Token contract address (chain-specific)
        char     asset_hex[43];         // "0x..." for v2 echo-back
        uint32_t max_timeout;           // Max timeout in seconds
        char     resource_url[512];     // Resource URL
        char     asset_name[64];        // EIP-712 domain name from extra.name
        char     asset_version[16];     // EIP-712 domain version from extra.version
        uint8_t  verifying_contract[20]; // EIP-712 verifyingContract from extra
        bool     has_verifying_contract; // true if extra.verifyingContract was present
    } BoatX402PaymentReq;

#### boat_x402_request

    BoatResult boat_x402_request(const char *url, const BoatX402ReqOpts *opts,
                                 BoatX402PaymentReq *req,
                                 uint8_t **response, size_t *response_len);

Send the application's HTTP request (GET or POST per opts).
- url [in]: Resource URL
- opts [in]: Request options (method, headers, body). NULL for simple GET.
- req [out]: Parsed payment requirements (filled only on 402)
- response [out]: Heap-allocated response body if 2xx (caller must boat_free())
- response_len [out]: Response body length if 2xx
- Returns: BOAT_SUCCESS if 2xx (resource in response, no payment needed),
  BOAT_ERROR_HTTP_402 if payment required (req filled), or other error.

#### boat_x402_make_payment

    BoatResult boat_x402_make_payment(const BoatX402PaymentReq *req,
                                      const BoatKey *key,
                                      const BoatEvmChainConfig *chain,
                                      char **payment_b64);

Sign EIP-3009 payment and produce base64-encoded payment payload.
- req [in]: Payment requirements from boat_x402_request()
- key [in]: Payer signing key
- chain [in]: Chain config (for domain separator chain_id)
- payment_b64 [out]: Heap-allocated base64 string (caller must boat_free())

#### boat_x402_pay_and_get

    BoatResult boat_x402_pay_and_get(const char *url, const BoatX402ReqOpts *opts,
                                     const char *payment_b64,
                                     uint8_t **response, size_t *response_len);

Replay the original request with X-Payment header appended.
- url [in]: Resource URL
- opts [in]: Same request options as the original request (NULL for simple GET)
- payment_b64 [in]: Base64 payment payload from boat_x402_make_payment()
- response [out]: Heap-allocated response body (caller must boat_free())
- response_len [out]: Response body length

#### boat_x402_process

    BoatResult boat_x402_process(const char *url, const BoatX402ReqOpts *opts,
                                 const BoatKey *key, const BoatEvmChainConfig *chain,
                                 uint8_t **response, size_t *response_len);

Convenience function: full x402 flow in one call. Sends the original request; if 2xx,
returns immediately. If 402, signs payment and replays with X-Payment header.

### 7.5 Circle Nanopayments (requires BOAT_PAY_NANO_ENABLED)

#### BoatNanoConfig

    typedef struct {
        uint8_t            gateway_wallet_addr[20];   // Gateway Wallet contract (deposit + balance)
        uint8_t            usdc_addr[20];             // USDC contract address on this chain
        BoatEvmChainConfig chain;                     // Chain config
    } BoatNanoConfig;

#### boat_nano_deposit

    BoatResult boat_nano_deposit(const BoatNanoConfig *config, const BoatKey *key,
                                 const uint8_t amount[32], BoatEvmRpc *rpc,
                                 uint8_t txhash[32]);

Deposit USDC to Gateway Wallet contract (one-time on-chain transaction).
Two on-chain transactions: ERC20 approve(gateway_wallet, amount) + Gateway deposit(token, amount).
- amount [in]: Deposit amount (uint256 big-endian)
- rpc [in]: EVM RPC context
- txhash [out]: Transaction hash of the deposit() call

#### boat_nano_authorize

    BoatResult boat_nano_authorize(const BoatNanoConfig *config, const BoatKey *key,
                                   const uint8_t to[20], const uint8_t amount[32],
                                   const uint8_t nonce[32],
                                   BoatEip3009Auth *auth_out, uint8_t sig65[65]);

Sign an off-chain EIP-3009 payment authorization using the batch scheme.
- to [in]: Payee (merchant) address
- amount [in]: Payment amount (uint256 big-endian)
- nonce [in]: 32-byte random nonce
- auth_out [out]: Filled authorization struct
- sig65 [out]: 65-byte signature
- EIP-712 domain: { name: "GatewayWalletBatched", version: "1", verifyingContract: gateway_wallet }

#### boat_nano_get_balance

    BoatResult boat_nano_get_balance(const BoatNanoConfig *config,
                                     const uint8_t addr[20],
                                     BoatEvmRpc *rpc, uint8_t balance[32]);

Query Gateway Wallet on-chain balance via availableBalance(token, depositor).
- addr [in]: Account address
- rpc [in]: EVM RPC connection
- balance [out]: Balance (uint256 big-endian)

#### boat_nano_pay

    BoatResult boat_nano_pay(const char *url, const BoatX402ReqOpts *opts,
                             const BoatNanoConfig *config, const BoatKey *key,
                             uint8_t **response, size_t *response_len);

Full x402 nanopayment flow: HTTP GET → 402 challenge → sign EIP-3009 → retry with payment.
The 402 response from a Gateway-enabled seller includes extra.name="GatewayWalletBatched"
and extra.verifyingContract, which is handled automatically.
Requires BOAT_PAY_X402_ENABLED in addition to BOAT_PAY_NANO_ENABLED.
- url [in]: Resource URL (the seller endpoint)
- opts [in]: Optional request options (headers, etc.), may be NULL
- config [in]: Nanopayment configuration
- key [in]: Buyer's signing key
- response [out]: Allocated response body (caller must free)
- response_len [out]: Response body length

### 7.6 Circle Gateway (requires BOAT_PAY_GATEWAY_ENABLED)

#### BoatGatewayConfig

    typedef struct {
        uint8_t            gateway_wallet_addr[20];  // Gateway Wallet contract address
        uint8_t            gateway_minter_addr[20];  // gatewayMint contract on this chain
        uint8_t            usdc_addr[20];            // USDC contract address on this chain
        uint32_t           domain;                   // Circle Gateway domain ID
        char               gateway_api_url[128];     // Gateway API URL (empty = testnet default)
        BoatEvmChainConfig chain;                    // Chain config
    } BoatGatewayConfig;

- gateway_minter_addr: Used for cross-chain mint on destination chain
- domain: Circle Gateway domain ID (e.g. 7 for Polygon, 6 for Base Sepolia)
- gateway_api_url: e.g. "https://gateway-api.circle.com/v1" for mainnet; empty string falls back to testnet

Testnet Gateway Wallet: 0x0077777d7EBA4688BDeF3E311b846F25870A19B9
Testnet Gateway Minter: 0x0022222ABE238Cc2C7Bb1f21003F0a260052475B
Mainnet Gateway Wallet (Polygon): 0x77777777Dcc4d5A8B6E418Fd04D8997ef11000eE
Mainnet Gateway Minter (Polygon): 0x2222222d7164433c4C09B0b0D809a9b52C04C205
Mainnet Gateway Wallet: 0x77777777Dcc4d5A8B6E418Fd04D8997ef11000eE

#### BoatGatewayTransferResult

    typedef struct {
        uint8_t mint_txhash[32];
    } BoatGatewayTransferResult;

Result of a cross-chain or same-chain Gateway transfer.
- mint_txhash: Transaction hash of the mint/transfer on the destination chain

#### boat_gateway_deposit

    BoatResult boat_gateway_deposit(const BoatGatewayConfig *config,
                                    const BoatKey *key,
                                    const uint8_t amount[32],
                                    BoatEvmRpc *rpc, uint8_t txhash[32]);

Deposit USDC into Gateway Wallet. Performs two transactions:
1. ERC20 approve(gateway_wallet, amount) on USDC contract
2. Gateway Wallet deposit(address token, uint256 value)
- txhash [out]: Hash of the deposit transaction (second tx)

#### boat_gateway_balance

    BoatResult boat_gateway_balance(const BoatGatewayConfig *config,
                                    const uint8_t addr[20],
                                    BoatEvmRpc *rpc, uint8_t balance[32]);

Query available balance via Gateway Wallet availableBalance(token, depositor).
- balance [out]: Balance (uint256 big-endian)

#### boat_gateway_transfer

    BoatResult boat_gateway_transfer(const BoatGatewayConfig *src_config,
                                     const BoatGatewayConfig *dst_config,
                                     const BoatKey *key,
                                     const uint8_t *recipient,
                                     const uint8_t amount[32],
                                     const uint8_t max_fee[32],
                                     BoatEvmRpc *dst_rpc,
                                     BoatGatewayTransferResult *result);

Transfer USDC via Circle Gateway. Supports three modes:
- Same-chain instant withdrawal (src == dst): withdraws from Gateway to wallet, no delay
- Cross-chain transfer (src != dst): burns on source chain, attests via Gateway API, mints on destination chain

Parameters:
- src_config [in]: Source chain Gateway configuration
- dst_config [in]: Destination chain Gateway configuration (same as src for withdrawal)
- key [in]: Signing key
- recipient [in]: 20-byte EVM destination address, or NULL for self-transfer (sender's own address)
- amount [in]: Transfer amount (uint256 big-endian)
- max_fee [in]: Maximum fee willing to pay (uint256 big-endian, 0 for same-chain)
- dst_rpc [in]: RPC connection to destination chain
- result [out]: Transfer result containing mint transaction hash

#### boat_gateway_trustless_withdraw

    BoatResult boat_gateway_trustless_withdraw(const BoatGatewayConfig *config,
                                               const BoatKey *key,
                                               const uint8_t amount[32],
                                               BoatEvmRpc *rpc, uint8_t txhash[32]);

Emergency withdrawal step 1: calls initiateWithdrawal(token, amount) on Gateway Wallet.
Starts a 7-day delay period. Only needed when Circle's API is unavailable.
- amount: Amount to withdraw (uint256 big-endian)
- txhash [out]: Transaction hash

#### boat_gateway_trustless_complete

    BoatResult boat_gateway_trustless_complete(const BoatGatewayConfig *config,
                                               const BoatKey *key,
                                               BoatEvmRpc *rpc, uint8_t txhash[32]);

Emergency withdrawal step 2: calls withdraw(token) on Gateway Wallet.
Must be called after the 7-day delay from boat_gateway_trustless_withdraw().
- txhash [out]: Transaction hash

#### Instant Withdrawal (Recommended)

For normal withdrawal, use boat_gateway_transfer() with src_config == dst_config
(same-chain transfer). This is the recommended path — instant, no delay, no 7-day wait.
Set max_fee to 0 for same-chain transfers (only gas cost applies).

### 7.4 Circle Gateway — Solana

Guarded by `BOAT_PAY_GATEWAY_ENABLED && BOAT_SOL_ENABLED`.

#### BoatGatewaySolConfig

    typedef struct {
        uint8_t            gateway_wallet_program[32];  // GatewayWallet program ID
        uint8_t            gateway_minter_program[32];  // GatewayMinter program ID
        uint8_t            usdc_mint[32];               // USDC SPL token mint
        uint32_t           domain;                      // Circle Gateway domain ID (5 for Solana)
        char               gateway_api_url[128];        // Gateway API URL (empty = testnet default)
        BoatSolChainConfig chain;                       // Solana chain config
    } BoatGatewaySolConfig;

#### BoatGatewaySolTransferResult

    typedef struct {
        uint8_t signature[64];   // Solana transaction signature
    } BoatGatewaySolTransferResult;

#### BoatGatewaySolDepositInfo

    typedef struct {
        uint64_t available_amount;    // Available balance (raw, 6 decimals)
        uint64_t withdrawing_amount;  // Amount in withdrawal process
        uint64_t withdrawal_slot;     // Slot when withdrawal was initiated
    } BoatGatewaySolDepositInfo;

#### Well-known Program IDs

    extern const uint8_t BOAT_GW_SOL_DEVNET_WALLET[32];
    extern const uint8_t BOAT_GW_SOL_DEVNET_MINTER[32];
    extern const uint8_t BOAT_GW_SOL_MAINNET_WALLET[32];
    extern const uint8_t BOAT_GW_SOL_MAINNET_MINTER[32];
    extern const uint8_t BOAT_GW_SOL_DEVNET_USDC[32];

#### boat_sol_find_pda

    BoatResult boat_sol_find_pda(const uint8_t *seeds[], const size_t seed_lens[],
                                 size_t num_seeds, const uint8_t program_id[32],
                                 uint8_t pda[32]);

Generic Solana PDA (Program Derived Address) derivation. Equivalent to
Solana's `findProgramAddress(seeds, programId)`.
- seeds [in]: Array of seed byte pointers
- seed_lens [in]: Array of seed lengths
- num_seeds [in]: Number of seeds
- program_id [in]: 32-byte program ID
- pda [out]: Derived 32-byte PDA

#### boat_gateway_sol_deposit

    BoatResult boat_gateway_sol_deposit(const BoatGatewaySolConfig *config,
                                        const BoatKey *key, uint64_t amount,
                                        BoatSolRpc *rpc, uint8_t sig[64]);

Deposit USDC into Gateway Wallet on Solana. Builds a single transaction with
the deposit instruction (discriminator [22,0]).
- config [in]: Solana Gateway configuration
- key [in]: Ed25519 signing key (payer and owner)
- amount [in]: Amount in raw units (1 USDC = 1000000)
- rpc [in]: Solana RPC connection
- sig [out]: 64-byte transaction signature

#### boat_gateway_sol_balance

    BoatResult boat_gateway_sol_balance(const BoatGatewaySolConfig *config,
                                        const uint8_t owner[32],
                                        BoatSolRpc *rpc,
                                        BoatGatewaySolDepositInfo *info);

Query Gateway deposit balance for an owner on Solana (on-chain).
- owner [in]: 32-byte owner pubkey
- info [out]: Deposit info (available, withdrawing, withdrawal_slot).
  All zeros if deposit account does not exist.

#### boat_gateway_sol_api_balance

    BoatResult boat_gateway_sol_api_balance(const BoatGatewaySolConfig *config,
                                            const uint8_t owner[32],
                                            uint64_t *available);

Query Gateway balance via Circle Gateway REST API.
- owner [in]: 32-byte owner pubkey
- available [out]: Available balance in raw units (1 USDC = 1000000)
- Returns: BOAT_SUCCESS or error. Uses POST to /v1/balances endpoint.

This returns the off-chain view that the Gateway protocol uses for transfer decisions,
which may differ from the on-chain balance during pending operations.

#### boat_gateway_sol_transfer

    BoatResult boat_gateway_sol_transfer(const BoatGatewaySolConfig *src_config,
                                         const BoatGatewaySolConfig *dst_config,
                                         const BoatKey *key,
                                         const uint8_t *recipient,
                                         uint64_t amount, uint64_t max_fee,
                                         BoatSolRpc *dst_rpc,
                                         BoatGatewaySolTransferResult *result);

Transfer USDC via Circle Gateway on Solana. Supports:
- Same-chain instant withdrawal (src == dst): set max_fee = 0
- Cross-chain Solana-to-Solana transfer (different configs)

Flow: encode binary BurnIntent -> Ed25519 sign -> POST to Gateway API ->
parse ReducedMintAttestation -> build gatewayMint instruction -> send on destination.
- recipient [in]: 32-byte recipient pubkey (NULL for self-transfer)
- amount [in]: Transfer amount (raw u64)
- max_fee [in]: Maximum fee (raw u64, 0 for same-chain)
- result [out]: Transaction signature on destination chain

#### boat_gateway_sol_trustless_withdraw

    BoatResult boat_gateway_sol_trustless_withdraw(const BoatGatewaySolConfig *config,
                                                   const BoatKey *key, uint64_t amount,
                                                   BoatSolRpc *rpc, uint8_t sig[64]);

Emergency withdrawal step 1 on Solana. Initiates withdrawal with delay period.
Only needed when Circle's API is unavailable.

#### boat_gateway_sol_trustless_complete

    BoatResult boat_gateway_sol_trustless_complete(const BoatGatewaySolConfig *config,
                                                   const BoatKey *key,
                                                   BoatSolRpc *rpc, uint8_t sig[64]);

Emergency withdrawal step 2 on Solana. Completes withdrawal after delay period.

### 7.5 Circle Gateway — Cross-chain (EVM <-> Solana)

Guarded by `BOAT_PAY_GATEWAY_ENABLED && BOAT_EVM_ENABLED && BOAT_SOL_ENABLED`.

#### boat_gateway_transfer_evm_to_sol

    BoatResult boat_gateway_transfer_evm_to_sol(
        const BoatGatewayConfig    *src_config,
        const BoatGatewaySolConfig *dst_config,
        const BoatKey *evm_key,
        const BoatKey *sol_key,
        const uint8_t *sol_recipient,
        const uint8_t amount[32],
        const uint8_t max_fee[32],
        BoatSolRpc *dst_rpc,
        BoatGatewaySolTransferResult *result);

Cross-chain transfer: EVM -> Solana.
- src_config [in]: EVM source chain Gateway config
- dst_config [in]: Solana destination Gateway config
- evm_key [in]: secp256k1 key (signs EIP-712 BurnIntent on EVM)
- sol_key [in]: Ed25519 key (signs mint transaction on Solana)
- sol_recipient [in]: 32-byte Solana pubkey of recipient, or NULL for self-transfer (sol_key's address)
- amount [in]: uint256 big-endian (EVM source amount)
- max_fee [in]: uint256 big-endian
- dst_rpc [in]: Solana RPC connection
- result [out]: Solana transaction signature

#### boat_gateway_transfer_sol_to_evm

    BoatResult boat_gateway_transfer_sol_to_evm(
        const BoatGatewaySolConfig *src_config,
        const BoatGatewayConfig    *dst_config,
        const BoatKey *sol_key,
        const BoatKey *evm_key,
        const uint8_t *evm_recipient,
        uint64_t amount,
        uint64_t max_fee,
        BoatEvmRpc *dst_rpc,
        BoatGatewayTransferResult *result);

Cross-chain transfer: Solana -> EVM.
- src_config [in]: Solana source Gateway config
- dst_config [in]: EVM destination Gateway config
- sol_key [in]: Ed25519 key (signs binary BurnIntent on Solana)
- evm_key [in]: secp256k1 key (signs mint transaction on EVM)
- evm_recipient [in]: 20-byte EVM destination address, or NULL for self-transfer (evm_key's address)
- amount [in]: u64 raw amount (Solana source)
- max_fee [in]: u64 raw max fee
- dst_rpc [in]: EVM RPC connection
- result [out]: EVM mint transaction hash

---

## 8. Compile-Time Configuration Macros

| Macro | Default | Description |
|-------|---------|-------------|
| BOAT_EVM_ENABLED | 1 | Enable EVM module |
| BOAT_SOL_ENABLED | 1 | Enable Solana module |
| BOAT_PAY_X402_ENABLED | 0 | Enable x402 payment |
| BOAT_PAY_NANO_ENABLED | 0 | Enable Circle Nanopayments |
| BOAT_PAY_GATEWAY_ENABLED | 0 | Enable Circle Gateway |
| BOAT_KEY_SE_ENABLED | 0 | Enable Secure Element backend |
| BOAT_KEY_TEE_ENABLED | 0 | Enable TEE backend |
| BOAT_LOG_LEVEL | BOAT_LOG_NORMAL | Default log verbosity |

Set via compiler -D flags, CMake options, or Kconfig.

---

## 9. Memory Ownership Summary

| Function | Allocates | Caller Frees With |
|----------|-----------|-------------------|
| boat_key_generate/import_raw/load | BoatKey* | boat_key_free() |
| boat_evm_tx_set_data | tx.data | boat_free(tx.data) |
| boat_evm_tx_sign | raw_out | boat_free() |
| boat_evm_abi_encode_func | calldata | boat_evm_abi_free() |
| boat_evm_eth_call | result | boat_free() |
| boat_evm_abi_decode_bytes | out | boat_free() |
| boat_sol_tx_sign | raw | boat_free() |
| boat_x402_make_payment | payment_b64 | boat_free() |
| boat_x402_pay_and_get | response | boat_free() |
| boat_x402_process | response | boat_free() |
| boat_sol_rpc_call | result_json | boat_free() |
| boat_gateway_deposit | — (fills txhash[32]) | n/a |
| boat_gateway_balance | — (fills balance[32]) | n/a |
| boat_gateway_transfer | — (fills result struct) | n/a |
| boat_gateway_sol_deposit | — (fills sig[64]) | n/a |
| boat_gateway_sol_balance | — (fills info struct) | n/a |
| boat_gateway_sol_api_balance | — (fills uint64_t) | n/a |
| boat_gateway_sol_transfer | — (fills result struct) | n/a |
| boat_gateway_transfer_evm_to_sol | — (fills result struct) | n/a |
| boat_gateway_transfer_sol_to_evm | — (fills result struct) | n/a |

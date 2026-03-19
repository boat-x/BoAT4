# BoAT v4 High-Level Design

**Document:** High-Level Design
**Project:** BoAT v4 SDK
**Author:** TLAY.io
**Date:** March 2026

---

## 1. Overview

BoAT v4 is a lightweight, modular C-language blockchain SDK for resource-constrained IoT
devices. It enables IoT devices to interact with EVM-compatible blockchains (Ethereum, Base,
Polygon, Arbitrum, etc.) and Solana, with built-in support for machine payment protocols
(x402, Circle Nanopayments, Circle Gateway).

### 1.1 Design Goals

- Minimal footprint: Target < 250 kB flash, < 15 kB RAM on ARM Cortex-M4
- Composable: Application picks only the modules it needs; no monolithic framework
- Portable: Two-tier PAL makes porting to new platforms trivial
- Secure: Opaque key handles, secure wipe, hardware backend support
- Build-system agnostic: Integrates with CMake, GNU Make, Kconfig, or raw file lists

### 1.2 Supported Chains and Protocols

| Category | Chains / Protocols |
|----------|--------------------|
| EVM | Ethereum, Base, Polygon, Arbitrum, BNB Chain, peaq, any EVM-compatible |
| Solana | Solana mainnet, devnet, testnet |
| Payment | x402 (Coinbase), Circle Nanopayments, Circle Gateway |

---

## 2. Architecture Overview

The architecture follows a composable module pattern. The application directly uses the
modules it needs without going through intermediate layers.

### Module Stack (top to bottom)

    Application
        |
    +---+---+---+---+---+
    |   |   |   |   |   |
    EVM SOL PAY KEY UTIL
    |   |   |   |   |
    +---+---+---+---+
            |
          PAL
            |
      Third-Party
    (trezor-crypto, cJSON)

### 2.1 Module Independence

Each module is independently compilable. Dependencies flow downward only:

- EVM Module depends on: Core (RPC, Key, Util), Third-party (cJSON, trezor-crypto)
- Solana Module depends on: Core (RPC, Key, Util), Third-party (cJSON, trezor-crypto)
- Payment Module depends on: Core (Key, Util), EVM Module, Third-party
- Core depends on: PAL, Third-party

No module depends on another module at the same level. EVM does not depend on Solana.
Payment depends on EVM (because EIP-3009 is EVM-specific) but not on Solana.

---

## 3. Directory Structure

    BoAT4/
      include/              Public API headers (6 files)
        boat.h              Master header: types, errors, logging, config, buffer
        boat_key.h          Key management API (opaque handle)
        boat_pal.h          Platform abstraction API
        boat_evm.h          EVM chain API (RPC, tx builder, ABI codec)
        boat_sol.h          Solana chain API (RPC, tx builder, SPL, Borsh)
        boat_pay.h          Payment protocols API (EIP-712, x402, Nano, Gateway)
      src/
        core/               Chain-agnostic foundation (4 files)
          boat_util.c       Hex/bin conversion, logging, dynamic buffer
          boat_key.c        Key handle lifecycle, dispatch, storage
          boat_key_soft.c   Software crypto backend (trezor-crypto)
          boat_rpc.c        Generic JSON-RPC client
        evm/                EVM-compatible chains (3 files)
          evm_rpc.c         Typed EVM RPC methods
          evm_tx.c          Transaction builder + inline RLP encoder
          evm_abi.c         ABI encoder/decoder
        sol/                Solana (5 files)
          sol_rpc.c         Solana RPC methods + base58/base64
          sol_tx.c          Transaction builder + compact-array encoding
          sol_spl.c         SPL Token helpers + ATA derivation
          sol_ix.c          Generic instruction builder
          sol_borsh.c       Borsh encoder
        pay/                Payment protocols (4 files)
          pay_common.c      EIP-712 domain hash, EIP-3009 signing
          pay_x402.c        x402 HTTP 402 challenge-response
          pay_nano.c        Circle Nanopayments
          pay_gateway.c     Circle Gateway deposit/withdraw
        pal/                Platform ports
          linux/
            pal_linux.c     Linux reference (POSIX + libcurl)
      third-party/          Vendored dependencies
        crypto/             trezor-crypto
        cJSON/              cJSON parser
      tools/
        abi2c.py            ABI-to-C codec generator
      examples/             5 example programs
      CMakeLists.txt        CMake build
      boat.mk              GNU Make fragment
      sources.txt           Flat source list
      Kconfig               Menuconfig entries

---

## 4. Core Module Design

### 4.1 Types and Error Codes (boat.h)

boat.h is the master header included by all other headers. It provides:

- Standard C types via stdint.h (no custom BUINT8 typedefs)
- BoatResult (int32_t) error codes organized in category ranges
- Logging macros: BOAT_LOG(level, fmt, ...) wrapping printf, overridable via PAL
- Configuration macros: BOAT_EVM_ENABLED, BOAT_SOL_ENABLED, BOAT_PAY_*_ENABLED
- BoatBuf dynamic buffer struct with init/append/free operations
- BoatKeyType enum shared between key management and chain modules

### 4.2 Platform Abstraction Layer (boat_pal.h)

Two-tier design:

Tier 1 - Mandatory Primitives (every platform implements these):
- boat_malloc / boat_free -- memory allocation
- boat_storage_write / boat_storage_read / boat_storage_delete -- persistent storage
- boat_time_ms -- monotonic clock
- boat_sleep_ms -- delay
- boat_random -- cryptographic random bytes
- boat_mutex_init / lock / unlock / destroy -- thread safety

Tier 2 - Service Contracts (function pointer structs):
- BoatHttpOps: post(), get(), free_response() function pointers
- boat_set_http_ops() / boat_get_http_ops() -- register and retrieve HTTP implementation
- Linux port provides curl-based default via boat_pal_linux_default_http_ops()

The function pointer approach bridges the gap between synchronous SDK calls and
platform-specific async/callback HTTP implementations. The port implementor wraps their
HTTP stack to present a synchronous interface.

### 4.3 Key Management (boat_key.h)

Opaque BoatKey handle pattern:

- Application receives BoatKey* pointer; internal struct is hidden in boat_key.c
- Creation: boat_key_generate(type), boat_key_import_raw(type, privkey, len),
  boat_key_import_mnemonic(mnemonic, path, type), boat_key_from_se(type, slot)
- Signing: boat_key_sign(key, hash, hash_len, sig, sig_len) for standard signatures,
  boat_key_sign_recoverable(key, hash32, sig65) for EVM ecrecover-compatible signatures
- Info: boat_key_get_info(key, info) returns public key and derived address
- Storage: boat_key_save(key, name), boat_key_load(name, type), boat_key_delete(name)
- Cleanup: boat_key_free(key) zeroes and frees all key material

Supported key types:
- BOAT_KEY_TYPE_SECP256K1 -- EVM chains (65-byte uncompressed pubkey, 20-byte address)
- BOAT_KEY_TYPE_SECP256R1 -- WebAuthn/FIDO2 compatibility
- BOAT_KEY_TYPE_ED25519 -- Solana (32-byte pubkey = address)

### 4.4 Generic RPC Client (boat_rpc.c)

Minimal JSON-RPC 2.0 client:
- Composes request: {"jsonrpc":"2.0","method":"...","params":...,"id":N}
- POSTs via BoatHttpOps.post()
- Parses response with cJSON: extracts "result" or reports "error"
- Returns result as string (caller parses chain-specific format)

Both EVM and Solana RPC modules build on this common client.

### 4.5 Utilities (boat_util.c)

- boat_hex_to_bin / boat_bin_to_hex -- hex string conversion with 0x prefix handling
- BoatBuf operations -- dynamic buffer with doubling growth strategy
- Logging global -- g_boat_log_level controls runtime log verbosity

---

## 5. EVM Module Design

### 5.1 Chain Configuration

BoatEvmChainConfig captures all per-chain differences:
- chain_id (uint64_t) -- used in EIP-155 replay protection
- rpc_url (char[256]) -- JSON-RPC endpoint
- eip1559 (bool) -- enables type-2 transaction encoding

Preset macros provide convenience: BOAT_EVM_BASE_MAINNET, BOAT_EVM_ETH_MAINNET, etc.
Applications can also construct configs at runtime for any EVM chain.

### 5.2 RPC Methods (evm_rpc.c)

Thin typed wrappers over the generic RPC client:
- boat_evm_block_number -- returns uint64_t
- boat_evm_get_balance -- returns uint256 (32-byte big-endian)
- boat_evm_get_nonce -- returns uint64_t
- boat_evm_gas_price -- returns uint256
- boat_evm_send_raw_tx -- sends raw signed transaction, returns txhash
- boat_evm_eth_call -- calls contract view function, returns decoded bytes

Each method formats the JSON params, calls boat_rpc_call(), and parses the hex result.

### 5.3 Transaction Builder (evm_tx.c)

Step-by-step transaction construction:

1. boat_evm_tx_init(tx, chain) -- zero-init, set chain_id
2. boat_evm_tx_set_to/value/data/nonce/gas_price/gas_limit -- set fields
3. boat_evm_tx_auto_fill(tx, rpc, key) -- query nonce + gas_price from chain
4. boat_evm_tx_sign(tx, key, raw_out, raw_len) -- RLP encode, keccak256, sign, re-encode
5. boat_evm_tx_send(tx, key, rpc, txhash) -- sign + send_raw_tx

The RLP encoder is self-contained (~100 lines) within evm_tx.c. It supports:
- String encoding (single byte, short string, long string)
- Integer encoding (big-endian, no leading zeros)
- uint256 encoding (32-byte big-endian, strip leading zeros)
- List wrapping (length prefix for concatenated encoded items)

EIP-155 replay protection: signing hash includes chain_id, 0, 0 as the last three RLP
fields. Final transaction v = recovery_id + chain_id * 2 + 35.

### 5.4 ABI Codec (evm_abi.c)

Encoder for static types (32-byte slots):
- boat_evm_abi_encode_uint256 / uint64 / address / bool

Encoder for dynamic types (length-prefixed, padded):
- boat_evm_abi_encode_bytes / string -- writes to BoatBuf

Function call encoder:
- boat_evm_abi_encode_func(func_sig, args, arg_lens, n_args, calldata, len)
- Computes selector: keccak256(func_sig)[0:4]
- Concatenates selector + encoded args

Decoder:
- boat_evm_abi_decode_uint256 / address / bool -- read from offset
- boat_evm_abi_decode_bytes -- follows offset pointer, reads length + data

### 5.5 ABI Code Generator (tools/abi2c.py)

Python script that reads Solidity ABI JSON and generates C encode/decode functions.
Generated functions call boat_evm_abi_encode_* / boat_evm_abi_decode_* primitives.
Output is pure codec -- no I/O, no wallet, no RPC calls.

---

## 6. Solana Module Design

### 6.1 Chain Configuration

BoatSolChainConfig:
- rpc_url (char[256]) -- JSON-RPC endpoint
- commitment (enum) -- finalized, confirmed, or processed

### 6.2 RPC Methods (sol_rpc.c)

Solana JSON-RPC uses [param, {config}] style params (different from EVM flat arrays):
- boat_sol_rpc_get_latest_blockhash -- returns 32-byte blockhash + lastValidBlockHeight
- boat_sol_rpc_get_balance -- returns lamports (uint64_t)
- boat_sol_rpc_get_token_balance -- returns SPL token amount + decimals
- boat_sol_rpc_send_transaction -- base64-encodes signed tx, returns 64-byte signature
- boat_sol_rpc_get_signature_status -- returns confirmed/finalized status

Includes self-contained base58 encoder/decoder and base64 encoder/decoder for Solana
address and transaction encoding.

### 6.3 Transaction Builder (sol_tx.c)

Solana transaction format:
- Compact-array encoding for counts (signatures, accounts, instructions)
- Message: header (num_signers, num_readonly_signed, num_readonly_unsigned) +
  account keys + recent_blockhash + instructions
- Each instruction: program_id_index + account_indices + data (all compact-array)

Key features:
- Automatic account deduplication: adding the same pubkey twice upgrades permissions
- Account sorting: signers+writable first, then signers+readonly, then non-signer+writable,
  then non-signer+readonly. Fee payer is always index 0.
- Transaction size check: rejects if serialized size > 1232 bytes (Solana MTU)

### 6.4 SPL Token (sol_spl.c)

- boat_sol_ata_address(wallet, mint, ata) -- PDA derivation via SHA-256
- boat_sol_spl_transfer(from_ata, to_ata, owner, amount, ix) -- builds Transfer instruction
  (program_id = TOKEN_PROGRAM_ID, data = [3, amount_u64_le])
- boat_sol_spl_create_ata(payer, wallet, mint, ix) -- builds CreateAssociatedTokenAccount

Well-known program IDs defined as constants:
- BOAT_SOL_SYSTEM_PROGRAM_ID
- BOAT_SOL_TOKEN_PROGRAM_ID
- BOAT_SOL_ATA_PROGRAM_ID

### 6.5 Generic Instruction Builder (sol_ix.c)

For custom program interactions:
- boat_sol_ix_init(ix, program_id)
- boat_sol_ix_add_account(ix, pubkey, is_signer, is_writable)
- boat_sol_ix_set_data(ix, data, len)

### 6.6 Borsh Encoder (sol_borsh.c)

For Solana program instruction data serialization:
- boat_borsh_write_u8 / u32 / u64 -- little-endian integers
- boat_borsh_write_pubkey -- 32-byte public key
- boat_borsh_write_bytes / string -- length-prefixed data

---

## 7. Payment Module Design

### 7.1 Common Layer (pay_common.c)

All three payment protocols use EIP-3009 transferWithAuthorization, which requires EIP-712
typed data signing. The common layer provides:

EIP-712:
- boat_eip712_domain_hash(domain) -- computes domain separator
- boat_eip712_hash_struct(type_hash, encoded_data, len) -- computes struct hash
- boat_eip712_sign(domain_hash, struct_hash, key) -- computes and signs EIP-712 message

EIP-3009:
- boat_eip3009_sign(auth, domain, key) -- encodes TransferWithAuthorization fields,
  computes hashStruct, and signs via EIP-712

The EIP-3009 type hash is pre-computed as a constant to avoid runtime string hashing.

### 7.2 x402 Protocol (pay_x402.c)

HTTP 402 challenge-response flow:

1. boat_x402_request(url, req) -- HTTP GET, parse 402 JSON response (network, amount,
   payTo, asset, maxTimeoutSeconds)
2. boat_x402_make_payment(req, key, chain, payment_b64) -- compute time bounds, random
   nonce, sign EIP-3009, build JSON payload, base64 encode
3. boat_x402_pay_and_get(url, payment_b64, response, len) -- HTTP GET with X-Payment header
4. boat_x402_process(url, key, chain, response, len) -- convenience combining steps 1-3

Uses non-EIP-155 v values (27/28) per Coinbase x402 specification.

### 7.3 Circle Nanopayments (pay_nano.c)

High-frequency micropayment flow:

1. boat_nano_deposit(config, key, amount, rpc, txhash) -- one-time on-chain USDC deposit
   to Gateway Wallet contract
2. boat_nano_authorize(config, key, to, amount, nonce, auth, sig) -- sign EIP-3009
   authorization off-chain (identical signing to x402)
3. boat_nano_get_balance(config, addr, balance) -- query Gateway REST API

The Gateway validates authorizations in a TEE (AWS Nitro Enclave) and batches thousands
of authorizations into single on-chain settlement transactions.

### 7.4 Circle Gateway (pay_gateway.c)

On-chain USDC treasury management:

1. boat_gateway_deposit(config, key, amount, rpc, txhash) -- ERC20 approve + Gateway
   Wallet deposit (two EVM transactions)
2. boat_gateway_balance(config, addr, rpc, balance) -- call balanceOf via eth_call
3. boat_gateway_transfer(src, dst, key, amount, max_fee, rpc, result) -- instant
   withdrawal (same-chain, src == dst) or cross-chain transfer via Circle Gateway API
4. boat_gateway_trustless_withdraw(config, key, amount, rpc, txhash) -- emergency
   on-chain withdrawal step 1: initiateWithdrawal (starts 7-day delay)
5. boat_gateway_trustless_complete(config, key, rpc, txhash) -- emergency
   on-chain withdrawal step 2: withdraw (after 7-day delay)

Instant withdrawal is a same-chain transfer (src == dst) via the Gateway API.
Cross-chain transfer triggers CCTP burn on source chain; Circle mints on destination.
Trustless withdrawal is only needed when Circle's API is unavailable.

### 7.5 Compile Flags

- BOAT_PAY_X402_ENABLED -- includes pay_common.c + pay_x402.c
- BOAT_PAY_NANO_ENABLED -- includes pay_common.c + pay_nano.c
- BOAT_PAY_GATEWAY_ENABLED -- includes pay_common.c + pay_gateway.c

pay_common.c is auto-included when any payment module is enabled. All payment modules
require the EVM module (EIP-3009 is EVM-specific).

---

## 8. Build System Design

### 8.1 CMakeLists.txt

- Options: BOAT_EVM, BOAT_SOL, BOAT_PAY_X402, BOAT_PAY_NANO, BOAT_PAY_GATEWAY, BOAT_PAL
- Conditional source lists based on options
- ESP-IDF detection: uses idf_component_register() when ESP_PLATFORM is defined
- Standalone: produces libboat4.a static library
- Optional example builds via BOAT_BUILD_EXAMPLES

### 8.2 boat.mk

- GNU Make includable fragment
- Defines BOAT_SRCS, BOAT_INCS, BOAT_CFLAGS
- Conditional source inclusion via ifdef BOAT_EVM, BOAT_SOL, etc.
- Vendor Makefiles include boat.mk and add variables to their build

### 8.3 sources.txt

- Flat list of .c files with conditional [TAG] markers
- Universal fallback for any build system
- Tags: [EVM], [SOL], [PAY_X402], [PAY_NANO], [PAY_GATEWAY], [PAL_LINUX]

### 8.4 Kconfig

- Menuconfig entries for chain selection, payment protocol selection, key backend selection
- Compatible with ESP-IDF and Zephyr menuconfig systems

---

## 9. Third-Party Dependencies

| Library | Purpose | Source |
|---------|---------|--------|
| trezor-crypto | secp256k1, ed25519, keccak256, SHA-256, base58, BIP-32/39 | Copied from BoAT-SupportLayer |
| cJSON | JSON parsing and generation | Copied from BoAT-SupportLayer |

Both libraries are pure C with no external dependencies, making them suitable for
bare-metal and RTOS environments.

---

## 10. Data Flow Examples

### 10.1 Send ETH on Base

    Application:
      key = boat_key_import_raw(SECP256K1, privkey, 32)
      boat_evm_rpc_init(&rpc, "https://mainnet.base.org")
      boat_evm_tx_init(&tx, &base_config)
      boat_evm_tx_set_to(&tx, recipient)
      boat_evm_tx_set_value(&tx, amount)
      boat_evm_tx_auto_fill(&tx, &rpc, key)   // queries nonce + gas_price
      boat_evm_tx_send(&tx, key, &rpc, txhash) // sign + send

    Internal flow:
      auto_fill -> evm_rpc -> boat_rpc -> BoatHttpOps.post() -> curl -> RPC node
      sign -> RLP encode -> keccak256 -> boat_key_sign_recoverable -> ecdsa_sign_digest
      send -> evm_rpc -> boat_rpc -> BoatHttpOps.post() -> curl -> RPC node

### 10.2 x402 Payment

    Application:
      key = boat_key_import_raw(SECP256K1, privkey, 32)
      boat_x402_process(url, key, &chain, &response, &len)

    Internal flow:
      request -> BoatHttpOps.get() -> HTTP 402 -> parse JSON (cJSON)
      make_payment -> EIP-3009 sign -> EIP-712 hash -> keccak256 -> boat_key_sign_recoverable
                   -> build JSON payload -> base64 encode
      pay_and_get -> BoatHttpOps.get() with X-Payment header -> HTTP 200 -> return body

### 10.3 Solana SPL Transfer

    Application:
      key = boat_key_import_raw(ED25519, privkey, 32)
      boat_sol_rpc_init(&rpc, "https://api.mainnet-beta.solana.com")
      boat_sol_ata_address(wallet, mint, sender_ata)
      boat_sol_ata_address(recipient, mint, recipient_ata)
      boat_sol_spl_transfer(sender_ata, recipient_ata, wallet, amount, &ix)
      boat_sol_tx_init(&tx)
      boat_sol_tx_set_fee_payer(&tx, wallet)
      boat_sol_rpc_get_latest_blockhash(&rpc, FINALIZED, blockhash, &height)
      boat_sol_tx_set_blockhash(&tx, blockhash)
      boat_sol_tx_add_instruction(&tx, &ix)
      boat_sol_tx_send(&tx, key, &rpc, sig)

    Internal flow:
      spl_transfer -> builds instruction with TOKEN_PROGRAM_ID + accounts + [3, amount_le]
      tx_sign -> deduplicate accounts -> sort -> serialize message -> ed25519_sign
      tx_send -> base64 encode -> sol_rpc -> boat_rpc -> BoatHttpOps.post()

---

## 11. Porting Guide Summary

To port BoAT v4 to a new platform:

1. Implement Tier 1 functions in a new pal_<platform>.c (~10 functions, mostly trivial)
2. Implement BoatHttpOps (post, get, free_response) using the platform HTTP stack
3. Register HTTP ops via boat_set_http_ops() at initialization
4. Add the new PAL source file to your build system
5. Include BoAT4 sources via CMakeLists.txt, boat.mk, or sources.txt

Typical porting effort: 200-400 lines of C code, 1-2 days for an experienced developer.

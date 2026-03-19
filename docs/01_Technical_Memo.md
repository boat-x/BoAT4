# BoAT v4 Technical Design Memo

**Document:** Technical Design Memo — Architecture Decisions and Rationale
**Project:** BoAT v4 SDK
**Author:** TLAY.io
**Date:** March 2026
**Status:** Implementation Complete (Phase 1-6)

---

## 1. Executive Summary

BoAT (Blockchain of AI Things) is a lightweight C-language blockchain SDK designed for
resource-constrained IoT devices. Since its inception, BoAT has been deployed on 52+ IoT
modules from major chipset vendors (Qualcomm, MediaTek, UNISOC, Fibocom, Quectel, SIMCom,
etc.), with a production footprint of approximately 210 kB flash and 10 kB RAM on ARM
Cortex-M4 class hardware.

BoAT v3 (V3.1.0) served the project well through its initial market phase, but accumulated
significant architectural debt as the blockchain landscape evolved. BoAT v4 is a clean
rewrite that addresses these issues while positioning the SDK for the emerging machine
economy, where IoT devices autonomously transact, pay for resources, and participate in
decentralized networks.

This memo records the technical discussions, design decisions, and rationale behind every
major architectural choice in BoAT v4.

---

## 2. Problems with BoAT v3

### 2.1 Per-Chain Code Duplication

BoAT v3 maintained separate protocol implementations for each blockchain: Ethereum, PlatON,
Quorum, and others. Each chain had its own wallet API, network management, transaction
builder, and RPC interface. In practice, PlatON and Quorum are EVM-compatible chains with
minor differences (address format, bech32 encoding, private transaction fields). The
duplicated code meant:

- **Maintenance burden:** Every bug fix or feature addition had to be replicated across
  chain implementations.
- **Binary bloat:** An application targeting only Ethereum still pulled in PlatON/Quorum
  code paths through shared headers.
- **Slow chain addition:** Adding a new EVM chain (e.g., Base, Polygon, Arbitrum) required
  forking an entire chain implementation rather than just adding a config struct.

**Decision:** Unify all EVM-compatible chains into a single implementation. Per-chain
differences are captured in a BoatEvmChainConfig struct containing chain_id, rpc_url, and
an eip1559 flag. PlatON and Quorum support is dropped as there are no active users on those
chains, and the EVM path covers their functionality for any future need.

### 2.2 Over-Layered Engine Architecture

The v3 engine had five layers that every RPC call traversed:

    Application -> C Contract API -> Wallet -> Protocol -> Web3Intf -> HTTP

Adding a single new RPC method (e.g., eth_gasPrice) required writing functions in three
separate layers (Protocol, Web3Intf, and sometimes Wallet). This layering was designed for
abstraction, but in practice it created:

- **Indirection overhead:** Both in code size and developer cognitive load.
- **Tight coupling:** The Wallet layer mixed key management with transaction building. The
  Protocol layer mixed RPC calls with transaction serialization.
- **Rigidity:** The generated C Contract API was the only way to interact with smart
  contracts, making ad-hoc calls unnecessarily complex.

**Decision:** Replace the layered architecture with three composable modules:

1. **RPC Client** -- Direct JSON-RPC access. Adding a new RPC method is one function in
   one file.
2. **Tx Builder** -- Build, sign, and serialize transactions. Uses RPC Client when it needs
   chain data (nonce, gas price).
3. **Contract Codec** -- Generated from ABI JSON, produces pure encode/decode functions with
   no I/O. The application composes codec output with Tx Builder.

The Wallet and Protocol layers are eliminated. The application directly composes the modules
it needs. This is a toolkit philosophy, not a framework philosophy.

### 2.3 Build System Conflicts

BoAT v3 used a custom config.py script that generated Makefiles. This conflicted with vendor
toolchains:

- ESP-IDF uses CMake with idf_component_register().
- Zephyr uses CMake with Kconfig.
- Module vendors (Fibocom, Quectel) use proprietary Make-based build systems.
- STM32CubeIDE uses CMake or Eclipse-managed builds.

The config.py approach forced every integration to fight the build system rather than work
with it.

**Decision:** BoAT v4 becomes a source library, not a build system. It provides:

- sources.txt -- Flat file list, universal fallback for any build system.
- CMakeLists.txt -- For CMake toolchains. Detects ESP-IDF (ESP_PLATFORM) and uses
  idf_component_register() automatically.
- boat.mk -- Includable GNU Make fragment. Vendor Makefiles just include boat.mk and add
  $(BOAT_SRCS) to their source list.
- Kconfig -- For menuconfig integration in ESP-IDF and Zephyr.

No more config.py. No more generated Makefiles. The vendor build system is always in control.

### 2.4 Tangled Key Management

In v3, key management was spread across multiple layers:

- Key generation and storage in BoAT-SupportLayer/keystore/
- Key indices exposed to the application layer
- Import format (raw, PKCS, mnemonic) propagated through the API
- Software and hardware (SE) backends interleaved in the same code paths

This made it difficult to add new backends (TEE, cloud HSM) and exposed implementation
details to application code.

**Decision:** Opaque BoatKey handle pattern:

- The application never sees private key bytes or key indices after creation.
- Import format is consumed at creation time and never propagated.
- boat_key_sign() dispatches internally based on backend (software, SE, TEE).
- Backend modules (boat_key_soft.c, future boat_key_se.c, boat_key_tee.c) are compile-time
  optional.
- Secure wipe on boat_key_free() -- private key material is zeroed before deallocation.

---

## 3. Strategic Context

### 3.1 Machine Economy Vision

BoAT v4 is designed for the emerging machine economy where IoT devices:

- Pay for resources autonomously (API calls, data feeds, compute, connectivity)
- Earn revenue by providing services (sensor data, compute, bandwidth)
- Hold and manage digital assets (stablecoins, utility tokens)
- Prove their identity cryptographically (Machine DID)

The top three strategic priorities driving v4 design:

1. **Production x402 library** -- Harden the existing C x402 client into a multi-chain,
   multi-stablecoin, enterprise-grade payment module.
2. **Account Abstraction (ERC-4337/ERC-7702) + Paymaster** -- Remove gas token friction
   for machines. A device should pay for API calls in USDC without needing ETH for gas.
3. **Base chain formal support + Machine DID** -- Leverage hardware trust anchors for
   verifiable machine identity on Base (Coinbase ecosystem).

### 3.2 Why These Chains

**EVM (unified):** The dominant smart contract ecosystem. Base (Coinbase L2) is the primary
target for machine payments due to low fees, USDC native support, and x402 protocol origin.
Ethereum mainnet, Polygon, Arbitrum, BNB Chain, and peaq are all covered by the same code
path.

**Solana:** The highest-throughput chain with sub-second finality and sub-cent fees. Ideal
for high-frequency machine-to-machine micropayments. The DePIN ecosystem (Helium, Hivemapper,
Render) runs primarily on Solana. SPL token payments are the Solana equivalent of ERC-20
transfers.

**Dropped chains:** PlatON and Quorum had zero active BoAT users. Their EVM-compatible
features are covered by the unified EVM path. Hyperledger Fabric was never fully integrated
and is architecturally incompatible (permissioned, no client-side signing).

### 3.3 Why Payment Protocols

Payment is the killer use case for IoT blockchain. Three protocols cover the spectrum:

| Protocol | Provider | Settlement | Use Case |
|----------|----------|------------|----------|
| x402 | Coinbase/Cloudflare | Per-request on-chain via facilitator | Pay-per-API-call |
| Circle Nanopayments | Circle | Off-chain ledger, batched on-chain | High-frequency micropayments |
| Circle Gateway | Circle | Cross-chain via CCTP | Treasury management, cross-chain USDC |

All three use EIP-3009 transferWithAuthorization for signing, which is why they share a
common EIP-712/EIP-3009 layer (pay_common.c). This was a deliberate design choice: the
signing logic is identical, only the delivery target differs.

---

## 4. Architecture Decisions in Detail

### 4.1 Composable Modules vs. Layered Architecture

The v3 layered architecture was inspired by traditional networking stacks (OSI model). Each
layer had a well-defined interface to the layer above and below. This works well for
protocols with strict layering (TCP/IP), but blockchain interactions are not strictly layered:

- An application might need raw RPC access (e.g., querying a custom contract view function)
  without going through the Wallet layer.
- A payment protocol (x402) needs to sign EIP-3009 data but never sends a transaction.
- A DePIN application might build custom Solana instructions that do not map to any
  pre-generated contract API.

The composable module approach lets the application pick exactly what it needs:

    // Direct RPC call -- no wallet, no protocol layer
    boat_evm_get_balance(&rpc, addr, balance);

    // Build and send a transaction -- uses RPC internally for nonce/gas
    boat_evm_tx_auto_fill(&tx, &rpc, key);
    boat_evm_tx_send(&tx, key, &rpc, txhash);

    // ABI encode + eth_call -- no transaction needed
    boat_evm_abi_encode_func("balanceOf(address)", args, lens, 1, &calldata, &len);
    boat_evm_eth_call(&rpc, contract, calldata, len, &result, &result_len);

    // x402 payment -- signs EIP-3009, never touches the chain directly
    boat_x402_process(url, key, &chain, &response, &response_len);

Each module has zero knowledge of the others. The RPC client does not know about transactions.
The Tx Builder does not know about ABI encoding. The payment modules do not know about the
Tx Builder (they use EIP-712 signing directly). The application is the composer.

### 4.2 Toolkit Philosophy

A critical design principle: BoAT is a toolkit, not a framework. It never makes business
decisions for the application.

Examples of what this means in practice:

- **Solana blockhash expiry:** The SDK returns BOAT_ERROR_SOL_BLOCKHASH_EXPIRED. It does NOT
  automatically retry. The application decides whether, when, and how to retry. A payment
  application might retry immediately; a sensor logging application might wait for the next
  measurement cycle.

- **Gas estimation:** boat_evm_tx_auto_fill() queries nonce and gas price from the chain.
  But the application can override any field. If you want a specific gas price strategy
  (e.g., 110% of current for faster confirmation), set it yourself.

- **Key storage:** boat_key_save() persists a key. But the SDK does not decide when to save,
  what name to use, or whether to encrypt. The application controls the lifecycle.

- **Error handling:** The SDK returns error codes. It does not retry, does not fall back to
  alternative endpoints, does not log to external services. The application owns error policy.

This philosophy is essential for embedded systems where every byte of flash and every
millisecond of CPU time is accounted for. A framework that "helpfully" retries a failed
transaction might cause a cellular modem to stay awake for an extra 30 seconds, draining
a battery-powered sensor.

### 4.3 Platform Abstraction: Two-Tier Design

The v3 PAL (OSAL + DAL) had too many functions, many of which were rarely used. Porting to
a new platform required implementing dozens of functions, most of which were trivial wrappers.

The v4 PAL has two tiers:

**Tier 1 -- Mandatory Primitives (~10 functions):**
Every platform can implement these trivially because they map directly to OS primitives:

- boat_malloc / boat_free -- wraps stdlib or RTOS heap
- boat_storage_write / boat_storage_read / boat_storage_delete -- file or flash sector
- boat_time_ms -- monotonic clock
- boat_sleep_ms -- delay
- boat_random -- hardware RNG or /dev/urandom
- boat_mutex_init / lock / unlock / destroy -- OS mutex or critical section

**Tier 2 -- Service Contracts (function pointer structs):**
HTTP is the most complex platform dependency (sync vs. async, TLS configuration, proxy
support). Rather than abstracting every possible HTTP feature, v4 defines a BoatHttpOps
struct with three function pointers: post(), get(), free_response().

The platform port fills in these function pointers. On Linux, the default implementation
uses libcurl. On an LTE module, it might use the AT+HTTP commands. On ESP-IDF, it uses
esp_http_client. The BoAT core always calls synchronously through these pointers.

Why function pointers instead of weak symbols or compile-time dispatch? Because a single
application might need multiple HTTP implementations (e.g., one for cellular, one for WiFi)
and switch between them at runtime.

### 4.4 Key Management: Opaque Handle Pattern

The opaque BoatKey handle is inspired by PKCS#11 session objects and OpenSSL EVP_PKEY. The
application receives a pointer to an opaque struct; the internal layout is defined only in
boat_key.c, not in the public header.

Why this matters:

- **Backend flexibility:** The same application code works whether the key is in software
  RAM, a secure element slot, or a TEE enclave. The dispatch happens inside boat_key_sign().
- **No key leakage:** The application cannot accidentally log or transmit private key bytes
  because it never has access to them after import.
- **ABI stability:** The internal struct layout can change between SDK versions without
  breaking application code. Only the public API (boat_key.h) is the contract.

The software backend (boat_key_soft.c) uses trezor-crypto for all cryptographic operations:

- secp256k1: ecdsa_sign_digest(&secp256k1, ...) for EVM transaction signing
- secp256r1: ecdsa_sign_digest(&nist256p1, ...) for WebAuthn/FIDO2 compatibility
- ed25519: ed25519_sign() for Solana transaction signing

Address derivation is chain-specific:
- EVM: keccak256(uncompressed_pubkey[1..64])[12..31] -- 20-byte address
- Solana: raw 32-byte ed25519 public key -- the public key IS the address

Address display is also chain-specific: boat_address_to_string() outputs 0x-prefixed hex
for EVM and base58 for Solana. boat_address_from_string() auto-detects the format.

Private key validation for secp256k1 ensures the key is in range [1, n-1] where n is the
curve order. This prevents subtle signing failures from invalid keys.

**Multi-format key import:** Solana wallets (Solflare, Phantom) export private keys as
base58 strings or JSON byte arrays `[n1,n2,...]`, not hex. The SDK supports all three
formats through dedicated import functions:

- boat_key_import_base58() -- decodes base58, handles 64-byte (seed||pubkey) → 32-byte seed
- boat_key_import_json_array() -- parses `[n1,n2,...]` via cJSON, validates each byte 0-255
- boat_key_import_string() -- auto-detects format (JSON array, base58, or hex) and delegates

All three validate input strictly: wrong element count, out-of-range values, invalid base58
characters, and incorrect decoded lengths are rejected with descriptive log messages.

### 4.5 EVM: Unified Chain Support

All EVM chains share identical:
- RLP transaction encoding (legacy and EIP-1559)
- ABI encoding/decoding
- JSON-RPC method signatures
- ECDSA secp256k1 signing with keccak256 hashing

The only per-chain differences are:
- chain_id (used in EIP-155 replay protection: v = recovery_id + chain_id * 2 + 35)
- RPC endpoint URL
- EIP-1559 support flag (determines tx type: legacy vs. type-2 envelope)
- Gas price ranges (informational, not enforced by SDK)

These differences are captured in BoatEvmChainConfig. Preset macros (BOAT_EVM_BASE_MAINNET,
BOAT_EVM_ETH_MAINNET, etc.) provide convenience, but the application can construct any
config at runtime.

The RLP encoder is self-contained within evm_tx.c (~100 lines). We chose to rewrite it
rather than reuse the v3 boatrlp.c because:
- The v3 RLP encoder depended on boatiotsdk.h (the entire v3 header tree)
- It used dynamic allocation for RLP objects (unnecessary for transaction encoding)
- The new encoder operates directly on BoatBuf, matching the v4 buffer pattern
- Total code is smaller and has zero external dependencies beyond boat.h

### 4.6 Solana: Separate Implementation

Solana is fundamentally different from EVM chains:

- **Account model:** Solana uses an account-based model where programs (smart contracts) are
  stateless and operate on accounts passed as instruction parameters.
- **Transaction format:** Compact-array encoding, not RLP. Message = header + account keys +
  recent blockhash + instructions.
- **Signing:** Ed25519, not ECDSA secp256k1. No recovery ID, no EIP-155.
- **Addresses:** 32-byte public keys encoded as base58, not 20-byte keccak256 hashes.
  The trezor-crypto base58 library (b58enc/b58tobin) is exposed via boat_base58_encode()
  and boat_base58_decode() wrappers in boat_util.c. The Solana RPC module uses these
  wrappers for all base58 encoding/decoding (pubkeys, blockhashes, signatures).
- **Token standard:** SPL Token program, not ERC-20. Associated Token Accounts (ATAs) are
  Program Derived Addresses (PDAs).

Sharing code between EVM and Solana would create more complexity than it saves. The only
shared components are:
- boat_rpc.c (generic JSON-RPC client -- both chains use JSON-RPC)
- boat_key.c (key handle lifecycle -- ed25519 is just another key type)
- boat_util.c (hex/bin conversion, base58 encode/decode, amount conversion, buffer helpers)

Solana support is organized in two tiers:

**Tier 1 (implemented):**
- RPC client with typed methods (getLatestBlockhash, getBalance, sendTransaction, etc.)
- Transaction builder with compact-array encoding and account deduplication
- SPL Token helpers (ATA derivation, transfer instruction, create-ATA instruction)

**Tier 2 (implemented):**
- Generic instruction builder (boat_sol_ix_init/add_account/set_data)
- Borsh encoder (boat_borsh_write_u8/u32/u64/pubkey/bytes/string)

**Not implemented (future):**
- Tier 3: IDL-based code generation (Anchor framework compatibility)
- Durable nonces (deliberately omitted -- see toolkit philosophy)

### 4.7 Payment Protocols: Shared EIP-3009 Layer

The key insight driving the payment module architecture: x402, Circle Nanopayments, and
Circle Gateway all use EIP-3009 transferWithAuthorization for payment signing. The signing
process is identical:

1. Construct EIP-712 domain separator for the USDC contract
2. Encode EIP-3009 TransferWithAuthorization struct (from, to, value, validAfter,
   validBefore, nonce)
3. Compute hashStruct = keccak256(typeHash || encodeData)
4. Compute signing hash = keccak256(0x19 0x01 || domainSeparator || hashStruct)
5. Sign with secp256k1 recoverable signature

This shared logic lives in pay_common.c. Each payment protocol module adds only its
protocol-specific flow:

- **pay_x402.c:** HTTP 402 challenge parsing, X-Payment header construction, base64 encoding
- **pay_nano.c:** Gateway API interaction, deposit transaction, authorization delivery
- **pay_gateway.c:** On-chain deposit/withdraw via Gateway Wallet contract

The EIP-3009 type hash is pre-computed as a constant:
keccak256("TransferWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)")

This avoids runtime string hashing on every payment operation.

x402 uses non-EIP-155 v values (27/28) per the Coinbase x402 specification, while on-chain
EVM transactions use EIP-155 v values (chain_id * 2 + 35 + recovery_id). This distinction
is handled in the respective signing paths.

---

## 5. Reuse vs. Rewrite Decisions

Every component was evaluated for reuse from v3 or the x402-client-c codebase. The decision
framework was:

- **Copy as-is** if the component is self-contained and has no v3 header dependencies.
- **Reference and rewrite** if the component has good logic but bad dependencies or API.
- **Write from scratch** if the component does not exist or is fundamentally different.

| Component | Source | Decision | Rationale |
|-----------|--------|----------|-----------|
| trezor-crypto | BoAT-SupportLayer/third-party/crypto | Copy as-is | Self-contained, no BoAT dependencies, battle-tested |
| cJSON | BoAT-SupportLayer/third-party/cJSON | Copy as-is | Single .c/.h, no dependencies. Can be excluded via -DBOAT_VENDOR_CJSON=ON when the IoT platform already provides cJSON (see §5.1) |
| RLP encoder | BoAT-SupportLayer/third-party/rlp | Rewrite | Depended on boatiotsdk.h; new version is ~100 lines inline in evm_tx.c |
| JSON-RPC client | BoAT-Engine/protocol/common/web3intf | Reference, rewrite | Good pattern but too many layers; boat_rpc.c is one file, ~100 lines |
| EVM tx building | BoAT-Engine/protocol/boatethereum | Reference, rewrite | RLP field ordering and EIP-155 logic reused; removed wallet coupling |
| Software signing | BoAT-SupportLayer/keystore/soft | Reference, rewrite | trezor-crypto call patterns reused; removed key index system |
| Hex/bin utilities | BoAT-SupportLayer/common/utilities | Reference, rewrite | Simpler API, no TRIMBIN modes, no BUINT8 typedefs |
| ABI codegen | BoAT-Engine/tools/eth2c.py | Reference, rewrite | New abi2c.py generates codec-only output (no I/O, no wallet calls) |
| curl HTTP | BoAT-SupportLayer/platform/linux-default | Reference, rewrite | Rewritten as BoatHttpOps implementation in pal_linux.c |
| EIP-712/3009 | x402-client-c/eip-3009.c | Reference, rewrite | Field encoding order and domain separator logic reused; cleaner API |
| x402 HTTP flow | x402-client-c/x402-demo.c | Reference, rewrite | 402 parsing and X-Payment header logic reused; uses BoatHttpOps |

The trezor-crypto library deserves special mention. It provides secp256k1, ed25519-donna,
keccak256, SHA-256, base58, BIP-32, and BIP-39 in pure C with no external dependencies.
It has been audited and is used in production by Trezor hardware wallets. There is no reason
to rewrite any of it. The base58 functions (b58enc/b58tobin) are wrapped by boat_base58_encode()
and boat_base58_decode() in boat_util.c to provide a consistent BoatResult-based API.

### 5.1 Vendor-Provided cJSON

Many IoT development suites (e.g., Qualcomm QCS, MediaTek LinkIt, ESP-IDF) bundle their own
copy of cJSON. Compiling the bundled cJSON alongside the vendor's copy causes symbol conflicts
(duplicate definitions of cJSON_Parse, cJSON_Delete, etc.) at link time.

The CMake option `-DBOAT_VENDOR_CJSON=ON` addresses this:

- Excludes `third-party/cJSON/cJSON.c` from the BoAT build
- Removes `third-party/cJSON/` from the include path, so the bundled `cJSON.h` is never used
- The vendor's cJSON header and library are picked up from the platform's own include/link paths

Usage:

    cmake -DBOAT_VENDOR_CJSON=ON ...

The SDK uses only standard cJSON API (cJSON_Parse, cJSON_IsArray, cJSON_GetArraySize,
cJSON_GetArrayItem, cJSON_GetObjectItem, cJSON_IsString, cJSON_IsNumber, cJSON_IsNull,
cJSON_Delete). Any cJSON version providing these symbols is compatible.

---

## 6. Type System Decisions

### 6.1 No Custom Integer Types

BoAT v3 defined custom types: BUINT8, BUINT16, BUINT32, BUINT64, BCHAR, BBOOL, etc. These
added no value over stdint.h types and confused developers who expected standard C types.

BoAT v4 uses stdint.h directly: uint8_t, uint16_t, uint32_t, uint64_t, bool (from
stdbool.h), size_t (from stddef.h). The only custom type is BoatResult (typedef int32_t)
for error codes.

### 6.2 Error Code Ranges

Error codes are negative integers organized by category:

- 0: BOAT_SUCCESS
- -1: BOAT_ERROR (generic)
- -100..-199: Argument errors (null, invalid, out of range)
- -200..-299: Memory errors (alloc, overflow)
- -300..-399: RPC errors (fail, parse, timeout, server)
- -400..-499: Key errors (gen, import, sign, not found, type mismatch)
- -500..-599: Storage errors (write, read, not found)
- -600..-699: EVM errors (RLP, ABI, tx, nonce)
- -700..-799: Solana errors (blockhash expired, insufficient funds, tx too large)
- -800..-899: HTTP errors (fail, 402)

This categorization allows applications to handle errors at the appropriate granularity:
check for BOAT_SUCCESS for the happy path, check category ranges for recovery strategies,
or check specific codes for precise error handling.

### 6.3 Buffer Pattern

BoatBuf is the universal dynamic buffer:

    typedef struct {
        uint8_t *data;
        size_t   len;
        size_t   cap;
    } BoatBuf;

It is used for RLP encoding, ABI encoding, Solana message serialization, and HTTP response
accumulation. The growth strategy is doubling (cap *= 2), which amortizes allocation cost.
All BoatBuf operations use boat_malloc/boat_free from the PAL, ensuring platform portability.

### 6.4 Amount Conversion

Blockchain amounts are integers in minimum units (wei for ETH, lamports for SOL, base units
for tokens). Human-readable amounts are decimal (e.g. 0.001 ETH, 1.5 SOL). The SDK provides
conversion helpers to bridge this gap:

- boat_amount_to_uint256(double amount, uint8_t decimals, uint8_t value[32]) -- for EVM
  uint256 values (e.g. ETH with 18 decimals)
- boat_amount_to_uint64(double amount, uint8_t decimals, uint64_t *value) -- for Solana
  lamports and SPL token amounts
- boat_uint256_to_amount() / boat_uint64_to_amount() -- reverse conversion for display

The uint256 conversion uses string-based intermediate representation (sprintf %.15g then
digit-by-digit multiply-and-add into big-endian bytes) to avoid floating-point precision
loss. This is critical: naive `0.1 * 1e18` in IEEE 754 double produces 100000000000000096
instead of 100000000000000000. The string approach produces exact results for amounts
expressible within double's 15-digit precision.

---

## 7. Security Considerations

### 7.1 Private Key Handling

- Private keys are stored in the opaque BoatKey struct, never exposed via public API.
- boat_key_free() zeroes all key material before deallocation (memset to 0).
- boat_key_save() stores keys via the PAL storage interface. On Linux, this is file-based.
  On production devices, this should map to secure storage (encrypted flash, SE, TEE).
- Key generation uses boat_random() which maps to /dev/urandom on Linux. On production
  devices, this should map to a hardware RNG.

### 7.2 Signature Security

- secp256k1 private keys are validated against the curve order on import and generation.
  Keys outside [1, n-1] are rejected.
- ECDSA signing uses deterministic k (RFC 6979) via trezor-crypto, preventing nonce reuse
  attacks.
- Ed25519 signing follows the standard Ed25519 algorithm with no modifications.

### 7.3 Transport Security

- The SDK does not enforce HTTPS. The application provides the RPC URL and is responsible
  for using TLS. On embedded devices, TLS is typically handled by the cellular modem or
  an mbedTLS/wolfSSL stack.
- The BoatHttpOps abstraction allows the application to inject TLS-enabled HTTP
  implementations without modifying SDK code.

---

## 8. Future Work

### 8.1 Near-Term (v4.1)

- BIP-39 mnemonic import (boat_key_import_mnemonic) -- uses trezor-crypto bip39/bip32
- EIP-1559 type-2 transaction support (fields are in the struct, encoding path needed)
- Secure Element backend (boat_key_se.c) for NXP SE050/SE051
- Proper ed25519 on-curve check for Solana PDA derivation

**Completed (originally planned for v4.1, now in v4.0):**
- Base58 and JSON array key import (boat_key_import_base58, boat_key_import_json_array,
  boat_key_import_string) -- Solana wallet compatibility
- Base58 encode/decode public API (boat_base58_encode, boat_base58_decode) -- wraps
  trezor-crypto b58enc/b58tobin
- Address string conversion (boat_address_to_string, boat_address_from_string) -- base58
  for Solana, hex for EVM
- Amount conversion utilities (boat_amount_to_uint256, boat_amount_to_uint64, etc.) --
  human-readable amounts to/from minimum unit integers

### 8.2 Medium-Term (v4.2)

- Account Abstraction: ERC-4337 UserOperation builder, Paymaster integration
- Machine DID: W3C DID document generation using hardware-bound keys
- Solana Tier 3: Anchor IDL code generation

### 8.3 Long-Term

- Streaming payments (payment channels for continuous machine-to-machine value transfer)
- On-device policy engine (spending limits, approved counterparties, time-based rules)
- AI agent interoperability bridge (MCP/A2A protocol support)
- BoAT Enterprise: cloud HSM backend, audit logging, compliance features

---

## 9. Conclusion

BoAT v4 is a ground-up rewrite that preserves the core strengths of the project (lightweight,
portable, production-proven on 52+ modules) while eliminating the architectural debt that
limited v3. The composable module architecture, source library build integration, opaque key
handles, and payment protocol support position BoAT for the machine economy era.

The design philosophy throughout has been: minimal, composable, portable, and honest. The SDK
does what it does well and does not pretend to do more. It is a toolkit for developers who
know what they want to build, not a framework that prescribes how to build it.

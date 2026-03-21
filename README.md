# BoAT v4 SDK

Blockchain of AI Things — lightweight C SDK for embedded and IoT devices to interact with EVM and Solana blockchains.

<!-- AI-agent-friendly metadata
project_type: embedded-sdk
language: C (C11)
build_system: CMake (primary), GNU Make (boat.mk), ESP-IDF (Kconfig)
entry_headers: include/boat.h, include/boat_key.h, include/boat_evm.h, include/boat_sol.h, include/boat_pay.h, include/boat_pal.h
build_file: CMakeLists.txt
options_file: CMakeLists.txt (lines 9-15)
examples_dir: examples/
tests_dir: tests/
third_party: third-party/crypto/ (trezor-crypto), third-party/cJSON/
tools: tools/abi2c.py (ABI-to-C codec generator)
license: Apache-2.0
-->

## Background

BoAT v4 is a ground-up rewrite of the [BoAT v3 SDK](https://github.com/boat-x/BoAT-EdgeDocs). The v3 codebase grew organically across many chains and HALs, accumulating complexity that made it difficult to maintain, port, and audit. v4 starts fresh with a minimal, modular architecture:

- Single-header-per-module API (`boat.h`, `boat_key.h`, `boat_evm.h`, `boat_sol.h`, `boat_pay.h`, `boat_pal.h`)
- Zero dynamic allocation in the transaction path (stack buffers + caller-owned structs)
- Portable PAL (Platform Abstraction Layer) — swap one file to run on any RTOS or bare-metal target
- Built-in payment protocol support (x402, Circle Nanopayments, Circle Gateway)

## Features

- **EVM chains** — Ethereum, Base, Polygon, and any EVM-compatible chain. Legacy and EIP-1559 transactions, ABI encode/decode, `eth_call`, auto-fill nonce and gas price.
- **Solana** — SOL transfers, SPL token transfers, ATA derivation, custom instructions via Borsh encoder, blockhash management.
- **Payment protocols** — x402 (HTTP 402 payment flow), Circle Nanopayments (EIP-3009 gasless micropayments), Circle Gateway (cross-chain USDC via CCTP).
- **Key management** — Generate, import (raw hex, base58, JSON byte array, mnemonic, auto-detect string), sign, store/load. Pluggable backends: software, Secure Element, TEE.
- **Amount conversion** — Human-readable ↔ minimum-unit conversion for any token decimals (ETH 18, SOL 9, USDC 6, etc.).
- **Compact footprint** — ~30 KB code for EVM-only, ~40 KB for EVM+Solana (excluding crypto).

## Project Structure

```
BoAT4/
├── CMakeLists.txt          # Primary build — CMake 3.12+
├── boat.mk                 # GNU Make includable fragment
├── sources.txt             # Flat tagged source list for custom build systems
├── Kconfig                 # ESP-IDF / menuconfig integration
├── include/
│   ├── boat.h              # Core types, error codes, utilities
│   ├── boat_key.h          # Key management API
│   ├── boat_evm.h          # EVM chain + ABI API
│   ├── boat_sol.h          # Solana chain + SPL + Borsh API
│   ├── boat_pay.h          # Payment protocols (x402, Nano, Gateway)
│   └── boat_pal.h          # Platform abstraction layer interface
├── src/
│   ├── core/               # boat_util, boat_key, boat_key_soft, boat_rpc
│   ├── evm/                # evm_rpc, evm_tx, evm_abi
│   ├── sol/                # sol_rpc, sol_tx, sol_spl, sol_ix, sol_borsh
│   ├── pay/                # pay_common, pay_x402, pay_nano, pay_gateway, pay_gateway_sol, pay_gateway_cross
│   └── pal/linux/          # Linux PAL (POSIX + libcurl)
├── third-party/
│   ├── crypto/             # trezor-crypto (secp256k1, ed25519, sha, keccak, etc.)
│   └── cJSON/              # cJSON parser (can be replaced with vendor cJSON)
├── examples/               # Runnable demo programs
├── tests/                  # Testnet integration tests
├── tools/
│   └── abi2c.py            # ABI-to-C codec generator
└── docs/                   # Design docs and technical memo
```

## Quick Start

### Prerequisites

- C compiler with C11 support (GCC, Clang, or MSVC)
- CMake 3.12+
- libcurl development headers (Linux PAL)

On Debian/Ubuntu:
```bash
sudo apt install build-essential cmake libcurl4-openssl-dev
```

### Build

```bash
mkdir build && cd build
cmake .. -DBOAT_EVM=ON -DBOAT_SOL=ON
make
```

The output is `libboat4.a` (static library).

### Build and Run Tests

Tests run against live testnets and require funded private keys:

```bash
mkdir build_test && cd build_test
cmake .. -DBOAT_BUILD_TESTS=ON -DBOAT_EVM=ON -DBOAT_SOL=ON
make

export BOAT_TEST_EVM_PRIVKEY="your_secp256k1_hex_privkey"
export BOAT_TEST_EVM_RPC_URL="https://sepolia.base.org"
export BOAT_TEST_SOL_PRIVKEY="your_ed25519_base58_privkey"
export BOAT_TEST_SOL_RPC_URL="https://api.devnet.solana.com"

./test_eth_transfer
./test_sol_transfer
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BOAT_EVM` | `ON` | Enable EVM chain support (Ethereum, Base, Polygon, etc.) |
| `BOAT_SOL` | `ON` | Enable Solana chain support |
| `BOAT_PAY_X402` | `OFF` | Enable x402 payment protocol |
| `BOAT_PAY_NANO` | `OFF` | Enable Circle Nanopayments |
| `BOAT_PAY_GATEWAY` | `OFF` | Enable Circle Gateway |
| `BOAT_VENDOR_CJSON` | `OFF` | Use vendor-provided cJSON instead of bundled copy |
| `BOAT_BUILD_EXAMPLES` | `OFF` | Build example programs |
| `BOAT_BUILD_TESTS` | `OFF` | Build testnet integration tests |
| `BOAT_PAL` | `"linux"` | Platform abstraction layer selection |

## Build Integration

### CMake (subdirectory)

```cmake
add_subdirectory(BoAT4)
target_link_libraries(your_app boat4)
```

### GNU Make (boat.mk)

```makefile
BOAT_EVM := 1
BOAT_SOL := 1
BOAT_PAL_LINUX := 1
include path/to/BoAT4/boat.mk

your_app: your_app.c $(BOAT_SRCS)
	$(CC) $(BOAT_CFLAGS) $(BOAT_INCS) -o $@ $^ -lcurl
```

To use a vendor-provided cJSON (e.g. from ESP-IDF), define `BOAT_VENDOR_CJSON`:

```makefile
BOAT_VENDOR_CJSON := 1
include path/to/BoAT4/boat.mk
# BOAT_INCS and BOAT_SRCS will exclude bundled cJSON
```

### sources.txt

A flat tagged source list for build systems that don't use CMake or Make. Lines prefixed with `[TAG]` are conditional — include them only when the corresponding feature is enabled. See `sources.txt` for details.

### ESP-IDF / Kconfig

The `Kconfig` file provides menuconfig integration. Place or symlink BoAT4 as an ESP-IDF component and run `idf.py menuconfig` to configure features.

## API Overview

### Key Management

```c
#include "boat_key.h"

// Auto-detect format: hex, base58, or JSON byte array
BoatKey *key = boat_key_import_string(BOAT_KEY_TYPE_SECP256K1, privkey_str);

// Or import specific formats
BoatKey *key = boat_key_import_raw(BOAT_KEY_TYPE_ED25519, raw_bytes, 32);
BoatKey *key = boat_key_import_base58(BOAT_KEY_TYPE_ED25519, "base58_privkey");
BoatKey *key = boat_key_import_json_array(BOAT_KEY_TYPE_ED25519, "[1,2,3,...]");
BoatKey *key = boat_key_generate(BOAT_KEY_TYPE_SECP256K1);

// Get address
BoatKeyInfo info;
boat_key_get_info(key, &info);
char addr[64];
boat_address_to_string(&info, addr, sizeof(addr));

// Persistent storage
boat_key_save(key, "my_wallet");
BoatKey *loaded = boat_key_load("my_wallet", BOAT_KEY_TYPE_SECP256K1);

boat_key_free(key);
```

### EVM Transaction

```c
#include "boat_evm.h"

BoatEvmRpc rpc;
boat_evm_rpc_init(&rpc, "https://sepolia.base.org");

BoatEvmChainConfig chain = { .chain_id = 84532, .eip1559 = false };
strncpy(chain.rpc_url, "https://sepolia.base.org", sizeof(chain.rpc_url) - 1);

BoatEvmTx tx;
boat_evm_tx_init(&tx, &chain);
boat_evm_tx_set_to(&tx, recipient_addr);

uint8_t value[32];
boat_amount_to_uint256(0.001, 18, value);  // 0.001 ETH
boat_evm_tx_set_value(&tx, value);
boat_evm_tx_set_gas_limit(&tx, 21000);
boat_evm_tx_auto_fill(&tx, &rpc, key);     // nonce + gas price from chain

uint8_t txhash[32];
boat_evm_tx_send(&tx, key, &rpc, txhash);
```

### Solana Transaction

```c
#include "boat_sol.h"

BoatSolRpc rpc;
boat_sol_rpc_init(&rpc, "https://api.devnet.solana.com");

uint8_t blockhash[32];
uint64_t last_valid;
boat_sol_rpc_get_latest_blockhash(&rpc, BOAT_SOL_COMMITMENT_FINALIZED,
                                   blockhash, &last_valid);

// SPL token transfer
BoatSolInstruction ix;
uint64_t amount;
boat_amount_to_uint64(1.0, 6, &amount);  // 1 USDC (6 decimals)
boat_sol_spl_transfer(sender_ata, recipient_ata, owner_pubkey, amount, &ix);

BoatSolTx tx;
boat_sol_tx_init(&tx);
boat_sol_tx_set_fee_payer(&tx, fee_payer_pubkey);
boat_sol_tx_set_blockhash(&tx, blockhash);
boat_sol_tx_add_instruction(&tx, &ix);

uint8_t sig[64];
boat_sol_tx_send(&tx, key, &rpc, sig);
```

### Utilities

```c
// Hex ↔ binary
uint8_t bin[32];
size_t len;
boat_hex_to_bin("0xdeadbeef", bin, sizeof(bin), &len);

char hex[67];
boat_bin_to_hex(bin, 32, hex, sizeof(hex), true);  // "0x..."

// Base58
char b58[64];
boat_base58_encode(bin, 32, b58, sizeof(b58));
boat_base58_decode("4zMMC9...", bin, sizeof(bin), &len);

// Amount conversion
uint8_t wei[32];
boat_amount_to_uint256(1.5, 18, wei);    // 1.5 ETH → wei

uint64_t lamports;
boat_amount_to_uint64(0.1, 9, &lamports); // 0.1 SOL → lamports

// Address from string (auto-detects EVM 0x... or Solana base58)
uint8_t addr[32];
boat_address_from_string("4zMMC9srt5...", addr, sizeof(addr), &len);
```

## Examples

| Example | Description |
|---------|-------------|
| `evm_transfer.c` | Send ETH on Base Sepolia |
| `evm_erc20.c` | ERC-20 token transfer with ABI encoding |
| `evm_erc20_transferfrom.c` | ERC-20 transferFrom using abi2c-generated codec |
| `erc20_codec.c` | Generated ERC-20 codec (abi2c output) |
| `sol_transfer.c` | Send SOL / SPL tokens on Solana devnet |
| `pay_x402_demo.c` | x402 payment protocol flow (HTTP 402 → pay → retry) |
| `pay_nano_demo.c` | Circle Nanopayments (EIP-3009 gasless micropayments) |
| `pay_gateway_demo.c` | Circle Gateway deposit and cross-chain transfer (EVM) |
| `pay_gateway_sol_demo.c` | Circle Gateway on Solana (deposit, balance, transfer) |

Build examples:
```bash
cmake .. -DBOAT_BUILD_EXAMPLES=ON -DBOAT_EVM=ON -DBOAT_SOL=ON \
         -DBOAT_PAY_X402=ON -DBOAT_PAY_NANO=ON -DBOAT_PAY_GATEWAY=ON
make
```

## Tools

### abi2c.py — ABI-to-C Codec Generator

Generates C encode/decode functions from a Solidity ABI JSON file. Produces both a `.c` and `.h` file. Supports static types (address, bool, uintN, bytes32) and dynamic types (bytes, string). Output calls `boat_evm_abi_encode_*` / `boat_evm_abi_decode_*` primitives — pure codec, no I/O or RPC.

```bash
python3 tools/abi2c.py erc20_abi.json -o erc20_codec.c
# generates erc20_codec.c and erc20_codec.h
```

## Environment Variables (Tests)

| Variable | Description |
|----------|-------------|
| `BOAT_TEST_EVM_PRIVKEY` | Secp256k1 private key (hex) for EVM tests |
| `BOAT_TEST_EVM_RPC_URL` | EVM RPC endpoint (e.g. Base Sepolia) |
| `BOAT_TEST_SOL_PRIVKEY` | Ed25519 private key (base58) for Solana tests |
| `BOAT_TEST_SOL_PRIVKEY2` | Ed25519 recipient key for SOL-to-SOL transfer test |
| `BOAT_TEST_SOL_RPC` | Solana RPC endpoint override |
| `BOAT_TEST_POLYGON_RPC` | Polygon RPC endpoint override |
| `BOAT_TEST_X402_URL` | x402-enabled resource URL for payment tests |
| `BOAT_TEST_USDC_CONTRACT` | USDC contract address for payment tests |

## Documentation

- [Technical Memo](docs/01_Technical_Memo.md) — Architecture decisions and design rationale
- [High-Level Design](docs/02_High_Level_Design.md) — Module overview and data flow
- [User Guide](docs/03_User_Guide.md) — Getting started and integration guide
- [API Manual](docs/04_API_Manual.md) — Complete API reference
- [BoAT v3 Documentation](https://github.com/boat-x/BoAT-EdgeDocs) — Previous version

## License

Apache-2.0 — see [LICENSE](LICENSE).

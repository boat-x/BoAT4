# BoAT v4 SDK — Release Notes

## v0.0.2 (2026-03-21)

### Circle Gateway — Solana + Cross-chain Support

- **Gateway on Solana:** deposit, balance (on-chain + off-chain API), SOL-to-SOL transfer, trustless withdraw/complete
- **Cross-chain transfers:** EVM-to-Solana and Solana-to-EVM USDC transfers via Circle Gateway
- **Generic PDA derivation:** `boat_sol_find_pda()` — reusable for any Solana program
- **Off-chain balance query:** `boat_gateway_sol_api_balance()` for Gateway API balance without on-chain RPC
- **Configurable Gateway API URL:** `gateway_api_url` field in `BoatGatewayConfig` and `BoatGatewaySolConfig` (testnet/mainnet)
- **Mainnet constants:** `BOAT_GW_SOL_MAINNET_USDC`, `BOAT_GW_SOL_MAINNET_WALLET`, `BOAT_GW_SOL_MAINNET_MINTER`

### API: Recipient Address Support for All Gateway Transfers

All Gateway transfer functions now accept an explicit recipient address (NULL = self-transfer):
- `boat_gateway_transfer()` — `const uint8_t *recipient` (20-byte EVM address)
- `boat_gateway_transfer_evm_to_sol()` — `const uint8_t *sol_recipient` (32-byte Solana pubkey)
- `boat_gateway_transfer_sol_to_evm()` — `const uint8_t *evm_recipient` (20-byte EVM address)
- `boat_gateway_sol_transfer()` already supported this in v0.0.1

### Build System Fixes

- Added `pay_gateway_sol.c` and `pay_gateway_cross.c` to `boat.mk` and `sources.txt` with conditional guards (`GATEWAY+SOL`, `GATEWAY+EVM+SOL`)
- Fixed `Kconfig`: Circle Gateway no longer requires EVM-only — works with EVM or SOL enabled

### Bug Fixes

- Fixed BurnIntent `recipient` field to use USDC ATA (not wallet pubkey) for Solana destinations
- Updated Ed25519 to expose raw sign (no pre-hash) for Solana Gateway binary signing

### New Examples

| Example | Description |
|---------|-------------|
| `pay_gateway_sol_demo.c` | Circle Gateway on Solana (deposit, balance, transfer) |

### New Integration Tests

Four new tests (total: 11):
- `test_gateway_sol` — Gateway Solana PDA derivation + balance
- `test_gateway_sol_transfer` — SOL-to-SOL transfer on mainnet
- `test_gateway_evm_to_sol` — Polygon mainnet → Solana mainnet cross-chain
- `test_gateway_sol_to_evm` — Solana mainnet → Polygon mainnet cross-chain

### Documentation

- Aligned all docs (README, High-Level Design, User Guide, API Manual) with current codebase
- Fixed struct definitions in API Manual (`BoatX402PaymentReq`, `BoatNanoConfig`)
- Added all Gateway Solana and cross-chain function signatures to API Manual

---

## v0.0.1 (2026-03-19) — Initial Release

BoAT v4 is a ground-up rewrite of the BoAT SDK — a lightweight C-language blockchain SDK for resource-constrained IoT and embedded devices. It enables autonomous on-chain transactions with a compact footprint (~30 KB code for EVM-only, ~40 KB for EVM+Solana, excluding crypto).

### Supported Blockchains

- **EVM chains:** Ethereum, Base, Polygon, Arbitrum, BNB Chain, peaq, and any EVM-compatible chain
  - Legacy and EIP-1559 transactions
  - ABI encode/decode (static and dynamic types)
  - `eth_call` for read-only contract queries
  - Auto-fill nonce and gas price
- **Solana:** SOL transfers, SPL token transfers, ATA derivation, custom instructions via Borsh encoder

### Machine Payment Protocols

- **x402:** HTTP 402 payment flow (Coinbase)
- **Circle Nanopayments:** EIP-3009 gasless micropayments
- **Circle Gateway:** Cross-chain USDC transfer (deposit, transfer, trustless withdrawal)
  - EVM: deposit, balance, transfer, trustless withdraw/complete
  - Solana: deposit, balance (on-chain + API), transfer (SOL-to-SOL), trustless withdraw/complete
  - Cross-chain: EVM-to-Solana and Solana-to-EVM transfers

### Key Management

- Generate, import (raw hex, base58, JSON byte array, mnemonic, auto-detect), sign, store/load
- Pluggable backends: software, Secure Element, TEE
- Opaque key handles with secure wipe

### Platform Abstraction Layer (PAL)

- Two-tier design: mandatory primitives (memory, storage, time, random, mutex) + pluggable HTTP service contract
- Linux PAL included (POSIX + libcurl)
- Portable to RTOS, bare-metal, ESP-IDF, Zephyr, STM32CubeIDE

### Build System

- CMake 3.12+ (primary, auto-detects ESP-IDF component mode)
- GNU Make (`boat.mk` includable fragment)
- Kconfig (ESP-IDF/Zephyr menuconfig)
- Raw file list (`sources.txt` for custom build systems)

### Tooling

- **abi2c.py:** Generates C encode/decode functions from Solidity ABI JSON. Supports static types (`address`, `bool`, `uintN`, `bytesN`) and dynamic types (`bytes`, `string`). Produces `.c` and `.h` files.

### Examples

| Example | Description |
|---------|-------------|
| `evm_transfer.c` | Send ETH on Base |
| `evm_erc20.c` | ERC-20 token transfer |
| `evm_erc20_transferfrom.c` | ERC-20 transferFrom with allowance check |
| `erc20_codec.c` | Generated ERC-20 codec (abi2c output) |
| `sol_transfer.c` | Send SPL token on Solana |
| `pay_x402_demo.c` | x402 payment protocol |
| `pay_nano_demo.c` | Circle Nanopayments |
| `pay_gateway_demo.c` | Circle Gateway (EVM) |
| `pay_gateway_sol_demo.c` | Circle Gateway on Solana |

### Integration Tests

Eleven integration tests covering ETH transfer, ERC-20 transfer, SOL transfer, SPL transfer, x402 payment, Nanopayments, Gateway (EVM), Gateway Solana (PDA + balance), Gateway SOL-to-SOL transfer, Gateway EVM-to-SOL cross-chain, and Gateway SOL-to-EVM cross-chain. Configured via environment variables (`BOAT_TEST_EVM_PRIVKEY`, `BOAT_TEST_SOL_PRIVKEY`, etc.).

### Security

- trezor-crypto RNG (`random32()`, `random_buffer()`) redirected to `/dev/urandom` via PAL on Linux, replacing the insecure LCG fallback
- `RAND_PLATFORM_INDEPENDENT` compile flag excludes the LCG implementation from production builds

### Documentation

- Technical Memo — Architecture decisions and design rationale
- High-Level Design — Module overview and data flow
- User Guide — Getting started and integration guide
- API Manual — Complete API reference

### License

Apache-2.0

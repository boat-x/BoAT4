# BoAT v4 SDK — Release Notes

## v0.0.3 (2026-03-30)

### MPP (Machine Payments Protocol) — Tempo Charge

BoAT now supports the **Machine Payments Protocol (MPP)**, the HTTP 402-based machine-to-machine payment standard co-authored by Stripe and Tempo. BoAT acts as a client-side payer: it parses the server's 402 payment challenge, executes an on-chain token transfer, and retries the request with a payment credential.

This release implements the **MPP envelope** (protocol-agnostic HTTP 402 handling) and the **Tempo Charge** payment method (one-shot TIP-20/ERC-20 token transfer on the Tempo blockchain).

### New Features

- **MPP envelope** (`pay_mpp.c`): Parse `WWW-Authenticate: Payment` challenges, build `Authorization: Payment` credentials (base64url-encoded JSON), parse `Payment-Receipt` headers
- **Base64url encode/decode**: RFC 4648 URL-safe variant without padding — shared utility for MPP
- **Tempo Charge handler** (`pay_mpp_tempo.c`): Execute TIP-20 token transfer on Tempo chain, return tx hash as credential payload (`{ "type": "hash", "hash": "0x..." }`)
- **DID source attribution**: Credentials include `did:pkh:eip155:<chainId>:<address>` payer identity
- **Multi-method negotiation**: Parser supports multiple `WWW-Authenticate: Payment` headers per response

### PAL Extension

- `BoatHttpResponse` extended with `headers` / `headers_len` fields for response header capture
- curl PAL (`pal_linux.c`) captures response headers via `CURLOPT_HEADERFUNCTION` in both GET and POST
- Backward compatible — existing code unaffected (new fields are zero-initialized)

### API

- `BoatPayReqOpts` — common HTTP request options type shared by x402 and MPP (`BoatX402ReqOpts` remains as backward-compatible alias)

| Function | Description |
|----------|-------------|
| `boat_base64url_encode()` | Base64url encode (RFC 4648 §5, no padding) |
| `boat_base64url_decode()` | Base64url decode with padding recovery |
| `boat_mpp_parse_challenges()` | Parse MPP challenge(s) from response headers |
| `boat_mpp_build_credential()` | Build `Authorization: Payment` credential |
| `boat_mpp_parse_receipt()` | Parse `Payment-Receipt` from response headers |
| `boat_mpp_request()` | Send HTTP request, parse 402 challenge if returned |
| `boat_mpp_pay_and_get()` | Retry request with payment credential |
| `boat_mpp_tempo_charge()` | Execute Tempo Charge (transfer + credential) |
| `boat_mpp_tempo_process()` | Full MPP Tempo Charge flow in one call |

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `BOAT_MPP_TEMPO_MAINNET_CHAIN_ID` | 4217 | Tempo mainnet |
| `BOAT_MPP_TEMPO_TESTNET_CHAIN_ID` | 42431 | Tempo Moderato testnet |
| `BOAT_MPP_TEMPO_PATHUSD_TESTNET` | `0x20c000...0000` | pathUSD on testnet |
| `BOAT_MPP_TEMPO_USDC_MAINNET` | `0x20c000...8b50` | USDC on mainnet |

### New Examples

| Example | Description |
|---------|-------------|
| `pay_mpp_demo.c` | End-to-end MPP Tempo Charge payment |

### New Tests

| Test | Type | Description |
|------|------|-------------|
| `test_mpp_unit` | Unit | Base64url, challenge parsing, credential building, receipt parsing (50 assertions, no network) |
| `test_mpp_tempo` | Integration | Full MPP flow against Tempo Moderato testnet via `mpp.dev/api/ping/paid` |

### Test Results

- **Unit test**: 50 passed, 0 failed
- **Integration test**: 7 passed, 0 failed (live Tempo Moderato testnet, pathUSD payment confirmed on-chain)

### Documentation

- `05_MPP_Feasibility_Analysis.md` — Protocol analysis, architecture decisions, implementation plan

---

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

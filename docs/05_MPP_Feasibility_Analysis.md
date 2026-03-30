# BoAT v4 — MPP Payment Method Feasibility Analysis

> Date: 2026-03-27 | Author: Claude (Opus 4.6)
> Status: Phase 1 implemented (MPP Envelope + Tempo Charge)
> Reference: "Claude Study on Tempo MPP v2.md", paymentauth.org, mppx SDK

---

## 1. Objective

Analyze the feasibility of adding **Machine Payments Protocol (MPP)** support to BoAT v4 as a client-side (payer) payment method. BoAT does not define new payment methods or act as a merchant — it consumes the HTTP 402 challenge, executes payment via an existing MPP-supported rail, and attaches the proof to complete the transaction.

### Scope

| Item | In Scope | Rationale |
|------|----------|-----------|
| MPP envelope (402 parse + credential build) | Yes | Foundation for all MPP methods |
| Tempo Charge (`intent="charge"`) | Yes | Straightforward ERC-20 transfer on EVM chain |
| Tempo Session (`intent="session"`) | Deferred | Stateful; requires escrow ABI, SSE support |
| Lightning Session (`intent="session"`) | Deferred | Requires external Lightning service dependency |
| Fiat methods (Stripe, Visa) | Out of scope | Not relevant for IoT/embedded devices |

---

## 2. Background: BoAT's Role in MPP

MPP is an orchestration layer. It defines:
- How the merchant advertises payment requirements (HTTP 402 challenge)
- How the client attaches payment proof (Authorization header)
- How the merchant confirms receipt (Payment-Receipt header)

The actual fund transfer happens entirely within the chosen payment method's domain. MPP doesn't need to understand the payment method's internals — it only needs a verifiable proof artifact.

**BoAT's position is strictly client-side:**

```
IoT Device (BoAT)              Merchant Server                 Chain
  |                                |                              |
  |-- HTTP request --------------->|                              |
  |<-- 402 + WWW-Authenticate -----|                              |
  |                                |                              |
  |  [Parse challenge]             |                              |
  |  [Select payment method]       |                              |
  |  [Execute payment] ------------|----------------------------->|
  |  [Get proof (tx hash)]         |                              |
  |                                |                              |
  |-- Retry + Authorization ------>|-- Verify ------------------>|
  |<-- 200 OK + Receipt -----------|                              |
```

BoAT implements steps a), b), and c):
- **a)** Recognize the payment requirements in the HTTP 402 challenge
- **b)** Execute the payment and obtain a proof artifact
- **c)** Retry the HTTP request with the proof in `Authorization: Payment`

---

## 3. MPP Protocol Envelope

### 3.1 What It Is

The MPP envelope is the generic HTTP-level machinery shared by all payment methods. It handles the 402 challenge-response handshake without knowing anything about the underlying payment rail.

### 3.2 Specification (from paymentauth.org)

**Challenge format** (`WWW-Authenticate: Payment`):
```
WWW-Authenticate: Payment id="x7Tg2pLqR9mKvNwY3hBcZa", realm="api.example.com",
    method="tempo", intent="charge", request="<base64url-encoded JSON>"
```

Required parameters: `id`, `realm`, `method`, `intent`, `request`.
Optional parameters: `digest`, `expires`, `description`, `opaque`.

**Credential format** (`Authorization: Payment`):
```
Authorization: Payment <base64url-encoded JSON>
```
Where the JSON is:
```json
{
  "challenge": { "id": "...", "realm": "...", "method": "...", "intent": "...", "request": "..." },
  "source": "did:example:client-wallet",
  "payload": { /* method-specific payment proof */ }
}
```

**Receipt format** (`Payment-Receipt`):
```json
{
  "status": "success",
  "method": "tempo",
  "timestamp": "2026-03-26T12:00:00Z",
  "reference": "0xabc123..."
}
```

**Method negotiation**: When the merchant supports multiple methods, it sends multiple `WWW-Authenticate` headers. The client picks one.

### 3.3 Existing BoAT v4 Capabilities

| Capability | Status | Location |
|-----------|--------|----------|
| HTTP request/response | Yes | PAL (`BoatHttpOps`) |
| JSON parsing | Yes | cJSON (third-party, in tree) |
| Base64 encoding | Yes | `pay_x402.c` (standard base64) |
| Base64**url** encoding | **Partial** | Need to add URL-safe variant (`-_` instead of `+/`, no padding) |
| HTTP header parsing | **Partial** | x402 parses custom headers; MPP header format differs |

### 3.4 What Needs to Be Built

**Module: `pay_mpp.c` + additions to `boat_pay.h`**

1. **`boat_mpp_parse_402()`** — Parse one or more `WWW-Authenticate: Payment` headers from an HTTP 402 response. Extract `id`, `realm`, `method`, `intent`, decode `request` from base64url JSON. Return an array of parsed challenges.

2. **`boat_mpp_select_method()`** — Match parsed challenges against a table of locally supported methods. Return the best match (or error if none supported).

3. **`boat_mpp_build_credential()`** — Given the original challenge and a method-specific payload (opaque bytes from the payment handler), produce the base64url-encoded JSON credential for the `Authorization: Payment` header.

4. **`boat_mpp_parse_receipt()`** — Decode the `Payment-Receipt` header and extract status, reference, timestamp.

5. **`boat_mpp_process()`** — Convenience function: send request → if 402 parse challenge → dispatch to method handler → retry with credential → return response + receipt. Analogous to `boat_x402_process()`.

6. **Base64url encoder/decoder** — Minor utility: standard base64 with `+/` replaced by `-_` and padding stripped.

### 3.5 Feasibility Assessment

**High.** The MPP envelope is structurally identical to what x402 already does — the difference is header names and JSON schemas. All building blocks exist. The new code is mostly parsing and serialization, with no cryptographic or protocol complexity.

**Estimated new code:** ~400-500 lines of C (comparable to `pay_x402.c`).

### 3.6 Design Considerations

**Method dispatch pattern:**

```c
/* Method handler function signature */
typedef BoatResult (*BoatMppMethodHandler)(
    const BoatMppChallenge *challenge,   /* parsed 402 challenge */
    const void             *method_ctx,  /* method-specific config (e.g., BoatEvmChainConfig) */
    const BoatKey          *key,         /* signing key */
    uint8_t               **payload_json,/* OUT: method-specific proof JSON */
    size_t                 *payload_len  /* OUT: proof length */
);

/* Registry: method name -> handler */
typedef struct {
    const char            *method_name;  /* "tempo", "lightning", etc. */
    BoatMppMethodHandler   handler;
    const void            *ctx;          /* method-specific config */
} BoatMppMethodEntry;
```

This keeps the envelope generic and allows new methods to be added by registering a handler — no changes to the envelope code.

**Relationship to x402:**

MPP and x402 are parallel protocols, not nested. They use different 402 header formats:
- MPP: `WWW-Authenticate: Payment` / `Authorization: Payment`
- x402: `PAYMENT-REQUIRED` / `PAYMENT-SIGNATURE` (custom headers, non-standard)

BoAT should detect which protocol the 402 response uses and dispatch accordingly. A future `boat_pay_process()` could unify both:

```c
BoatResult boat_pay_process(const char *url, ...);
// Internally: send request → if 402 → detect MPP vs x402 → dispatch
```

---

## 4. Tempo Charge (`method="tempo"`, `intent="charge"`)

### 4.1 What It Is

A one-shot on-chain payment on the Tempo blockchain. The client transfers ERC-20 tokens (USDC or pathUSD) to the merchant's address, then provides the transaction hash as proof.

### 4.2 Payment Flow

1. Server's 402 challenge contains:
   - `method="tempo"`, `intent="charge"`
   - `request`: `{ amount, currency, recipient, methodDetails: { chainId, tokenAddress } }`

2. Client executes:
   - Build an ERC-20 `transfer(recipient, amount)` transaction on Tempo
   - Sign with secp256k1 key
   - Submit via Tempo RPC
   - Wait for confirmation (sub-second on Tempo)

3. Client builds credential payload:
   - `{ transactionHash: "0x..." }` (or whatever the Tempo method spec defines)

### 4.3 Existing BoAT v4 Capabilities

| Capability | Status | Location |
|-----------|--------|----------|
| EVM transaction builder | Yes | `boat_evm.h` / `src/evm/` |
| ERC-20 `transfer()` ABI encoding | Yes | `evm_erc20` example; `abi2c.py` can generate |
| secp256k1 signing | Yes | `boat_key.h` / key management |
| JSON-RPC client | Yes | `src/evm/evm_rpc.c` |
| Chain config (chain_id, RPC URL) | Yes | `BoatEvmChainConfig` struct |

### 4.4 What Needs to Be Built

**Module: `pay_mpp_tempo.c`**

1. **Tempo chain config constants** — Chain ID, RPC URL for mainnet/Moderato testnet, USDC and pathUSD token addresses.

2. **`boat_mpp_tempo_charge()`** — The method handler registered with the MPP envelope:
   - Parse `amount`, `currency`, `recipient` from the challenge request
   - Resolve token address from currency identifier
   - Build ERC-20 `transfer(recipient, amount)` transaction
   - Sign and submit via Tempo RPC
   - Wait for tx confirmation
   - Return tx hash as payload

3. **Gas model adaptation** — Tempo has no native gas token; fees are paid in stablecoin. Need to verify:
   - Does Tempo use standard EIP-1559 fields (`maxFeePerGas`, `maxPriorityFeePerGas`)?
   - Or a custom EIP-2718 transaction type?
   - Does `eth_gasPrice` / `eth_estimateGas` work normally?
   - If standard EIP-1559: existing EVM tx builder works as-is
   - If custom: minor adaptation to tx serialization

### 4.5 Unknowns and Risks

| Item | Risk | Mitigation |
|------|------|-----------|
| Tempo gas model details | Low-Medium | Test on Moderato testnet; Tempo is EVM-compatible (built on Reth) so likely standard |
| Tempo method spec (`payload` format) | Low | Check `mpp-specs` repo for `draft-tempo-charge-00` |
| Fee sponsorship | Low | Optional for MVP; device can pay its own fees initially |
| Testnet access | Low | Moderato testnet is public; faucet at faucet.tempo.xyz |
| MPP preview access | Medium | Requires contacting machine-payments@stripe.com; US entities only (excl. NY/TX) — **but this is a merchant-side constraint, not a client-side constraint** |

### 4.6 Feasibility Assessment

**Very High.** Tempo Charge is an ERC-20 transfer on an EVM-compatible chain. This is BoAT v4's core competency. The only new work is:
- Wrapping the transfer in MPP envelope format (~100 lines)
- Tempo-specific chain config (~30 lines)
- Possible gas model adaptation (~50 lines if needed)

**Estimated new code:** ~150-200 lines of C for the method handler.

**Combined with MPP envelope:** ~600-700 lines total new code.

---

## 5. Deferred: Tempo Session

### 5.1 Why Deferred

Tempo Session adds significant complexity beyond Tempo Charge:

1. **Stateful session management** — Must track per-channel state (`channelId`, `deposit`, `acceptedCumulative`) across multiple HTTP requests. The charge flow is stateless.

2. **Escrow contract interaction** — Requires ABI for `open()`, `topUp()`, `settle()`, `requestClose()`, `withdraw()`. The contract ABI must be obtained from the Tempo team or `mpp-specs` repo.

3. **EIP-712 voucher signing** — BoAT already has EIP-712 support, but the specific voucher type hash and encoding need to match the spec exactly.

4. **SSE (Server-Sent Events)** — For streaming scenarios (e.g., pay-per-LLM-token), the server sends `payment-need-voucher` events mid-stream. The current PAL HTTP interface is request-response only; SSE requires a streaming callback mode.

### 5.2 Path to Implementation (When Pursued)

- **Phase 1:** Non-streaming session (open → voucher per request → close). No SSE needed. Feasibility: Medium-High.
- **Phase 2:** Add SSE support to PAL for true streaming. Feasibility: Medium.

### 5.3 Prerequisites

- Escrow contract ABI (from `mpp-specs` or Tempo docs)
- Voucher EIP-712 type definition (from `draft-tempo-session-00`)
- Decision on session state persistence (RAM only vs. PAL storage)

---

## 6. Deferred: Lightning Session

### 6.1 Why Deferred

Lightning Session requires capabilities fundamentally absent from BoAT:

1. **Paying BOLT11 invoices** — Requires a Lightning node or custodial Lightning service API. No embedded Lightning client exists for constrained devices.

2. **Generating BOLT11 invoices** (for refund) — Same dependency.

3. **No existing Lightning code in BoAT** — Unlike Tempo (EVM, fully supported), Lightning is an entirely new protocol stack.

### 6.2 Path to Implementation (When Pursued)

The only viable approach for IoT devices is a **thin wrapper around a Lightning service provider API** (e.g., Lightspark REST API):
- `pay_invoice(bolt11_string)` → HTTP POST to service → returns preimage
- `create_invoice(amount)` → HTTP POST to service → returns BOLT11 string

This follows the same HTTP-based pattern as BoAT's other flows but adds an external service dependency.

---

## 7. Implementation Plan

### Phase 1: MPP Envelope + Tempo Charge

```
boat_pay.h          — Add MPP types and function declarations
pay_mpp.c           — MPP envelope (402 parse, credential build, receipt parse, process)
pay_mpp_tempo.c     — Tempo Charge method handler
examples/           — pay_mpp_demo.c (end-to-end Tempo Charge via MPP)
```

### Dependency Graph

```
pay_mpp.c  (MPP envelope)
    |
    +-- pay_mpp_tempo.c  (Tempo Charge handler)
    |       |
    |       +-- evm_tx.c     (existing: EVM transaction builder)
    |       +-- evm_rpc.c    (existing: JSON-RPC client)
    |       +-- pay_common.c (existing: EIP-712, not needed for charge but ready)
    |
    +-- [future: pay_mpp_tempo_session.c]
    +-- [future: pay_mpp_lightning.c]
```

### Build Integration

```makefile
# Kconfig
config BOAT_PAY_MPP_ENABLED
    bool "MPP protocol support"
    default y

config BOAT_PAY_MPP_TEMPO_ENABLED
    bool "MPP Tempo method"
    depends on BOAT_PAY_MPP_ENABLED && BOAT_EVM_ENABLED
    default y
```

### Verification Plan

1. **Unit test: MPP envelope** — Hardcoded 402 response → parse → verify extracted fields
2. **Unit test: Credential build** — Known challenge + payload → verify base64url output matches expected
3. **Integration test: Tempo Charge** — Against Moderato testnet (requires faucet funds)
4. **End-to-end: `pay_mpp_demo`** — Against a test MPP server (from `stripe-samples/machine-payments`)

---

## 8. Comparison: MPP vs x402 in BoAT

| Aspect | x402 (existing) | MPP (proposed) |
|--------|-----------------|----------------|
| Protocol detection | `PAYMENT-REQUIRED` header | `WWW-Authenticate: Payment` header |
| Credential header | `PAYMENT-SIGNATURE` or `X-PAYMENT` | `Authorization: Payment` |
| Receipt | None specified | `Payment-Receipt` header |
| Payment proof | EIP-3009 signature (off-chain) | Method-dependent (tx hash for Tempo Charge) |
| Settlement | Via Facilitator (Coinbase CDP) | Direct on-chain (Tempo) |
| Session support | No | Yes (deferred) |
| Encoding | Base64 | Base64url |
| Method extensibility | Fixed (EVM exact scheme) | Pluggable (any method) |

Both protocols coexist in BoAT. A future unified `boat_pay_process()` can auto-detect which protocol the 402 response uses and dispatch accordingly.

---

## 9. Conclusion

Adding MPP Tempo Charge support to BoAT v4 is **highly feasible** with **low implementation effort**. The MPP envelope is a thin HTTP-level protocol, and Tempo Charge is a standard ERC-20 transfer — both align perfectly with BoAT v4's existing architecture.

The main risk is Tempo's gas model, which can be resolved with testnet experimentation.

Tempo Session and Lightning Session are feasible but involve substantially more complexity and are appropriately deferred.

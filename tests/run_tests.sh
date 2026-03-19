#!/bin/bash
# BoAT v4 — Testnet integration test runner
#
# Usage:
#   BOAT_TEST_EVM_PRIVKEY=<hex> BOAT_TEST_SOL_PRIVKEY=<hex> ./run_tests.sh
#
# Environment variables:
#   BOAT_TEST_EVM_PRIVKEY — EVM (secp256k1) private key hex. No 0x prefix.
#   BOAT_TEST_SOL_PRIVKEY — Solana (ed25519) private key hex. No 0x prefix.
#   BOAT_TEST_RPC        — Base Sepolia RPC URL (default: https://sepolia.base.org)
#   BOAT_TEST_SOL_RPC    — Solana RPC URL (default: https://api.devnet.solana.com)
#   BOAT_TEST_SPL_MINT   — SPL token mint hex (default: devnet USDC)
#   BOAT_TEST_ARC_RPC    — Arc Testnet RPC URL (default: https://rpc.testnet.arc.network)
#   BOAT_TEST_X402_URL   — x402 endpoint (default: https://x402.payai.network/api/base-sepolia/paid-content)
#   BOAT_TEST_NANO_SELLER_URL — Nano seller endpoint (default: none; start nano_seller.js first)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

export BOAT_TEST_RPC="${BOAT_TEST_RPC:-https://sepolia.base.org}"
export BOAT_TEST_SOL_RPC="${BOAT_TEST_SOL_RPC:-https://api.devnet.solana.com}"
export BOAT_TEST_ARC_RPC="${BOAT_TEST_ARC_RPC:-https://rpc.testnet.arc.network}"

PASS=0
FAIL=0
SKIP=0

run_test() {
    local name="$1"
    if [ -f "./$name" ]; then
        echo ""
        echo "================================================================"
        echo " Running: $name"
        echo "================================================================"
        if ./"$name"; then
            ((PASS++))
        else
            ((FAIL++))
        fi
    else
        echo "[SKIP] $name — binary not found"
        ((SKIP++))
    fi
}

echo "BoAT v4 Testnet Integration Tests"
echo "=================================="
echo "RPC (Base Sepolia): $BOAT_TEST_RPC"
echo "RPC (Solana):       $BOAT_TEST_SOL_RPC"
echo "RPC (Arc Testnet):  $BOAT_TEST_ARC_RPC"
if [ -n "$BOAT_TEST_EVM_PRIVKEY" ]; then
    echo "EVM private key: set (${#BOAT_TEST_EVM_PRIVKEY} chars)"
else
    echo "EVM private key: NOT SET — EVM tests will generate fresh keys"
fi
if [ -n "$BOAT_TEST_SOL_PRIVKEY" ]; then
    echo "SOL private key: set (${#BOAT_TEST_SOL_PRIVKEY} chars)"
else
    echo "SOL private key: NOT SET — Solana tests will generate fresh keys"
fi
echo ""

# Core EVM tests
run_test test_eth_transfer
run_test test_erc20_transfer

# Solana tests
run_test test_sol_transfer
run_test test_spl_transfer

# Payment protocol tests
run_test test_x402_payment
run_test test_gateway
run_test test_nano

echo ""
echo "================================================================"
echo " Final Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "================================================================"

exit $FAIL

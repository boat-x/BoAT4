/**
 * BoAT v4 — Circle Gateway Nanopayments test seller server
 *
 * Uses the official @circle-fin/x402-batching middleware for REAL on-chain
 * settlement via Circle Gateway. No simulation.
 *
 * x402 flow:
 *   GET /premium-data  → 402 with GatewayWalletBatched payment requirements
 *   GET /premium-data + PAYMENT-SIGNATURE header → settle via Gateway → return resource
 *
 * Setup:
 *   Requires node_modules from nanopayment-client-c.CC/code/seller-server/
 *   Or install: npm install @circle-fin/x402-batching @x402/core @x402/evm viem express
 *
 * Usage:
 *   node nano_seller.mjs [port]
 *   SELLER_ADDRESS=0x... node nano_seller.mjs
 *
 * Default port: 3000
 */
import express from 'express';
import { createGatewayMiddleware } from '@circle-fin/x402-batching/server';
import { createPublicClient, http, parseAbi } from 'viem';

const app = express();
const PORT = process.argv[2] && !process.argv[2].startsWith('-') ? parseInt(process.argv[2]) : 3000;

/* Seller address — the address that receives USDC payments.
 * For loopback testing, use the same address as the buyer. */
const SELLER_ADDRESS = process.env.SELLER_ADDRESS || '0x0000000000000000000000000000000000000001';

/* Arc Testnet config */
const GATEWAY_WALLET = '0x0077777d7EBA4688BDeF3E311b846F25870A19B9';
const ARC_USDC = '0x3600000000000000000000000000000000000000';
const ARC_RPC = 'https://rpc.testnet.arc.network';

const arcClient = createPublicClient({
    transport: http(ARC_RPC),
});

async function getGatewayBalance(address) {
    try {
        const balance = await arcClient.readContract({
            address: GATEWAY_WALLET,
            abi: parseAbi(['function availableBalance(address token, address depositor) view returns (uint256)']),
            functionName: 'availableBalance',
            args: [ARC_USDC, address],
        });
        return Number(balance) / 1e6;
    } catch (e) {
        return null;
    }
}

/* Create Gateway middleware — handles 402 response, signature verification,
 * and real on-chain settlement via Circle Gateway BatchFacilitatorClient. */
const gateway = createGatewayMiddleware({
    sellerAddress: SELLER_ADDRESS,
    networks: ['eip155:5042002'],  /* Arc Testnet only */
});

/* Protected endpoint — requires 0.001 USDC payment */
app.get('/premium-data', gateway.require('$0.001'), async (req, res) => {
    console.log('\n=== Payment Settled via Circle Gateway ===');
    if (req.payment) {
        console.log('Payer:', req.payment.payer);
        console.log('Amount:', req.payment.amount);
        console.log('Network:', req.payment.network);
    }

    const sellerBal = await getGatewayBalance(SELLER_ADDRESS);
    if (sellerBal !== null) console.log('Seller Gateway balance:', sellerBal.toFixed(6), 'USDC');
    if (req.payment?.payer) {
        const payerBal = await getGatewayBalance(req.payment.payer);
        if (payerBal !== null) console.log('Payer Gateway balance:', payerBal.toFixed(6), 'USDC');
    }

    res.json({
        success: true,
        data: 'Premium content from BoAT v4 nanopayment seller',
        paid_by: req.payment?.payer,
        network: req.payment?.network,
        timestamp: new Date().toISOString()
    });
});

/* Health check (no payment required) */
app.get('/health', (req, res) => {
    res.json({
        status: 'ok',
        seller: SELLER_ADDRESS,
        network: 'Arc Testnet (eip155:5042002)',
        settlement: 'real (Circle Gateway)'
    });
});

app.listen(PORT, async () => {
    console.log(`\n=== BoAT v4 Nanopayment Seller (Official SDK) ===`);
    console.log(`http://localhost:${PORT}`);
    console.log(`  GET /premium-data  — x402 protected (0.001 USDC)`);
    console.log(`  GET /health        — health check`);
    console.log(`Seller: ${SELLER_ADDRESS}`);
    console.log(`Settlement: REAL (Circle Gateway BatchFacilitatorClient)`);
    console.log(`Network: Arc Testnet (eip155:5042002)`);
    const bal = await getGatewayBalance(SELLER_ADDRESS);
    if (bal !== null) console.log(`Seller Gateway balance: ${bal.toFixed(6)} USDC`);
    console.log();
});

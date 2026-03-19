/**
 * BoAT v4 — Fallback local x402 test server
 *
 * Minimal Express server that:
 * - Returns 402 with standard x402 JSON body on GET /resource
 * - On retry with X-Payment header, forwards to facilitator for on-chain settlement
 * - Returns 200 with resource content after facilitator confirms
 *
 * Usage: node x402_server.js [port]
 * Default port: 4020
 *
 * Requires: npm install express node-fetch
 */
const express = require('express');
const app = express();
const PORT = process.argv[2] || 4020;

/* Base Sepolia USDC config */
const CHAIN_ID = 84532;
const USDC_CONTRACT = '0x036CbD53842c5426634e7929541eC2318f3dCF7e';
const FACILITATOR_URL = 'https://x402.org/facilitator';

/* Payment recipient — set to your test wallet or use a burn address */
const PAY_TO = process.env.X402_PAY_TO || '0x0000000000000000000000000000000000000001';

/* Amount in USDC atomic units (1000 = 0.001 USDC) */
const AMOUNT = '1000';

app.use(express.json());

app.get('/resource', async (req, res) => {
    const xPayment = req.headers['x-payment'];

    if (!xPayment) {
        /* Return 402 with payment requirements */
        return res.status(402).json({
            x402Version: 1,
            accepts: [{
                scheme: 'exact',
                network: 'base-sepolia',
                maxAmountRequired: AMOUNT,
                resource: `http://localhost:${PORT}/resource`,
                description: 'BoAT v4 test resource',
                mimeType: 'text/plain',
                payTo: PAY_TO,
                maxTimeoutSeconds: 300,
                asset: USDC_CONTRACT,
                extra: {
                    name: 'USDC',
                    version: '2'
                }
            }]
        });
    }

    /* Verify payment via facilitator */
    try {
        let fetch;
        try {
            fetch = (await import('node-fetch')).default;
        } catch {
            fetch = globalThis.fetch;
        }

        const verifyRes = await fetch(`${FACILITATOR_URL}/verify`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                x402Version: 1,
                payment: xPayment,
                paymentRequirements: {
                    scheme: 'exact',
                    network: 'base-sepolia',
                    maxAmountRequired: AMOUNT,
                    resource: `http://localhost:${PORT}/resource`,
                    payTo: PAY_TO,
                    maxTimeoutSeconds: 300,
                    asset: USDC_CONTRACT,
                    extra: { name: 'USDC', version: '2' }
                }
            })
        });

        if (verifyRes.ok) {
            /* Payment verified — settle on-chain */
            const settleRes = await fetch(`${FACILITATOR_URL}/settle`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    x402Version: 1,
                    payment: xPayment,
                    paymentRequirements: {
                        scheme: 'exact',
                        network: 'base-sepolia',
                        maxAmountRequired: AMOUNT,
                        resource: `http://localhost:${PORT}/resource`,
                        payTo: PAY_TO,
                        maxTimeoutSeconds: 300,
                        asset: USDC_CONTRACT,
                        extra: { name: 'USDC', version: '2' }
                    }
                })
            });

            if (settleRes.ok) {
                return res.status(200).send('BoAT v4 x402 test resource — payment settled on-chain');
            }
            const err = await settleRes.text();
            return res.status(402).json({ error: 'settlement_failed', details: err });
        }

        const err = await verifyRes.text();
        return res.status(402).json({ error: 'verification_failed', details: err });
    } catch (e) {
        return res.status(500).json({ error: 'facilitator_unreachable', details: e.message });
    }
});

app.listen(PORT, () => {
    console.log(`x402 test server listening on http://localhost:${PORT}`);
    console.log(`  GET http://localhost:${PORT}/resource  → 402 (no payment)`);
    console.log(`  GET with X-Payment header              → settles via ${FACILITATOR_URL}`);
    console.log(`  Pay-to: ${PAY_TO}`);
    console.log(`  Amount: ${AMOUNT} USDC units (0.001 USDC)`);
});

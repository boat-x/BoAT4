/******************************************************************************
 * Minimal ed25519 for BoAT v4 — based on TweetNaCl (public domain)
 * D.J. Bernstein, T. Lange, P. Schwabe
 * Only implements: ed25519_publickey, ed25519_sign
 *****************************************************************************/
#include <string.h>
#include <stdint.h>
#include "sha2.h"
#include "ed25519-donna/ed25519.h"

typedef int64_t gf[16];
static const gf gf0 = {0};
static const gf gf1 = {1};
static const gf D = {0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,
                      0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203};
static const gf D2= {0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,
                      0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406};
static const gf X = {0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,
                      0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169};
static const gf Y = {0x6658,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,
                      0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666};
static const uint8_t Lmod[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,
    0xde,0xf9,0xde,0x14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i+1)*(i<15)] += c - 1 + 37*(c-1)*(i==15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t; q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n) {
    gf m, t;
    memcpy(t, n, sizeof(gf));
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1]>>16)&1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14]>>16)&1);
        int b = (m[15]>>16)&1;
        m[14] &= 0xffff;
        sel25519(t, m, 1-b);
    }
    for (int i = 0; i < 16; i++) {
        o[2*i] = t[i] & 0xff;
        o[2*i+1] = t[i] >> 8;
    }
}

static int par25519(const gf a) {
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((int64_t)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}
static void Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}
static void M(gf o, const gf a, const gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    memcpy(o, t, 16 * sizeof(int64_t));
    car25519(o); car25519(o);
}
static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf a) {
    gf c;
    memcpy(c, a, sizeof(gf));
    for (int i = 253; i >= 0; i--) {
        S(c, c);
        if (i != 2 && i != 4) M(c, c, a);
    }
    memcpy(o, c, sizeof(gf));
}

/* Extended point: [X, Y, Z, T] where x=X/Z, y=Y/Z, x*y=T/Z */
typedef gf gf4[4];

static void cswap25519(gf4 p, gf4 q, uint8_t b) {
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void pt_add(gf4 p, gf4 q) {
    gf a,b,c,d,t,e,f,g,h;
    Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
    A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
    M(c, p[3], q[3]); M(c, c, D2);
    M(d, p[2], q[2]); A(d, d, d);
    Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
    M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}

static void scalarmult(gf4 p, gf4 q, const uint8_t *s) {
    memcpy(p[0], gf0, sizeof(gf));
    memcpy(p[1], gf1, sizeof(gf));
    memcpy(p[2], gf1, sizeof(gf));
    memcpy(p[3], gf0, sizeof(gf));
    for (int i = 255; i >= 0; i--) {
        uint8_t b = (s[i/8] >> (i&7)) & 1;
        cswap25519(p, q, b);
        pt_add(q, p);
        pt_add(p, p);
        cswap25519(p, q, b);
    }
}

static void scalarbase(gf4 p, const uint8_t *s) {
    gf4 q;
    memcpy(q[0], X, sizeof(gf));
    memcpy(q[1], Y, sizeof(gf));
    memcpy(q[2], gf1, sizeof(gf));
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

static void pt_pack(uint8_t *r, gf4 p) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

static void modL(uint8_t *r, int64_t x[64]) {
    for (int i = 63; i >= 32; i--) {
        int64_t carry = 0;
        int j;
        for (j = i-32; j < i-12; j++) {
            x[j] += carry - 16 * x[i] * Lmod[j-(i-32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    int64_t carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31]>>4) * Lmod[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++) x[j] -= carry * Lmod[j];
    for (int i = 0; i < 32; i++) { x[i+1] += x[i]>>8; r[i] = x[i] & 255; }
}

static void reduce(uint8_t *r) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (uint64_t)r[i];
    memset(r, 0, 64);
    modL(r, x);
}

/* SHA-512 wrappers using trezor-crypto */
static void ed_sha512(uint8_t out[64], const uint8_t *m, size_t n) {
    DEFAULT_SHA512_CTX ctx;
    sha512_Init(&ctx);
    sha512_Update(&ctx, m, n);
    sha512_Final(&ctx, out);
}

static void ed_sha512_3(uint8_t out[64],
    const uint8_t *a, size_t al,
    const uint8_t *b, size_t bl,
    const uint8_t *c, size_t cl) {
    DEFAULT_SHA512_CTX ctx;
    sha512_Init(&ctx);
    sha512_Update(&ctx, a, al);
    sha512_Update(&ctx, b, bl);
    sha512_Update(&ctx, c, cl);
    sha512_Final(&ctx, out);
}

/*--- Public API ---*/

void ed25519_publickey(const ed25519_secret_key sk, ed25519_public_key pk) {
    uint8_t d[64];
    gf4 p;
    ed_sha512(d, sk, 32);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
    scalarbase(p, d);
    pt_pack(pk, p);
}

void ed25519_sign(const unsigned char *m, size_t mlen,
                  const ed25519_secret_key sk, const ed25519_public_key pk,
                  ed25519_signature RS) {
    uint8_t d[64], h[64], r[64];
    int64_t x[64];
    gf4 p;

    ed_sha512(d, sk, 32);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    /* r = SHA-512(d[32..63] || m) mod L */
    {
        DEFAULT_SHA512_CTX ctx;
        sha512_Init(&ctx);
        sha512_Update(&ctx, d + 32, 32);
        sha512_Update(&ctx, m, mlen);
        sha512_Final(&ctx, r);
    }
    reduce(r);

    /* R = r * B */
    scalarbase(p, r);
    pt_pack(RS, p);

    /* h = SHA-512(R || pk || m) mod L */
    ed_sha512_3(h, RS, 32, pk, 32, m, mlen);
    reduce(h);

    /* S = (r + h * a) mod L */
    memset(x, 0, sizeof(x));
    for (int i = 0; i < 32; i++) x[i] = (uint64_t)r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++)
            x[i+j] += (int64_t)h[i] * (int64_t)d[j];
    modL(RS + 32, x);
}

int ed25519_sign_open(const unsigned char *m, size_t mlen,
                      const ed25519_public_key pk, const ed25519_signature RS) {
    (void)m; (void)mlen; (void)pk; (void)RS;
    return -1; /* verification not needed for BoAT signing-only use */
}

/*--- On-curve check for Solana PDA derivation ---*/
/* Returns 1 if the 32-byte value is a valid ed25519 point (on curve), 0 otherwise.
 * Used by boat_sol_find_pda() to reject on-curve hashes. */
int ed25519_point_is_on_curve(const uint8_t p[32])
{
    gf y, y2, u, v, v3, vxx, check;

    /* Unpack y coordinate (clear sign bit) */
    unpack25519(y, p);

    /* u = y^2 - 1 */
    S(y2, y);
    Z(u, y2, gf1);

    /* v = d*y^2 + 1 */
    M(v, D, y2);
    A(v, v, gf1);

    /* x^2 = u / v = u * v^(-1)
     * But we check via: x^2 * v == u
     * Compute x = u * v^3 * (u * v^7)^((p-5)/8)
     * Then verify x^2 * v == u */

    /* v3 = v^3 */
    S(v3, v);
    M(v3, v3, v);

    /* vxx candidate: u * v^3 */
    gf uv3;
    M(uv3, u, v3);

    /* uv7 = u * v^7 */
    gf v7, uv7;
    S(v7, v3);       /* v^6 */
    M(v7, v7, v);    /* v^7 */
    M(uv7, u, v7);

    /* x = uv3 * uv7^((p-5)/8)
     * (p-5)/8 exponent: we compute uv7^(2^252 - 3) via pow2523 */
    gf x;
    /* pow2523: raise to (2^252 - 3) */
    {
        gf c;
        memcpy(c, uv7, sizeof(gf));
        for (int i = 250; i >= 0; i--) {
            S(c, c);
            if (i != 1) M(c, c, uv7);
        }
        M(x, uv3, c);
    }

    /* vxx = v * x^2 */
    S(vxx, x);
    M(vxx, vxx, v);

    /* Check if vxx == u */
    gf diff;
    Z(diff, vxx, u);
    uint8_t d1[32];
    pack25519(d1, diff);
    int is_zero = 1;
    for (int i = 0; i < 32; i++) is_zero &= (d1[i] == 0);
    if (is_zero) return 1;

    /* Check if vxx == -u (the other root) */
    A(diff, vxx, u);
    pack25519(d1, diff);
    is_zero = 1;
    for (int i = 0; i < 32; i++) is_zero &= (d1[i] == 0);
    if (is_zero) return 1;

    return 0; /* not on curve */
}

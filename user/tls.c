/* tls.c - TLS 1.3 client and the cryptography under it (see tls.h).

   Everything here follows the published specifications:
     SHA-256            FIPS 180-4
     HMAC, HKDF         RFC 2104, RFC 5869
     X25519             RFC 7748 (field arithmetic as in TweetNaCl)
     ChaCha20-Poly1305  RFC 8439 (Poly1305 in 26-bit limbs, as in poly1305-donna)
     AES-128, GCM       FIPS 197, NIST SP 800-38D
     TLS 1.3            RFC 8446
   and is checked against their test vectors by tlstest.exe. */
#include "tls.h"
#include "x509.h"

typedef unsigned char u8;
typedef unsigned int u32;

static u32 load32_le(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void store32_le(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static u32 load32_be(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3]; }
static void store32_be(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}
static void store64_be(u8 *p, u64 v) {
    store32_be(p, (u32)(v >> 32));
    store32_be(p + 4, (u32)v);
}
static u32 rotr(u32 x, int n) { return x >> n | x << (32 - n); }
static u32 rotl(u32 x, int n) { return x << n | x >> (32 - n); }

/* Compare without leaking where they differ through timing. */
static int same(const u8 *a, const u8 *b, size_t n) {
    u8 diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

/* Forget secrets. */
static void wipe(void *p, size_t n) {
    volatile u8 *v = p;
    while (n--) *v++ = 0;
}

/* ================================================================ SHA-256 */

static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_block(sha256_ctx *c, const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = load32_be(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    u32 e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
    c->state[4] += e;
    c->state[5] += f;
    c->state[6] += g;
    c->state[7] += h;
}

void sha256_init(sha256_ctx *c) {
    static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (int i = 0; i < 8; i++) c->state[i] = iv[i];
    c->length = 0;
    c->used = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len) {
    const u8 *p = data;
    c->length += len;
    while (len) {
        size_t take = 64 - c->used;
        if (take > len) take = len;
        memcpy(c->block + c->used, p, take);
        c->used += (unsigned)take;
        p += take;
        len -= take;
        if (c->used == 64) {
            sha256_block(c, c->block);
            c->used = 0;
        }
    }
}

void sha256_final(sha256_ctx *c, u8 out[32]) {
    u64 bits = c->length * 8;
    u8 pad = 0x80;
    sha256_update(c, &pad, 1);
    u8 zero = 0;
    while (c->used != 56) sha256_update(c, &zero, 1);
    u8 len[8];
    store64_be(len, bits);
    sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) store32_be(out + 4 * i, c->state[i]);
}

void sha256(const void *data, size_t len, u8 out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ================================================================ HMAC and HKDF */

void hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, u8 out[32]) {
    u8 k[64], pad[64], inner[32];
    memset(k, 0, 64);
    if (key_len > 64) sha256(key, key_len, k);
    else memcpy(k, key, key_len);
    sha256_ctx c;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, data, len);
    sha256_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
    wipe(k, 64);
}

/* HKDF-Expand-Label from TLS 1.3 (RFC 8446 7.1), for outputs of up to 32
   bytes (all TLS 1.3 needs with SHA-256). */
static void expand_label(const u8 secret[32], const char *label, const u8 *context, size_t context_len, u8 *out,
                         size_t out_len) {
    u8 info[128];
    size_t n = 0, label_len = strlen(label);
    info[n++] = (u8)(out_len >> 8);
    info[n++] = (u8)out_len;
    info[n++] = (u8)(6 + label_len);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, label_len);
    n += label_len;
    info[n++] = (u8)context_len;
    memcpy(info + n, context, context_len);
    n += context_len;
    info[n++] = 1; /* HKDF-Expand's block counter: one block is enough */
    u8 block[32];
    hmac_sha256(secret, 32, info, n, block);
    memcpy(out, block, out_len);
    wipe(block, 32);
}

/* ================================================================ X25519 */

typedef long long gf[16];
static const gf C121665 = {0xDB41, 1};

static void carry(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += 65536;
        long long c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

static void swap_if(gf p, gf q, int b) {
    long long mask = ~((long long)b - 1);
    for (int i = 0; i < 16; i++) {
        long long t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack(u8 *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    carry(t);
    carry(t);
    carry(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        swap_if(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (u8)(t[i] & 0xff);
        o[2 * i + 1] = (u8)(t[i] >> 8);
    }
}

static void unpack(gf o, const u8 *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((long long)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}
static void fsub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}
static void fmul(gf o, const gf a, const gf b) {
    long long t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    carry(o);
    carry(o);
}
static void fsquare(gf o, const gf a) { fmul(o, a, a); }

static void finvert(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        fsquare(c, c);
        if (a != 2 && a != 4) fmul(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]) {
    u8 z[32];
    long long x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (u8)((scalar[31] & 127) | 64);
    z[0] &= 248;
    unpack(x, point);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        swap_if(a, b, r);
        swap_if(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fsquare(d, e);
        fsquare(f, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fsquare(b, a);
        fsub(c, d, f);
        fmul(a, c, C121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fsquare(b, e);
        swap_if(a, b, r);
        swap_if(c, d, r);
    }
    for (int i = 0; i < 16; i++) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    finvert(x + 32, x + 32);
    fmul(x + 16, x + 16, x + 32);
    pack(out, x + 16);
    wipe(z, 32);
}

/* ================================================================ ChaCha20 and Poly1305 */

#define QR(a, b, c, d)                                                                                                 \
    a += b, d ^= a, d = rotl(d, 16), c += d, b ^= c, b = rotl(b, 12), a += b, d ^= a, d = rotl(d, 8), c += d,         \
        b ^= c, b = rotl(b, 7)

static void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]) {
    u32 s[16], x[16];
    s[0] = 0x61707865;
    s[1] = 0x3320646e;
    s[2] = 0x79622d32;
    s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = load32_le(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = load32_le(nonce + 4 * i);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) store32_le(out + 4 * i, x[i] + s[i]);
}

void chacha20_xor(const u8 key[32], u32 counter, const u8 nonce[12], u8 *data, size_t len) {
    u8 stream[64];
    while (len) {
        chacha20_block(key, counter++, nonce, stream);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) data[i] ^= stream[i];
        data += n;
        len -= n;
    }
    wipe(stream, 64);
}

void poly1305(u8 tag[16], const u8 *msg, size_t len, const u8 key[32]) {
    u32 r0 = load32_le(key + 0) & 0x3ffffff;
    u32 r1 = (load32_le(key + 3) >> 2) & 0x3ffff03;
    u32 r2 = (load32_le(key + 6) >> 4) & 0x3ffc0ff;
    u32 r3 = (load32_le(key + 9) >> 6) & 0x3f03fff;
    u32 r4 = (load32_le(key + 12) >> 8) & 0x00fffff;
    u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    u32 h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0, c;
    while (len) {
        u8 block[17];
        size_t n = len < 16 ? len : 16;
        u32 hibit;
        memset(block, 0, sizeof(block));
        memcpy(block, msg, n);
        if (n == 16) {
            hibit = 1u << 24;
        } else {
            block[n] = 1; /* a short last block: the 1 bit goes after it */
            hibit = 0;
        }
        h0 += load32_le(block + 0) & 0x3ffffff;
        h1 += (load32_le(block + 3) >> 2) & 0x3ffffff;
        h2 += (load32_le(block + 6) >> 4) & 0x3ffffff;
        h3 += (load32_le(block + 9) >> 6) & 0x3ffffff;
        h4 += (load32_le(block + 12) >> 8) | hibit;
        u64 d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
        u64 d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
        u64 d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
        u64 d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
        u64 d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;
        c = (u32)(d0 >> 26);
        h0 = (u32)d0 & 0x3ffffff;
        d1 += c;
        c = (u32)(d1 >> 26);
        h1 = (u32)d1 & 0x3ffffff;
        d2 += c;
        c = (u32)(d2 >> 26);
        h2 = (u32)d2 & 0x3ffffff;
        d3 += c;
        c = (u32)(d3 >> 26);
        h3 = (u32)d3 & 0x3ffffff;
        d4 += c;
        c = (u32)(d4 >> 26);
        h4 = (u32)d4 & 0x3ffffff;
        h0 += c * 5;
        c = h0 >> 26;
        h0 &= 0x3ffffff;
        h1 += c;
        msg += n;
        len -= n;
    }
    c = h1 >> 26;
    h1 &= 0x3ffffff;
    h2 += c;
    c = h2 >> 26;
    h2 &= 0x3ffffff;
    h3 += c;
    c = h3 >> 26;
    h3 &= 0x3ffffff;
    h4 += c;
    c = h4 >> 26;
    h4 &= 0x3ffffff;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffff;
    h1 += c;
    /* h - p, chosen instead of h if h >= p (constant time). */
    u32 g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= 0x3ffffff;
    u32 g1 = h1 + c;
    c = g1 >> 26;
    g1 &= 0x3ffffff;
    u32 g2 = h2 + c;
    c = g2 >> 26;
    g2 &= 0x3ffffff;
    u32 g3 = h3 + c;
    c = g3 >> 26;
    g3 &= 0x3ffffff;
    u32 g4 = h4 + c - (1u << 26);
    u32 mask = (g4 >> 31) - 1;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;
    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);
    u64 f = (u64)h0 + load32_le(key + 16);
    h0 = (u32)f;
    f = (u64)h1 + load32_le(key + 20) + (f >> 32);
    h1 = (u32)f;
    f = (u64)h2 + load32_le(key + 24) + (f >> 32);
    h2 = (u32)f;
    f = (u64)h3 + load32_le(key + 28) + (f >> 32);
    h3 = (u32)f;
    store32_le(tag + 0, h0);
    store32_le(tag + 4, h1);
    store32_le(tag + 8, h2);
    store32_le(tag + 12, h3);
}

/* The Poly1305 tag over AAD and ciphertext, as RFC 8439 2.8 lays them out. */
static void chacha_tag(const u8 key[32], const u8 nonce[12], const u8 *aad, size_t aad_len, const u8 *ct, size_t len,
                       u8 tag[16]) {
    u8 otk[64];
    chacha20_block(key, 0, nonce, otk);
    size_t total = ((aad_len + 15) & ~(size_t)15) + ((len + 15) & ~(size_t)15) + 16;
    u8 *mac = malloc(total);
    if (!mac) {
        memset(tag, 0, 16);
        return;
    }
    memset(mac, 0, total);
    memcpy(mac, aad, aad_len);
    size_t at = (aad_len + 15) & ~(size_t)15;
    memcpy(mac + at, ct, len);
    at += (len + 15) & ~(size_t)15;
    for (int i = 0; i < 8; i++) mac[at + i] = (u8)((u64)aad_len >> (8 * i));
    for (int i = 0; i < 8; i++) mac[at + 8 + i] = (u8)((u64)len >> (8 * i));
    poly1305(tag, mac, total, otk);
    free(mac);
    wipe(otk, 64);
}

/* ================================================================ AES-128 and GCM */

static u8 SBOX[256];
static int sbox_ready;

static u8 rotl8(u8 x, int n) { return (u8)(x << n | x >> (8 - n)); }

/* Work the S-box out (inverse in GF(2^8), then the affine map) rather
   than typing in 256 numbers. */
static void make_sbox(void) {
    u8 p = 1, q = 1;
    do {
        p = (u8)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0)); /* p * 3 */
        q ^= (u8)(q << 1);                              /* q / 3 */
        q ^= (u8)(q << 2);
        q ^= (u8)(q << 4);
        if (q & 0x80) q ^= 0x09;
        SBOX[p] = (u8)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    SBOX[0] = 0x63;
    sbox_ready = 1;
}

static u8 xtime(u8 x) { return (u8)(x << 1 ^ ((x >> 7) * 0x1b)); }

static void aes128_expand(const u8 key[16], u8 round_keys[176]) {
    if (!sbox_ready) make_sbox();
    memcpy(round_keys, key, 16);
    u8 rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        u8 t[4];
        memcpy(t, round_keys + i - 4, 4);
        if (i % 16 == 0) {
            u8 first = t[0];
            t[0] = (u8)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[first];
            rcon = xtime(rcon);
        }
        for (int k = 0; k < 4; k++) round_keys[i + k] = round_keys[i - 16 + k] ^ t[k];
    }
}

static void aes128_block(const u8 rk[176], const u8 in[16], u8 out[16]) {
    u8 s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int round = 1; round <= 10; round++) {
        u8 t[16];
        /* SubBytes and ShiftRows together: row r moves r columns left. */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[r + 4 * c] = SBOX[s[r + 4 * ((c + r) % 4)]];
        if (round < 10) {
            for (int c = 0; c < 4; c++) { /* MixColumns */
                u8 *a = t + 4 * c;
                u8 a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                u8 all = a0 ^ a1 ^ a2 ^ a3;
                a[0] ^= all ^ xtime(a0 ^ a1);
                a[1] ^= all ^ xtime(a1 ^ a2);
                a[2] ^= all ^ xtime(a2 ^ a3);
                a[3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * round + i];
    }
    memcpy(out, s, 16);
}

void aes128_encrypt_block(const u8 key[16], const u8 in[16], u8 out[16]) {
    u8 rk[176];
    aes128_expand(key, rk);
    aes128_block(rk, in, out);
    wipe(rk, sizeof(rk));
}

/* Multiply in GF(2^128) the way GCM numbers its bits (SP 800-38D, alg. 1). */
static void gf128_mul(u8 x[16], const u8 h[16]) {
    u64 xh = 0, xl = 0, vh = 0, vl = 0, zh = 0, zl = 0;
    for (int i = 0; i < 8; i++) {
        xh = xh << 8 | x[i];
        xl = xl << 8 | x[8 + i];
        vh = vh << 8 | h[i];
        vl = vl << 8 | h[8 + i];
    }
    for (int i = 0; i < 128; i++) {
        u64 bit = i < 64 ? (xh >> (63 - i)) & 1 : (xl >> (127 - i)) & 1;
        u64 mask = 0 - bit;
        zh ^= vh & mask;
        zl ^= vl & mask;
        u64 lsb = vl & 1;
        vl = vl >> 1 | vh << 63;
        vh >>= 1;
        vh ^= 0xE100000000000000ULL & (0 - lsb);
    }
    store64_be(x, zh);
    store64_be(x + 8, zl);
}

static void ghash(const u8 h[16], const u8 *aad, size_t aad_len, const u8 *ct, size_t len, u8 out[16]) {
    u8 y[16];
    memset(y, 0, 16);
    const u8 *parts[2] = {aad, ct};
    size_t sizes[2] = {aad_len, len};
    for (int p = 0; p < 2; p++) {
        for (size_t i = 0; i < sizes[p]; i += 16) {
            size_t n = sizes[p] - i < 16 ? sizes[p] - i : 16;
            for (size_t k = 0; k < n; k++) y[k] ^= parts[p][i + k];
            gf128_mul(y, h);
        }
    }
    u8 lengths[16];
    store64_be(lengths, (u64)aad_len * 8);
    store64_be(lengths + 8, (u64)len * 8);
    for (int k = 0; k < 16; k++) y[k] ^= lengths[k];
    gf128_mul(y, h);
    memcpy(out, y, 16);
}

/* CTR mode from counter block `ctr` (incremented in its last 4 bytes). */
static void gcm_ctr(const u8 rk[176], u8 ctr[16], u8 *data, size_t len) {
    u8 stream[16];
    while (len) {
        store32_be(ctr + 12, load32_be(ctr + 12) + 1);
        aes128_block(rk, ctr, stream);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) data[i] ^= stream[i];
        data += n;
        len -= n;
    }
}

static void gcm_tag(const u8 rk[176], const u8 nonce[12], const u8 *aad, size_t aad_len, const u8 *ct, size_t len,
                    u8 tag[16]) {
    u8 h[16], zero[16], j0[16], s[16];
    memset(zero, 0, 16);
    aes128_block(rk, zero, h);
    ghash(h, aad, aad_len, ct, len, s);
    memcpy(j0, nonce, 12);
    store32_be(j0 + 12, 1);
    u8 ek[16];
    aes128_block(rk, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = ek[i] ^ s[i];
}

/* ================================================================ AEAD */

void aead_seal(int cipher, const u8 *key, const u8 nonce[12], const u8 *aad, size_t aad_len, u8 *data, size_t len,
               u8 tag[16]) {
    if (cipher == 0) {
        chacha20_xor(key, 1, nonce, data, len);
        chacha_tag(key, nonce, aad, aad_len, data, len, tag);
    } else {
        u8 rk[176], ctr[16];
        aes128_expand(key, rk);
        memcpy(ctr, nonce, 12);
        store32_be(ctr + 12, 1);
        gcm_ctr(rk, ctr, data, len);
        gcm_tag(rk, nonce, aad, aad_len, data, len, tag);
        wipe(rk, sizeof(rk));
    }
}

int aead_open(int cipher, const u8 *key, const u8 nonce[12], const u8 *aad, size_t aad_len, u8 *data, size_t len,
              const u8 tag[16]) {
    u8 expected[16];
    if (cipher == 0) {
        chacha_tag(key, nonce, aad, aad_len, data, len, expected);
        if (!same(expected, tag, 16)) return -1;
        chacha20_xor(key, 1, nonce, data, len);
    } else {
        u8 rk[176], ctr[16];
        aes128_expand(key, rk);
        gcm_tag(rk, nonce, aad, aad_len, data, len, expected);
        if (!same(expected, tag, 16)) {
            wipe(rk, sizeof(rk));
            return -1;
        }
        memcpy(ctr, nonce, 12);
        store32_be(ctr + 12, 1);
        gcm_ctr(rk, ctr, data, len);
        wipe(rk, sizeof(rk));
    }
    return 0;
}

/* ================================================================ TLS 1.3 */

#define TLS_CHACHA 0x1303
#define TLS_AES128 0x1301
/* TLS 1.2: ECDHE key exchange, signed with ECDSA or RSA */
#define TLS12_ECDSA_CHACHA 0xCCA9
#define TLS12_RSA_CHACHA 0xCCA8
#define TLS12_ECDSA_AES128 0xC02B
#define TLS12_RSA_AES128 0xC02F
#define MAX_RECORD (16384 + 256)

typedef struct {
    u8 key[32];
    u8 iv[12];
    u64 seq;
} Direction;

struct tls_session {
    int fd;
    int cipher;        /* 0 ChaCha20-Poly1305, 1 AES-128-GCM */
    int key_len;
    Direction send, recv;
    u8 record[5 + MAX_RECORD];
    u8 *data;          /* application data received but not yet read */
    size_t data_len, data_at;
    int finished;      /* the server has said goodbye */
    int failed;
    char host[128];
    u8 send_secret[32], recv_secret[32]; /* for KeyUpdate */
    int v12;           /* TLS 1.2 (else 1.3) */
    int verified;      /* the certificate chain and its signature checked out */
    char who[96];      /* the authority that vouches for the site */
    char why[160];     /* if not verified: why not */
};

static void set_keys(Direction *d, const u8 secret[32], int key_len) {
    expand_label(secret, "key", NULL, 0, d->key, (size_t)key_len);
    expand_label(secret, "iv", NULL, 0, d->iv, 12);
    d->seq = 0;
}

static void nonce_for(const Direction *d, u8 nonce[12]) {
    memcpy(nonce, d->iv, 12);
    for (int i = 0; i < 8; i++) nonce[4 + i] ^= (u8)(d->seq >> (56 - 8 * i));
}

/* Read exactly n bytes; 0 on success, -1 on error, end or timeout. */
static int read_exact(int fd, u8 *buf, size_t n, int timeout_ms) {
    size_t got = 0;
    while (got < n) {
        pollfd p = {fd, POLLIN, 0};
        int ready = poll(&p, 1, timeout_ms);
        if (ready == EINTR) continue;
        if (ready <= 0) return -1;
        i64 r = read(fd, buf + got, n - got);
        if (r == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Read one record. Returns its type and fills t->record (header + body),
   or -1. */
static int read_record(tls_session *t, size_t *len, int timeout_ms) {
    if (read_exact(t->fd, t->record, 5, timeout_ms) != 0) return -1;
    size_t n = (size_t)t->record[3] << 8 | t->record[4];
    if (n > MAX_RECORD) return -1;
    if (read_exact(t->fd, t->record + 5, n, timeout_ms) != 0) return -1;
    *len = n;
    return t->record[0];
}

/* Decrypt the record in t->record (type 23). The inner content type is
   returned, and *out / *out_len point at the content in the buffer. */
static void aad12(u8 aad[13], u64 seq, int type, size_t len) {
    for (int i = 0; i < 8; i++) aad[i] = (u8)(seq >> (56 - 8 * i));
    aad[8] = (u8)type;
    aad[9] = 3;
    aad[10] = 3;
    aad[11] = (u8)(len >> 8);
    aad[12] = (u8)len;
}

static int open_record(tls_session *t, size_t len, u8 **out, size_t *out_len) {
    if (t->v12) {
        /* TLS 1.2: AES-GCM carries 8 bytes of its nonce in the record;
           ChaCha20 makes it from the sequence number (RFC 7905). */
        u8 nonce[12], aad[13], *body = t->record + 5;
        size_t n = len;
        if (t->cipher == 1) {
            if (n < 24) return -1;
            memcpy(nonce, t->recv.iv, 4);
            memcpy(nonce + 4, body, 8);
            body += 8;
            n -= 8;
        } else {
            if (n < 16) return -1;
            nonce_for(&t->recv, nonce);
        }
        size_t plain = n - 16;
        aad12(aad, t->recv.seq, t->record[0], plain);
        if (aead_open(t->cipher, t->recv.key, nonce, aad, 13, body, plain, body + plain) != 0) return -1;
        t->recv.seq++;
        *out = body;
        *out_len = plain;
        return t->record[0];
    }
    if (len < 17) return -1;
    u8 nonce[12];
    nonce_for(&t->recv, nonce);
    u8 *body = t->record + 5;
    if (aead_open(t->cipher, t->recv.key, nonce, t->record, 5, body, len - 16, body + len - 16) != 0) return -1;
    t->recv.seq++;
    size_t n = len - 16;
    while (n > 0 && body[n - 1] == 0) n--; /* padding */
    if (n == 0) return -1;
    int type = body[n - 1];
    *out = body;
    *out_len = n - 1;
    return type;
}

/* Encrypt and send `data` as records of the given inner type. */
static int send_encrypted(tls_session *t, int type, const u8 *data, size_t len) {
    if (t->v12) {
        while (len > 0 || type != 23) {
            size_t n = len > 16384 ? 16384 : len, explicit_len = t->cipher == 1 ? 8 : 0;
            size_t total = explicit_len + n + 16;
            u8 *rec = malloc(5 + total);
            if (!rec) return -1;
            rec[0] = (u8)type;
            rec[1] = 3;
            rec[2] = 3;
            rec[3] = (u8)(total >> 8);
            rec[4] = (u8)total;
            u8 nonce[12], aad[13];
            if (t->cipher == 1) {
                memcpy(nonce, t->send.iv, 4);
                for (int i = 0; i < 8; i++) nonce[4 + i] = rec[5 + i] = (u8)(t->send.seq >> (56 - 8 * i));
            } else {
                nonce_for(&t->send, nonce);
            }
            u8 *body = rec + 5 + explicit_len;
            memcpy(body, data, n);
            aad12(aad, t->send.seq, type, n);
            aead_seal(t->cipher, t->send.key, nonce, aad, 13, body, n, body + n);
            t->send.seq++;
            int ok = fd_write(t->fd, rec, 5 + total) == (i64)(5 + total);
            free(rec);
            if (!ok) return -1;
            data += n;
            len -= n;
            if (type != 23) break;
        }
        return 0;
    }
    while (len > 0 || type != 23) {
        size_t n = len > 16384 ? 16384 : len;
        u8 *rec = malloc(5 + n + 1 + 16);
        if (!rec) return -1;
        rec[0] = 23;
        rec[1] = 3;
        rec[2] = 3;
        rec[3] = (u8)((n + 17) >> 8);
        rec[4] = (u8)(n + 17);
        memcpy(rec + 5, data, n);
        rec[5 + n] = (u8)type;
        u8 nonce[12];
        nonce_for(&t->send, nonce);
        aead_seal(t->cipher, t->send.key, nonce, rec, 5, rec + 5, n + 1, rec + 5 + n + 1);
        t->send.seq++;
        int ok = fd_write(t->fd, rec, 5 + n + 17) == (i64)(5 + n + 17);
        free(rec);
        if (!ok) return -1;
        data += n;
        len -= n;
        if (type != 23) break; /* handshake messages are sent whole */
    }
    return 0;
}

static void put16(u8 *p, size_t v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void say(char *error, const char *text) {
    size_t n = strlen(text);
    if (n > 159) n = 159;
    memcpy(error, text, n);
    error[n] = 0;
}

static const char *alert_text(int code) {
    switch (code) {
    case 40: return "the server refused the handshake (handshake_failure): it may need something RDX does not offer";
    case 42: case 43: case 44: case 45: case 46: return "the server rejected a certificate";
    case 47: return "the server found a value it did not accept (illegal_parameter)";
    case 50: return "the server could not decode a message (decode_error)";
    case 51: return "the server could not decrypt a message (decrypt_error)";
    case 70: return "the server does not speak TLS 1.3 (protocol_version)";
    case 71: return "the server wants stronger security (insufficient_security)";
    case 80: return "the server had an internal error";
    case 109: return "the server needs an extension RDX did not send";
    case 112: return "the server does not know this host name (unrecognized_name)";
    case 120: return "the server and RDX have no application protocol in common";
    }
    return "the server sent an alert";
}

tls_session *tls_connect(int fd, const char *host, char *error) {
    const char *insecure = getenv("TLS_INSECURE");
    return tls_connect_options(fd, host, insecure && insecure[0] == '1' ? TLS_NO_VERIFY : 0, error);
}

/* Check the Certificate message (TLS 1.3 layout) against the trusted
   roots. Keeps a copy of the site's own certificate for CertificateVerify. */
static void check_certificates(tls_session *t, const u8 *m, size_t len, u8 **leaf, size_t *leaf_len) {
    const u8 *certs[8];
    size_t lens[8];
    int count = 0;
    if (len < 4) return;
    size_t at = t->v12 ? 0 : 1 + (size_t)m[0]; /* TLS 1.3: a request context first */
    if (at + 3 > len) return;
    size_t list_end = at + 3 + ((size_t)m[at] << 16 | (size_t)m[at + 1] << 8 | m[at + 2]);
    at += 3;
    while (at + 3 <= list_end && list_end <= len && count < 8) {
        size_t n = (size_t)m[at] << 16 | (size_t)m[at + 1] << 8 | m[at + 2];
        if (at + 3 + n + (t->v12 ? 0 : 2) > list_end) break;
        certs[count] = m + at + 3;
        lens[count++] = n;
        at += 3 + n;
        if (!t->v12) at += 2 + ((size_t)m[at] << 8 | m[at + 1]); /* TLS 1.3: the entry's extensions */
    }
    if (!count) {
        say(t->why, "the server sent no certificate");
        return;
    }
    *leaf = malloc(lens[0]);
    if (*leaf) {
        memcpy(*leaf, certs[0], lens[0]);
        *leaf_len = lens[0];
    }
    if (x509_check_chain(certs, lens, count, t->host, t->who, sizeof(t->who), t->why) == 0) t->verified = 1;
}

/* ================================================================ TLS 1.2 */

/* The TLS 1.2 PRF with SHA-256 (RFC 5246 section 5). */
static void prf12(const u8 *secret, size_t secret_len, const char *label, const u8 *seed, size_t seed_len, u8 *out, size_t out_len) {
    u8 ls[128], a[32], buf[32 + 128], block[32];
    size_t ll = strlen(label);
    memcpy(ls, label, ll);
    memcpy(ls + ll, seed, seed_len);
    ll += seed_len;
    hmac_sha256(secret, secret_len, ls, ll, a);
    for (size_t done = 0; done < out_len;) {
        memcpy(buf, a, 32);
        memcpy(buf + 32, ls, ll);
        hmac_sha256(secret, secret_len, buf, 32 + ll, block);
        size_t n = out_len - done < 32 ? out_len - done : 32;
        memcpy(out + done, block, n);
        done += n;
        hmac_sha256(secret, secret_len, a, 32, a);
    }
    wipe(block, 32);
}

static void send_plain_handshake(tls_session *t, const u8 *msg, size_t len, sha256_ctx *transcript) {
    u8 head[5] = {22, 3, 3, (u8)(len >> 8), (u8)len};
    fd_write(t->fd, head, 5);
    fd_write(t->fd, msg, len);
    sha256_update(transcript, msg, len);
}

/* The rest of a TLS 1.2 handshake, after ServerHello. `extra` is what came
   after ServerHello in its record. 0 on success. */
static int tls12_handshake(tls_session *t, sha256_ctx *transcript, const u8 client_random[32], const u8 server_random_in[32], int ems,
                           const u8 x25519_private[32], const u8 *extra, size_t extra_len, int flags, char *error) {
    u8 server_random[32];
    memcpy(server_random, server_random_in, 32); /* t->record is about to be reused */
    u8 *messages = malloc(65536 * 4);
    if (!messages) {
        say(error, "out of memory");
        return -1;
    }
    size_t have = extra_len;
    memcpy(messages, extra, extra_len);
    u8 *leaf = NULL;
    size_t leaf_len = 0;
    int chain_ok = 0, signed_ok = 0, server_done = 0, curve = 0, failed = 0;
    u8 server_pub[133];
    size_t server_pub_len = 0;
    while (!server_done && !failed) {
        size_t at = 0;
        while (at + 4 <= have && !failed && !server_done) {
            size_t mlen = (size_t)messages[at + 1] << 16 | (size_t)messages[at + 2] << 8 | messages[at + 3];
            if (at + 4 + mlen > have) break;
            int mtype = messages[at];
            const u8 *body = messages + at + 4;
            if (mtype == 11) {
                check_certificates(t, body, mlen, &leaf, &leaf_len);
                chain_ok = t->verified;
                if (!chain_ok && !(flags & TLS_NO_VERIFY)) {
                    say(error, "untrusted certificate: ");
                    size_t n = strlen(error);
                    for (size_t k = 0; t->why[k] && n < 159; k++) error[n++] = t->why[k];
                    error[n] = 0;
                    failed = 1;
                }
            } else if (mtype == 12) { /* ServerKeyExchange: an ECDHE key, signed */
                if (mlen < 4 || body[0] != 3 || 4 + (size_t)body[3] + 4 > mlen) {
                    say(error, "the server's key exchange was not understood");
                    failed = 1;
                    break;
                }
                curve = body[1] << 8 | body[2];
                server_pub_len = body[3];
                if (server_pub_len > sizeof(server_pub)) server_pub_len = 0;
                memcpy(server_pub, body + 4, server_pub_len);
                size_t params = 4 + (size_t)body[3];
                int scheme = body[params] << 8 | body[params + 1];
                size_t sig_len = (size_t)body[params + 2] << 8 | body[params + 3];
                u8 *signed_data = malloc(64 + params);
                if (signed_data && params + 4 + sig_len <= mlen && leaf) {
                    memcpy(signed_data, client_random, 32);
                    memcpy(signed_data + 32, server_random, 32);
                    memcpy(signed_data + 64, body, params);
                    signed_ok = x509_check_signature(leaf, leaf_len, scheme, signed_data, 64 + params, body + params + 4, sig_len) == 0;
                }
                free(signed_data);
                if (!signed_ok) {
                    t->verified = 0;
                    say(t->why, "the server could not prove it holds the certificate's key");
                    if (!(flags & TLS_NO_VERIFY)) {
                        say(error, "untrusted certificate: the server could not prove it holds the certificate's key");
                        failed = 1;
                    }
                }
            } else if (mtype == 13) {
                say(error, "the server asked for a client certificate, which RDX cannot give");
                failed = 1;
            } else if (mtype == 14) {
                server_done = 1;
            }
            sha256_update(transcript, messages + at, 4 + mlen);
            at += 4 + mlen;
        }
        memmove(messages, messages + at, have - at);
        have -= at;
        if (server_done || failed) break;
        size_t len;
        int type = read_record(t, &len, 15000);
        if (type == 21 && len >= 2) {
            say(error, alert_text(t->record[6]));
            failed = 1;
        } else if (type != 22 || have + len > 65536 * 4) {
            say(error, type < 0 ? "the server stopped answering during the handshake" : "unexpected message during the handshake");
            failed = 1;
        } else {
            memcpy(messages + have, t->record + 5, len);
            have += len;
        }
    }
    free(messages);
    free(leaf);
    if (failed) return -1;
    if (!server_pub_len) {
        say(error, "the server offered no key exchange RDX can do (it needs ECDHE)");
        return -1;
    }
    /* Our half of the key exchange, and the shared secret. */
    u8 shared[48], our_pub[97], cke[4 + 1 + 97];
    size_t our_len, shared_len;
    if (curve == 0x1d && server_pub_len == 32) {
        u8 base[32] = {9};
        x25519(our_pub, x25519_private, base);
        x25519(shared, x25519_private, server_pub);
        our_len = 32;
        shared_len = 32;
    } else if (curve == 0x17 || curve == 0x18) {
        int bits = curve == 0x17 ? 256 : 384;
        u8 priv[48];
        if (ecdh_keypair(bits, priv, our_pub) != 0 || ecdh_shared(bits, shared, priv, server_pub, server_pub_len) != 0) {
            say(error, "the server's key exchange value was not valid");
            return -1;
        }
        wipe(priv, 48);
        our_len = bits == 256 ? 65 : 97;
        shared_len = bits / 8;
    } else {
        say(error, "the server chose a key exchange curve RDX did not offer");
        return -1;
    }
    cke[0] = 16;
    cke[1] = 0;
    cke[2] = 0;
    cke[3] = (u8)(our_len + 1);
    cke[4] = (u8)our_len;
    memcpy(cke + 5, our_pub, our_len);
    send_plain_handshake(t, cke, 5 + our_len, transcript);

    u8 master[48], seed[64], hash[32], key_block[2 * 32 + 2 * 12];
    if (ems) {
        sha256_ctx snapshot = *transcript;
        sha256_final(&snapshot, hash);
        prf12(shared, shared_len, "extended master secret", hash, 32, master, 48);
    } else {
        memcpy(seed, client_random, 32);
        memcpy(seed + 32, server_random, 32);
        prf12(shared, shared_len, "master secret", seed, 64, master, 48);
    }
    wipe(shared, sizeof(shared));
    memcpy(seed, server_random, 32);
    memcpy(seed + 32, client_random, 32);
    size_t kl = (size_t)t->key_len, ivl = t->cipher == 1 ? 4 : 12;
    prf12(master, 48, "key expansion", seed, 64, key_block, 2 * kl + 2 * ivl);
    memset(&t->send, 0, sizeof(t->send));
    memset(&t->recv, 0, sizeof(t->recv));
    memcpy(t->send.key, key_block, kl);
    memcpy(t->recv.key, key_block + kl, kl);
    memcpy(t->send.iv, key_block + 2 * kl, ivl);
    memcpy(t->recv.iv, key_block + 2 * kl + ivl, ivl);
    wipe(key_block, sizeof(key_block));

    /* ChangeCipherSpec and our Finished. */
    static const u8 ccs[] = {20, 3, 3, 0, 1, 1};
    fd_write(t->fd, ccs, sizeof(ccs));
    u8 finished[16] = {20, 0, 0, 12};
    sha256_ctx snapshot = *transcript;
    sha256_final(&snapshot, hash);
    prf12(master, 48, "client finished", hash, 32, finished + 4, 12);
    sha256_update(transcript, finished, 16);
    if (send_encrypted(t, 22, finished, 16) != 0) {
        say(error, "could not send the end of the handshake");
        wipe(master, 48);
        return -1;
    }
    /* The server's ChangeCipherSpec and Finished. */
    snapshot = *transcript;
    sha256_final(&snapshot, hash);
    u8 expected[12];
    prf12(master, 48, "server finished", hash, 32, expected, 12);
    wipe(master, 48);
    int seen_ccs = 0;
    for (;;) {
        size_t len;
        int type = read_record(t, &len, 15000);
        if (type == 20 && !seen_ccs) {
            seen_ccs = 1;
            continue;
        }
        if (type == 22 && !seen_ccs) continue; /* e.g. NewSessionTicket: not asked for, ignored */
        if (type == 21 && !seen_ccs && len >= 2) {
            say(error, alert_text(t->record[6]));
            return -1;
        }
        if (type != 22 && type != 21) {
            say(error, type < 0 ? "the server stopped answering during the handshake" : "unexpected message during the handshake");
            return -1;
        }
        u8 *content;
        size_t content_len;
        int inner = open_record(t, len, &content, &content_len);
        if (inner < 0) {
            say(error, "could not decrypt the server's handshake (keys do not match)");
            return -1;
        }
        if (inner == 21) {
            say(error, alert_text(content_len >= 2 ? content[1] : 0));
            return -1;
        }
        if (content_len != 16 || content[0] != 20 || !same(content + 4, expected, 12)) {
            say(error, "the server's handshake did not check out (Finished was wrong)");
            return -1;
        }
        break;
    }
    t->verified = chain_ok && signed_ok;
    if (!t->verified && !t->why[0]) say(t->why, "the server did not sign its key exchange");
    return 0;
}

tls_session *tls_connect_options(int fd, const char *host, int flags, char *error) {
    tls_session *t = malloc(sizeof(tls_session));
    if (!t) {
        say(error, "out of memory");
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->fd = fd;
    size_t host_len = strlen(host);
    if (host_len > 127) host_len = 127;
    memcpy(t->host, host, host_len);

    /* Our half of the key exchange. */
    u8 private_key[32], public_key[32], base[32];
    memset(base, 0, 32);
    base[0] = 9;
    getrandom(private_key, 32);
    x25519(public_key, private_key, base);

    /* ---- ClientHello ---- */
    u8 hello[512];
    size_t n = 0;
    hello[n++] = 1; /* handshake type: client_hello */
    n += 3;         /* length, filled in below */
    hello[n++] = 3; /* legacy_version TLS 1.2 */
    hello[n++] = 3;
    getrandom(hello + n, 32); /* random */
    n += 32;
    hello[n++] = 32; /* a session ID, which some middleboxes insist on */
    getrandom(hello + n, 32);
    n += 32;
    /* Cipher suites. TLS_CIPHERS=aes or =chacha offers just that one,
       to try each against real servers. */
    const char *only = getenv("TLS_CIPHERS");
    int offer_chacha = !only || strcmp(only, "aes") != 0, offer_aes = !only || strcmp(only, "chacha") != 0;
    put16(hello + n, (size_t)(6 * (offer_chacha + offer_aes)));
    n += 2;
    if (offer_chacha) {
        put16(hello + n, TLS_CHACHA);
        put16(hello + n + 2, TLS12_ECDSA_CHACHA);
        put16(hello + n + 4, TLS12_RSA_CHACHA);
        n += 6;
    }
    if (offer_aes) {
        put16(hello + n, TLS_AES128);
        put16(hello + n + 2, TLS12_ECDSA_AES128);
        put16(hello + n + 4, TLS12_RSA_AES128);
        n += 6;
    }
    hello[n++] = 1; /* compression: none */
    hello[n++] = 0;
    size_t ext_start = n;
    n += 2;
    /* server_name */
    put16(hello + n, 0);
    put16(hello + n + 2, host_len + 5);
    put16(hello + n + 4, host_len + 3);
    hello[n + 6] = 0;
    put16(hello + n + 7, host_len);
    memcpy(hello + n + 9, host, host_len);
    n += 9 + host_len;
    /* supported_groups: x25519, P-256; for TLS 1.2 also: uncompressed
       points, the extended master secret, secure renegotiation (none) */
    static const u8 groups[] = {0, 10, 0, 8, 0, 6, 0, 0x1d, 0, 0x17, 0, 0x18, 0, 11, 0, 2, 1, 0, 0, 0x17, 0, 0, 0xff, 1, 0, 1, 0};
    memcpy(hello + n, groups, sizeof(groups));
    n += sizeof(groups);
    /* signature_algorithms: what the server may sign with - the ones x509.c
       can check (ECDSA P-256/P-384, RSA-PSS, and RSA PKCS#1 for certificates) */
    static const u8 sigs[] = {0, 13, 0, 18, 0, 16, 4, 3, 5, 3, 8, 4, 8, 5, 8, 6, 4, 1, 5, 1, 6, 1};
    memcpy(hello + n, sigs, sizeof(sigs));
    n += sizeof(sigs);
    /* supported_versions: TLS 1.3 and 1.2 */
    static const u8 versions[] = {0, 0x2b, 0, 5, 4, 3, 4, 3, 3};
    memcpy(hello + n, versions, sizeof(versions));
    n += sizeof(versions);
    /* psk_key_exchange_modes (some servers want to see it) */
    static const u8 psk_modes[] = {0, 0x2d, 0, 2, 1, 1};
    memcpy(hello + n, psk_modes, sizeof(psk_modes));
    n += sizeof(psk_modes);
    /* key_share: our X25519 public key */
    static const u8 share_head[] = {0, 0x33, 0, 38, 0, 36, 0, 0x1d, 0, 32};
    memcpy(hello + n, share_head, sizeof(share_head));
    n += sizeof(share_head);
    memcpy(hello + n, public_key, 32);
    n += 32;
    /* ALPN: http/1.1 */
    static const u8 alpn[] = {0, 16, 0, 11, 0, 9, 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    memcpy(hello + n, alpn, sizeof(alpn));
    n += sizeof(alpn);
    put16(hello + ext_start, n - ext_start - 2);
    hello[1] = 0;
    put16(hello + 2, n - 4);

    sha256_ctx transcript;
    sha256_init(&transcript);
    sha256_update(&transcript, hello, n);
    u8 record_head[5] = {22, 3, 1, (u8)(n >> 8), (u8)n};
    if (fd_write(fd, record_head, 5) != 5 || fd_write(fd, hello, n) != (i64)n) {
        say(error, "could not send the greeting");
        free(t);
        return NULL;
    }

    /* ---- ServerHello ---- */
    size_t len;
    int type;
    for (;;) {
        type = read_record(t, &len, 15000);
        if (type != 20) break; /* change_cipher_spec: only for compatibility, ignored */
    }
    if (type == 21 && len >= 2) {
        say(error, alert_text(t->record[6]));
        free(t);
        return NULL;
    }
    u8 *sh = t->record + 5;
    if (type != 22 || len < 44 || sh[0] != 2) {
        say(error, type < 0 ? "no answer from the server" : "the server's answer was not a TLS greeting");
        free(t);
        return NULL;
    }
    size_t sh_len = (size_t)sh[1] << 16 | (size_t)sh[2] << 8 | sh[3];
    if (sh_len + 4 > len) {
        say(error, "the server's greeting was split up, which is not handled");
        free(t);
        return NULL;
    }
    static const u8 retry_random[32] = {0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
                                        0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
                                        0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};
    if (memcmp(sh + 6, retry_random, 32) == 0) {
        say(error, "the server wants a key exchange other than X25519, which RDX does not offer yet");
        free(t);
        return NULL;
    }
    size_t p = 4 + 2 + 32;
    p += 1 + sh[p]; /* session id echo */
    int suite = sh[p] << 8 | sh[p + 1];
    p += 3; /* suite and compression */
    size_t ext_end = p + 2 + ((size_t)sh[p] << 8 | sh[p + 1]);
    p += 2;
    int tls13 = 0, ems = 0;
    u8 server_key[32];
    int have_key = 0;
    while (p + 4 <= ext_end && ext_end <= len) {
        int ext = sh[p] << 8 | sh[p + 1];
        size_t elen = (size_t)sh[p + 2] << 8 | sh[p + 3];
        const u8 *e = sh + p + 4;
        if (ext == 0x2b && elen == 2 && e[0] == 3 && e[1] == 4) tls13 = 1;
        if (ext == 0x17) ems = 1;
        if (ext == 0x33 && elen == 36 && e[0] == 0 && e[1] == 0x1d && e[3] == 32) {
            memcpy(server_key, e + 4, 32);
            have_key = 1;
        }
        p += 4 + elen;
    }
    if (!tls13) {
        static const u8 downgrade[7] = {'D', 'O', 'W', 'N', 'G', 'R', 'D'};
        if (memcmp(sh + 6 + 24, downgrade, 7) == 0) {
            say(error, "the connection was tampered with (a TLS 1.3 server was made to use an older version)");
        } else if (sh[4] != 3 || sh[5] != 3) {
            say(error, "the server only speaks TLS 1.1 or older, which are no longer safe");
        } else if (suite != TLS12_ECDSA_CHACHA && suite != TLS12_RSA_CHACHA && suite != TLS12_ECDSA_AES128 && suite != TLS12_RSA_AES128) {
            say(error, "the server chose settings RDX did not offer");
        } else {
            sha256_update(&transcript, sh, sh_len + 4);
            t->v12 = 1;
            t->cipher = suite == TLS12_ECDSA_CHACHA || suite == TLS12_RSA_CHACHA ? 0 : 1;
            t->key_len = t->cipher == 0 ? 32 : 16;
            if (tls12_handshake(t, &transcript, hello + 6, sh + 6, ems, private_key, sh + sh_len + 4, len - sh_len - 4, flags, error) == 0)
                return t;
        }
        wipe(private_key, 32);
        free(t);
        return NULL;
    }
    if (!have_key || (suite != TLS_CHACHA && suite != TLS_AES128)) {
        say(error, "the server chose settings RDX did not offer");
        free(t);
        return NULL;
    }
    t->cipher = suite == TLS_CHACHA ? 0 : 1;
    t->key_len = t->cipher == 0 ? 32 : 16;
    sha256_update(&transcript, sh, sh_len + 4);

    /* ---- the key schedule (RFC 8446 7.1) ---- */
    u8 shared[32], zeros[32], empty_hash[32], early[32], derived[32], handshake[32];
    u8 client_hs[32], server_hs[32], hash[32], master[32];
    x25519(shared, private_key, server_key);
    wipe(private_key, 32);
    memset(zeros, 0, 32);
    sha256("", 0, empty_hash);
    hmac_sha256(zeros, 32, zeros, 32, early);
    expand_label(early, "derived", empty_hash, 32, derived, 32);
    hmac_sha256(derived, 32, shared, 32, handshake);
    wipe(shared, 32);
    sha256_ctx snapshot = transcript;
    sha256_final(&snapshot, hash);
    expand_label(handshake, "c hs traffic", hash, 32, client_hs, 32);
    expand_label(handshake, "s hs traffic", hash, 32, server_hs, 32);
    set_keys(&t->send, client_hs, t->key_len);
    set_keys(&t->recv, server_hs, t->key_len);

    /* ---- EncryptedExtensions, Certificate, CertificateVerify, Finished ---- */
    u8 *messages = malloc(65536 * 4);
    size_t have = 0;
    int done = 0, chain_ok = 0, signature_ok = 0;
    u8 *leaf = NULL;
    size_t leaf_len = 0;
    if (!messages) {
        say(error, "out of memory");
        free(t);
        return NULL;
    }
    while (!done) {
        type = read_record(t, &len, 15000);
        if (type == 20) continue;
        if (type == 21 && len >= 2) {
            say(error, alert_text(t->record[6]));
            break;
        }
        if (type != 23) {
            say(error, type < 0 ? "the server stopped answering during the handshake" : "unexpected message during the handshake");
            break;
        }
        u8 *content;
        size_t content_len;
        int inner = open_record(t, len, &content, &content_len);
        if (inner < 0) {
            say(error, "could not decrypt the server's handshake (keys do not match)");
            break;
        }
        if (inner == 21) {
            say(error, alert_text(content_len >= 2 ? content[1] : 0));
            break;
        }
        if (inner != 22 || have + content_len > 65536 * 4) {
            say(error, "unexpected message during the handshake");
            break;
        }
        memcpy(messages + have, content, content_len);
        have += content_len;
        /* Take every complete message so far. */
        size_t at = 0;
        while (!done && at + 4 <= have) {
            size_t mlen = (size_t)messages[at + 1] << 16 | (size_t)messages[at + 2] << 8 | messages[at + 3];
            if (at + 4 + mlen > have) break;
            int mtype = messages[at];
            if (mtype == 13) {
                say(error, "the server asked for a client certificate, which RDX cannot give");
                done = -1;
                break;
            }
            if (mtype == 11) { /* Certificate */
                check_certificates(t, messages + at + 4, mlen, &leaf, &leaf_len);
                chain_ok = t->verified;
                if (!chain_ok && !(flags & TLS_NO_VERIFY)) {
                    say(error, "untrusted certificate: ");
                    size_t n = strlen(error);
                    for (size_t k = 0; t->why[k] && n < 159; k++) error[n++] = t->why[k];
                    error[n] = 0;
                    done = -1;
                    break;
                }
            }
            if (mtype == 15) { /* CertificateVerify: the server signs the transcript with the certificate's key */
                u8 content[64 + 34 + 32];
                memset(content, ' ', 64);
                memcpy(content + 64, "TLS 1.3, server CertificateVerify", 34); /* with its terminating 0 */
                snapshot = transcript;
                sha256_final(&snapshot, content + 98);
                const u8 *body = messages + at + 4;
                int scheme = mlen >= 4 ? body[0] << 8 | body[1] : 0;
                size_t sig_len = mlen >= 4 ? (size_t)body[2] << 8 | body[3] : 0;
                signature_ok = leaf && sig_len + 4 <= mlen &&
                               x509_check_signature(leaf, leaf_len, scheme, content, sizeof(content), body + 4, sig_len) == 0;
                if (!signature_ok) {
                    t->verified = 0;
                    say(t->why, "the server could not prove it holds the certificate's key");
                    if (!(flags & TLS_NO_VERIFY)) {
                        say(error, "untrusted certificate: the server could not prove it holds the certificate's key");
                        done = -1;
                        break;
                    }
                }
            }
            if (mtype == 20) { /* the server's Finished: check it */
                u8 finished_key[32], expected[32];
                snapshot = transcript;
                sha256_final(&snapshot, hash);
                expand_label(server_hs, "finished", NULL, 0, finished_key, 32);
                hmac_sha256(finished_key, 32, hash, 32, expected);
                if (mlen != 32 || !same(expected, messages + at + 4, 32)) {
                    say(error, "the server's handshake did not check out (Finished was wrong)");
                    done = -1;
                    break;
                }
                sha256_update(&transcript, messages + at, 4 + mlen);
                done = 1;
            } else {
                sha256_update(&transcript, messages + at, 4 + mlen);
            }
            at += 4 + mlen;
        }
        memmove(messages, messages + at, have - at);
        have -= at;
    }
    free(messages);
    free(leaf);
    if (done == 1 && !signature_ok) { /* a server must sign; one that did not is not who it claims */
        t->verified = 0;
        if (!t->why[0]) say(t->why, "the server did not sign the handshake");
        if (!(flags & TLS_NO_VERIFY)) {
            say(error, "untrusted certificate: the server did not sign the handshake");
            done = -1;
        }
    }
    t->verified = t->verified && chain_ok && signature_ok;
    if (done != 1) {
        free(t);
        return NULL;
    }

    /* Application keys come from the transcript up to the server's Finished. */
    snapshot = transcript;
    sha256_final(&snapshot, hash);
    u8 client_app[32], server_app[32], finished_key[32], verify[36];
    expand_label(handshake, "derived", empty_hash, 32, derived, 32);
    hmac_sha256(derived, 32, zeros, 32, master);
    expand_label(master, "c ap traffic", hash, 32, client_app, 32);
    expand_label(master, "s ap traffic", hash, 32, server_app, 32);

    /* ---- our Finished ---- */
    static const u8 ccs[] = {20, 3, 3, 0, 1, 1}; /* a dummy change_cipher_spec, for middleboxes */
    fd_write(fd, ccs, sizeof(ccs));
    expand_label(client_hs, "finished", NULL, 0, finished_key, 32);
    verify[0] = 20;
    verify[1] = 0;
    verify[2] = 0;
    verify[3] = 32;
    hmac_sha256(finished_key, 32, hash, 32, verify + 4);
    if (send_encrypted(t, 22, verify, 36) != 0) {
        say(error, "could not send the end of the handshake");
        free(t);
        return NULL;
    }
    set_keys(&t->send, client_app, t->key_len);
    set_keys(&t->recv, server_app, t->key_len);
    memcpy(t->send_secret, client_app, 32);
    memcpy(t->recv_secret, server_app, 32);
    wipe(handshake, 32);
    wipe(master, 32);
    wipe(client_hs, 32);
    wipe(server_hs, 32);
    wipe(client_app, 32);
    wipe(server_app, 32);
    return t;
}

int tls_write(tls_session *t, const void *data, size_t len) {
    if (t->failed) return -1;
    if (send_encrypted(t, 23, data, len) != 0) {
        t->failed = 1;
        return -1;
    }
    return 0;
}

i64 tls_read(tls_session *t, void *buf, size_t max, int timeout_ms) {
    for (;;) {
        if (t->data_at < t->data_len) {
            size_t n = t->data_len - t->data_at;
            if (n > max) n = max;
            memcpy(buf, t->data + t->data_at, n);
            t->data_at += n;
            return (i64)n;
        }
        if (t->finished) return 0;
        if (t->failed) return -1;
        size_t len;
        int type = read_record(t, &len, timeout_ms);
        if (type < 0) {
            /* The server closed without saying goodbye: treat as the end
               (many do; HTTP's own lengths tell if anything is missing). */
            t->finished = 1;
            return 0;
        }
        if (type == 20) continue;
        if (type == 21) {
            t->finished = 1;
            return 0;
        }
        if (type != 23 && !(t->v12 && type == 22)) { /* TLS 1.2: a HelloRequest may come; ignored */
            t->failed = 1;
            return -1;
        }
        u8 *content;
        size_t content_len;
        int inner = open_record(t, len, &content, &content_len);
        if (inner < 0) {
            t->failed = 1;
            return -1;
        }
        if (inner == 21) { /* close_notify (or another alert): the end */
            t->finished = 1;
            return 0;
        }
        if (inner == 22) {
            /* After the handshake: NewSessionTicket (ignored), or KeyUpdate,
               which means the server's key changes from here on. */
            size_t at = 0;
            while (at + 4 <= content_len) {
                size_t mlen = (size_t)content[at + 1] << 16 | (size_t)content[at + 2] << 8 | content[at + 3];
                if (content[at] == 24 && mlen == 1) {
                    /* KeyUpdate: the server's key changes from here on, and
                       if it asks, ours does too (RFC 8446 4.6.3). */
                    u8 next[32];
                    expand_label(t->recv_secret, "traffic upd", NULL, 0, next, 32);
                    memcpy(t->recv_secret, next, 32);
                    set_keys(&t->recv, next, t->key_len);
                    if (content[at + 4] == 1) {
                        static const u8 update[] = {24, 0, 0, 1, 0};
                        if (send_encrypted(t, 22, update, sizeof(update)) != 0) {
                            t->failed = 1;
                            return -1;
                        }
                        expand_label(t->send_secret, "traffic upd", NULL, 0, next, 32);
                        memcpy(t->send_secret, next, 32);
                        set_keys(&t->send, next, t->key_len);
                    }
                    wipe(next, 32);
                }
                at += 4 + mlen;
            }
            continue;
        }
        if (inner == 23) {
            if (!t->data) t->data = malloc(MAX_RECORD);
            if (!t->data) return -1;
            memcpy(t->data, content, content_len);
            t->data_len = content_len;
            t->data_at = 0;
        }
    }
}

void tls_close(tls_session *t) {
    if (!t) return;
    if (!t->failed) {
        static const u8 close_notify[] = {1, 0};
        send_encrypted(t, 21, close_notify, 2);
    }
    free(t->data);
    wipe(t, sizeof(*t));
    free(t);
}

void tls_describe(tls_session *t, char *out, size_t max) {
    const char *parts[4] = {t->v12 ? (t->cipher == 0 ? "encrypted (TLS 1.2, ChaCha20-Poly1305), " : "encrypted (TLS 1.2, AES-128-GCM), ")
                                   : (t->cipher == 0 ? "encrypted (TLS 1.3, ChaCha20-Poly1305), " : "encrypted (TLS 1.3, AES-128-GCM), "),
                            t->verified ? "identity verified by " : "site identity NOT verified: ", t->verified ? t->who : t->why, NULL};
    size_t n = 0;
    for (int i = 0; parts[i]; i++) {
        for (const char *p = parts[i]; *p && n + 1 < max; p++) out[n++] = *p;
    }
    out[n] = 0;
}

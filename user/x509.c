/* x509.c - certificates and the cryptography to check them (see x509.h).

   Following the published specifications:
     SHA-384, SHA-512        FIPS 180-4
     RSA PKCS#1 v1.5, PSS    RFC 8017
     ECDSA, P-256, P-384     FIPS 186-4, SEC 1
     X.509, DER              RFC 5280, X.690
     host names              RFC 6125
   Big numbers are multiplied in Montgomery form; nothing here is secret
   except in p256_keypair/p256_shared (key exchange), whose arithmetic is
   the same but not hardened against timing measurements. */
#include "x509.h"
#include "tls.h"

typedef unsigned char u8;
typedef unsigned int u32;

/* ================================================================ SHA-512 */

static const u64 K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
    0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
    0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
    0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
    0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
    0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
    0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

static u64 ror64(u64 x, int n) { return x >> n | x << (64 - n); }

static void sha512_block(sha512_ctx *s, const u8 *p) {
    u64 w[80];
    for (int i = 0; i < 16; i++) {
        u64 v = 0;
        for (int k = 0; k < 8; k++) v = v << 8 | p[i * 8 + k];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        u64 s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u64 a = s->state[0], b = s->state[1], c = s->state[2], d = s->state[3];
    u64 e = s->state[4], f = s->state[5], g = s->state[6], h = s->state[7];
    for (int i = 0; i < 80; i++) {
        u64 t1 = h + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        u64 t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->state[0] += a;
    s->state[1] += b;
    s->state[2] += c;
    s->state[3] += d;
    s->state[4] += e;
    s->state[5] += f;
    s->state[6] += g;
    s->state[7] += h;
}

void sha512_init(sha512_ctx *c) {
    static const u64 iv[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                              0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    memcpy(c->state, iv, sizeof(iv));
    c->length = 0;
    c->used = 0;
    c->out_len = 64;
}

void sha384_init(sha512_ctx *c) {
    static const u64 iv[8] = {0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
                              0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull};
    memcpy(c->state, iv, sizeof(iv));
    c->length = 0;
    c->used = 0;
    c->out_len = 48;
}

void sha512_update(sha512_ctx *c, const void *data, size_t len) {
    const u8 *p = data;
    c->length += len;
    while (len > 0) {
        size_t n = 128 - c->used;
        if (n > len) n = len;
        memcpy(c->block + c->used, p, n);
        c->used += (unsigned)n;
        p += n;
        len -= n;
        if (c->used == 128) {
            sha512_block(c, c->block);
            c->used = 0;
        }
    }
}

void sha512_final(sha512_ctx *c, unsigned char *out) {
    u64 bits_low = c->length << 3, bits_high = c->length >> 61;
    c->block[c->used++] = 0x80;
    if (c->used > 112) {
        memset(c->block + c->used, 0, 128 - c->used);
        sha512_block(c, c->block);
        c->used = 0;
    }
    memset(c->block + c->used, 0, 112 - c->used);
    for (int i = 0; i < 8; i++) {
        c->block[112 + i] = (u8)(bits_high >> (56 - 8 * i));
        c->block[120 + i] = (u8)(bits_low >> (56 - 8 * i));
    }
    sha512_block(c, c->block);
    for (unsigned i = 0; i < c->out_len; i++) out[i] = (u8)(c->state[i / 8] >> (56 - 8 * (i % 8)));
}

void sha512(const void *data, size_t len, unsigned char out[64]) {
    sha512_ctx c;
    sha512_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}

void sha384(const void *data, size_t len, unsigned char out[48]) {
    sha512_ctx c;
    sha384_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}

/* Hash with SHA-256, -384 or -512; returns the length (0: unknown). */
static int hash_with(int bits, const void *data, size_t len, u8 *out) {
    if (bits == 256) {
        sha256(data, len, out);
        return 32;
    }
    if (bits == 384) {
        sha384(data, len, out);
        return 48;
    }
    if (bits == 512) {
        sha512(data, len, out);
        return 64;
    }
    return 0;
}

/* ================================================================ big numbers */

/* Numbers are little-endian arrays of 32-bit limbs. LIMBS is enough for
   4096-bit RSA keys. */
#define LIMBS 132

typedef struct {
    int n;          /* limbs in use */
    u32 m[LIMBS];   /* the (odd) modulus */
    u32 inv;        /* -m^-1 mod 2^32 */
    u32 rr[LIMBS];  /* R^2 mod m, R = 2^(32n) */
    u32 one[LIMBS]; /* R mod m: 1 in Montgomery form */
} Mont;

static int cmp_limbs(const u32 *a, const u32 *b, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

static u32 sub_limbs(u32 *r, const u32 *a, const u32 *b, int n) {
    u64 borrow = 0;
    for (int i = 0; i < n; i++) {
        u64 d = (u64)a[i] - b[i] - borrow;
        r[i] = (u32)d;
        borrow = (d >> 32) & 1;
    }
    return (u32)borrow;
}

static u32 add_limbs(u32 *r, const u32 *a, const u32 *b, int n) {
    u64 carry = 0;
    for (int i = 0; i < n; i++) {
        carry += (u64)a[i] + b[i];
        r[i] = (u32)carry;
        carry >>= 32;
    }
    return (u32)carry;
}

static int is_zero(const u32 *a, int n) {
    u32 any = 0;
    for (int i = 0; i < n; i++) any |= a[i];
    return any == 0;
}

/* r = a * b / R mod m. a, b < m. r may be a or b. */
static void mont_mul(u32 *r, const u32 *a, const u32 *b, const Mont *M) {
    int n = M->n;
    u32 t[LIMBS + 2];
    memset(t, 0, (size_t)(n + 2) * 4);
    for (int i = 0; i < n; i++) {
        u64 c = 0;
        u32 bi = b[i];
        for (int j = 0; j < n; j++) {
            c += (u64)a[j] * bi + t[j];
            t[j] = (u32)c;
            c >>= 32;
        }
        c += t[n];
        t[n] = (u32)c;
        t[n + 1] = (u32)(c >> 32);
        u32 q = t[0] * M->inv;
        c = ((u64)q * M->m[0] + t[0]) >> 32;
        for (int j = 1; j < n; j++) {
            c += (u64)q * M->m[j] + t[j];
            t[j - 1] = (u32)c;
            c >>= 32;
        }
        c += t[n];
        t[n - 1] = (u32)c;
        c >>= 32;
        t[n] = t[n + 1] + (u32)c;
    }
    if (t[n] || cmp_limbs(t, M->m, n) >= 0) sub_limbs(t, t, M->m, n);
    memcpy(r, t, (size_t)n * 4);
}

/* Big-endian bytes to limbs (n of them). -1 if it does not fit. */
static int from_bytes(u32 *out, int n, const u8 *b, size_t len) {
    while (len > 0 && b[0] == 0) b++, len--;
    if (len > (size_t)n * 4) return -1;
    memset(out, 0, (size_t)n * 4);
    for (size_t i = 0; i < len; i++) {
        size_t k = len - 1 - i; /* byte k from the end */
        out[k / 4] |= (u32)b[i] << (8 * (k % 4));
    }
    return 0;
}

static void to_bytes(u8 *out, size_t len, const u32 *a, int n) {
    for (size_t i = 0; i < len; i++) {
        size_t k = len - 1 - i;
        out[i] = k / 4 < (size_t)n ? (u8)(a[k / 4] >> (8 * (k % 4))) : 0;
    }
}

/* Prepare to work modulo the odd number in `bytes`. -1 if unusable. */
static int mont_setup(Mont *M, const u8 *bytes, size_t len) {
    while (len > 0 && bytes[0] == 0) bytes++, len--;
    if (len == 0 || len > LIMBS * 4 || !(bytes[len - 1] & 1)) return -1;
    M->n = (int)((len + 3) / 4);
    from_bytes(M->m, M->n, bytes, len);
    u32 x = M->m[0];
    for (int i = 0; i < 5; i++) x *= 2 - M->m[0] * x;
    M->inv = 0 - x;
    /* R^2 mod m by doubling 1, 64n times. */
    u32 r[LIMBS];
    memset(r, 0, sizeof(r));
    r[0] = 1;
    for (int i = 0; i < 64 * M->n; i++) {
        u32 carry = add_limbs(r, r, r, M->n);
        if (carry || cmp_limbs(r, M->m, M->n) >= 0) sub_limbs(r, r, M->m, M->n);
        if (i == 32 * M->n - 1) memcpy(M->one, r, (size_t)M->n * 4); /* R mod m on the way */
    }
    memcpy(M->rr, r, (size_t)M->n * 4);
    return 0;
}

/* r = x^e in Montgomery form (x in Montgomery form), e big-endian bytes. */
static void mont_pow(u32 *r, const u32 *x, const u8 *e, size_t e_len, const Mont *M) {
    u32 acc[LIMBS];
    memcpy(acc, M->one, (size_t)M->n * 4);
    for (size_t i = 0; i < e_len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            mont_mul(acc, acc, acc, M);
            if (e[i] >> bit & 1) mont_mul(acc, acc, x, M);
        }
    }
    memcpy(r, acc, (size_t)M->n * 4);
}

static void to_mont(u32 *r, const u32 *a, const Mont *M) { mont_mul(r, a, M->rr, M); }

static void from_mont(u32 *r, const u32 *a, const Mont *M) {
    u32 one[LIMBS];
    memset(one, 0, sizeof(one));
    one[0] = 1;
    mont_mul(r, a, one, M);
}

/* ================================================================ RSA */

static const u8 DIGEST_INFO_256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
static const u8 DIGEST_INFO_384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
static const u8 DIGEST_INFO_512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};

/* m = sig^e mod n, as k bytes (k: the modulus' length). -1 if invalid. */
static int rsa_open(const u8 *n, size_t n_len, const u8 *e, size_t e_len, const u8 *sig, size_t sig_len, u8 *em,
                    size_t *k, int *mod_bits) {
    static Mont M;
    while (n_len > 0 && n[0] == 0) n++, n_len--;
    while (e_len > 0 && e[0] == 0) e++, e_len--;
    if (n_len < 128 || e_len == 0 || e_len > 8 || sig_len > n_len) return -1; /* keys under 1024 bits are refused */
    if (mont_setup(&M, n, n_len) != 0) return -1;
    u32 s[LIMBS], x[LIMBS];
    if (from_bytes(s, M.n, sig, sig_len) != 0 || cmp_limbs(s, M.m, M.n) >= 0) return -1;
    to_mont(x, s, &M);
    mont_pow(x, x, e, e_len, &M);
    from_mont(x, x, &M);
    to_bytes(em, n_len, x, M.n);
    *k = n_len;
    int bits = (int)n_len * 8;
    for (u8 top = n[0]; !(top & 0x80); top <<= 1) bits--;
    *mod_bits = bits;
    return 0;
}

static int rsa_pkcs1_verify(const u8 *n, size_t n_len, const u8 *e, size_t e_len, int hash_bits, const u8 *data,
                            size_t data_len, const u8 *sig, size_t sig_len) {
    static u8 em[LIMBS * 4], want[LIMBS * 4];
    size_t k;
    int bits;
    if (rsa_open(n, n_len, e, e_len, sig, sig_len, em, &k, &bits) != 0) return -1;
    const u8 *prefix = hash_bits == 256 ? DIGEST_INFO_256 : hash_bits == 384 ? DIGEST_INFO_384 : DIGEST_INFO_512;
    size_t prefix_len = sizeof(DIGEST_INFO_256);
    u8 hash[64];
    int h = hash_with(hash_bits, data, data_len, hash);
    if (!h || k < prefix_len + (size_t)h + 11) return -1;
    /* 00 01 FF..FF 00 DigestInfo hash */
    size_t t = prefix_len + (size_t)h;
    want[0] = 0;
    want[1] = 1;
    memset(want + 2, 0xFF, k - t - 3);
    want[k - t - 1] = 0;
    memcpy(want + k - t, prefix, prefix_len);
    memcpy(want + k - h, hash, (size_t)h);
    return memcmp(em, want, k) == 0 ? 0 : -1;
}

static int rsa_pss_verify(const u8 *n, size_t n_len, const u8 *e, size_t e_len, int hash_bits, const u8 *data,
                          size_t data_len, const u8 *sig, size_t sig_len) {
    static u8 buf[LIMBS * 4], db[LIMBS * 4];
    size_t k;
    int mod_bits;
    if (rsa_open(n, n_len, e, e_len, sig, sig_len, buf, &k, &mod_bits) != 0) return -1;
    u8 m_hash[64];
    int h = hash_with(hash_bits, data, data_len, m_hash);
    if (!h) return -1;
    int em_bits = mod_bits - 1;
    size_t em_len = (size_t)(em_bits + 7) / 8;
    const u8 *em = buf + (k - em_len);
    if (k > em_len && buf[0] != 0) return -1;
    if (em_len < (size_t)h * 2 + 2 || em[em_len - 1] != 0xBC) return -1;
    size_t db_len = em_len - h - 1;
    const u8 *H = em + db_len;
    int top_bits = (int)(8 * em_len) - em_bits; /* these leftmost bits must be 0 */
    if (top_bits && (em[0] >> (8 - top_bits))) return -1;
    /* dbMask = MGF1(H) */
    u8 seed[68], block[64];
    memcpy(seed, H, (size_t)h);
    for (size_t done = 0, counter = 0; done < db_len; counter++) {
        seed[h] = (u8)(counter >> 24);
        seed[h + 1] = (u8)(counter >> 16);
        seed[h + 2] = (u8)(counter >> 8);
        seed[h + 3] = (u8)counter;
        hash_with(hash_bits, seed, (size_t)h + 4, block);
        for (int i = 0; i < h && done < db_len; i++, done++) db[done] = em[done] ^ block[i];
    }
    if (top_bits) db[0] &= (u8)(0xFF >> top_bits);
    /* DB = 00..00 01 salt, with a salt as long as the hash (TLS's rule). */
    size_t salt_len = (size_t)h, ps = db_len - salt_len - 1;
    for (size_t i = 0; i < ps; i++) {
        if (db[i]) return -1;
    }
    if (db[ps] != 1) return -1;
    u8 m2[8 + 64 + 64];
    memset(m2, 0, 8);
    memcpy(m2 + 8, m_hash, (size_t)h);
    memcpy(m2 + 8 + h, db + db_len - salt_len, salt_len);
    u8 h2[64];
    hash_with(hash_bits, m2, 8 + (size_t)h + salt_len, h2);
    return memcmp(h2, H, (size_t)h) == 0 ? 0 : -1;
}

/* ================================================================ elliptic curves */

typedef struct {
    int bytes;           /* 32 or 48 */
    Mont p, n;           /* the field and the group order */
    u32 b[12], gx[12], gy[12]; /* in Montgomery form (mod p) */
    u8 p_minus_2[48], n_minus_2[48];
} Curve;

typedef struct {
    u32 x[12], y[12], z[12];
    int inf;
} Point;

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void hex_bytes(const char *hex, u8 *out, int bytes) {
    for (int i = 0; i < bytes; i++) out[i] = (u8)(hex_value(hex[2 * i]) << 4 | hex_value(hex[2 * i + 1]));
}

static void minus_two(u8 *out, const u8 *in, int bytes) {
    memcpy(out, in, (size_t)bytes);
    int borrow = 2;
    for (int i = bytes - 1; i >= 0 && borrow; i--) {
        int v = out[i] - borrow;
        borrow = v < 0;
        out[i] = (u8)(v & 0xFF);
    }
}

static Curve *curve_for(int bits) {
    static Curve p256, p384;
    static int ready256, ready384;
    static const char *P256[5] = {
        "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
        "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
        "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
        "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
        "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"};
    static const char *P384[5] = {
        "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
        "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
        "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
        "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
        "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f"};
    Curve *c = bits == 256 ? &p256 : bits == 384 ? &p384 : NULL;
    if (!c) return NULL;
    int *ready = bits == 256 ? &ready256 : &ready384;
    if (*ready) return c;
    const char **hex = bits == 256 ? P256 : P384;
    int bytes = bits / 8;
    u8 buf[48];
    u32 t[12];
    c->bytes = bytes;
    hex_bytes(hex[0], buf, bytes);
    mont_setup(&c->p, buf, (size_t)bytes);
    minus_two(c->p_minus_2, buf, bytes);
    hex_bytes(hex[2], buf, bytes);
    mont_setup(&c->n, buf, (size_t)bytes);
    minus_two(c->n_minus_2, buf, bytes);
    u32 *targets[3] = {c->b, c->gx, c->gy};
    int which[3] = {1, 3, 4};
    for (int i = 0; i < 3; i++) {
        hex_bytes(hex[which[i]], buf, bytes);
        from_bytes(t, c->p.n, buf, (size_t)bytes);
        to_mont(targets[i], t, &c->p);
    }
    *ready = 1;
    return c;
}

static void fadd(u32 *r, const u32 *a, const u32 *b, const Mont *M) {
    u32 carry = add_limbs(r, a, b, M->n);
    if (carry || cmp_limbs(r, M->m, M->n) >= 0) sub_limbs(r, r, M->m, M->n);
}

static void fsub(u32 *r, const u32 *a, const u32 *b, const Mont *M) {
    if (sub_limbs(r, a, b, M->n)) add_limbs(r, r, M->m, M->n);
}

/* Point doubling in Jacobian coordinates, a = -3 (dbl-2001-b). */
static void point_double(Point *r, const Point *a, const Curve *C) {
    const Mont *M = &C->p;
    int n = M->n;
    if (a->inf) {
        r->inf = 1;
        return;
    }
    u32 delta[12], gamma[12], beta[12], alpha[12], t1[12], t2[12], x3[12], y3[12], z3[12];
    mont_mul(delta, a->z, a->z, M);
    mont_mul(gamma, a->y, a->y, M);
    mont_mul(beta, a->x, gamma, M);
    fsub(t1, a->x, delta, M);
    fadd(t2, a->x, delta, M);
    mont_mul(alpha, t1, t2, M);
    fadd(t1, alpha, alpha, M);
    fadd(alpha, t1, alpha, M); /* 3 (x - delta)(x + delta) */
    fadd(t1, beta, beta, M);
    fadd(t1, t1, t1, M); /* 4 beta */
    fadd(t2, t1, t1, M); /* 8 beta */
    mont_mul(x3, alpha, alpha, M);
    fsub(x3, x3, t2, M);
    fadd(z3, a->y, a->z, M);
    mont_mul(z3, z3, z3, M);
    fsub(z3, z3, gamma, M);
    fsub(z3, z3, delta, M);
    fsub(t1, t1, x3, M);
    mont_mul(y3, alpha, t1, M);
    mont_mul(t2, gamma, gamma, M);
    fadd(t2, t2, t2, M);
    fadd(t2, t2, t2, M);
    fadd(t2, t2, t2, M); /* 8 gamma^2 */
    fsub(y3, y3, t2, M);
    memcpy(r->x, x3, (size_t)n * 4);
    memcpy(r->y, y3, (size_t)n * 4);
    memcpy(r->z, z3, (size_t)n * 4);
    r->inf = is_zero(z3, n);
}

/* Point addition in Jacobian coordinates (add-2007-bl). */
static void point_add(Point *r, const Point *a, const Point *b, const Curve *C) {
    const Mont *M = &C->p;
    int n = M->n;
    if (a->inf) {
        *r = *b;
        return;
    }
    if (b->inf) {
        *r = *a;
        return;
    }
    u32 z1z1[12], z2z2[12], u1[12], u2[12], s1[12], s2[12], h[12], rr[12], i[12], j[12], v[12], t[12];
    u32 x3[12], y3[12], z3[12];
    mont_mul(z1z1, a->z, a->z, M);
    mont_mul(z2z2, b->z, b->z, M);
    mont_mul(u1, a->x, z2z2, M);
    mont_mul(u2, b->x, z1z1, M);
    mont_mul(s1, a->y, b->z, M);
    mont_mul(s1, s1, z2z2, M);
    mont_mul(s2, b->y, a->z, M);
    mont_mul(s2, s2, z1z1, M);
    fsub(h, u2, u1, M);
    fsub(rr, s2, s1, M);
    if (is_zero(h, n)) {
        if (is_zero(rr, n)) {
            point_double(r, a, C);
        } else {
            r->inf = 1;
        }
        return;
    }
    fadd(rr, rr, rr, M);
    fadd(i, h, h, M);
    mont_mul(i, i, i, M);
    mont_mul(j, h, i, M);
    mont_mul(v, u1, i, M);
    mont_mul(x3, rr, rr, M);
    fsub(x3, x3, j, M);
    fsub(x3, x3, v, M);
    fsub(x3, x3, v, M);
    fsub(t, v, x3, M);
    mont_mul(y3, rr, t, M);
    mont_mul(t, s1, j, M);
    fadd(t, t, t, M);
    fsub(y3, y3, t, M);
    fadd(z3, a->z, b->z, M);
    mont_mul(z3, z3, z3, M);
    fsub(z3, z3, z1z1, M);
    fsub(z3, z3, z2z2, M);
    mont_mul(z3, z3, h, M);
    memcpy(r->x, x3, (size_t)n * 4);
    memcpy(r->y, y3, (size_t)n * 4);
    memcpy(r->z, z3, (size_t)n * 4);
    r->inf = 0;
}

/* r = k * P, k big-endian bytes. */
static void point_mul(Point *r, const u8 *k, size_t k_len, const Point *P, const Curve *C) {
    Point acc;
    acc.inf = 1;
    for (size_t i = 0; i < k_len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            point_double(&acc, &acc, C);
            if (k[i] >> bit & 1) point_add(&acc, &acc, P, C);
        }
    }
    *r = acc;
}

/* Affine x and y (normal form) of P. -1 at infinity. */
static int point_affine(u32 *x, u32 *y, const Point *P, const Curve *C) {
    if (P->inf) return -1;
    const Mont *M = &C->p;
    u32 zi[12], zi2[12], t[12];
    mont_pow(zi, P->z, C->p_minus_2, (size_t)C->bytes, M);
    mont_mul(zi2, zi, zi, M);
    mont_mul(t, P->x, zi2, M);
    from_mont(x, t, M);
    if (y) {
        mont_mul(zi2, zi2, zi, M);
        mont_mul(t, P->y, zi2, M);
        from_mont(y, t, M);
    }
    return 0;
}

/* Load an uncompressed public point (04 X Y), checking it is on the curve. */
static int load_point(Point *P, const u8 *data, size_t len, const Curve *C) {
    const Mont *M = &C->p;
    int b = C->bytes;
    if (len != (size_t)(1 + 2 * b) || data[0] != 4) return -1;
    u32 x[12], y[12];
    from_bytes(x, M->n, data + 1, (size_t)b);
    from_bytes(y, M->n, data + 1 + b, (size_t)b);
    if (cmp_limbs(x, M->m, M->n) >= 0 || cmp_limbs(y, M->m, M->n) >= 0) return -1;
    to_mont(P->x, x, M);
    to_mont(P->y, y, M);
    memcpy(P->z, M->one, (size_t)M->n * 4);
    P->inf = 0;
    /* y^2 = x^3 - 3x + b */
    u32 lhs[12], rhs[12], t[12];
    mont_mul(lhs, P->y, P->y, M);
    mont_mul(rhs, P->x, P->x, M);
    mont_mul(rhs, rhs, P->x, M);
    fadd(t, P->x, P->x, M);
    fadd(t, t, P->x, M);
    fsub(rhs, rhs, t, M);
    fadd(rhs, rhs, C->b, M);
    return cmp_limbs(lhs, rhs, M->n) == 0 ? 0 : -1;
}

static int ecdsa_verify(const Curve *C, const u8 *point, size_t point_len, const u8 *hash, size_t hash_len, const u8 *r_bytes,
                        size_t r_len, const u8 *s_bytes, size_t s_len) {
    const Mont *N = &C->n;
    int nl = N->n;
    Point Q, G, P1, P2;
    if (load_point(&Q, point, point_len, C) != 0) return -1;
    u32 r[12], s[12], e[12], w[12], t[12], u1[12], u2[12];
    if (from_bytes(r, nl, r_bytes, r_len) != 0 || from_bytes(s, nl, s_bytes, s_len) != 0) return -1;
    if (is_zero(r, nl) || is_zero(s, nl) || cmp_limbs(r, N->m, nl) >= 0 || cmp_limbs(s, N->m, nl) >= 0) return -1;
    from_bytes(e, nl, hash, hash_len > (size_t)C->bytes ? (size_t)C->bytes : hash_len);
    if (cmp_limbs(e, N->m, nl) >= 0) sub_limbs(e, e, N->m, nl);
    to_mont(t, s, N);
    mont_pow(w, t, C->n_minus_2, (size_t)C->bytes, N); /* s^-1, Montgomery form */
    to_mont(t, e, N);
    mont_mul(t, t, w, N);
    from_mont(u1, t, N);
    to_mont(t, r, N);
    mont_mul(t, t, w, N);
    from_mont(u2, t, N);
    u8 k1[48], k2[48];
    to_bytes(k1, (size_t)C->bytes, u1, nl);
    to_bytes(k2, (size_t)C->bytes, u2, nl);
    memcpy(G.x, C->gx, sizeof(G.x));
    memcpy(G.y, C->gy, sizeof(G.y));
    memcpy(G.z, C->p.one, sizeof(G.z));
    G.inf = 0;
    point_mul(&P1, k1, (size_t)C->bytes, &G, C);
    point_mul(&P2, k2, (size_t)C->bytes, &Q, C);
    point_add(&P1, &P1, &P2, C);
    u32 x[12];
    if (point_affine(x, NULL, &P1, C) != 0) return -1;
    if (cmp_limbs(x, N->m, nl) >= 0) sub_limbs(x, x, N->m, nl);
    return cmp_limbs(x, r, nl) == 0 ? 0 : -1;
}

int ecdh_keypair(int bits, unsigned char *priv, unsigned char *pub) {
    Curve *C = curve_for(bits);
    if (!C) return -1;
    int b = C->bytes;
    u32 k[12];
    for (int tries = 0; tries < 16; tries++) {
        getrandom(priv, (size_t)b);
        from_bytes(k, C->n.n, priv, (size_t)b);
        if (!is_zero(k, C->n.n) && cmp_limbs(k, C->n.m, C->n.n) < 0) break;
    }
    Point G, P;
    memcpy(G.x, C->gx, sizeof(G.x));
    memcpy(G.y, C->gy, sizeof(G.y));
    memcpy(G.z, C->p.one, sizeof(G.z));
    G.inf = 0;
    point_mul(&P, priv, (size_t)b, &G, C);
    u32 x[12], y[12];
    if (point_affine(x, y, &P, C) != 0) return -1;
    pub[0] = 4;
    to_bytes(pub + 1, (size_t)b, x, C->p.n);
    to_bytes(pub + 1 + b, (size_t)b, y, C->p.n);
    return 0;
}

int ecdh_shared(int bits, unsigned char *out, const unsigned char *priv, const unsigned char *peer, size_t peer_len) {
    Curve *C = curve_for(bits);
    Point Q, P;
    if (!C || load_point(&Q, peer, peer_len, C) != 0) return -1;
    point_mul(&P, priv, (size_t)C->bytes, &Q, C);
    u32 x[12];
    if (point_affine(x, NULL, &P, C) != 0) return -1;
    to_bytes(out, (size_t)C->bytes, x, C->p.n);
    return 0;
}

int p256_keypair(unsigned char priv[32], unsigned char pub[65]) { return ecdh_keypair(256, priv, pub); }

int p256_shared(unsigned char out[32], const unsigned char priv[32], const unsigned char *peer, size_t peer_len) {
    return ecdh_shared(256, out, priv, peer, peer_len);
}

/* ================================================================ DER */

typedef struct {
    const u8 *p;
    size_t len;
} Span;

/* Take one element from the front of *in: its tag and contents. */
static int der(Span *in, int *tag, Span *out) {
    if (in->len < 2) return -1;
    const u8 *p = in->p;
    int t = p[0];
    if ((t & 0x1F) == 0x1F) return -1;
    size_t len = p[1], head = 2;
    if (len & 0x80) {
        int k = (int)(len & 0x7F);
        if (k == 0 || k > 4 || in->len < 2 + (size_t)k) return -1;
        len = 0;
        for (int i = 0; i < k; i++) len = len << 8 | p[2 + i];
        head = 2 + (size_t)k;
    }
    if (len > in->len - head) return -1;
    *tag = t;
    out->p = p + head;
    out->len = len;
    in->p += head + len;
    in->len -= head + len;
    return 0;
}

static int der_expect(Span *in, int tag, Span *out) {
    int t;
    return der(in, &t, out) == 0 && t == tag ? 0 : -1;
}

/* An INTEGER's value bytes, without a leading zero. */
static void strip_integer(Span *s) {
    while (s->len > 1 && s->p[0] == 0) s->p++, s->len--;
}

static int oid_is(Span oid, const u8 *want, size_t len) { return oid.len == len && memcmp(oid.p, want, len) == 0; }

static const u8 OID_RSA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
static const u8 OID_RSA_SHA256[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B};
static const u8 OID_RSA_SHA384[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C};
static const u8 OID_RSA_SHA512[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D};
static const u8 OID_EC[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
static const u8 OID_P256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
static const u8 OID_P384[] = {0x2B, 0x81, 0x04, 0x00, 0x22};
static const u8 OID_ECDSA_SHA256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
static const u8 OID_ECDSA_SHA384[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03};
static const u8 OID_ECDSA_SHA512[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04};
static const u8 OID_SAN[] = {0x55, 0x1D, 0x11};
static const u8 OID_BASIC[] = {0x55, 0x1D, 0x13};
static const u8 OID_CN[] = {0x55, 0x04, 0x03};
static const u8 OID_O[] = {0x55, 0x04, 0x0A};

typedef struct {
    Span whole;          /* the whole certificate */
    Span tbs;            /* the signed part, header included */
    char sig_kind;       /* 'r' RSA PKCS#1, 'e' ECDSA, 0 unknown */
    int sig_hash;        /* 256, 384, 512 */
    Span sig;            /* the signature */
    Span issuer, subject;/* names, header included (compared byte for byte) */
    u64 not_before, not_after; /* YYYYMMDDHHMMSS */
    char key_kind;       /* 'r' RSA, 'e' EC, 0 unknown */
    int curve;           /* 256, 384 */
    Span key_n, key_e;   /* RSA */
    Span key_point;      /* EC */
    int is_ca;
    Span san;            /* subjectAltName: GeneralNames' contents */
    int has_san;
} Cert;

static u64 parse_time(int tag, Span t) {
    u64 v = 0;
    int digits = 0;
    for (size_t i = 0; i < t.len && t.p[i] >= '0' && t.p[i] <= '9'; i++, digits++) v = v * 10 + (u64)(t.p[i] - '0');
    if (tag == 0x17 && digits >= 12) { /* UTCTime: YYMMDDHHMMSS */
        u64 yy = v / 10000000000ull;
        v += (yy < 50 ? 2000 : 1900) * 10000000000ull;
        return v;
    }
    if (tag == 0x18 && digits >= 14) return v; /* GeneralizedTime */
    return 0;
}

static int parse_cert(const u8 *data, size_t len, Cert *c) {
    memset(c, 0, sizeof(*c));
    Span all = {data, len}, cert, tbs, alg, oid, bits;
    if (der_expect(&all, 0x30, &cert) != 0) return -1;
    c->whole.p = data;
    c->whole.len = len - all.len;
    Span rest = cert;
    const u8 *tbs_start = rest.p;
    if (der_expect(&rest, 0x30, &tbs) != 0) return -1;
    c->tbs.p = tbs_start;
    c->tbs.len = (size_t)(rest.p - tbs_start);
    if (der_expect(&rest, 0x30, &alg) != 0 || der_expect(&alg, 0x06, &oid) != 0) return -1;
    if (oid_is(oid, OID_RSA_SHA256, sizeof(OID_RSA_SHA256))) c->sig_kind = 'r', c->sig_hash = 256;
    else if (oid_is(oid, OID_RSA_SHA384, sizeof(OID_RSA_SHA384))) c->sig_kind = 'r', c->sig_hash = 384;
    else if (oid_is(oid, OID_RSA_SHA512, sizeof(OID_RSA_SHA512))) c->sig_kind = 'r', c->sig_hash = 512;
    else if (oid_is(oid, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256))) c->sig_kind = 'e', c->sig_hash = 256;
    else if (oid_is(oid, OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384))) c->sig_kind = 'e', c->sig_hash = 384;
    else if (oid_is(oid, OID_ECDSA_SHA512, sizeof(OID_ECDSA_SHA512))) c->sig_kind = 'e', c->sig_hash = 512;
    if (der_expect(&rest, 0x03, &bits) != 0 || bits.len < 1 || bits.p[0] != 0) return -1;
    c->sig.p = bits.p + 1;
    c->sig.len = bits.len - 1;

    int tag;
    Span item;
    if (tbs.len && tbs.p[0] == 0xA0 && der(&tbs, &tag, &item) != 0) return -1; /* version */
    if (der_expect(&tbs, 0x02, &item) != 0) return -1;                        /* serial number */
    if (der_expect(&tbs, 0x30, &item) != 0) return -1;                        /* signature algorithm */
    const u8 *start = tbs.p;
    if (der_expect(&tbs, 0x30, &item) != 0) return -1;
    c->issuer.p = start;
    c->issuer.len = (size_t)(tbs.p - start);
    Span validity, t1, t2;
    int tag1, tag2;
    if (der_expect(&tbs, 0x30, &validity) != 0 || der(&validity, &tag1, &t1) != 0 || der(&validity, &tag2, &t2) != 0) return -1;
    c->not_before = parse_time(tag1, t1);
    c->not_after = parse_time(tag2, t2);
    start = tbs.p;
    if (der_expect(&tbs, 0x30, &item) != 0) return -1;
    c->subject.p = start;
    c->subject.len = (size_t)(tbs.p - start);
    Span spki, key_alg, key_oid, key_bits;
    if (der_expect(&tbs, 0x30, &spki) != 0 || der_expect(&spki, 0x30, &key_alg) != 0 || der_expect(&key_alg, 0x06, &key_oid) != 0 ||
        der_expect(&spki, 0x03, &key_bits) != 0 || key_bits.len < 1)
        return -1;
    Span key = {key_bits.p + 1, key_bits.len - 1};
    if (oid_is(key_oid, OID_RSA, sizeof(OID_RSA))) {
        Span seq;
        if (der_expect(&key, 0x30, &seq) == 0 && der_expect(&seq, 0x02, &c->key_n) == 0 && der_expect(&seq, 0x02, &c->key_e) == 0) {
            strip_integer(&c->key_n);
            strip_integer(&c->key_e);
            c->key_kind = 'r';
        }
    } else if (oid_is(key_oid, OID_EC, sizeof(OID_EC))) {
        Span curve;
        if (der_expect(&key_alg, 0x06, &curve) == 0) {
            c->curve = oid_is(curve, OID_P256, sizeof(OID_P256)) ? 256 : oid_is(curve, OID_P384, sizeof(OID_P384)) ? 384 : 0;
            if (c->curve) {
                c->key_point = key;
                c->key_kind = 'e';
            }
        }
    }
    /* Optional parts, then the extensions ([3]). */
    while (tbs.len) {
        if (der(&tbs, &tag, &item) != 0) return -1;
        if (tag != 0xA3) continue;
        Span exts, ext;
        if (der_expect(&item, 0x30, &exts) != 0) return -1;
        while (exts.len) {
            if (der_expect(&exts, 0x30, &ext) != 0) return -1;
            Span ext_oid, value;
            if (der_expect(&ext, 0x06, &ext_oid) != 0) return -1;
            if (ext.len && ext.p[0] == 0x01 && der(&ext, &tag, &value) != 0) return -1; /* critical flag */
            if (der_expect(&ext, 0x04, &value) != 0) return -1;
            if (oid_is(ext_oid, OID_SAN, sizeof(OID_SAN))) {
                if (der_expect(&value, 0x30, &c->san) == 0) c->has_san = 1;
            } else if (oid_is(ext_oid, OID_BASIC, sizeof(OID_BASIC))) {
                Span seq, flag;
                if (der_expect(&value, 0x30, &seq) == 0 && seq.len && seq.p[0] == 0x01 && der_expect(&seq, 0x01, &flag) == 0)
                    c->is_ca = flag.len == 1 && flag.p[0] != 0;
            }
        }
    }
    return 0;
}

/* The common name (or else organization) in a Name, as text. */
static void name_text(Span name, const u8 *want, size_t want_len, char *out, size_t max) {
    Span seq, set, atv, oid, value;
    int tag;
    out[0] = 0;
    if (der_expect(&name, 0x30, &seq) != 0) return;
    while (seq.len) {
        if (der_expect(&seq, 0x31, &set) != 0) return;
        while (set.len) {
            if (der_expect(&set, 0x30, &atv) != 0 || der_expect(&atv, 0x06, &oid) != 0 || der(&atv, &tag, &value) != 0) return;
            if (oid_is(oid, want, want_len)) {
                size_t n = value.len < max - 1 ? value.len : max - 1;
                for (size_t i = 0; i < n; i++) out[i] = value.p[i] >= 32 && value.p[i] < 127 ? (char)value.p[i] : '?';
                out[n] = 0;
            }
        }
    }
}

static void name_of(Span name, char *out, size_t max) {
    name_text(name, OID_O, sizeof(OID_O), out, max);
    if (!out[0]) name_text(name, OID_CN, sizeof(OID_CN), out, max);
    if (!out[0] && max > 10) strcpy(out, "(unnamed)");
}

/* ================================================================ checking */

/* Is `sig` a signature over `data` by the key in `key`? */
static int check_with_key(const Cert *key, char kind, int hash_bits, int pss, const u8 *data, size_t data_len, const u8 *sig,
                          size_t sig_len) {
    if (kind == 'r') {
        if (key->key_kind != 'r') return -1;
        return pss ? rsa_pss_verify(key->key_n.p, key->key_n.len, key->key_e.p, key->key_e.len, hash_bits, data, data_len, sig, sig_len)
                   : rsa_pkcs1_verify(key->key_n.p, key->key_n.len, key->key_e.p, key->key_e.len, hash_bits, data, data_len, sig, sig_len);
    }
    if (kind == 'e') {
        if (key->key_kind != 'e') return -1;
        Curve *C = curve_for(key->curve);
        Span all = {sig, sig_len}, seq, r, s;
        if (!C || der_expect(&all, 0x30, &seq) != 0 || der_expect(&seq, 0x02, &r) != 0 || der_expect(&seq, 0x02, &s) != 0) return -1;
        u8 hash[64];
        int h = hash_with(hash_bits, data, data_len, hash);
        if (!h) return -1;
        return ecdsa_verify(C, key->key_point.p, key->key_point.len, hash, (size_t)h, r.p, r.len, s.p, s.len);
    }
    return -1;
}

/* Did `issuer` sign `child`? */
static int signed_by(const Cert *child, const Cert *issuer) {
    if (!child->sig_kind) return -1;
    return check_with_key(issuer, child->sig_kind, child->sig_hash, 0, child->tbs.p, child->tbs.len, child->sig.p, child->sig.len);
}

static int scheme_parts(int scheme, char *kind, int *hash, int *pss) {
    switch (scheme) {
    case 0x0401: *kind = 'r', *hash = 256, *pss = 0; return 0;
    case 0x0501: *kind = 'r', *hash = 384, *pss = 0; return 0;
    case 0x0601: *kind = 'r', *hash = 512, *pss = 0; return 0;
    case 0x0804: *kind = 'r', *hash = 256, *pss = 1; return 0;
    case 0x0805: *kind = 'r', *hash = 384, *pss = 1; return 0;
    case 0x0806: *kind = 'r', *hash = 512, *pss = 1; return 0;
    case 0x0403: *kind = 'e', *hash = 256, *pss = 0; return 0;
    case 0x0503: *kind = 'e', *hash = 384, *pss = 0; return 0;
    case 0x0603: *kind = 'e', *hash = 512, *pss = 0; return 0;
    }
    return -1;
}

int x509_check_signature(const unsigned char *cert, size_t cert_len, int scheme, const unsigned char *data, size_t data_len,
                         const unsigned char *sig, size_t sig_len) {
    Cert c;
    char kind;
    int hash, pss;
    if (parse_cert(cert, cert_len, &c) != 0 || scheme_parts(scheme, &kind, &hash, &pss) != 0) return -1;
    return check_with_key(&c, kind, hash, pss, data, data_len, sig, sig_len);
}

int x509_check_raw(int kind, const unsigned char *key, size_t key_len, const unsigned char *key2, size_t key2_len, int scheme,
                   const unsigned char *data, size_t data_len, const unsigned char *sig, size_t sig_len) {
    Cert c;
    char k;
    int hash, pss;
    memset(&c, 0, sizeof(c));
    if (scheme_parts(scheme, &k, &hash, &pss) != 0 || k != kind) return -1;
    if (kind == 'r') {
        c.key_kind = 'r';
        c.key_n = (Span){key, key_len};
        c.key_e = (Span){key2, key2_len};
    } else {
        c.key_kind = 'e';
        c.curve = key_len == 65 ? 256 : key_len == 97 ? 384 : 0;
        c.key_point = (Span){key, key_len};
    }
    return check_with_key(&c, k, hash, pss, data, data_len, sig, sig_len);
}

/* ---- the trusted roots ---- */

static Cert *roots;
static u8 **root_data;
static int root_count, roots_loaded;

static int b64_value(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void load_roots(void) {
    if (roots_loaded) return;
    roots_loaded = 1;
    size_t len;
    char *pem = read_all("/roots.pem", &len);
    if (!pem) return;
    int cap = 0;
    for (char *p = pem; (p = strstr(p, "-----BEGIN CERTIFICATE-----")) != NULL; p++) cap++;
    roots = malloc(sizeof(Cert) * (size_t)(cap + 1));
    root_data = malloc(sizeof(u8 *) * (size_t)(cap + 1));
    if (!roots || !root_data) return;
    for (char *p = pem; (p = strstr(p, "-----BEGIN CERTIFICATE-----")) != NULL;) {
        p += 27;
        char *end = strstr(p, "-----END CERTIFICATE-----");
        if (!end) break;
        u8 *der_bytes = malloc((size_t)(end - p) * 3 / 4 + 4);
        if (!der_bytes) break;
        size_t n = 0;
        u32 acc = 0;
        int got = 0;
        for (char *q = p; q < end; q++) {
            int v = b64_value((unsigned char)*q);
            if (v < 0) continue;
            acc = acc << 6 | (u32)v;
            if (++got == 4) {
                der_bytes[n++] = (u8)(acc >> 16);
                der_bytes[n++] = (u8)(acc >> 8);
                der_bytes[n++] = (u8)acc;
                got = 0;
                acc = 0;
            }
        }
        if (got == 3) {
            der_bytes[n++] = (u8)(acc >> 10);
            der_bytes[n++] = (u8)(acc >> 2);
        } else if (got == 2) {
            der_bytes[n++] = (u8)(acc >> 4);
        }
        if (parse_cert(der_bytes, n, &roots[root_count]) == 0) {
            root_data[root_count++] = der_bytes;
        } else {
            free(der_bytes);
        }
        p = end + 25;
    }
    free(pem);
}

/* Now, as YYYYMMDDHHMMSS (0 if the clock is clearly unset). */
static u64 now_digits(void) {
    u64 secs = time_now();
    long long days = (long long)(secs / 86400), rem = (long long)(secs % 86400);
    long long z = days + 719468, era = z / 146097, doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    if (y < 2024) return 0;
    return (u64)(((((y * 100 + m) * 100 + d) * 100 + rem / 3600) * 100 + rem / 60 % 60) * 100 + rem % 60);
}

static void date_text(u64 digits, char *out) {
    /* "2026-11-19" */
    u64 date = digits / 1000000;
    out[0] = (char)('0' + date / 10000000 % 10);
    out[1] = (char)('0' + date / 1000000 % 10);
    out[2] = (char)('0' + date / 100000 % 10);
    out[3] = (char)('0' + date / 10000 % 10);
    out[4] = '-';
    out[5] = (char)('0' + date / 1000 % 10);
    out[6] = (char)('0' + date / 100 % 10);
    out[7] = '-';
    out[8] = (char)('0' + date / 10 % 10);
    out[9] = (char)('0' + date % 10);
    out[10] = 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Does the certificate name `pattern` (perhaps "*.example.com") cover host? */
static int name_matches(const u8 *pattern, size_t plen, const char *host) {
    size_t hlen = strlen(host);
    if (hlen && host[hlen - 1] == '.') hlen--;
    if (plen >= 2 && pattern[0] == '*' && pattern[1] == '.') {
        /* The star stands for exactly one whole label. */
        const char *dot = NULL;
        for (size_t i = 0; i < hlen && !dot; i++) {
            if (host[i] == '.') dot = host + i;
        }
        if (!dot || dot == host) return 0;
        size_t rest = hlen - (size_t)(dot - host);
        if (rest != plen - 1) return 0;
        for (size_t i = 0; i < rest; i++) {
            if (lower((unsigned char)dot[i]) != lower(pattern[1 + i])) return 0;
        }
        return 1;
    }
    if (plen != hlen) return 0;
    for (size_t i = 0; i < hlen; i++) {
        if (lower((unsigned char)host[i]) != lower(pattern[i])) return 0;
    }
    return 1;
}

static int parse_ip(const char *s, u8 ip[4]) {
    for (int i = 0; i < 4; i++) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'), digits++;
        if (!digits || v > 255) return -1;
        ip[i] = (u8)v;
        if (i < 3 && *s++ != '.') return -1;
    }
    return *s ? -1 : 0;
}

static int host_covered(const Cert *c, const char *host, char *first_name, size_t max) {
    Span names = c->san, value;
    int tag;
    u8 ip[4];
    int is_ip = parse_ip(host, ip) == 0;
    first_name[0] = 0;
    while (names.len && der(&names, &tag, &value) == 0) {
        if (tag == 0x82) {
            if (!first_name[0]) {
                size_t n = value.len < max - 1 ? value.len : max - 1;
                memcpy(first_name, value.p, n);
                first_name[n] = 0;
            }
            if (!is_ip && name_matches(value.p, value.len, host)) return 1;
        } else if (tag == 0x87 && is_ip && value.len == 4 && memcmp(value.p, ip, 4) == 0) {
            return 1;
        }
    }
    return 0;
}

static void say(char *error, const char *a, const char *b, const char *c) {
    size_t n = 0;
    const char *parts[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        for (const char *p = parts[i]; p && *p && n < 159; p++) error[n++] = *p;
    }
    error[n] = 0;
}

static int same_span(Span a, Span b) { return a.len == b.len && memcmp(a.p, b.p, a.len) == 0; }

int x509_check_chain(const unsigned char *const *certs, const size_t *lens, int count, const char *host, char *who, size_t who_max,
                     char *error) {
    Cert chain[8], cur;
    char text[96], date[16];
    if (count < 1) {
        say(error, "the server sent no certificate", NULL, NULL);
        return -1;
    }
    if (count > 8) count = 8;
    for (int i = 0; i < count; i++) {
        if (parse_cert(certs[i], lens[i], &chain[i]) != 0) {
            say(error, "the server's certificate could not be read", NULL, NULL);
            return -1;
        }
    }
    /* The name. */
    if (!chain[0].has_san || !host_covered(&chain[0], host, text, 64)) {
        say(error, "the certificate is for ", text[0] ? text : "another site", ", not this one");
        return -1;
    }
    load_roots();
    if (!root_count) {
        say(error, "there is no list of trusted authorities (/roots.pem is missing)", NULL, NULL);
        return -1;
    }
    u64 now = now_digits();
    cur = chain[0];
    for (int depth = 0; depth < 8; depth++) {
        if (now && cur.not_after && now > cur.not_after) {
            date_text(cur.not_after, date);
            say(error, depth ? "an authority's certificate in the chain expired on " : "the certificate expired on ", date, NULL);
            return -1;
        }
        if (now && cur.not_before && now < cur.not_before) {
            date_text(cur.not_before, date);
            say(error, "the certificate is not valid until ", date, " (or this machine's clock is wrong)");
            return -1;
        }
        /* Signed by a trusted root? (Several roots can share a name.) */
        int named = 0;
        for (int r = 0; r < root_count; r++) {
            if (!same_span(roots[r].subject, cur.issuer)) continue;
            named = 1;
            if (now && roots[r].not_after && now > roots[r].not_after) continue;
            if (signed_by(&cur, &roots[r]) == 0) {
                name_of(depth ? chain[0].issuer : cur.issuer, who, who_max);
                return 0;
            }
        }
        /* Is it itself one of the roots? */
        for (int r = 0; r < root_count; r++) {
            if (same_span(roots[r].whole, cur.whole)) {
                name_of(chain[0].issuer, who, who_max);
                return 0;
            }
        }
        /* The next authority up, from what the server sent. */
        int found = -1;
        for (int i = 1; i < count && found < 0; i++) {
            if (same_span(chain[i].subject, cur.issuer) && !same_span(chain[i].whole, cur.whole) && signed_by(&cur, &chain[i]) == 0)
                found = i;
        }
        if (found < 0) {
            name_of(cur.issuer, text, sizeof(text));
            if (named) say(error, "a signature in the certificate chain does not check out (", text, ")");
            else if (same_span(cur.issuer, cur.subject)) say(error, "the certificate is self-signed: no trusted authority vouches for it", NULL, NULL);
            else if (!cur.sig_kind) say(error, "the certificate is signed in a way RDX cannot check yet", NULL, NULL);
            else say(error, "it is issued by \"", text, "\", which this machine does not trust");
            return -1;
        }
        if (!chain[found].is_ca) {
            say(error, "the certificate was signed by a certificate that may not sign others", NULL, NULL);
            return -1;
        }
        cur = chain[found];
    }
    say(error, "the certificate chain is too long", NULL, NULL);
    return -1;
}

static void append(char *out, size_t max, const char *text) {
    size_t n = strlen(out);
    while (*text && n + 1 < max) out[n++] = *text++;
    out[n] = 0;
}

void x509_describe(const unsigned char *cert, size_t len, char *out, size_t max) {
    Cert c;
    out[0] = 0;
    if (parse_cert(cert, len, &c) != 0) {
        append(out, max, "(not a certificate)");
        return;
    }
    char subject[96], issuer[96], until[16];
    name_text(c.subject, OID_CN, sizeof(OID_CN), subject, sizeof(subject));
    if (!subject[0]) name_of(c.subject, subject, sizeof(subject));
    name_of(c.issuer, issuer, sizeof(issuer));
    date_text(c.not_after, until);
    append(out, max, subject);
    append(out, max, ", issued by ");
    append(out, max, issuer);
    append(out, max, ", valid until ");
    append(out, max, until);
    append(out, max, c.key_kind == 'r' ? ", RSA key" : c.key_kind == 'e' ? (c.curve == 256 ? ", P-256 key" : ", P-384 key") : ", unknown key");
    if (c.is_ca) append(out, max, ", authority");
}

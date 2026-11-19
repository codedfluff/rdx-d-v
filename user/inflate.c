/* inflate.c - a deflate decoder (RFC 1951), with the zlib (RFC 1950) and
   gzip (RFC 1952) wrappers. The Huffman decoding follows the canonical
   code layout used by zlib's reference decoder "puff": codes of each
   length are counted, and symbols are listed in code order. */
#include "inflate.h"

typedef unsigned char u8;

#define MAXBITS 15

typedef struct {
    const u8 *in;
    size_t len, pos;
    unsigned bitbuf;
    int bitcnt;
    u8 *out;
    size_t out_len, out_cap, max_out;
    int error;
} State;

typedef struct {
    short count[MAXBITS + 1];
    short symbol[288];
} Huffman;

static int bits(State *s, int need) {
    unsigned val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->len) {
            s->error = 1;
            return 0;
        }
        val |= (unsigned)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int put(State *s, u8 byte) {
    if (s->out_len == s->out_cap) {
        if (s->out_cap >= s->max_out) {
            s->error = 1;
            return -1;
        }
        size_t cap = s->out_cap ? s->out_cap * 2 : 65536;
        if (cap > s->max_out) cap = s->max_out;
        u8 *bigger = realloc(s->out, cap + 1);
        if (!bigger) {
            s->error = 1;
            return -1;
        }
        s->out = bigger;
        s->out_cap = cap;
    }
    s->out[s->out_len++] = byte;
    return 0;
}

/* Decode one symbol, a bit at a time but without a call per bit. */
static int decode(State *s, const Huffman *h) {
    int code = 0, first = 0, index = 0, len = 1;
    unsigned bitbuf = s->bitbuf;
    int left = s->bitcnt;
    const short *next = h->count + 1;
    for (;;) {
        while (left--) {
            code |= bitbuf & 1;
            bitbuf >>= 1;
            int count = *next++;
            if (code - count < first) {
                s->bitbuf = bitbuf;
                s->bitcnt = (s->bitcnt - len) & 7;
                return h->symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            len++;
        }
        left = (MAXBITS + 1) - len;
        if (left == 0) break;
        if (s->pos >= s->len) break;
        bitbuf = s->in[s->pos++];
        if (left > 8) left = 8;
    }
    s->error = 1;
    return -1;
}

/* Build a decoding table from code lengths. Returns <0 for an
   over-subscribed set, 0 for complete, >0 for incomplete. */
static int construct(Huffman *h, const short *length, int n) {
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int symbol = 0; symbol < n; symbol++) {
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    }
    return left;
}

static const short LBASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DBASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(State *s, const Huffman *lencode, const Huffman *distcode) {
    for (;;) {
        int symbol = decode(s, lencode);
        if (s->error) return -1;
        if (symbol < 256) {
            if (put(s, (u8)symbol)) return -1;
        } else if (symbol == 256) {
            return 0;
        } else {
            symbol -= 257;
            if (symbol >= 29) return -1;
            int len = LBASE[symbol] + bits(s, LEXT[symbol]);
            int dsym = decode(s, distcode);
            if (s->error || dsym < 0 || dsym >= 30) return -1;
            size_t dist = (size_t)DBASE[dsym] + (size_t)bits(s, DEXT[dsym]);
            if (s->error || dist > s->out_len) return -1;
            while (len--) {
                if (put(s, s->out[s->out_len - dist])) return -1;
            }
        }
    }
}

static int stored(State *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->pos + 4 > s->len) return -1;
    unsigned len = s->in[s->pos] | (unsigned)s->in[s->pos + 1] << 8;
    unsigned nlen = s->in[s->pos + 2] | (unsigned)s->in[s->pos + 3] << 8;
    s->pos += 4;
    if (len != (~nlen & 0xffff) || s->pos + len > s->len) return -1;
    while (len--) {
        if (put(s, s->in[s->pos++])) return -1;
    }
    return 0;
}

static int fixed(State *s) {
    static Huffman lencode, distcode;
    static int ready;
    if (!ready) {
        short lengths[288];
        int symbol = 0;
        for (; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < 288; symbol++) lengths[symbol] = 8;
        construct(&lencode, lengths, 288);
        for (symbol = 0; symbol < 30; symbol++) lengths[symbol] = 5;
        construct(&distcode, lengths, 30);
        ready = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(State *s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    Huffman lencode, distcode;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->error || nlen > 286 || ndist > 30) return -1;
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->error || construct(&lencode, lengths, 19) != 0) return -1;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (s->error || symbol < 0) return -1;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            short len = 0;
            int repeat;
            if (symbol == 16) {
                if (index == 0) return -1;
                len = lengths[index - 1];
                repeat = 3 + bits(s, 2);
            } else if (symbol == 17) {
                repeat = 3 + bits(s, 3);
            } else {
                repeat = 11 + bits(s, 7);
            }
            if (index + repeat > nlen + ndist) return -1;
            while (repeat--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) return -1;
    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -1;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -1;
    return codes(s, &lencode, &distcode);
}

/* Inflate from `in`; on success *consumed says how many bytes were used. */
static int run(const u8 *in, size_t len, u8 **out, size_t *out_len, size_t max_out, size_t *consumed) {
    State s;
    memset(&s, 0, sizeof(s));
    s.in = in;
    s.len = len;
    s.max_out = max_out ? max_out : 64 * 1024 * 1024;
    int last, err = 0;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.error) break;
        err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (err || s.error) break;
    } while (!last);
    if (err || s.error) {
        free(s.out);
        return -1;
    }
    if (!s.out) s.out = malloc(1);
    if (!s.out) return -1;
    s.out[s.out_len] = 0; /* room was kept for this */
    *out = s.out;
    *out_len = s.out_len;
    if (consumed) *consumed = s.pos;
    return 0;
}

int inflate_raw(const u8 *in, size_t len, u8 **out, size_t *out_len, size_t max_out) {
    return run(in, len, out, out_len, max_out, NULL);
}

int zlib_decompress(const u8 *in, size_t len, u8 **out, size_t *out_len, size_t max_out) {
    if (len < 2 || (in[0] & 0x0f) != 8 || ((unsigned)in[0] << 8 | in[1]) % 31 != 0 || (in[1] & 0x20)) return -1;
    return run(in + 2, len - 2, out, out_len, max_out, NULL);
}

int gzip_decompress(const u8 *in, size_t len, u8 **out, size_t *out_len, size_t max_out) {
    if (len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return -1;
    int flags = in[3];
    size_t p = 10;
    if (flags & 4) { /* extra field */
        if (p + 2 > len) return -1;
        p += 2 + (in[p] | (size_t)in[p + 1] << 8);
    }
    if (flags & 8) { /* file name */
        while (p < len && in[p]) p++;
        p++;
    }
    if (flags & 16) { /* comment */
        while (p < len && in[p]) p++;
        p++;
    }
    if (flags & 2) p += 2; /* header checksum */
    if (p >= len) return -1;
    return run(in + p, len - p, out, out_len, max_out, NULL);
}

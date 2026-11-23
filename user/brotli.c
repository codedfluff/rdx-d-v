/* brotli.c - reading Brotli-compressed data (RFC 7932).

   A stream is a series of meta-blocks. Each holds prefix (Huffman) codes
   for literals, for insert-and-copy commands and for distances, chosen
   per "block type" and, for literals and distances, per context; copies
   reach back into what was already written, or into a built-in
   dictionary of words, changed by one of 121 transforms. */
#include "brotli.h"

/* RFC 7932 Appendix B: the 121 word transforms (prefix, kind, suffix); kinds: 0 identity, 1-9 omit last n,
   10 uppercase first, 11 uppercase all, 12-20 omit first n - 11. (Made from brotli's transform.c, MIT licence.) */
static const struct { const char *prefix; unsigned char kind; const char *suffix; } BROTLI_TRANSFORMS[121] = {
    {"", 0, ""},
    {"", 0, " "},
    {" ", 0, " "},
    {"", 12, ""},
    {"", 10, " "},
    {"", 0, " the "},
    {" ", 0, ""},
    {"s ", 0, " "},
    {"", 0, " of "},
    {"", 10, ""},
    {"", 0, " and "},
    {"", 13, ""},
    {"", 1, ""},
    {", ", 0, " "},
    {"", 0, ", "},
    {" ", 10, " "},
    {"", 0, " in "},
    {"", 0, " to "},
    {"e ", 0, " "},
    {"", 0, "\x22"""},
    {"", 0, "."},
    {"", 0, "\x22"">"},
    {"", 0, "\x0a"""},
    {"", 3, ""},
    {"", 0, "]"},
    {"", 0, " for "},
    {"", 14, ""},
    {"", 2, ""},
    {"", 0, " a "},
    {"", 0, " that "},
    {" ", 10, ""},
    {"", 0, ". "},
    {".", 0, ""},
    {" ", 0, ", "},
    {"", 15, ""},
    {"", 0, " with "},
    {"", 0, "'"},
    {"", 0, " from "},
    {"", 0, " by "},
    {"", 16, ""},
    {"", 17, ""},
    {" the ", 0, ""},
    {"", 4, ""},
    {"", 0, ". The "},
    {"", 11, ""},
    {"", 0, " on "},
    {"", 0, " as "},
    {"", 0, " is "},
    {"", 7, ""},
    {"", 1, "ing "},
    {"", 0, "\x0a""\x09"""},
    {"", 0, ":"},
    {" ", 0, ". "},
    {"", 0, "ed "},
    {"", 20, ""},
    {"", 18, ""},
    {"", 6, ""},
    {"", 0, "("},
    {"", 10, ", "},
    {"", 8, ""},
    {"", 0, " at "},
    {"", 0, "ly "},
    {" the ", 0, " of "},
    {"", 5, ""},
    {"", 9, ""},
    {" ", 10, ", "},
    {"", 10, "\x22"""},
    {".", 0, "("},
    {"", 11, " "},
    {"", 10, "\x22"">"},
    {"", 0, "=\x22"""},
    {" ", 0, "."},
    {".com/", 0, ""},
    {" the ", 0, " of the "},
    {"", 10, "'"},
    {"", 0, ". This "},
    {"", 0, ","},
    {".", 0, " "},
    {"", 10, "("},
    {"", 10, "."},
    {"", 0, " not "},
    {" ", 0, "=\x22"""},
    {"", 0, "er "},
    {" ", 11, " "},
    {"", 0, "al "},
    {" ", 11, ""},
    {"", 0, "='"},
    {"", 11, "\x22"""},
    {"", 10, ". "},
    {" ", 0, "("},
    {"", 0, "ful "},
    {" ", 10, ". "},
    {"", 0, "ive "},
    {"", 0, "less "},
    {"", 11, "'"},
    {"", 0, "est "},
    {" ", 10, "."},
    {"", 11, "\x22"">"},
    {" ", 0, "='"},
    {"", 10, ","},
    {"", 0, "ize "},
    {"", 11, "."},
    {"\xc2""\xa0""", 0, ""},
    {" ", 0, ","},
    {"", 10, "=\x22"""},
    {"", 11, "=\x22"""},
    {"", 0, "ous "},
    {"", 11, ", "},
    {"", 10, "='"},
    {" ", 10, ","},
    {" ", 11, "=\x22"""},
    {" ", 11, ", "},
    {"", 11, ","},
    {"", 11, "("},
    {"", 11, ". "},
    {" ", 11, "."},
    {"", 11, "='"},
    {" ", 11, ". "},
    {" ", 10, "=\x22"""},
    {" ", 11, "='"},
    {" ", 10, "='"},
};

static const unsigned char *dictionary;

void brotli_set_dictionary(const unsigned char *dict) { dictionary = dict; }

static int load_dictionary(void) {
    if (dictionary) return 1;
    size_t len;
    char *d = read_all("/brotli-dict.bin", &len);
    if (!d || len != 122784) {
        free(d);
        return 0;
    }
    dictionary = (const unsigned char *)d;
    return 1;
}

/* ---------------------------------------------------------------- bits */

typedef struct {
    const unsigned char *p;
    size_t len, pos;
    unsigned long long bits;
    int nbits, error;
} Br;

static void br_fill(Br *b, int n) {
    while (b->nbits < n) {
        if (b->pos >= b->len) {
            b->error = 1; /* (read past the end: damaged) */
            b->pos++;
            b->nbits += 8;
            continue;
        }
        b->bits |= (unsigned long long)b->p[b->pos++] << b->nbits;
        b->nbits += 8;
    }
}

static unsigned br_peek(Br *b, int n) {
    br_fill(b, n);
    return (unsigned)(b->bits & ((1ull << n) - 1));
}

static void br_drop(Br *b, int n) {
    b->bits >>= n;
    b->nbits -= n;
}

static unsigned br_bits(Br *b, int n) {
    if (!n) return 0;
    unsigned v = br_peek(b, n);
    br_drop(b, n);
    return v;
}

static void br_align(Br *b) { /* to the next whole byte */
    b->bits = 0;
    b->nbits = 0;
}

/* ---------------------------------------------------------------- prefix codes */

typedef struct {
    unsigned short count[16];
    unsigned short *symbol;
    int single; /* -1, or the one symbol (a code of no bits) */
} Huff;

static void huff_free(Huff *h) {
    free(h->symbol);
    h->symbol = NULL;
}

/* A canonical code from code lengths (0: not used). */
static int huff_build(Huff *h, const unsigned char *lens, int n) {
    memset(h->count, 0, sizeof(h->count));
    h->single = -1;
    int used = 0, last = 0;
    for (int i = 0; i < n; i++)
        if (lens[i]) {
            h->count[lens[i]]++;
            used++;
            last = i;
        }
    h->symbol = malloc(sizeof(unsigned short) * (size_t)(used ? used : 1));
    if (!h->symbol) return 0;
    if (used == 1) {
        h->single = last;
        return 1;
    }
    unsigned short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (unsigned short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (unsigned short)i;
    return 1;
}

static int huff_decode(Br *b, const Huff *h) {
    if (h->single >= 0) return h->single;
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)br_bits(b, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    b->error = 1;
    return 0;
}

static int read_prefix_code(Br *b, int alpha, Huff *h) {
    unsigned char *lens = malloc((size_t)alpha);
    if (!lens) return 0;
    memset(lens, 0, (size_t)alpha);
    h->symbol = NULL;
    int hskip = (int)br_bits(b, 2);
    if (hskip == 1) { /* a simple code: up to four symbols */
        int nsym = (int)br_bits(b, 2) + 1, abits = 0;
        while ((1 << abits) < alpha) abits++;
        int syms[4];
        for (int i = 0; i < nsym; i++) {
            syms[i] = (int)br_bits(b, abits);
            if (syms[i] >= alpha) b->error = 1;
            for (int k = 0; k < i; k++)
                if (syms[k] == syms[i]) b->error = 1;
        }
        if (b->error) {
            free(lens);
            return 0;
        }
        if (nsym == 1) {
            memset(h->count, 0, sizeof(h->count));
            h->symbol = malloc(sizeof(unsigned short));
            h->single = syms[0];
            free(lens);
            return h->symbol != NULL;
        }
        if (nsym == 2) lens[syms[0]] = lens[syms[1]] = 1;
        else if (nsym == 3) {
            lens[syms[0]] = 1;
            lens[syms[1]] = lens[syms[2]] = 2;
        } else if (br_bits(b, 1)) {
            lens[syms[0]] = 1;
            lens[syms[1]] = 2;
            lens[syms[2]] = lens[syms[3]] = 3;
        } else {
            for (int i = 0; i < 4; i++) lens[syms[i]] = 2;
        }
    } else { /* a complex code: code lengths, themselves prefix coded */
        static const unsigned char order[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        static const unsigned char cl_len[16] = {2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4};
        static const unsigned char cl_val[16] = {0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5};
        unsigned char cl[18] = {0};
        int space = 32, num = 0;
        for (int i = hskip; i < 18; i++) {
            unsigned v = br_peek(b, 4);
            br_drop(b, cl_len[v]);
            cl[order[i]] = cl_val[v];
            if (cl_val[v]) {
                space -= 32 >> cl_val[v];
                num++;
                if (space <= 0) break;
            }
        }
        if (num != 1 && space != 0) {
            free(lens);
            b->error = 1;
            return 0;
        }
        Huff clh;
        if (!huff_build(&clh, cl, 18)) {
            free(lens);
            return 0;
        }
        int prev = 8, repeat = 0, repeat_len = 0, left = 32768;
        for (int s = 0; s < alpha && left > 0 && !b->error;) {
            int code = huff_decode(b, &clh);
            if (code < 16) {
                repeat = 0;
                lens[s++] = (unsigned char)code;
                if (code) {
                    prev = code;
                    left -= 32768 >> code;
                }
            } else {
                int extra = code == 16 ? 2 : 3, new_len = code == 16 ? prev : 0;
                if (repeat_len != new_len) {
                    repeat = 0;
                    repeat_len = new_len;
                }
                int old = repeat;
                if (repeat > 0) {
                    repeat -= 2;
                    repeat <<= extra;
                }
                repeat += (int)br_bits(b, extra) + 3;
                int delta = repeat - old;
                if (s + delta > alpha) {
                    b->error = 1;
                    break;
                }
                for (int k = 0; k < delta; k++) lens[s++] = (unsigned char)new_len;
                if (new_len) left -= delta * (32768 >> new_len);
            }
        }
        huff_free(&clh);
        if (left != 0) {
            int used = 0; /* (one symbol alone is allowed) */
            for (int i = 0; i < alpha; i++) used += lens[i] != 0;
            if (used != 1) b->error = 1;
        }
    }
    int ok = !b->error && huff_build(h, lens, alpha);
    free(lens);
    return ok;
}

static Huff *calloc_huffs(int n) {
    Huff *h = malloc(sizeof(Huff) * (size_t)(n > 0 ? n : 1));
    if (h) memset(h, 0, sizeof(Huff) * (size_t)(n > 0 ? n : 1));
    return h;
}

/* ---------------------------------------------------------------- small numbers */

static int read_varlen8(Br *b) { /* 0..255 */
    if (!br_bits(b, 1)) return 0;
    int n = (int)br_bits(b, 3);
    if (!n) return 1;
    return (int)br_bits(b, n) + (1 << n);
}

static const unsigned short BLOCK_BASE[26] = {1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113, 145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289, 4337, 8433, 16625};
static const unsigned char BLOCK_BITS[26] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};

static const unsigned INSERT_BASE[24] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26, 34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
static const unsigned char INSERT_BITS[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
static const unsigned COPY_BASE[24] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
static const unsigned char COPY_BITS[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};

/* ---------------------------------------------------------------- block types */

typedef struct {
    int ntypes, type, prev, count;
    Huff types, counts;
} Blocks;

static int read_block_count(Br *b, Blocks *k) {
    int s = huff_decode(b, &k->counts);
    if (s < 0 || s >= 26) {
        b->error = 1;
        return 0;
    }
    return (int)BLOCK_BASE[s] + (int)br_bits(b, BLOCK_BITS[s]);
}

static int read_blocks(Br *b, Blocks *k) {
    memset(k, 0, sizeof(*k));
    k->ntypes = read_varlen8(b) + 1;
    k->prev = 1;
    k->count = 1 << 28;
    if (k->ntypes < 2) return 1;
    if (!read_prefix_code(b, k->ntypes + 2, &k->types) || !read_prefix_code(b, 26, &k->counts)) return 0;
    k->count = read_block_count(b, k);
    return !b->error;
}

static void next_block(Br *b, Blocks *k) {
    if (k->count == 0) {
        int s = huff_decode(b, &k->types);
        int t = s == 0 ? k->prev : s == 1 ? k->type + 1 : s - 2;
        if (t >= k->ntypes) t -= k->ntypes;
        k->prev = k->type;
        k->type = t;
        k->count = read_block_count(b, k);
    }
    k->count--;
}

static void blocks_free(Blocks *k) {
    huff_free(&k->types);
    huff_free(&k->counts);
}

/* ---------------------------------------------------------------- context maps */

static int read_context_map(Br *b, int size, int ntrees, unsigned char *map) {
    if (ntrees < 2) {
        memset(map, 0, (size_t)size);
        return 1;
    }
    int rlemax = br_bits(b, 1) ? (int)br_bits(b, 4) + 1 : 0;
    Huff h;
    if (!read_prefix_code(b, ntrees + rlemax, &h)) return 0;
    for (int i = 0; i < size && !b->error;) {
        int code = huff_decode(b, &h);
        if (code == 0) map[i++] = 0;
        else if (code <= rlemax) {
            int reps = (1 << code) + (int)br_bits(b, code);
            while (reps--) {
                if (i >= size) {
                    b->error = 1;
                    break;
                }
                map[i++] = 0;
            }
        } else {
            if (code - rlemax >= ntrees) b->error = 1;
            map[i++] = (unsigned char)(code - rlemax);
        }
    }
    huff_free(&h);
    if (br_bits(b, 1)) { /* inverse move-to-front */
        unsigned char mtf[256];
        for (int i = 0; i < 256; i++) mtf[i] = (unsigned char)i;
        for (int i = 0; i < size; i++) {
            int idx = map[i];
            unsigned char v = mtf[idx];
            map[i] = v;
            for (; idx; idx--) mtf[idx] = mtf[idx - 1];
            mtf[0] = v;
        }
    }
    return !b->error;
}

/* the literal contexts (RFC 7932 section 7.1) */
static const unsigned char LUT0[256] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  4,  0,  0,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    8,  12, 16, 12, 12, 20, 12, 16, 24, 28, 12, 12, 32, 12, 36, 12, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 32, 32, 24, 40, 28, 12,
    12, 48, 52, 52, 52, 48, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 24, 12, 28, 12, 12,
    12, 56, 60, 60, 60, 56, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 24, 12, 28, 12, 0,
    0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,
    0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,
    2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,
    2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3};
static const unsigned char LUT1[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static int lut2(int c) { return c == 0 ? 0 : c < 16 ? 1 : c < 64 ? 2 : c < 128 ? 3 : c < 192 ? 4 : c < 240 ? 5 : c < 255 ? 6 : 7; }

static int literal_context(int mode, int p1, int p2) {
    switch (mode) {
    case 0: return p1 & 0x3F;                   /* LSB6 */
    case 1: return p1 >> 2;                     /* MSB6 */
    case 2: return LUT0[p1] | LUT1[p2];         /* UTF8 */
    default: return lut2(p1) << 3 | lut2(p2);  /* signed */
    }
}

/* ---------------------------------------------------------------- the dictionary */

static const unsigned char DICT_BITS[25] = {0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10, 9, 9, 8, 7, 7, 8, 7, 7, 6, 6, 5, 5};

static int to_upper(unsigned char *p, int left) {
    if (p[0] < 0xC0) {
        if (p[0] >= 'a' && p[0] <= 'z') p[0] ^= 32;
        return 1;
    }
    if (p[0] < 0xE0) {
        if (left > 1) p[1] ^= 32;
        return 2;
    }
    if (left > 2) p[2] ^= 5;
    return 3;
}

/* A dictionary word with transform t: into out (at least 64 bytes); its length. */
static int dictionary_word(int len, int index, int t, unsigned char *out) {
    unsigned offset = 0;
    for (int l = 4; l < len; l++) offset += (unsigned)l << DICT_BITS[l];
    const unsigned char *word = dictionary + offset + (unsigned)index * (unsigned)len;
    const char *pre = BROTLI_TRANSFORMS[t].prefix, *suf = BROTLI_TRANSFORMS[t].suffix;
    int kind = BROTLI_TRANSFORMS[t].kind, n = 0;
    while (*pre) out[n++] = (unsigned char)*pre++;
    int from = 0, to = len;
    if (kind >= 1 && kind <= 9) to -= kind;
    if (kind >= 12 && kind <= 20) from = kind - 11 < len ? kind - 11 : len;
    int start = n;
    for (int i = from; i < to; i++) out[n++] = word[i];
    if (kind == 10 && n > start) to_upper(out + start, n - start);
    if (kind == 11)
        for (int i = start; i < n;) i += to_upper(out + i, n - i);
    while (*suf) out[n++] = (unsigned char)*suf++;
    return n;
}

/* ---------------------------------------------------------------- the stream */

int brotli_decompress(const unsigned char *in, size_t len, unsigned char **out_p, size_t *out_len, size_t max_out) {
    Br b = {in, len, 0, 0, 0, 0};
    size_t cap = len * 4 + 1024, n = 0;
    if (cap > max_out) cap = max_out ? max_out : 1;
    unsigned char *out = malloc(cap);
    if (!out) return -1;
    /* the window */
    int wbits;
    if (!br_bits(&b, 1)) wbits = 16;
    else {
        int k = (int)br_bits(&b, 3);
        if (k) wbits = 17 + k;
        else {
            k = (int)br_bits(&b, 3);
            if (k == 1) goto fail; /* (a large window: not in RFC 7932) */
            wbits = k ? 8 + k : 17;
        }
    }
    size_t max_back = ((size_t)1 << wbits) - 16;
    int dist_rb[4] = {16, 15, 11, 4}, rb_pos = 3; /* the last four distances; rb[rb_pos] is the last */
    for (int last = 0; !last && !b.error;) {
        last = (int)br_bits(&b, 1);
        if (last && br_bits(&b, 1)) break; /* ISLASTEMPTY */
        int nibbles = (int)br_bits(&b, 2);
        if (nibbles == 3) { /* metadata: skipped */
            if (br_bits(&b, 1)) goto fail;
            int skip_bytes = (int)br_bits(&b, 2);
            size_t skip = 0;
            for (int i = 0; i < skip_bytes; i++) skip |= (size_t)br_bits(&b, 8) << (8 * i);
            if (skip_bytes) skip++;
            br_align(&b);
            b.pos += skip;
            if (b.pos > b.len) goto fail;
            continue;
        }
        nibbles += 4;
        size_t mlen = 0;
        for (int i = 0; i < nibbles; i++) mlen |= (size_t)br_bits(&b, 4) << (4 * i);
        mlen++;
        if (n + mlen > max_out) goto fail;
        if (n + mlen > cap) {
            while (cap < n + mlen) cap *= 2;
            if (cap > max_out) cap = max_out;
            unsigned char *bigger = realloc(out, cap);
            if (!bigger) goto fail;
            out = bigger;
        }
        if (!last && br_bits(&b, 1)) { /* stored as it is */
            br_align(&b);
            if (b.pos + mlen > b.len) goto fail;
            memcpy(out + n, in + b.pos, mlen);
            b.pos += mlen;
            n += mlen;
            continue;
        }
        /* the meta-block's codes */
        Blocks lit, cmd, dst;
        memset(&lit, 0, sizeof(lit));
        memset(&cmd, 0, sizeof(cmd));
        memset(&dst, 0, sizeof(dst));
        unsigned char *modes = NULL, *cmap_l = NULL, *cmap_d = NULL;
        Huff *lit_codes = NULL, *cmd_codes = NULL, *dst_codes = NULL;
        int ntrees_l = 0, ntrees_d = 0, ok = 0;
        if (!read_blocks(&b, &lit) || !read_blocks(&b, &cmd) || !read_blocks(&b, &dst)) goto block_done;
        int npostfix = (int)br_bits(&b, 2), ndirect = (int)br_bits(&b, 4) << npostfix;
        modes = malloc((size_t)lit.ntypes);
        if (!modes) goto block_done;
        for (int i = 0; i < lit.ntypes; i++) modes[i] = (unsigned char)br_bits(&b, 2);
        ntrees_l = read_varlen8(&b) + 1;
        cmap_l = malloc((size_t)lit.ntypes * 64);
        if (!cmap_l || !read_context_map(&b, lit.ntypes * 64, ntrees_l, cmap_l)) goto block_done;
        ntrees_d = read_varlen8(&b) + 1;
        cmap_d = malloc((size_t)dst.ntypes * 4);
        if (!cmap_d || !read_context_map(&b, dst.ntypes * 4, ntrees_d, cmap_d)) goto block_done;
        lit_codes = calloc_huffs(ntrees_l);
        cmd_codes = calloc_huffs(cmd.ntypes);
        dst_codes = calloc_huffs(ntrees_d);
        if (!lit_codes || !cmd_codes || !dst_codes) goto block_done;
        int dist_alpha = 16 + ndirect + (48 << npostfix);
        for (int i = 0; i < ntrees_l; i++)
            if (!read_prefix_code(&b, 256, &lit_codes[i])) goto block_done;
        for (int i = 0; i < cmd.ntypes; i++)
            if (!read_prefix_code(&b, 704, &cmd_codes[i])) goto block_done;
        for (int i = 0; i < ntrees_d; i++)
            if (!read_prefix_code(&b, dist_alpha, &dst_codes[i])) goto block_done;
        /* the commands */
        size_t end = n + mlen;
        while (n < end && !b.error) {
            next_block(&b, &cmd);
            int c = huff_decode(&b, &cmd_codes[cmd.type]);
            static const unsigned char INS_CELL[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
            static const unsigned char CPY_CELL[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};
            int cell = c >> 6;
            int ins_code = INS_CELL[cell] + ((c >> 3) & 7), cpy_code = CPY_CELL[cell] + (c & 7);
            size_t insert = INSERT_BASE[ins_code] + br_bits(&b, INSERT_BITS[ins_code]);
            size_t copy = COPY_BASE[cpy_code] + br_bits(&b, COPY_BITS[cpy_code]);
            for (size_t i = 0; i < insert; i++) {
                if (n >= end) goto fail_block;
                next_block(&b, &lit);
                int p1 = n > 0 ? out[n - 1] : 0, p2 = n > 1 ? out[n - 2] : 0;
                int tree = cmap_l[lit.type * 64 + literal_context(modes[lit.type], p1, p2)];
                out[n++] = (unsigned char)huff_decode(&b, &lit_codes[tree]);
            }
            if (n >= end) break;
            /* the distance */
            long long dist;
            int dcode = 0;
            if (cell >= 2) {
                next_block(&b, &dst);
                int dctx = copy > 4 ? 3 : (int)copy - 2;
                dcode = huff_decode(&b, &dst_codes[cmap_d[dst.type * 4 + dctx]]);
                if (dcode < 16) {
                    int last1 = dist_rb[rb_pos], last2 = dist_rb[(rb_pos + 3) & 3];
                    static const signed char DELTA[16] = {0, 0, 0, 0, -1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3};
                    if (dcode < 4) dist = dist_rb[(rb_pos - dcode + 4) & 3];
                    else dist = (dcode < 10 ? last1 : last2) + DELTA[dcode];
                    if (dist <= 0) goto fail_block;
                } else if (dcode < 16 + ndirect) {
                    dist = dcode - 15;
                } else {
                    int d = dcode - ndirect - 16;
                    int nbits = 1 + (d >> (npostfix + 1));
                    long long extra = br_bits(&b, nbits > 24 ? 24 : nbits);
                    if (nbits > 24) extra |= (long long)br_bits(&b, nbits - 24) << 24;
                    int hcode = d >> npostfix, lcode = d & ((1 << npostfix) - 1);
                    long long offset = ((2LL + (hcode & 1)) << nbits) - 4;
                    dist = ((offset + extra) << npostfix) + lcode + ndirect + 1;
                }
            } else {
                dist = dist_rb[rb_pos]; /* (the implied distance: the last one) */
            }
            size_t reach = n < max_back ? n : max_back;
            if ((size_t)dist > reach) { /* a word from the dictionary */
                if (copy < 4 || copy > 24 || !load_dictionary()) goto fail_block;
                long long word_id = dist - (long long)reach - 1;
                int bits = DICT_BITS[copy];
                int index = (int)(word_id & ((1 << bits) - 1)), t = (int)(word_id >> bits);
                if (t >= 121) goto fail_block;
                unsigned char word[64];
                int wl = dictionary_word((int)copy, index, t, word);
                if (n + (size_t)wl > end) goto fail_block;
                memcpy(out + n, word, (size_t)wl);
                n += (size_t)wl;
            } else {
                if (dcode != 0) { /* (code 0, the last distance again, is not remembered again) */
                    rb_pos = (rb_pos + 1) & 3;
                    dist_rb[rb_pos] = (int)dist;
                }
                if (n + copy > end) goto fail_block;
                for (size_t i = 0; i < copy; i++, n++) out[n] = out[n - (size_t)dist];
            }
        }
        if (b.error || n != end) goto fail_block;
        ok = 1;
    fail_block:
    block_done:
        blocks_free(&lit);
        blocks_free(&cmd);
        blocks_free(&dst);
        if (lit_codes)
            for (int i = 0; i < ntrees_l; i++) huff_free(&lit_codes[i]);
        if (cmd_codes)
            for (int i = 0; i < cmd.ntypes; i++) huff_free(&cmd_codes[i]);
        if (dst_codes)
            for (int i = 0; i < ntrees_d; i++) huff_free(&dst_codes[i]);
        free(lit_codes);
        free(cmd_codes);
        free(dst_codes);
        free(modes);
        free(cmap_l);
        free(cmap_d);
        if (!ok) goto fail;
    }
    if (b.error) goto fail;
    *out_p = out;
    *out_len = n;
    return 0;
fail:
    free(out);
    return -1;
}

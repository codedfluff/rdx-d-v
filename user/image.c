/* image.c - PNG (PNG specification, 2nd edition), GIF (GIF89a) and JPEG
   (ITU T.81: baseline and progressive Huffman-coded) decoders. */
#include "image.h"
#include "inflate.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define MAX_PIXELS (16 * 1024 * 1024)

static void say(char *error, const char *text) {
    size_t n = strlen(text);
    if (n > 95) n = 95;
    memcpy(error, text, n);
    error[n] = 0;
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static int allocate(image *out, int w, int h, char *error) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (long long)w * h > MAX_PIXELS) {
        say(error, "the picture is too big (or has no size)");
        return -1;
    }
    out->width = w;
    out->height = h;
    out->pixels = malloc((size_t)w * (size_t)h * 4);
    if (!out->pixels) {
        say(error, "not enough memory for the picture");
        return -1;
    }
    return 0;
}

void image_free(image *img) {
    free(img->pixels);
    img->pixels = NULL;
}

int image_sniff(const u8 *d, size_t len) {
    if (len >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return 1;
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return 1;
    if (len >= 6 && memcmp(d, "GIF8", 4) == 0) return 1;
    return 0;
}

/* ================================================================ PNG */

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/* Undo the per-row filters of `rows` rows of `stride` bytes (each row in
   `src` starts with its filter type byte). Returns 0, or -1 if a filter
   type is invalid. */
static int unfilter(const u8 *src, u8 *dst, size_t stride, int rows, int bpp) {
    for (int y = 0; y < rows; y++) {
        int type = src[0];
        const u8 *in = src + 1;
        u8 *out = dst + (size_t)y * stride;
        const u8 *prev = y ? out - stride : NULL;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? out[i - (size_t)bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= (size_t)bpp ? prev[i - (size_t)bpp] : 0;
            int v = in[i];
            switch (type) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            default: return -1;
            }
            out[i] = (u8)v;
        }
        src += stride + 1;
    }
    return 0;
}

typedef struct {
    int color, depth;
    u8 palette[256][4];
    int key[3], has_key; /* tRNS for grey / RGB: this colour is transparent */
} PngInfo;

static int sample(const u8 *row, int index, int depth) {
    if (depth == 8) return row[index];
    if (depth == 16) return row[2 * index] << 8 | row[2 * index + 1];
    int per_byte = 8 / depth;
    int byte = row[index / per_byte];
    int shift = 8 - depth * (index % per_byte + 1);
    return (byte >> shift) & ((1 << depth) - 1);
}

static unsigned png_pixel(const PngInfo *p, const u8 *row, int x) {
    int max = (1 << p->depth) - 1;
    int to8 = p->depth == 16 ? 8 : 0;
    switch (p->color) {
    case 0: { /* grey */
        int g = sample(row, x, p->depth);
        int a = p->has_key && g == p->key[0] ? 0 : 255;
        g = p->depth == 16 ? g >> 8 : g * 255 / max;
        return (unsigned)a << 24 | (unsigned)g * 0x010101u;
    }
    case 2: { /* RGB */
        int r = sample(row, 3 * x, p->depth), g = sample(row, 3 * x + 1, p->depth), b = sample(row, 3 * x + 2, p->depth);
        int a = p->has_key && r == p->key[0] && g == p->key[1] && b == p->key[2] ? 0 : 255;
        return (unsigned)a << 24 | (unsigned)(r >> to8) << 16 | (unsigned)(g >> to8) << 8 | (unsigned)(b >> to8);
    }
    case 3: { /* palette */
        const u8 *c = p->palette[sample(row, x, p->depth) & 255];
        return (unsigned)c[3] << 24 | (unsigned)c[0] << 16 | (unsigned)c[1] << 8 | c[2];
    }
    case 4: { /* grey + alpha */
        int g = sample(row, 2 * x, p->depth) >> to8, a = sample(row, 2 * x + 1, p->depth) >> to8;
        return (unsigned)a << 24 | (unsigned)g * 0x010101u;
    }
    default: { /* 6: RGBA */
        int r = sample(row, 4 * x, p->depth) >> to8, g = sample(row, 4 * x + 1, p->depth) >> to8;
        int b = sample(row, 4 * x + 2, p->depth) >> to8, a = sample(row, 4 * x + 3, p->depth) >> to8;
        return (unsigned)a << 24 | (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b;
    }
    }
}

static int png_decode(const u8 *d, size_t len, image *out, char *error) {
    PngInfo info;
    memset(&info, 0, sizeof(info));
    for (int i = 0; i < 256; i++) info.palette[i][3] = 255;
    int w = 0, h = 0, interlace = 0, channels = 0;
    u8 *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    size_t p = 8;
    while (p + 12 <= len) {
        u32 n = be32(d + p);
        const u8 *type = d + p + 4, *data = d + p + 8;
        if (n > len - p - 12) break;
        if (!memcmp(type, "IHDR", 4) && n >= 13) {
            w = (int)be32(data);
            h = (int)be32(data + 4);
            info.depth = data[8];
            info.color = data[9];
            interlace = data[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            for (u32 i = 0; i < n / 3 && i < 256; i++) {
                info.palette[i][0] = data[3 * i];
                info.palette[i][1] = data[3 * i + 1];
                info.palette[i][2] = data[3 * i + 2];
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            if (info.color == 3) {
                for (u32 i = 0; i < n && i < 256; i++) info.palette[i][3] = data[i];
            } else if (info.color == 0 && n >= 2) {
                info.key[0] = data[0] << 8 | data[1];
                info.has_key = 1;
            } else if (info.color == 2 && n >= 6) {
                for (int k = 0; k < 3; k++) info.key[k] = data[2 * k] << 8 | data[2 * k + 1];
                info.has_key = 1;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat_len + n > idat_cap) {
                idat_cap = (idat_len + n) * 2 + 65536;
                u8 *bigger = realloc(idat, idat_cap);
                if (!bigger) {
                    free(idat);
                    say(error, "not enough memory for the picture");
                    return -1;
                }
                idat = bigger;
            }
            memcpy(idat + idat_len, data, n);
            idat_len += n;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        p += 12 + n;
    }
    switch (info.color) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    }
    int depth_ok = info.depth == 8 || info.depth == 16 ||
                   ((info.color == 0 || info.color == 3) && (info.depth == 1 || info.depth == 2 || info.depth == 4));
    if (!channels || !depth_ok || !idat || (info.color == 3 && info.depth == 16)) {
        free(idat);
        say(error, "the PNG file is damaged or of an unknown kind");
        return -1;
    }
    u8 *raw;
    size_t raw_len;
    int bits = channels * info.depth, bpp = bits >= 8 ? bits / 8 : 1;
    /* The most the image data can decompress to, generously. */
    size_t limit = ((size_t)w * (size_t)bits / 8 + 2) * (size_t)h * 2 + 1024;
    if (allocate(out, w, h, error) != 0) {
        free(idat);
        return -1;
    }
    if (zlib_decompress(idat, idat_len, &raw, &raw_len, limit) != 0) {
        image_free(out);
        free(idat);
        say(error, "the PNG file's compressed data is damaged");
        return -1;
    }
    free(idat);
    /* Adam7 passes (start x, start y, step x, step y), or the whole image. */
    static const int passes[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                     {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    int pass_count = interlace ? 7 : 1;
    size_t at = 0;
    for (int pass = 0; pass < pass_count; pass++) {
        int sx = interlace ? passes[pass][0] : 0, sy = interlace ? passes[pass][1] : 0;
        int dx = interlace ? passes[pass][2] : 1, dy = interlace ? passes[pass][3] : 1;
        int pw = (w - sx + dx - 1) / dx, ph = (h - sy + dy - 1) / dy;
        if (pw <= 0 || ph <= 0) continue;
        size_t stride = ((size_t)pw * (size_t)bits + 7) / 8;
        if (at + (stride + 1) * (size_t)ph > raw_len) {
            free(raw);
            image_free(out);
            say(error, "the PNG file is cut short");
            return -1;
        }
        u8 *rows = malloc(stride * (size_t)ph);
        if (!rows || unfilter(raw + at, rows, stride, ph, bpp) != 0) {
            free(rows);
            free(raw);
            image_free(out);
            say(error, "the PNG file's data is damaged");
            return -1;
        }
        for (int y = 0; y < ph; y++) {
            const u8 *row = rows + (size_t)y * stride;
            unsigned *dst = out->pixels + (size_t)(sy + y * dy) * (size_t)w;
            for (int x = 0; x < pw; x++) dst[sx + x * dx] = png_pixel(&info, row, x);
        }
        free(rows);
        at += (stride + 1) * (size_t)ph;
    }
    free(raw);
    return 0;
}

/* ================================================================ GIF */

static int gif_decode(const u8 *d, size_t len, image *out, char *error) {
    if (len < 13) {
        say(error, "the GIF file is cut short");
        return -1;
    }
    int w = d[6] | d[7] << 8, h = d[8] | d[9] << 8, flags = d[10];
    u8 global[256][3];
    int global_size = 0;
    size_t p = 13;
    if (flags & 0x80) {
        global_size = 2 << (flags & 7);
        if (p + 3 * (size_t)global_size > len) {
            say(error, "the GIF file is cut short");
            return -1;
        }
        memcpy(global, d + p, 3 * (size_t)global_size);
        p += 3 * (size_t)global_size;
    }
    if (allocate(out, w, h, error) != 0) return -1;
    memset(out->pixels, 0, (size_t)w * (size_t)h * 4); /* transparent until drawn */
    int transparent = -1;
    while (p < len) {
        int block = d[p++];
        if (block == 0x3B) break;
        if (block == 0x21) { /* extension */
            if (p >= len) break;
            int label = d[p++];
            if (label == 0xF9 && p + 5 < len && d[p] >= 4) transparent = (d[p + 1] & 1) ? d[p + 4] : -1;
            while (p < len && d[p]) p += 1 + d[p]; /* sub-blocks */
            p++;
            continue;
        }
        if (block != 0x2C || p + 9 > len) break;
        int fx = d[p] | d[p + 1] << 8, fy = d[p + 2] | d[p + 3] << 8;
        int fw = d[p + 4] | d[p + 5] << 8, fh = d[p + 6] | d[p + 7] << 8, f = d[p + 8];
        p += 9;
        u8 local[256][3];
        u8 (*table)[3] = global;
        int table_size = global_size;
        if (f & 0x80) {
            table_size = 2 << (f & 7);
            if (p + 3 * (size_t)table_size > len) break;
            memcpy(local, d + p, 3 * (size_t)table_size);
            table = local;
            p += 3 * (size_t)table_size;
        }
        int interlaced = f & 0x40;
        if (p >= len) break;
        int min_size = d[p++];
        if (min_size < 2 || min_size > 11) break;
        /* Gather the data sub-blocks. */
        size_t data_len = 0;
        for (size_t q = p; q < len && d[q]; q += 1 + d[q]) data_len += d[q];
        u8 *data = malloc(data_len + 1);
        if (!data) break;
        size_t k = 0;
        while (p < len && d[p]) {
            size_t n = d[p];
            if (p + 1 + n > len) n = len - p - 1;
            memcpy(data + k, d + p + 1, n);
            k += n;
            p += 1 + d[p];
        }
        p++;
        /* LZW: codes of growing width, least significant bit first. */
        static u16 prefix[4096];
        static u8 suffix[4096], stack[4097];
        int clear = 1 << min_size, end = clear + 1, next = clear + 2, size = min_size + 1;
        for (int i = 0; i < clear; i++) suffix[i] = (u8)i;
        int old = -1, first = 0;
        u32 bitbuf = 0;
        int bitcnt = 0;
        size_t q = 0;
        long pixel = 0, total = (long)fw * fh;
        int row = 0, col = 0, pass_index = 0;
        static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
        while (pixel < total) {
            while (bitcnt < size && q < k) {
                bitbuf |= (u32)data[q++] << bitcnt;
                bitcnt += 8;
            }
            if (bitcnt < size) break;
            int code = (int)(bitbuf & ((1u << size) - 1));
            bitbuf >>= size;
            bitcnt -= size;
            if (code == clear) {
                size = min_size + 1;
                next = clear + 2;
                old = -1;
                continue;
            }
            if (code == end) break;
            int sp = 0;
            if (old == -1) {
                if (code >= clear) break;
                stack[sp++] = suffix[code];
                first = code;
            } else {
                int in = code;
                if (code >= next) {
                    if (code > next) break;
                    stack[sp++] = (u8)first;
                    code = old;
                }
                while (code >= clear && sp < 4096) {
                    stack[sp++] = suffix[code];
                    code = prefix[code];
                }
                stack[sp++] = suffix[code];
                first = suffix[code];
                if (next < 4096) {
                    prefix[next] = (u16)old;
                    suffix[next] = (u8)first;
                    next++;
                    if (next == (1 << size) && size < 12) size++;
                }
                code = in;
            }
            old = code;
            while (sp > 0 && pixel < total) {
                int index = stack[--sp];
                int x = fx + col, y = fy + row;
                if (x < w && y < h && index != transparent && index < table_size)
                    out->pixels[(size_t)y * (size_t)w + x] =
                        0xFF000000u | (unsigned)table[index][0] << 16 | (unsigned)table[index][1] << 8 | table[index][2];
                pixel++;
                if (++col == fw) {
                    col = 0;
                    if (interlaced) {
                        row += step[pass_index];
                        while (row >= fh && pass_index < 3) row = start[++pass_index];
                    } else {
                        row++;
                    }
                }
            }
        }
        free(data);
        return 0; /* the first frame is the picture */
    }
    return 0;
}

/* ================================================================ JPEG */

static const u8 ZIGZAG[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                              12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                              35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                              58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

typedef struct {
    int maxcode[18], valptr[17], mincode[17];
    u8 values[256];
    int present;
} Huff;

typedef struct {
    int id, h, v, tq, td, ta;
    int bw, bh;    /* blocks across and down, padded to whole MCUs */
    short *coef;   /* bw * bh blocks of 64, natural order */
    int pred;      /* DC prediction */
    u8 *plane;     /* samples after the IDCT */
} Comp;

typedef struct {
    const u8 *d;
    size_t len, pos;
    u32 buf;
    int cnt;
    int marker; /* hit a marker: the data has ended */
    u16 qt[4][64];
    Huff dc[4], ac[4];
    Comp comp[4];
    int ncomp, width, height, hmax, vmax, mcux, mcuy;
    int progressive, restart, adobe_transform;
    int eobrun;
} Jpeg;

static void build_huff(Huff *h, const u8 *counts, const u8 *values, int total) {
    int code = 0, k = 0;
    memcpy(h->values, values, (size_t)total);
    for (int len = 1; len <= 16; len++) {
        h->valptr[len] = k;
        h->mincode[len] = code;
        code += counts[len - 1];
        k += counts[len - 1];
        h->maxcode[len] = counts[len - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
    h->present = 1;
}

static void fill_bits(Jpeg *j) {
    while (j->cnt <= 24) {
        u32 byte = 0;
        if (!j->marker && j->pos < j->len) {
            byte = j->d[j->pos];
            if (byte == 0xFF) {
                u8 next = j->pos + 1 < j->len ? j->d[j->pos + 1] : 0xD9;
                if (next == 0) {
                    j->pos += 2;
                } else {
                    j->marker = next; /* stays at the 0xFF for the marker parser */
                    byte = 0;
                }
            } else {
                j->pos++;
            }
        }
        j->buf |= byte << (24 - j->cnt);
        j->cnt += 8;
    }
}

static int get_bits(Jpeg *j, int n) {
    if (n == 0) return 0;
    fill_bits(j);
    int v = (int)(j->buf >> (32 - n));
    j->buf <<= n;
    j->cnt -= n;
    return v;
}

static int get_bit(Jpeg *j) { return get_bits(j, 1); }

static int extend(int v, int n) { return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

static int huff_decode(Jpeg *j, const Huff *h) {
    int code = get_bit(j), len = 1;
    while (code > h->maxcode[len]) {
        code = code << 1 | get_bit(j);
        if (++len > 16) return 0; /* bad data: treat as 0 */
    }
    int index = h->valptr[len] + code - h->mincode[len];
    return index >= 0 && index < 256 ? h->values[index] : 0;
}

static void decode_block(Jpeg *j, Comp *c, short *block, int ss, int se, int ah, int al) {
    if (!j->progressive) {
        int t = huff_decode(j, &j->dc[c->td]);
        c->pred += t ? extend(get_bits(j, t), t) : 0;
        block[0] = (short)c->pred;
        for (int k = 1; k < 64;) {
            int rs = huff_decode(j, &j->ac[c->ta]), r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r != 15) break;
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) break;
            block[ZIGZAG[k++]] = (short)extend(get_bits(j, s), s);
        }
        return;
    }
    if (ss == 0) { /* DC */
        if (ah == 0) {
            int t = huff_decode(j, &j->dc[c->td]);
            c->pred += t ? extend(get_bits(j, t), t) : 0;
            block[0] = (short)(c->pred * (1 << al));
        } else if (get_bit(j)) {
            block[0] |= (short)(1 << al);
        }
        return;
    }
    if (ah == 0) { /* AC, first pass */
        if (j->eobrun > 0) {
            j->eobrun--;
            return;
        }
        for (int k = ss; k <= se; k++) {
            int rs = huff_decode(j, &j->ac[c->ta]), r = rs >> 4, s = rs & 15;
            if (s) {
                k += r;
                if (k > 63) break;
                block[ZIGZAG[k]] = (short)(extend(get_bits(j, s), s) * (1 << al));
            } else if (r == 15) {
                k += 15;
            } else {
                j->eobrun = (1 << r) - 1;
                if (r) j->eobrun += get_bits(j, r);
                break;
            }
        }
        return;
    }
    /* AC, refinement (as libjpeg's decode_mcu_AC_refine). */
    int p1 = 1 << al, m1 = -1 * (1 << al);
    int k = ss;
    if (j->eobrun == 0) {
        for (; k <= se; k++) {
            int rs = huff_decode(j, &j->ac[c->ta]), r = rs >> 4, s = rs & 15;
            if (s) {
                s = get_bit(j) ? p1 : m1;
            } else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += get_bits(j, r);
                break;
            }
            do {
                short *coef = &block[ZIGZAG[k]];
                if (*coef != 0) {
                    if (get_bit(j) && (*coef & p1) == 0) *coef = (short)(*coef >= 0 ? *coef + p1 : *coef + m1);
                } else {
                    if (--r < 0) break;
                }
                k++;
            } while (k <= se);
            if (s && k <= 63) block[ZIGZAG[k]] = (short)s;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; k++) {
            short *coef = &block[ZIGZAG[k]];
            if (*coef != 0 && get_bit(j) && (*coef & p1) == 0) *coef = (short)(*coef >= 0 ? *coef + p1 : *coef + m1);
        }
        j->eobrun--;
    }
}

/* At a restart interval: realign to the RSTn marker and reset predictions. */
static void restart(Jpeg *j, Comp **scan, int ns) {
    j->buf = 0;
    j->cnt = 0;
    j->marker = 0;
    while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)) j->pos++;
    if (j->pos + 1 < j->len) j->pos += 2;
    for (int i = 0; i < ns; i++) scan[i]->pred = 0;
    j->eobrun = 0;
}

static void decode_scan(Jpeg *j, Comp **scan, int ns, int ss, int se, int ah, int al) {
    j->buf = 0;
    j->cnt = 0;
    j->marker = 0;
    j->eobrun = 0;
    for (int i = 0; i < ns; i++) scan[i]->pred = 0;
    int units = 0;
    if (ns == 1) {
        /* Not interleaved: the component's own blocks, without MCU padding. */
        Comp *c = scan[0];
        int bw = ((j->width * c->h / j->hmax) + 7) / 8, bh = ((j->height * c->v / j->vmax) + 7) / 8;
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                if (j->restart && units && units % j->restart == 0) restart(j, scan, ns);
                decode_block(j, c, c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64, ss, se, ah, al);
                units++;
            }
        }
        return;
    }
    for (int my = 0; my < j->mcuy; my++) {
        for (int mx = 0; mx < j->mcux; mx++) {
            if (j->restart && units && units % j->restart == 0) restart(j, scan, ns);
            for (int i = 0; i < ns; i++) {
                Comp *c = scan[i];
                for (int v = 0; v < c->v; v++)
                    for (int h = 0; h < c->h; h++) {
                        size_t bx = (size_t)mx * (size_t)c->h + (size_t)h, by = (size_t)my * (size_t)c->v + (size_t)v;
                        decode_block(j, c, c->coef + (by * (size_t)c->bw + bx) * 64, ss, se, ah, al);
                    }
            }
            units++;
        }
    }
}

/* 8x8 inverse DCT (separable), with level shift and clamping. */
static void idct(const short *in, const u16 *q, u8 *out, int stride) {
    static float table[8][8]; /* table[x][u] = C(u)/2 * cos((2x+1)u pi/16) */
    static int ready;
    if (!ready) {
        static const float cosines[9] = {1.0f,        0.98078528f, 0.92387953f, 0.83146961f, 0.70710678f,
                                         0.55557023f, 0.38268343f, 0.19509032f, 0.0f};
        for (int x = 0; x < 8; x++) {
            for (int u = 0; u < 8; u++) {
                int n = ((2 * x + 1) * u) % 32; /* angle n*pi/16 */
                float c;
                if (n <= 8) c = cosines[n];
                else if (n <= 16) c = -cosines[16 - n];
                else if (n <= 24) c = -cosines[n - 16];
                else c = cosines[32 - n];
                table[x][u] = (u == 0 ? 0.70710678f : 1.0f) * c / 2.0f;
            }
        }
        ready = 1;
    }
    float tmp[64], coef[64];
    for (int i = 0; i < 64; i++) coef[i] = (float)in[i] * q[i];
    for (int v = 0; v < 8; v++) /* rows: along u */
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += table[x][u] * coef[v * 8 + u];
            tmp[v * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++) /* columns: along v */
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += table[y][v] * tmp[v * 8 + x];
            int value = (int)(s + 128.5f);
            if (s + 128.5f < 0) value = 0;
            out[y * stride + x] = (u8)(value < 0 ? 0 : value > 255 ? 255 : value);
        }
}

static int jpeg_decode(const u8 *d, size_t len, image *out, char *error) {
    Jpeg *j = malloc(sizeof(Jpeg));
    if (!j) {
        say(error, "not enough memory");
        return -1;
    }
    memset(j, 0, sizeof(*j));
    j->d = d;
    j->len = len;
    j->adobe_transform = -1;
    int ok = 0, frame = 0;
    size_t p = 2;
    while (p + 4 <= len) {
        if (d[p] != 0xFF) {
            p++;
            continue;
        }
        int marker = d[p + 1];
        if (marker == 0xFF || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01 || marker == 0) {
            p++;
            continue;
        }
        if (marker == 0xD9) break;
        size_t seg = (size_t)d[p + 2] << 8 | d[p + 3];
        const u8 *s = d + p + 4;
        if (seg < 2 || p + 2 + seg > len) break;
        size_t n = seg - 2;
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            if (n < 6 || s[0] != 8) {
                say(error, "this JPEG uses 12-bit samples, which are not supported");
                goto done;
            }
            j->progressive = marker == 0xC2;
            j->height = s[1] << 8 | s[2];
            j->width = s[3] << 8 | s[4];
            j->ncomp = s[5];
            if (j->ncomp != 1 && j->ncomp != 3) {
                say(error, "this JPEG is CMYK, which is not supported");
                goto done;
            }
            if (n < 6 + 3 * (size_t)j->ncomp) goto done;
            j->hmax = j->vmax = 1;
            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->comp[i];
                c->id = s[6 + 3 * i];
                c->h = s[7 + 3 * i] >> 4;
                c->v = s[7 + 3 * i] & 15;
                c->tq = s[8 + 3 * i] & 3;
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) goto done;
                if (c->h > j->hmax) j->hmax = c->h;
                if (c->v > j->vmax) j->vmax = c->v;
            }
            if (allocate(out, j->width, j->height, error) != 0) goto done;
            j->mcux = (j->width + 8 * j->hmax - 1) / (8 * j->hmax);
            j->mcuy = (j->height + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->comp[i];
                c->bw = j->mcux * c->h;
                c->bh = j->mcuy * c->v;
                c->coef = malloc((size_t)c->bw * (size_t)c->bh * 64 * sizeof(short));
                if (!c->coef) {
                    say(error, "not enough memory for the picture");
                    goto done;
                }
                memset(c->coef, 0, (size_t)c->bw * (size_t)c->bh * 64 * sizeof(short));
            }
            frame = 1;
        } else if ((marker >= 0xC3 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            say(error, "this JPEG uses a kind of compression that is not supported (lossless or arithmetic)");
            goto done;
        } else if (marker == 0xC4) { /* Huffman tables */
            size_t q = 0;
            while (q + 17 <= n) {
                int tc = s[q] >> 4, th = s[q] & 3;
                int total = 0;
                for (int i = 0; i < 16; i++) total += s[q + 1 + i];
                if (total > 256 || q + 17 + (size_t)total > n) break;
                build_huff(tc ? &j->ac[th] : &j->dc[th], s + q + 1, s + q + 17, total);
                q += 17 + (size_t)total;
            }
        } else if (marker == 0xDB) { /* quantisation tables */
            size_t q = 0;
            while (q < n) {
                int precision = s[q] >> 4, tq = s[q] & 3;
                q++;
                for (int k = 0; k < 64 && q < n; k++) {
                    int value = precision ? s[q] << 8 | s[q + 1] : s[q];
                    q += precision ? 2 : 1;
                    j->qt[tq][ZIGZAG[k]] = (u16)value;
                }
            }
        } else if (marker == 0xDD && n >= 2) {
            j->restart = s[0] << 8 | s[1];
        } else if (marker == 0xEE && n >= 12 && memcmp(s, "Adobe", 5) == 0) {
            j->adobe_transform = s[11];
        } else if (marker == 0xDA) { /* start of scan */
            if (!frame || n < 1) goto done;
            int ns = s[0];
            if (ns < 1 || ns > 4 || n < 4 + 2 * (size_t)ns) goto done;
            Comp *scan[4];
            for (int i = 0; i < ns; i++) {
                scan[i] = NULL;
                for (int k = 0; k < j->ncomp; k++) {
                    if (j->comp[k].id == s[1 + 2 * i]) scan[i] = &j->comp[k];
                }
                if (!scan[i]) goto done;
                scan[i]->td = s[2 + 2 * i] >> 4 & 3;
                scan[i]->ta = s[2 + 2 * i] & 3;
            }
            int ss = s[1 + 2 * ns], se = s[2 + 2 * ns], ah = s[3 + 2 * ns] >> 4, al = s[3 + 2 * ns] & 15;
            if (se > 63) se = 63;
            j->pos = p + 2 + seg;
            decode_scan(j, scan, ns, ss, se, ah, al);
            p = j->pos;
            ok = 1;
            continue;
        }
        p += 2 + seg;
    }
    if (!ok) {
        if (!error[0]) say(error, "the JPEG file is damaged or empty");
        goto done;
    }
    /* Inverse DCT for every block, then colour conversion. */
    for (int i = 0; i < j->ncomp; i++) {
        Comp *c = &j->comp[i];
        int pw = c->bw * 8;
        c->plane = malloc((size_t)pw * (size_t)c->bh * 8);
        if (!c->plane) {
            say(error, "not enough memory for the picture");
            ok = 0;
            goto done;
        }
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++)
                idct(c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64, j->qt[c->tq],
                     c->plane + (size_t)by * 8 * (size_t)pw + (size_t)bx * 8, pw);
    }
    for (int y = 0; y < j->height; y++) {
        unsigned *row = out->pixels + (size_t)y * (size_t)j->width;
        for (int x = 0; x < j->width; x++) {
            int v[3];
            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->comp[i];
                int sx = x * c->h / j->hmax, sy = y * c->v / j->vmax; /* nearest sample */
                v[i] = c->plane[(size_t)sy * (size_t)(c->bw * 8) + (size_t)sx];
            }
            int r, g, b;
            if (j->ncomp == 1) {
                r = g = b = v[0];
            } else if (j->adobe_transform == 0) {
                r = v[0];
                g = v[1];
                b = v[2];
            } else {
                int yy = v[0], cb = v[1] - 128, cr = v[2] - 128;
                r = yy + ((91881 * cr) >> 16);
                g = yy - ((22554 * cb + 46802 * cr) >> 16);
                b = yy + ((116130 * cb) >> 16);
            }
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            row[x] = 0xFF000000u | (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b;
        }
    }
done:
    for (int i = 0; i < 4; i++) {
        free(j->comp[i].coef);
        free(j->comp[i].plane);
    }
    free(j);
    if (!ok) {
        if (out->pixels) image_free(out);
        return -1;
    }
    return 0;
}

/* ================================================================ common */

int image_decode(const u8 *data, size_t len, image *out, char *error) {
    memset(out, 0, sizeof(*out));
    error[0] = 0;
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return png_decode(data, len, out, error);
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8) return jpeg_decode(data, len, out, error);
    if (len >= 6 && memcmp(data, "GIF8", 4) == 0) return gif_decode(data, len, out, error);
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) say(error, "WebP pictures are not supported yet");
    else if (len >= 5 && (memcmp(data, "<svg", 4) == 0 || memcmp(data, "<?xml", 5) == 0)) say(error, "SVG pictures are not supported yet");
    else say(error, "not a picture this system can read (PNG, JPEG and GIF are)");
    return -1;
}

void image_draw(canvas *c, const image *img, int x, int y, int w, int h) {
    if (!img->pixels || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;
    int x1 = x + w > c->width ? c->width - x : w, y1 = y + h > c->height ? c->height - y : h;
    for (int dy = y0; dy < y1; dy++) {
        int sy = (int)((long long)dy * img->height / h);
        const unsigned *src = img->pixels + (size_t)sy * (size_t)img->width;
        unsigned *dst = c->pixels + (size_t)(y + dy) * (size_t)c->stride + x;
        for (int dx = x0; dx < x1; dx++) {
            unsigned p = src[(long long)dx * img->width / w];
            unsigned a = p >> 24;
            if (a == 255) {
                dst[dx] = p & 0xFFFFFF;
            } else if (a) {
                unsigned q = dst[dx];
                unsigned r = (((p >> 16) & 255) * a + ((q >> 16) & 255) * (255 - a)) / 255;
                unsigned g = (((p >> 8) & 255) * a + ((q >> 8) & 255) * (255 - a)) / 255;
                unsigned b = ((p & 255) * a + (q & 255) * (255 - a)) / 255;
                dst[dx] = r << 16 | g << 8 | b;
            }
        }
    }
}

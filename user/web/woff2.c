/* web/woff2.c - WOFF2 fonts (what most sites serve) back into plain
   TrueType files. Part of the browser.

   A WOFF2 file is a table directory and one Brotli stream holding the
   tables. The glyf and loca tables are usually stored "transformed": the
   outlines split into separate streams of contour counts, point counts,
   flags, packed coordinates, composites, bounding boxes and instructions.
   hmtx may leave out side bearings that equal the glyph's xMin. */

typedef struct {
    const unsigned char *p;
    size_t len, pos;
    int bad;
} W2;

static unsigned w2_u8(W2 *r) {
    if (r->pos >= r->len) {
        r->bad = 1;
        return 0;
    }
    return r->p[r->pos++];
}
static unsigned w2_u16(W2 *r) {
    unsigned a = w2_u8(r);
    return a << 8 | w2_u8(r);
}
static int w2_s16(W2 *r) { return (short)w2_u16(r); }
static unsigned w2_u32(W2 *r) {
    unsigned a = w2_u16(r);
    return a << 16 | w2_u16(r);
}
static unsigned w2_base128(W2 *r) {
    unsigned v = 0;
    for (int i = 0; i < 5; i++) {
        unsigned b = w2_u8(r);
        if (i == 0 && b == 0x80) r->bad = 1;
        if (v & 0xFE000000u) r->bad = 1;
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
    r->bad = 1;
    return 0;
}
static unsigned w2_255u16(W2 *r) {
    unsigned b = w2_u8(r);
    if (b == 253) return w2_u16(r);
    if (b == 255) return w2_u8(r) + 253;
    if (b == 254) return w2_u8(r) + 506;
    return b;
}

/* a growing output */
typedef struct {
    unsigned char *p;
    size_t len, cap;
    int bad;
} W2Out;

static void w2_put(W2Out *o, const void *d, size_t n) {
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        while (cap < o->len + n) cap *= 2;
        unsigned char *p = realloc(o->p, cap);
        if (!p) {
            o->bad = 1;
            return;
        }
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->len, d, n);
    o->len += n;
}
static void w2_put16(W2Out *o, unsigned v) {
    unsigned char b[2] = {(unsigned char)(v >> 8), (unsigned char)v};
    w2_put(o, b, 2);
}

static const char W2_TAGS[63][5] = {
    "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf", "loca", "prep", "CFF ", "VORG", "EBDT",
    "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE", "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH",
    "CBDT", "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt", "avar", "bdat", "bloc", "bsln", "cvar", "fdsc", "feat", "fmtx", "fvar",
    "gvar", "hsty", "just", "lcar", "mort", "morx", "opbd", "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill"};

static unsigned w2_tag(const char *t) { return (unsigned)(unsigned char)t[0] << 24 | (unsigned)(unsigned char)t[1] << 16 | (unsigned)(unsigned char)t[2] << 8 | (unsigned char)t[3]; }

/* One packed point (the "triplet" encoding): returns bytes used, or 0. */
static int w2_triplet(unsigned flag, const unsigned char *in, size_t left, int *dx, int *dy) {
    flag &= 0x7F;
    int n = flag < 84 ? 1 : flag < 120 ? 2 : flag < 124 ? 3 : 4;
    if (flag < 10) n = 1;
    if ((size_t)n > left) return 0;
#define W2_SIGN(f, v) (((f) & 1) ? (int)(v) : -(int)(v))
    if (flag < 10) {
        *dx = 0;
        *dy = W2_SIGN(flag, ((flag & 14) << 7) + in[0]);
    } else if (flag < 20) {
        *dx = W2_SIGN(flag, (((flag - 10) & 14) << 7) + in[0]);
        *dy = 0;
    } else if (flag < 84) {
        unsigned b0 = flag - 20, b1 = in[0];
        *dx = W2_SIGN(flag, 1 + (b0 & 0x30) + (b1 >> 4));
        *dy = W2_SIGN(flag >> 1, 1 + ((b0 & 0x0C) << 2) + (b1 & 0x0F));
    } else if (flag < 120) {
        unsigned b0 = flag - 84;
        *dx = W2_SIGN(flag, 1 + ((b0 / 12) << 8) + in[0]);
        *dy = W2_SIGN(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + in[1]);
    } else if (flag < 124) {
        *dx = W2_SIGN(flag, ((unsigned)in[0] << 4) + (in[1] >> 4));
        *dy = W2_SIGN(flag >> 1, (((unsigned)in[1] & 0x0F) << 8) + in[2]);
    } else {
        *dx = W2_SIGN(flag, ((unsigned)in[0] << 8) + in[1]);
        *dy = W2_SIGN(flag >> 1, ((unsigned)in[2] << 8) + in[3]);
    }
#undef W2_SIGN
    return n;
}

/* The transformed glyf table back into glyf and loca (long offsets).
   xmins: each glyph's xMin (for hmtx). */
static int w2_glyf(const unsigned char *t, size_t tl, W2Out *glyf, W2Out *loca, int *num_glyphs, short **xmins) {
    W2 h = {t, tl, 0, 0};
    w2_u16(&h); /* reserved */
    unsigned options = w2_u16(&h);
    unsigned glyphs = w2_u16(&h);
    w2_u16(&h); /* index format: loca is rebuilt with long offsets anyway */
    unsigned sizes[7];
    for (int i = 0; i < 7; i++) sizes[i] = w2_u32(&h);
    if (h.bad) return 0;
    size_t at = h.pos;
    W2 s[7]; /* contour counts, point counts, flags, glyphs, composites, boxes, instructions */
    for (int i = 0; i < 7; i++) {
        if (at + sizes[i] > tl) return 0;
        s[i] = (W2){t + at, sizes[i], 0, 0};
        at += sizes[i];
    }
    /* the bounding-box stream begins with a bit per glyph: has a stored box */
    size_t bitmap_len = ((glyphs + 31) >> 5) << 2;
    if (bitmap_len > s[5].len) return 0;
    const unsigned char *bbox_bits = s[5].p;
    s[5].pos = bitmap_len;
    (void)options; /* (bit 0: an overlap bitmap follows; not needed to draw) */
    *num_glyphs = (int)glyphs;
    *xmins = malloc(sizeof(short) * (glyphs ? glyphs : 1));
    if (!*xmins) return 0;
    int *xs = NULL, *ys = NULL;
    unsigned char *on = NULL;
    size_t pts_cap = 0;
    for (unsigned g = 0; g < glyphs; g++) {
        unsigned char box[4] = {(unsigned char)(glyf->len >> 24), (unsigned char)(glyf->len >> 16), (unsigned char)(glyf->len >> 8), (unsigned char)glyf->len};
        w2_put(loca, box, 4);
        int contours = w2_s16(&s[0]);
        int has_box = bbox_bits[g >> 3] & (0x80 >> (g & 7));
        (*xmins)[g] = 0;
        if (contours == 0) continue;
        if (contours < 0) { /* composite: copied, with its box and instructions */
            if (!has_box) goto bad;
            int bx0 = w2_s16(&s[5]), by0 = w2_s16(&s[5]), bx1 = w2_s16(&s[5]), by1 = w2_s16(&s[5]);
            (*xmins)[g] = (short)bx0;
            w2_put16(glyf, 0xFFFF);
            w2_put16(glyf, (unsigned)bx0);
            w2_put16(glyf, (unsigned)by0);
            w2_put16(glyf, (unsigned)bx1);
            w2_put16(glyf, (unsigned)by1);
            size_t start = s[4].pos;
            unsigned flags, any_instructions = 0;
            do {
                flags = w2_u16(&s[4]);
                any_instructions |= flags & 0x100;
                size_t skip = 2 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
                s[4].pos += skip;
                if (s[4].pos > s[4].len) goto bad;
            } while (flags & 0x20);
            w2_put(glyf, s[4].p + start, s[4].pos - start);
            if (any_instructions) {
                unsigned il = w2_255u16(&s[3]);
                if (s[6].pos + il > s[6].len) goto bad;
                w2_put16(glyf, il);
                w2_put(glyf, s[6].p + s[6].pos, il);
                s[6].pos += il;
            }
        } else { /* simple */
            unsigned total = 0, ends[1024];
            if (contours > 1024) goto bad;
            for (int c = 0; c < contours; c++) {
                total += w2_255u16(&s[1]);
                ends[c] = total - 1;
            }
            if (total > 65535) goto bad;
            if (total > pts_cap) {
                free(xs);
                free(ys);
                free(on);
                pts_cap = total + 64;
                xs = malloc(sizeof(int) * pts_cap);
                ys = malloc(sizeof(int) * pts_cap);
                on = malloc(pts_cap);
                if (!xs || !ys || !on) goto bad;
            }
            int x = 0, y = 0, x0 = 0x7FFFFFFF, y0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, y1 = -0x7FFFFFFF;
            for (unsigned i = 0; i < total; i++) {
                unsigned f = w2_u8(&s[2]);
                int dx, dy, used = w2_triplet(f, s[3].p + s[3].pos, s[3].len - s[3].pos, &dx, &dy);
                if (!used) goto bad;
                s[3].pos += (size_t)used;
                x += dx;
                y += dy;
                xs[i] = x;
                ys[i] = y;
                on[i] = !(f & 0x80);
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
            unsigned il = w2_255u16(&s[3]);
            if (has_box) {
                x0 = w2_s16(&s[5]);
                y0 = w2_s16(&s[5]);
                x1 = w2_s16(&s[5]);
                y1 = w2_s16(&s[5]);
            }
            if (!total) x0 = y0 = x1 = y1 = 0;
            (*xmins)[g] = (short)x0;
            if (s[6].pos + il > s[6].len) goto bad;
            w2_put16(glyf, (unsigned)contours);
            w2_put16(glyf, (unsigned)x0);
            w2_put16(glyf, (unsigned)y0);
            w2_put16(glyf, (unsigned)x1);
            w2_put16(glyf, (unsigned)y1);
            for (int c = 0; c < contours; c++) w2_put16(glyf, ends[c]);
            w2_put16(glyf, il);
            w2_put(glyf, s[6].p + s[6].pos, il);
            s[6].pos += il;
            /* points as plain 16-bit deltas: flags first, then x, then y */
            for (unsigned i = 0; i < total; i++) {
                unsigned char f = on[i] ? 1 : 0;
                w2_put(glyf, &f, 1);
            }
            for (unsigned i = 0; i < total; i++) w2_put16(glyf, (unsigned)(xs[i] - (i ? xs[i - 1] : 0)));
            for (unsigned i = 0; i < total; i++) w2_put16(glyf, (unsigned)(ys[i] - (i ? ys[i - 1] : 0)));
        }
        while (glyf->len & 3) w2_put(glyf, "", 1);
        if (s[0].bad || s[1].bad || s[2].bad || s[3].bad || s[4].bad || s[5].bad || glyf->bad) goto bad;
    }
    unsigned char end[4] = {(unsigned char)(glyf->len >> 24), (unsigned char)(glyf->len >> 16), (unsigned char)(glyf->len >> 8), (unsigned char)glyf->len};
    w2_put(loca, end, 4);
    free(xs);
    free(ys);
    free(on);
    return !glyf->bad && !loca->bad;
bad:
    free(xs);
    free(ys);
    free(on);
    return 0;
}

/* A WOFF2 font as a plain TrueType file (malloc'd), or NULL. */
static unsigned char *woff2_to_sfnt(const unsigned char *d, size_t len, size_t *out_len) {
    W2 r = {d, len, 0, 0};
    if (w2_u32(&r) != 0x774F4632) return NULL; /* wOF2 */
    unsigned flavor = w2_u32(&r);
    if (flavor == 0x74746366) return NULL; /* a collection: not supported */
    w2_u32(&r);                            /* length */
    unsigned tables = w2_u16(&r);
    w2_u16(&r);
    w2_u32(&r); /* total sfnt size */
    unsigned compressed = w2_u32(&r);
    r.pos = 48;
    if (!tables || tables > 200) return NULL;
    struct {
        unsigned tag, orig, size, version;
        size_t at;
    } dir[200];
    for (unsigned i = 0; i < tables; i++) {
        unsigned flags = w2_u8(&r), tag;
        if ((flags & 63) == 63) tag = w2_u32(&r);
        else tag = w2_tag(W2_TAGS[flags & 63]);
        unsigned version = flags >> 6, orig = w2_base128(&r), size = orig;
        int is_gl = tag == w2_tag("glyf") || tag == w2_tag("loca");
        if ((is_gl && version == 0) || (tag == w2_tag("hmtx") && version == 1)) size = w2_base128(&r);
        dir[i].tag = tag;
        dir[i].orig = orig;
        dir[i].size = size;
        dir[i].version = version;
    }
    if (r.bad || r.pos + compressed > len) return NULL;
    size_t total = 0;
    for (unsigned i = 0; i < tables; i++) {
        dir[i].at = total;
        total += dir[i].size;
    }
    unsigned char *data;
    size_t data_len;
    if (brotli_decompress(d + r.pos, compressed, &data, &data_len, total + 16) != 0 || data_len < total) return NULL;
    /* glyf and loca first (hmtx may need the glyphs' xMin) */
    W2Out glyf = {0}, loca = {0};
    int num_glyphs = 0, have_glyf = 0;
    short *xmins = NULL;
    for (unsigned i = 0; i < tables; i++)
        if (dir[i].tag == w2_tag("glyf") && dir[i].version == 0) {
            if (!w2_glyf(data + dir[i].at, dir[i].size, &glyf, &loca, &num_glyphs, &xmins)) goto fail;
            have_glyf = 1;
        }
    /* hhea: how many full metrics */
    unsigned num_hmetrics = 0;
    for (unsigned i = 0; i < tables; i++)
        if (dir[i].tag == w2_tag("hhea") && dir[i].size >= 36)
            num_hmetrics = (unsigned)data[dir[i].at + 34] << 8 | data[dir[i].at + 35];
    W2Out sfnt = {0};
    unsigned char header[12] = {0, 1, 0, 0, (unsigned char)(tables >> 8), (unsigned char)tables, 0, 0, 0, 0, 0, 0};
    w2_put(&sfnt, header, 12);
    size_t dir_at = sfnt.len;
    for (unsigned i = 0; i < tables * 16; i++) w2_put(&sfnt, "", 1);
    for (unsigned i = 0; i < tables; i++) {
        unsigned tag = dir[i].tag;
        size_t start = sfnt.len;
        if (tag == w2_tag("glyf") && have_glyf) w2_put(&sfnt, glyf.p, glyf.len);
        else if (tag == w2_tag("loca") && have_glyf) w2_put(&sfnt, loca.p, loca.len);
        else if (tag == w2_tag("hmtx") && dir[i].version == 1) { /* side bearings left out where they equal xMin */
            W2 h = {data + dir[i].at, dir[i].size, 0, 0};
            unsigned flags = w2_u8(&h);
            if (!xmins || !num_hmetrics || num_hmetrics > (unsigned)num_glyphs) goto fail_sfnt;
            unsigned short *adv = malloc(sizeof(unsigned short) * num_hmetrics);
            if (!adv) goto fail_sfnt;
            for (unsigned g = 0; g < num_hmetrics; g++) adv[g] = (unsigned short)w2_u16(&h);
            for (unsigned g = 0; g < num_hmetrics; g++) {
                w2_put16(&sfnt, adv[g]);
                w2_put16(&sfnt, (unsigned)(flags & 1 ? xmins[g] : w2_s16(&h)));
            }
            for (unsigned g = num_hmetrics; g < (unsigned)num_glyphs; g++) w2_put16(&sfnt, (unsigned)(flags & 2 ? xmins[g] : w2_s16(&h)));
            free(adv);
            if (h.bad) goto fail_sfnt;
        } else {
            w2_put(&sfnt, data + dir[i].at, dir[i].size);
            if (tag == w2_tag("head") && dir[i].size >= 52) { /* our loca has long offsets */
                sfnt.p[start + 50] = 0;
                sfnt.p[start + 51] = have_glyf ? 1 : sfnt.p[start + 51];
                sfnt.p[start + 8] = sfnt.p[start + 9] = sfnt.p[start + 10] = sfnt.p[start + 11] = 0; /* (checksum adjustment) */
            }
        }
        size_t tl = sfnt.len - start;
        while (sfnt.len & 3) w2_put(&sfnt, "", 1);
        if (sfnt.bad) goto fail_sfnt;
        unsigned char *e = sfnt.p + dir_at + i * 16;
        unsigned v[4] = {tag, 0, (unsigned)start, (unsigned)tl};
        for (int k = 0; k < 4; k++) {
            e[k * 4] = (unsigned char)(v[k] >> 24);
            e[k * 4 + 1] = (unsigned char)(v[k] >> 16);
            e[k * 4 + 2] = (unsigned char)(v[k] >> 8);
            e[k * 4 + 3] = (unsigned char)v[k];
        }
    }
    if (flavor == 0x4F54544F) { /* 'OTTO': CFF outlines, kept so */
        sfnt.p[0] = 'O';
        sfnt.p[1] = 'T';
        sfnt.p[2] = 'T';
        sfnt.p[3] = 'O';
    }
    free(glyf.p);
    free(loca.p);
    free(xmins);
    free(data);
    *out_len = sfnt.len;
    return sfnt.p;
fail_sfnt:
    free(sfnt.p);
fail:
    free(glyf.p);
    free(loca.p);
    free(xmins);
    free(data);
    return NULL;
}

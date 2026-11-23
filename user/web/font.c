/* web/font.c - text in real fonts. Part of the browser.

   TrueType outlines are read from font files on the boot disk (Liberation
   Sans, Serif and Mono: free fonts with the same character widths as
   Arial, Times New Roman and Courier New, which most pages ask for),
   scaled to any size and drawn anti-aliased. Drawn glyphs are kept in a
   cache. Without the files, text falls back to the 8x16 bitmap font. */

enum { FACE_SANS, FACE_SERIF, FACE_MONO, FACE_COUNT };

typedef struct {
    unsigned char *d;
    size_t len;
    signed char state;              /* 0 not tried yet, 1 ready, -1 missing or unreadable */
    int units, long_loca, num_glyphs, num_hmetrics;
    int ascent, descent, gap;       /* font units (descent is positive) */
    unsigned loca, glyf, hmtx, cmap; /* table offsets; cmap: the chosen subtable */
    int cmap_format;
    unsigned short ascii_glyph[128], ascii_adv[128];
} Font;

static Font font_table[FACE_COUNT * 4];
static const char *const FONT_FILES[FACE_COUNT * 4] = {
    "/liberationsans-regular.ttf", "/liberationsans-bold.ttf", "/liberationsans-italic.ttf", "/liberationsans-bolditalic.ttf",
    "/liberationserif-regular.ttf", "/liberationserif-bold.ttf", "/liberationserif-italic.ttf", "/liberationserif-bolditalic.ttf",
    "/liberationmono-regular.ttf", "/liberationmono-bold.ttf", "/liberationmono-italic.ttf", "/liberationmono-bolditalic.ttf"};

static unsigned ft_u8(const Font *f, unsigned at) { return at < f->len ? f->d[at] : 0; }
static unsigned ft_u16(const Font *f, unsigned at) { return at + 2 <= f->len ? (unsigned)f->d[at] << 8 | f->d[at + 1] : 0; }
static int ft_s16(const Font *f, unsigned at) { return (short)ft_u16(f, at); }
static unsigned ft_u32(const Font *f, unsigned at) { return ft_u16(f, at) << 16 | ft_u16(f, at + 2); }

static unsigned ft_glyph_index(const Font *f, unsigned code) {
    unsigned t = f->cmap;
    if (f->cmap_format == 4) {
        unsigned segs = ft_u16(f, t + 6) / 2;
        unsigned ends = t + 14, starts = ends + segs * 2 + 2, deltas = starts + segs * 2, ranges = deltas + segs * 2;
        unsigned lo = 0, hi = segs;
        while (lo < hi) { /* the first segment ending at or after code */
            unsigned mid = (lo + hi) / 2;
            if (ft_u16(f, ends + mid * 2) < code) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= segs) return 0;
        unsigned start = ft_u16(f, starts + lo * 2);
        if (code < start) return 0;
        unsigned delta = ft_u16(f, deltas + lo * 2), range = ft_u16(f, ranges + lo * 2);
        if (!range) return (code + delta) & 0xFFFF;
        unsigned g = ft_u16(f, ranges + lo * 2 + range + (code - start) * 2);
        return g ? (g + delta) & 0xFFFF : 0;
    }
    if (f->cmap_format == 12) {
        unsigned groups = ft_u32(f, t + 12), lo = 0, hi = groups;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2, g = t + 16 + mid * 12;
            if (code < ft_u32(f, g)) hi = mid;
            else if (code > ft_u32(f, g + 4)) lo = mid + 1;
            else return ft_u32(f, g + 8) + code - ft_u32(f, g);
        }
    }
    return 0;
}

static unsigned ft_advance(const Font *f, unsigned g) {
    if (!f->num_hmetrics) return 0;
    if (g >= (unsigned)f->num_hmetrics) g = (unsigned)f->num_hmetrics - 1;
    return ft_u16(f, f->hmtx + g * 4);
}

static int font_parse(Font *f) {
    if (f->len < 12) return 0;
    unsigned tables = ft_u16(f, 4), head = 0, hhea = 0, maxp = 0, cmap = 0;
    for (unsigned i = 0; i < tables; i++) {
        unsigned rec = 12 + i * 16, tag = ft_u32(f, rec), off = ft_u32(f, rec + 8), length = ft_u32(f, rec + 12);
        if (off > f->len || length > f->len - off) continue;
        if (tag == 0x68656164) head = off;      /* head */
        else if (tag == 0x68686561) hhea = off; /* hhea */
        else if (tag == 0x6D617870) maxp = off; /* maxp */
        else if (tag == 0x636D6170) cmap = off; /* cmap */
        else if (tag == 0x6C6F6361) f->loca = off;
        else if (tag == 0x676C7966) f->glyf = off;
        else if (tag == 0x686D7478) f->hmtx = off;
    }
    if (!head || !hhea || !maxp || !cmap || !f->loca || !f->glyf || !f->hmtx) return 0;
    f->units = (int)ft_u16(f, head + 18);
    f->long_loca = ft_s16(f, head + 50) != 0;
    f->ascent = ft_s16(f, hhea + 4);
    f->descent = -ft_s16(f, hhea + 6);
    f->gap = ft_s16(f, hhea + 8);
    f->num_hmetrics = (int)ft_u16(f, hhea + 34);
    f->num_glyphs = (int)ft_u16(f, maxp + 4);
    if (f->units < 16 || f->units > 16384) return 0;
    /* The character map: Unicode, full (format 12) or the first 65536 (format 4). */
    unsigned subtables = ft_u16(f, cmap + 2);
    for (unsigned i = 0; i < subtables; i++) {
        unsigned rec = cmap + 4 + i * 8, platform = ft_u16(f, rec), encoding = ft_u16(f, rec + 2);
        unsigned at = cmap + ft_u32(f, rec + 4), format = ft_u16(f, at);
        int unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) continue;
        if (format == 12) {
            f->cmap = at;
            f->cmap_format = 12;
        } else if (format == 4 && f->cmap_format != 12) {
            f->cmap = at;
            f->cmap_format = 4;
        }
    }
    if (!f->cmap_format) return 0;
    for (unsigned c = 0; c < 128; c++) {
        f->ascii_glyph[c] = (unsigned short)ft_glyph_index(f, c < 32 ? ' ' : c);
        f->ascii_adv[c] = (unsigned short)ft_advance(f, f->ascii_glyph[c]);
    }
    return 1;
}

/* The font for a face and style: loaded the first time; NULL without one. */
static Font *font_get(int face, int bold, int italic) {
    int i = (face < 0 || face >= FACE_COUNT ? FACE_SANS : face) * 4 + (bold ? 1 : 0) + (italic ? 2 : 0);
    Font *f = &font_table[i];
    if (!f->state) {
        size_t len;
        f->d = (unsigned char *)read_all(FONT_FILES[i], &len);
        f->len = len;
        f->state = f->d && font_parse(f) ? 1 : -1;
        if (f->state < 0 && f->d) {
            free(f->d);
            f->d = NULL;
        }
    }
    if (f->state > 0) return f;
    if (bold || italic) return font_get(face, 0, 0); /* (the regular one stands in) */
    return NULL;
}

static int font_cell(int size) { return size >= 30 ? 16 : size >= 22 ? 12 : 8; }

/* Next character of UTF-8 text. */
static unsigned font_next_char(const char *s, int len, int *i) {
    unsigned char c = (unsigned char)s[*i];
    (*i)++;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    unsigned code = c & (0x3F >> extra);
    for (int k = 0; k < extra && *i < len && ((unsigned char)s[*i] & 0xC0) == 0x80; k++, (*i)++)
        code = code << 6 | ((unsigned char)s[*i] & 0x3F);
    return extra ? code : 0xFFFD;
}

static unsigned font_glyph(const Font *f, unsigned code, unsigned *advance) {
    if ((code >= 0x200B && code <= 0x200F) || code == 0xFEFF || code == 0xAD || code == 0x2060) { /* (nothing to show) */
        *advance = 0;
        return 0;
    }
    unsigned g = code < 128 ? f->ascii_glyph[code] : ft_glyph_index(f, code);
    if (!g && code >= 128) g = ft_glyph_index(f, '?');
    *advance = code < 128 ? f->ascii_adv[code] : ft_advance(f, g);
    return g;
}

/* Line metrics in pixels: above the baseline, below it, and the extra gap
   (normal line height is their sum), rounded each as browsers do. */
static void font_metrics(int face, int bold, int italic, int size, int *ascent, int *descent, int *gap) {
    Font *f = font_get(face, bold, italic);
    if (!f) {
        int cell = font_cell(size);
        *ascent = cell * 2 * 13 / 16;
        *descent = cell * 2 - *ascent;
        *gap = 4;
        return;
    }
    *ascent = (f->ascent * size + f->units / 2) / f->units;
    *descent = (f->descent * size + f->units / 2) / f->units;
    *gap = f->gap > 0 ? (f->gap * size + f->units / 2) / f->units : 0;
}

/* The width of text in pixels, in 1/64 px. */
static long long font_width64(int face, int bold, int italic, int size, const char *s, int len) {
    Font *f = font_get(face, bold, italic);
    if (!f) {
        int n = 0;
        for (int i = 0; i < len;) {
            font_next_char(s, len, &i);
            n++;
        }
        return (long long)n * font_cell(size) * 64;
    }
    long long units = 0;
    for (int i = 0; i < len;) {
        unsigned adv, code = font_next_char(s, len, &i);
        font_glyph(f, code, &adv);
        units += adv;
    }
    return units * size * 64 / f->units;
}

static int font_width(int face, int bold, int italic, int size, const char *s, int len) {
    return (int)((font_width64(face, bold, italic, size, s, len) + 32) / 64);
}

/* ---------------------------------------------------------------- outlines */

typedef struct {
    float x0, y0, x1, y1;
} FontSeg;

static FontSeg *font_segs;
static int font_seg_count, font_seg_cap;

static void font_line(float x0, float y0, float x1, float y1) {
    if (y0 == y1) return; /* (horizontal lines add no coverage) */
    if (font_seg_count == font_seg_cap) {
        int cap = font_seg_cap ? font_seg_cap * 2 : 512;
        FontSeg *n = realloc(font_segs, sizeof(FontSeg) * (size_t)cap);
        if (!n) return;
        font_segs = n;
        font_seg_cap = cap;
    }
    font_segs[font_seg_count++] = (FontSeg){x0, y0, x1, y1};
}

static float font_sqrt(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static void font_quad(float x0, float y0, float cx, float cy, float x1, float y1) {
    float dx = x0 - 2 * cx + x1, dy = y0 - 2 * cy + y1;
    int n = 1 + (int)font_sqrt(font_sqrt(3 * (dx * dx + dy * dy)));
    if (n > 24) n = 24;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1 - t;
        float x = u * u * x0 + 2 * t * u * cx + t * t * x1, y = u * u * y0 + 2 * t * u * cy + t * t * y1;
        font_line(px, py, x, y);
        px = x;
        py = y;
    }
}

/* Add glyph g's outline as lines, through the transform m (font units to
   pixels: x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5). */
static void font_outline(const Font *f, unsigned g, const float m[6], int depth) {
    if (g >= (unsigned)f->num_glyphs || depth > 6) return;
    unsigned start = f->long_loca ? ft_u32(f, f->loca + g * 4) : ft_u16(f, f->loca + g * 2) * 2;
    unsigned end = f->long_loca ? ft_u32(f, f->loca + g * 4 + 4) : ft_u16(f, f->loca + g * 2 + 2) * 2;
    if (end <= start) return; /* an empty glyph (a space) */
    unsigned at = f->glyf + start;
    int contours = ft_s16(f, at);
    if (contours < 0) { /* made of other glyphs */
        unsigned p = at + 10, flags;
        do {
            flags = ft_u16(f, p);
            unsigned part = ft_u16(f, p + 2);
            p += 4;
            float dx, dy;
            if (flags & 1) {
                dx = (float)ft_s16(f, p);
                dy = (float)ft_s16(f, p + 2);
                p += 4;
            } else {
                dx = (float)(signed char)ft_u8(f, p);
                dy = (float)(signed char)ft_u8(f, p + 1);
                p += 2;
            }
            if (!(flags & 2)) dx = dy = 0; /* (matched points: not supported) */
            float a = 1, b = 0, c = 0, d = 1;
            if (flags & 8) {
                a = d = (float)ft_s16(f, p) / 16384.0f;
                p += 2;
            } else if (flags & 0x40) {
                a = (float)ft_s16(f, p) / 16384.0f;
                d = (float)ft_s16(f, p + 2) / 16384.0f;
                p += 4;
            } else if (flags & 0x80) {
                a = (float)ft_s16(f, p) / 16384.0f;
                b = (float)ft_s16(f, p + 2) / 16384.0f;
                c = (float)ft_s16(f, p + 4) / 16384.0f;
                d = (float)ft_s16(f, p + 6) / 16384.0f;
                p += 8;
            }
            /* this part's transform, then the parent's */
            float n[6] = {m[0] * a + m[2] * b, m[1] * a + m[3] * b, m[0] * c + m[2] * d, m[1] * c + m[3] * d,
                          m[0] * dx + m[2] * dy + m[4], m[1] * dx + m[3] * dy + m[5]};
            font_outline(f, part, n, depth + 1);
        } while (flags & 0x20);
        return;
    }
    unsigned ends = at + 10;
    int points = contours ? (int)ft_u16(f, ends + (unsigned)(contours - 1) * 2) + 1 : 0;
    if (points <= 0 || points > 4096) return;
    unsigned p = ends + (unsigned)contours * 2;
    p += 2 + ft_u16(f, p); /* skip the hinting instructions */
    unsigned char *flag = malloc((size_t)points);
    float *xs = malloc(sizeof(float) * (size_t)points * 2), *ys = xs ? xs + points : NULL;
    if (!flag || !xs) {
        free(flag);
        free(xs);
        return;
    }
    for (int i = 0; i < points;) {
        unsigned char fl = (unsigned char)ft_u8(f, p++);
        int repeat = fl & 8 ? (int)ft_u8(f, p++) : 0;
        for (int k = 0; k <= repeat && i < points; k++) flag[i++] = fl;
    }
    int v = 0;
    for (int i = 0; i < points; i++) {
        if (flag[i] & 2) {
            int dv = (int)ft_u8(f, p++);
            v += flag[i] & 16 ? dv : -dv;
        } else if (!(flag[i] & 16)) {
            v += ft_s16(f, p);
            p += 2;
        }
        xs[i] = (float)v;
    }
    v = 0;
    for (int i = 0; i < points; i++) {
        if (flag[i] & 4) {
            int dv = (int)ft_u8(f, p++);
            v += flag[i] & 32 ? dv : -dv;
        } else if (!(flag[i] & 32)) {
            v += ft_s16(f, p);
            p += 2;
        }
        ys[i] = (float)v;
    }
    for (int i = 0; i < points; i++) { /* to pixels */
        float x = xs[i], y = ys[i];
        xs[i] = m[0] * x + m[2] * y + m[4];
        ys[i] = m[1] * x + m[3] * y + m[5];
    }
    int first = 0;
    for (int ci = 0; ci < contours; ci++) {
        int last = (int)ft_u16(f, ends + (unsigned)ci * 2);
        if (last >= points || last < first) break;
        int count = last - first + 1;
        /* Start at an on-curve point (or between two off-curve ones). */
        int s = -1;
        for (int k = 0; k < count; k++)
            if (flag[first + k] & 1) {
                s = k;
                break;
            }
        float sx, sy;
        if (s >= 0) {
            sx = xs[first + s];
            sy = ys[first + s];
        } else {
            s = 0;
            sx = (xs[first] + xs[last]) / 2;
            sy = (ys[first] + ys[last]) / 2;
        }
        float px = sx, py = sy, cx = 0, cy = 0;
        int have_control = 0;
        for (int k = 1; k <= count; k++) {
            int idx = first + (s + k) % count;
            float x = xs[idx], y = ys[idx];
            if (flag[idx] & 1) {
                if (have_control) font_quad(px, py, cx, cy, x, y);
                else font_line(px, py, x, y);
                px = x;
                py = y;
                have_control = 0;
            } else {
                if (have_control) { /* two controls: an on-curve point between them */
                    float mx = (cx + x) / 2, my = (cy + y) / 2;
                    font_quad(px, py, cx, cy, mx, my);
                    px = mx;
                    py = my;
                }
                cx = x;
                cy = y;
                have_control = 1;
            }
        }
        if (have_control) font_quad(px, py, cx, cy, sx, sy);
        else font_line(px, py, sx, sy);
        first = last + 1;
    }
    free(flag);
    free(xs);
}


/* ---------------------------------------------------------------- drawing glyphs */

static int font_floor(float v) {
    int i = (int)v;
    return (float)i > v ? i - 1 : i;
}
static int font_ceil(float v) {
    int i = (int)v;
    return (float)i < v ? i + 1 : i;
}

/* One edge's share of coverage in a w-wide row grid `a` (rows w + 2 apart):
   signed area accumulation, as in font-rs. A pixel's coverage is later the
   running sum along its row. */
static void font_edge(float *a, int w, int h, FontSeg e) {
    float dir = 1;
    if (e.y0 > e.y1) {
        dir = -1;
        FontSeg t = {e.x1, e.y1, e.x0, e.y0};
        e = t;
    }
    float dxdy = (e.x1 - e.x0) / (e.y1 - e.y0), x = e.x0;
    int y0 = e.y0 < 0 ? 0 : (int)e.y0;
    if (e.y0 < 0) x -= e.y0 * dxdy;
    int y_end = font_ceil(e.y1);
    if (y_end > h) y_end = h;
    for (int y = y0; y < y_end; y++) {
        float *row = a + (size_t)y * (size_t)(w + 2);
        float top = (float)y > e.y0 ? (float)y : e.y0, bottom = (float)(y + 1) < e.y1 ? (float)(y + 1) : e.y1;
        float dy = bottom - top, xnext = x + dxdy * dy, d = dy * dir;
        float x0 = x < xnext ? x : xnext, x1 = x < xnext ? xnext : x;
        if (x0 < 0) x0 = 0;
        if (x1 < 0) x1 = 0;
        int x0i = font_floor(x0), x1i = font_ceil(x1);
        if (x1i > w) x1i = w;
        if (x0i > w) x0i = w;
        if (x1i <= x0i + 1) {
            float xmf = 0.5f * (x + xnext) - (float)x0i;
            row[x0i] += d - d * xmf;
            row[x0i + 1] += d * xmf;
        } else {
            float s = 1.0f / (x1 - x0), x0f = x0 - (float)x0i;
            float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
            float x1f = x1 - (float)x1i + 1;
            float am = 0.5f * s * x1f * x1f;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1 - a0 - am);
            } else {
                float a1 = s * (1.5f - x0f);
                row[x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) row[xi] += d * s;
                float a2 = a1 + (float)(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1 - a2 - am);
            }
            row[x1i] += d * am;
        }
        x = xnext;
    }
}

/* Drawn glyphs: 8-bit coverage, kept until the store fills up. */
typedef struct {
    unsigned key;        /* font, size, quarter-pixel offset; 0 = free */
    unsigned glyph;
    short left, top;     /* bitmap's top-left, from the pen on the baseline */
    unsigned short w, h;
    int at;              /* in glyph_store; -1 when blank */
} GlyphSlot;

#define GLYPH_SLOTS 8192
static GlyphSlot glyph_slots[GLYPH_SLOTS];
static int glyph_slots_used;
static unsigned char *glyph_store;
static int glyph_store_used;
#define GLYPH_STORE (3 << 20)

static unsigned glyph_hash(unsigned key, unsigned g) { return (key * 2654435761u ^ g * 40503u) & (GLYPH_SLOTS - 1); }

static GlyphSlot *font_render(int font_index, const Font *f, unsigned g, int size, int phase) {
    unsigned key = (unsigned)(font_index + 1) << 24 | (unsigned)(size & 0xFFFF) << 4 | (unsigned)phase;
    for (unsigned h = glyph_hash(key, g);; h = (h + 1) & (GLYPH_SLOTS - 1)) {
        GlyphSlot *slot = &glyph_slots[h];
        if (slot->key == key && slot->glyph == g) return slot;
        if (!slot->key) break;
    }
    if (!glyph_store) glyph_store = malloc(GLYPH_STORE);
    if (!glyph_store) return NULL;
    GlyphSlot made = {key, g, 0, 0, 0, 0, -1};
    unsigned char *bits = NULL;
    float scale = (float)size / (float)f->units;
    float m[6] = {scale, 0, 0, -scale, (float)phase / 4.0f, 0};
    font_seg_count = 0;
    font_outline(f, g, m, 0);
    if (font_seg_count) {
        float minx = font_segs[0].x0, maxx = minx, miny = font_segs[0].y0, maxy = miny;
        for (int i = 0; i < font_seg_count; i++) {
            FontSeg *e = &font_segs[i];
            if (e->x0 < minx) minx = e->x0;
            if (e->x1 < minx) minx = e->x1;
            if (e->x0 > maxx) maxx = e->x0;
            if (e->x1 > maxx) maxx = e->x1;
            if (e->y0 < miny) miny = e->y0;
            if (e->y1 < miny) miny = e->y1;
            if (e->y0 > maxy) maxy = e->y0;
            if (e->y1 > maxy) maxy = e->y1;
        }
        int left = font_floor(minx), top = font_floor(miny);
        int w = font_ceil(maxx) - left, h = font_ceil(maxy) - top;
        if (w > 0 && h > 0 && w < 1024 && h < 1024) {
            float *acc = malloc(sizeof(float) * (size_t)(w + 2) * (size_t)h);
            bits = malloc((size_t)w * (size_t)h);
            if (acc && bits) {
                memset(acc, 0, sizeof(float) * (size_t)(w + 2) * (size_t)h);
                for (int i = 0; i < font_seg_count; i++) {
                    FontSeg e = font_segs[i];
                    e.x0 -= (float)left;
                    e.x1 -= (float)left;
                    e.y0 -= (float)top;
                    e.y1 -= (float)top;
                    font_edge(acc, w, h, e);
                }
                for (int y = 0; y < h; y++) {
                    float sum = 0, *row = acc + (size_t)y * (size_t)(w + 2);
                    for (int x = 0; x < w; x++) {
                        sum += row[x];
                        float c = sum < 0 ? -sum : sum;
                        if (c > 1) c = 1;
                        /* a little darker in the middle tones, as screens show text */
                        float boosted = c * (1.6f - 0.6f * c);
                        bits[y * w + x] = (unsigned char)(boosted * 255.0f + 0.5f);
                    }
                }
                made.left = (short)left;
                made.top = (short)top;
                made.w = (unsigned short)w;
                made.h = (unsigned short)h;
            } else {
                free(bits);
                bits = NULL;
            }
            free(acc);
        }
    }
    int need = bits ? made.w * made.h : 0;
    if (glyph_slots_used >= GLYPH_SLOTS * 3 / 4 || glyph_store_used + need > GLYPH_STORE) { /* full: start over */
        memset(glyph_slots, 0, sizeof(glyph_slots));
        glyph_slots_used = 0;
        glyph_store_used = 0;
    }
    if (bits && need <= GLYPH_STORE) {
        memcpy(glyph_store + glyph_store_used, bits, (size_t)need);
        made.at = glyph_store_used;
        glyph_store_used += need;
    }
    free(bits);
    unsigned h = glyph_hash(key, g);
    while (glyph_slots[h].key) h = (h + 1) & (GLYPH_SLOTS - 1);
    glyph_slots[h] = made;
    glyph_slots_used++;
    return &glyph_slots[h];
}

static void font_blend(canvas *c, int x, int y, const GlyphSlot *s, unsigned color) {
    if (s->at < 0) return;
    const unsigned char *src = glyph_store + s->at;
    unsigned cr = color >> 16 & 255, cg = color >> 8 & 255, cb = color & 255, opacity = 255 - (color >> 24);
    color &= 0xFFFFFF;
    for (int row = 0; row < s->h; row++) {
        int py = y + row;
        if (py < 0 || py >= c->height) continue;
        unsigned *line = c->pixels + (size_t)py * (size_t)c->stride;
        for (int col = 0; col < s->w; col++) {
            int px = x + col;
            unsigned a = src[row * s->w + col];
            if (opacity < 255) a = a * opacity / 255;
            if (!a || px < 0 || px >= c->width) continue;
            if (a == 255) {
                line[px] = color;
                continue;
            }
            unsigned d = line[px], inv = 255 - a;
            unsigned r = (cr * a + (d >> 16 & 255) * inv + 127) / 255;
            unsigned gg = (cg * a + (d >> 8 & 255) * inv + 127) / 255;
            unsigned b = (cb * a + (d & 255) * inv + 127) / 255;
            line[px] = r << 16 | gg << 8 | b;
        }
    }
}

/* Draw UTF-8 text with its baseline at y. */
static void font_draw(canvas *c, int x, int baseline, const char *s, int len, int face, int bold, int italic, int size,
                      unsigned color) {
    Font *f = font_get(face, bold, italic);
    if (!f || size < 1) {
        int cell = font_cell(size), ascent = cell * 2 * 13 / 16;
        draw_glyphs(c, x, baseline - ascent, s, len, cell, color & 0xFFFFFF, (bold ? GLYPH_BOLD : 0) | (italic ? GLYPH_ITALIC : 0));
        return;
    }
    if (size > 400) size = 400;
    int index = (int)(f - font_table);
    if (baseline + size < 0 || baseline - size * 2 > c->height) return;
    long long pen = 0; /* 1/64 px */
    for (int i = 0; i < len;) {
        unsigned adv, code = font_next_char(s, len, &i);
        unsigned g = font_glyph(f, code, &adv);
        long long px64 = (long long)x * 64 + pen;
        int whole = (int)(px64 >> 6), phase = (int)((px64 & 63) + 8) / 16;
        if (phase == 4) {
            whole++;
            phase = 0;
        }
        if (whole > c->width) break;
        if (code != ' ' && g && whole > -size * 2) {
            GlyphSlot *slot = font_render(index, f, g, size, phase);
            if (slot) font_blend(c, whole + slot->left, baseline + slot->top, slot, color);
        }
        pen += (long long)adv * size * 64 / f->units;
    }
}

/* web/paint.c - drawing the display list, and finding what is under the
   pointer. Part of the browser. */

static Node *focused_control(void) { return focused_node; }
static int frame_paint(Node *n, canvas *v, int x, int y); /* (realm.c) */

static int is_text_control(Node *n) {
    if (!n || n->type != NODE_ELEMENT) return 0;
    if (is(n->tag, "textarea")) return 1;
    if (!is(n->tag, "input")) return 0;
    char type[16] = "text";
    attr(n, "type", type, sizeof(type));
    return strcasecmp_word(type, "checkbox") && strcasecmp_word(type, "radio") && strcasecmp_word(type, "submit") &&
           strcasecmp_word(type, "button") && strcasecmp_word(type, "reset") && strcasecmp_word(type, "image") &&
           strcasecmp_word(type, "hidden") && strcasecmp_word(type, "file") && strcasecmp_word(type, "range") &&
           strcasecmp_word(type, "color");
}

/* Text of a control in its font, vertically centred in h. Returns its width. */
static int control_text(canvas *v, Node *n, int x, int y, int h, const char *s, int len, unsigned color) {
    int a, d, g;
    font_metrics(FONT_OF(&n->style), &a, &d, &g);
    font_draw(v, x, y + (h - a - d) / 2 + a, s, len, FONT_OF(&n->style), color);
    return text_w(&n->style, s, len);
}

static void draw_control(canvas *v, Item *it, int x, int y) {
    Node *n = it->node;
    char type[16] = "text";
    attr(n, "type", type, sizeof(type));
    unsigned color = n->style.color;
    if (is(n->tag, "input") && (!strcasecmp_word(type, "checkbox") || !strcasecmp_word(type, "radio"))) {
        int radio = !strcasecmp_word(type, "radio");
        fill_rect(v, x, y, 13, 13, 0xFFFFFF);
        draw_rect(v, x, y, 13, 13, n == focused_control() ? 0x3D7DFF : 0x767676);
        if (n->checked) {
            if (radio) fill_rect(v, x + 4, y + 4, 5, 5, 0x1A5FD6);
            else { /* a tick: down from (3,6) to (5,9), then up to (10,3) */
                for (int i = 0; i <= 3; i++) fill_rect(v, x + 3 + i * 2 / 3, y + 6 + i, 2, 2, 0x1A5FD6);
                for (int i = 0; i <= 6; i++) fill_rect(v, x + 5 + i * 5 / 6, y + 9 - i, 2, 2, 0x1A5FD6);
            }
        }
        return;
    }
    if (is(n->tag, "input") && (!strcasecmp_word(type, "submit") || !strcasecmp_word(type, "button") || !strcasecmp_word(type, "reset"))) {
        char label[64];
        control_label(n, label, sizeof(label));
        int len = (int)strlen(label);
        control_text(v, n, x + (it->w - text_w(&n->style, label, len)) / 2, y, it->h, label, len, color);
        return;
    }
    if (is(n->tag, "select")) {
        char label[80];
        control_label(n, label, sizeof(label));
        int len = (int)strlen(label);
        while (len > 0 && text_w(&n->style, label, len) > it->w - 16) len--;
        control_text(v, n, x, y, it->h, label, len, color);
        /* the arrow */
        int ax = x + it->w - 10, ay = y + it->h / 2 - 2;
        for (int i = 0; i < 4; i++) fill_rect(v, ax + i, ay + i, 8 - i * 2, 1, color);
        return;
    }
    if (is(n->tag, "progress") || is(n->tag, "meter")) {
        char vbuf[16], mbuf[16];
        int value = attr(n, "value", vbuf, sizeof(vbuf)) ? atoi(vbuf) : 0;
        int max = attr(n, "max", mbuf, sizeof(mbuf)) ? atoi(mbuf) : 1;
        if (max <= 0) max = 1;
        fill_rect(v, x, y + 3, it->w, it->h - 6, 0xE0E0E0);
        fill_rect(v, x, y + 3, (int)((long long)it->w * (value > max ? max : value) / max), it->h - 6, 0x3D7DFF);
        return;
    }
    if (is_text_control(n)) {
        const char *text = n->value ? n->value : "";
        int password = is(n->tag, "input") && !strcasecmp_word(type, "password");
        char shown[256];
        int len = (int)strlen(text);
        if (len > 255) len = 255;
        for (int i = 0; i < len; i++) shown[i] = password ? '*' : text[i];
        unsigned text_color = color;
        if (!len) { /* the placeholder, dimmed */
            if (attr(n, "placeholder", shown, sizeof(shown))) {
                len = (int)strlen(shown);
                text_color = 0x8A8F9C;
            }
        }
        int a, d, g, lh = line_metrics(&n->style, NULL);
        font_metrics(FONT_OF(&n->style), &a, &d, &g);
        if (is(n->tag, "textarea")) {
            /* Lines, wrapped to the box. */
            int col = 0, row = 0, rows = it->h / (lh > 0 ? lh : 1);
            for (int i = 0; i < len && row < rows; i++) {
                int cw = text_w(&n->style, shown + i, 1);
                if (shown[i] == '\n' || col + cw > it->w) {
                    row++;
                    col = 0;
                    if (shown[i] == '\n') continue;
                }
                if (row < rows) font_draw(v, x + col, y + row * lh + a, shown + i, 1, FONT_OF(&n->style), text_color);
                col += cw;
            }
            if (n == focused_control()) fill_rect(v, x + col, y + row * lh, 1, a + d, 0x000000);
            return;
        }
        int start = 0;
        while (start < len && text_w(&n->style, shown + start, len - start) > it->w - 2) start++;
        int w = control_text(v, n, x, y, it->h, shown + start, len - start, text_color);
        if (n == focused_control()) {
            int caret = n->value && n->value[0] ? w : 0;
            fill_rect(v, x + caret, y + (it->h - a - d) / 2, 1, a + d, 0x000000);
        }
        return;
    }
    /* svg: its drawing, once made; video, canvas, iframe, object: a labelled box */
    if (is(n->tag, "svg")) {
        if (n->img_state == 1 && n->img) image_draw(v, (image *)n->img, x, y, it->w, it->h);
        return;
    }
    if (is(n->tag, "iframe") && frame_paint(n, v, x, y)) return;
    fill_rect(v, x, y, it->w, it->h, 0xEDEFF3);
    draw_rect(v, x, y, it->w, it->h, 0xC9CDD6);
    char label[32] = "[";
    strcpy(label + 1, n->tag);
    strcpy(label + strlen(label), "]");
    if (it->w > 48) font_draw(v, x + 6, y + 18, label, (int)strlen(label), FACE_SANS, 0, 0, 13, 0x8A8F9C);
}

/* ---------------------------------------------------------------- see-through colours and round corners */

static unsigned paint_opacity = 255; /* the item's element's opacity */

static int memmem_simple(const char *s, int len, const char *what) {
    int wl = (int)strlen(what);
    for (int i = 0; i + wl <= len; i++)
        if (!strncasecmp(s + i, what, (size_t)wl)) return 1;
    return 0;
}

static void *calloc_zero(size_t n) {
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

/* A colour's alpha with the element's opacity applied: 0..255. */
static unsigned color_alpha(unsigned color) { return COLOR_ALPHA(color) * paint_opacity / 255; }

static void blend_px(unsigned *p, unsigned rgb, unsigned a) {
    if (a >= 255) {
        *p = rgb & 0xFFFFFF;
        return;
    }
    unsigned d = *p, inv = 255 - a;
    unsigned r = ((rgb >> 16 & 255) * a + (d >> 16 & 255) * inv + 127) / 255;
    unsigned g = ((rgb >> 8 & 255) * a + (d >> 8 & 255) * inv + 127) / 255;
    unsigned b = ((rgb & 255) * a + (d & 255) * inv + 127) / 255;
    *p = r << 16 | g << 8 | b;
}

static void paint_fill(canvas *v, int x, int y, int w, int h, unsigned color) {
    unsigned a = color_alpha(color);
    if (!a) return;
    if (a >= 255) {
        fill_rect(v, x, y, w, h, color & 0xFFFFFF);
        return;
    }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > v->width ? v->width : x + w, y1 = y + h > v->height ? v->height : y + h;
    for (int py = y0; py < y1; py++) {
        unsigned *row = v->pixels + (size_t)py * (size_t)v->stride;
        for (int px = x0; px < x1; px++) blend_px(&row[px], color, a);
    }
}

/* Corner radii, resolved for a w x h box: across and down for top-left,
   top-right, bottom-right, bottom-left; scaled down to fit as CSS says. */
typedef struct {
    float rx[4], ry[4];
} Radii;

static int radii_for(const Style *s, int w, int h, Radii *r) {
    int any = 0;
    for (int i = 0; i < 4; i++) {
        int a = s->radius[i], d = s->radius[i + 4];
        r->rx[i] = a >= 0 ? (float)a : (float)w * (float)-a / 100.0f;
        r->ry[i] = d >= 0 ? (float)d : (float)h * (float)-d / 100.0f;
        if (r->rx[i] <= 0 || r->ry[i] <= 0) r->rx[i] = r->ry[i] = 0;
        any |= r->rx[i] > 0;
    }
    if (!any) return 0;
    float f = 1, t;
    if ((t = r->rx[0] + r->rx[1]) > (float)w && (float)w / t < f) f = (float)w / t;
    if ((t = r->rx[3] + r->rx[2]) > (float)w && (float)w / t < f) f = (float)w / t;
    if ((t = r->ry[0] + r->ry[3]) > (float)h && (float)h / t < f) f = (float)h / t;
    if ((t = r->ry[1] + r->ry[2]) > (float)h && (float)h / t < f) f = (float)h / t;
    for (int i = 0; i < 4; i++) {
        r->rx[i] *= f;
        r->ry[i] *= f;
    }
    return 1;
}

/* Is point (X, Y) inside a w x h box with round corners? */
static int round_inside(const Radii *r, float w, float h, float X, float Y) {
    if (X < 0 || Y < 0 || X >= w || Y >= h) return 0;
    float cx, cy, rx, ry;
    if (X < r->rx[0] && Y < r->ry[0]) rx = r->rx[0], ry = r->ry[0], cx = rx, cy = ry;
    else if (X > w - r->rx[1] && Y < r->ry[1]) rx = r->rx[1], ry = r->ry[1], cx = w - rx, cy = ry;
    else if (X > w - r->rx[2] && Y > h - r->ry[2]) rx = r->rx[2], ry = r->ry[2], cx = w - rx, cy = h - ry;
    else if (X < r->rx[3] && Y > h - r->ry[3]) rx = r->rx[3], ry = r->ry[3], cx = rx, cy = h - ry;
    else return 1;
    float dx = (X - cx) / rx, dy = (Y - cy) / ry;
    return dx * dx + dy * dy <= 1.0f;
}

/* How much of pixel (px, py) the shape covers, 0..16. */
static int round_cover(const Radii *r, float w, float h, int px, int py) {
    int n = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) n += round_inside(r, w, h, (float)px + (float)sx * 0.25f + 0.125f, (float)py + (float)sy * 0.25f + 0.125f);
    return n;
}

/* Fill a box with round corners at (x, y); with `inner` set, only the
   ring between it and the box inset by bt/br/bb/bl (a border). */
static void paint_round(canvas *v, int x, int y, int w, int h, const Radii *r, int inner, int bt, int br, int bb, int bl, unsigned color) {
    unsigned a = color_alpha(color);
    if (!a || w <= 0 || h <= 0) return;
    Radii in;
    float iw = (float)(w - bl - br), ih = (float)(h - bt - bb);
    if (inner) {
        static const int side_x[4] = {3, 1, 1, 3}, side_y[4] = {0, 0, 2, 2};
        int widths[4] = {bt, br, bb, bl};
        for (int i = 0; i < 4; i++) {
            in.rx[i] = r->rx[i] - (float)widths[side_x[i]];
            in.ry[i] = r->ry[i] - (float)widths[side_y[i]];
            if (in.rx[i] < 0 || in.ry[i] < 0) in.rx[i] = in.ry[i] = 0;
        }
    }
    float top_zone = r->ry[0] > r->ry[1] ? r->ry[0] : r->ry[1], bottom_zone = r->ry[2] > r->ry[3] ? r->ry[2] : r->ry[3];
    float left_zone = r->rx[0] > r->rx[3] ? r->rx[0] : r->rx[3], right_zone = r->rx[1] > r->rx[2] ? r->rx[1] : r->rx[2];
    for (int py = 0; py < h; py++) {
        int vy = y + py;
        if (vy < 0 || vy >= v->height) continue;
        unsigned *row = v->pixels + (size_t)vy * (size_t)v->stride;
        int corner_row = (float)py < top_zone + 1 || (float)py >= (float)h - bottom_zone - 1;
        int band = py < bt || py >= h - bb; /* a row of the top or bottom border */
        for (int px = 0; px < w; px++) {
            int vx = x + px;
            if (vx < 0 || vx >= v->width) continue;
            int corner = corner_row && ((float)px < left_zone + 1 || (float)px >= (float)w - right_zone - 1);
            int cover;
            if (corner) {
                cover = round_cover(r, (float)w, (float)h, px, py);
                if (inner && cover) cover -= round_cover(&in, iw, ih, px - bl, py - bt);
            } else if (inner) {
                cover = band || px < bl || px >= w - br ? 16 : 0;
            } else {
                cover = 16;
            }
            if (cover <= 0) continue;
            blend_px(&row[vx], color, a * (unsigned)cover / 16);
        }
    }
}

/* ---------------------------------------------------------------- pictures, scaled smoothly */

/* Pictures drawn at another size are resampled once (averaging when made
   smaller, blending neighbours when made bigger) and kept. */
typedef struct {
    const image *src;
    int w, h;
    unsigned *pixels;
    unsigned used;
} Scaled;
#define SCALED_MAX 48
static Scaled scaled[SCALED_MAX];
static unsigned scaled_clock;
static size_t scaled_bytes;

static void scaled_forget(const image *img) {
    for (int i = 0; i < SCALED_MAX; i++)
        if (scaled[i].pixels && (!img || scaled[i].src == img)) {
            scaled_bytes -= (size_t)scaled[i].w * (size_t)scaled[i].h * 4;
            free(scaled[i].pixels);
            memset(&scaled[i], 0, sizeof(scaled[i]));
        }
}

static const unsigned *scaled_pixels(const image *img, int w, int h) {
    if (w == img->width && h == img->height) return img->pixels;
    if (w <= 0 || h <= 0 || (long long)w * h > 4000000 || !img->width || !img->height) return NULL;
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < SCALED_MAX; i++) {
        if (scaled[i].pixels && scaled[i].src == img && scaled[i].w == w && scaled[i].h == h) {
            scaled[i].used = ++scaled_clock;
            return scaled[i].pixels;
        }
        if (!scaled[i].pixels && free_slot < 0) free_slot = i;
        if (scaled[i].used < scaled[oldest].used) oldest = i;
    }
    size_t bytes = (size_t)w * (size_t)h * 4;
    while (scaled_bytes + bytes > (48u << 20)) { /* over budget: the least recently drawn go */
        int victim = -1;
        for (int i = 0; i < SCALED_MAX; i++)
            if (scaled[i].pixels && (victim < 0 || scaled[i].used < scaled[victim].used)) victim = i;
        if (victim < 0) break;
        scaled_bytes -= (size_t)scaled[victim].w * (size_t)scaled[victim].h * 4;
        free(scaled[victim].pixels);
        memset(&scaled[victim], 0, sizeof(scaled[victim]));
        free_slot = victim;
    }
    if (free_slot < 0) {
        scaled_bytes -= (size_t)scaled[oldest].w * (size_t)scaled[oldest].h * 4;
        free(scaled[oldest].pixels);
        memset(&scaled[oldest], 0, sizeof(scaled[oldest]));
        free_slot = oldest;
    }
    unsigned *out = malloc(bytes);
    if (!out) return NULL;
    int iw = img->width, ih = img->height;
    const unsigned *src = img->pixels;
    if (w <= iw && h <= ih) { /* smaller: the average of the pixels each one covers (weighted by alpha) */
        for (int dy = 0; dy < h; dy++) {
            int y0 = (int)((long long)dy * ih / h), y1 = (int)((long long)(dy + 1) * ih / h);
            if (y1 <= y0) y1 = y0 + 1;
            for (int dx = 0; dx < w; dx++) {
                int x0 = (int)((long long)dx * iw / w), x1 = (int)((long long)(dx + 1) * iw / w);
                if (x1 <= x0) x1 = x0 + 1;
                unsigned long long sa = 0, sr = 0, sg = 0, sb = 0;
                for (int sy = y0; sy < y1; sy++) {
                    const unsigned *row = src + (size_t)sy * (size_t)iw;
                    for (int sx = x0; sx < x1; sx++) {
                        unsigned p = row[sx], a = p >> 24;
                        sa += a;
                        sr += (p >> 16 & 255) * a;
                        sg += (p >> 8 & 255) * a;
                        sb += (p & 255) * a;
                    }
                }
                unsigned count = (unsigned)((y1 - y0) * (x1 - x0));
                unsigned a = (unsigned)(sa / count);
                out[(size_t)dy * (size_t)w + (size_t)dx] = sa ? a << 24 | (unsigned)(sr / sa) << 16 | (unsigned)(sg / sa) << 8 | (unsigned)(sb / sa) : 0;
            }
        }
    } else { /* bigger (or mixed): blend the four nearest */
        for (int dy = 0; dy < h; dy++) {
            long long fy = ((long long)dy * 2 + 1) * ih * 128 / h - 128; /* (1/256 px) */
            if (fy < 0) fy = 0;
            int y0 = (int)(fy >> 8), y1 = y0 + 1 < ih ? y0 + 1 : y0;
            unsigned wy = (unsigned)(fy & 255);
            for (int dx = 0; dx < w; dx++) {
                long long fx = ((long long)dx * 2 + 1) * iw * 128 / w - 128;
                if (fx < 0) fx = 0;
                int x0 = (int)(fx >> 8), x1 = x0 + 1 < iw ? x0 + 1 : x0;
                if (x0 >= iw) x0 = x1 = iw - 1;
                if (y0 >= ih) y0 = y1 = ih - 1;
                unsigned wx = (unsigned)(fx & 255);
                unsigned p[4] = {src[(size_t)y0 * iw + x0], src[(size_t)y0 * iw + x1], src[(size_t)y1 * iw + x0], src[(size_t)y1 * iw + x1]};
                unsigned wgt[4] = {(256 - wx) * (256 - wy), wx * (256 - wy), (256 - wx) * wy, wx * wy};
                unsigned long long sa = 0, sr = 0, sg = 0, sb = 0;
                for (int k = 0; k < 4; k++) {
                    unsigned a = (p[k] >> 24) * wgt[k];
                    sa += a;
                    sr += (unsigned long long)(p[k] >> 16 & 255) * a;
                    sg += (unsigned long long)(p[k] >> 8 & 255) * a;
                    sb += (unsigned long long)(p[k] & 255) * a;
                }
                out[(size_t)dy * (size_t)w + (size_t)dx] = sa ? (unsigned)(sa >> 16) << 24 | (unsigned)(sr / sa) << 16 | (unsigned)(sg / sa) << 8 | (unsigned)(sb / sa) : 0;
            }
        }
    }
    scaled[free_slot] = (Scaled){img, w, h, out, ++scaled_clock};
    scaled_bytes += bytes;
    return out;
}

/* Draw a picture scaled to w x h (smoothly), with the element's opacity. */
static void paint_image(canvas *v, const image *img, int x, int y, int w, int h) {
    const unsigned *px = scaled_pixels(img, w, h);
    if (!px) {
        image_draw(v, img, x, y, w, h);
        return;
    }
    int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;
    int x1 = x + w > v->width ? v->width - x : w, y1 = y + h > v->height ? v->height - y : h;
    for (int dy = y0; dy < y1; dy++) {
        const unsigned *src = px + (size_t)dy * (size_t)w;
        unsigned *dst = v->pixels + (size_t)(y + dy) * (size_t)v->stride + x;
        for (int dx = x0; dx < x1; dx++) {
            unsigned p = src[dx], a = (p >> 24) * paint_opacity / 255;
            if (a) blend_px(&dst[dx], p, a);
        }
    }
}

/* ---------------------------------------------------------------- gradients */

typedef struct {
    float pos;
    unsigned color;
} GStop;

/* Split a function's arguments at the top-level commas. */
static int split_args(const char *s, int len, const char *start[], int lens[], int max) {
    int n = 0, depth = 0, from = 0;
    for (int i = 0; i <= len && n < max; i++) {
        char c = i < len ? s[i] : ',';
        if (c == '(') depth++;
        else if (c == ')') depth--;
        if (c == ',' && depth <= 0) {
            int a = from, b = i;
            while (a < b && is_space(s[a])) a++;
            while (b > a && is_space(s[b - 1])) b--;
            start[n] = s + a;
            lens[n++] = b - a;
            from = i + 1;
        }
    }
    return n;
}

/* A position along the gradient: "30%" or "12px" (of `length`); -1 if none. */
static float stop_pos(const char *t, float length) {
    int used, n = number100(t, &used);
    if (!used) return -1;
    if (t[used] == '%') return (float)n / 10000.0f;
    int ok;
    Len l = parse_len(t, 16, &ok);
    if (ok && l.unit == 'p' && length > 0) return (float)l.value / length;
    return -1;
}

/* The colours of a gradient in 256 steps (0xTTRRGGBB as everywhere). */
static int gradient_lut(const char *args[], const int lens[], int first, int count, float length, unsigned lut[256]) {
    GStop stops[16];
    int n = 0;
    for (int i = first; i < count && n < 15; i++) {
        char buf[160];
        int len = lens[i] < 159 ? lens[i] : 159;
        memcpy(buf, args[i], (size_t)len);
        buf[len] = 0;
        /* "color [pos [pos]]": the colour may itself have spaces inside brackets */
        const char *p = buf;
        char part[96];
        unsigned c;
        int got = 0;
        float pos[2] = {-1, -1};
        int k = 0;
        while (value_part(&p, part, sizeof(part)) > 0) {
            int g = parse_color(part, &c);
            if (g == 1) got = 1;
            else if (g == 2) {
                got = 1;
                c = BORDER_INVISIBLE;
            } else if (k < 2) pos[k++] = stop_pos(part, length);
        }
        if (!got) continue; /* (a hint between stops: ignored) */
        stops[n++] = (GStop){pos[0], c};
        if (pos[1] >= 0 && n < 15) stops[n++] = (GStop){pos[1], c};
    }
    if (!n) return 0;
    if (n == 1) stops[n++] = stops[0];
    if (stops[0].pos < 0) stops[0].pos = 0;
    if (stops[n - 1].pos < 0) stops[n - 1].pos = 1;
    for (int i = 1; i < n; i++) { /* positions never go back; missing ones share the gap */
        if (stops[i].pos >= 0 && stops[i].pos < stops[i - 1].pos) stops[i].pos = stops[i - 1].pos;
        if (stops[i].pos < 0) {
            int j = i;
            while (stops[j].pos < 0) j++;
            for (int k = i; k < j; k++) stops[k].pos = stops[i - 1].pos + (stops[j].pos - stops[i - 1].pos) * (float)(k - i + 1) / (float)(j - i + 1);
        }
    }
    for (int t = 0; t < 256; t++) {
        float f = (float)t / 255.0f;
        int k = 0;
        while (k < n - 1 && stops[k + 1].pos < f) k++;
        GStop a = stops[k], b = stops[k + 1 < n ? k + 1 : k];
        float span = b.pos - a.pos, m = span > 0 ? (f - a.pos) / span : (f < a.pos ? 0 : 1);
        if (m < 0) m = 0;
        if (m > 1) m = 1;
        /* in premultiplied colour, as browsers mix */
        float aa = (float)COLOR_ALPHA(a.color) / 255.0f, ba = (float)COLOR_ALPHA(b.color) / 255.0f;
        float alpha = aa + (ba - aa) * m;
        float ch[3];
        for (int q = 0; q < 3; q++) {
            float ca = (float)(a.color >> (16 - 8 * q) & 255) * aa, cb = (float)(b.color >> (16 - 8 * q) & 255) * ba;
            ch[q] = alpha > 0 ? (ca + (cb - ca) * m) / alpha : 0;
        }
        unsigned A = (unsigned)(alpha * 255.0f + 0.5f);
        lut[t] = (255 - A) << 24 | (unsigned)(ch[0] + 0.5f) << 16 | (unsigned)(ch[1] + 0.5f) << 8 | (unsigned)(ch[2] + 0.5f);
    }
    return 1;
}

static float gsqrt(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* A side of a box for "at <position>": a keyword, a percentage or px. */
static float at_value(const char *t, float size, float fallback) {
    if (!strcasecmp(t, "left") || !strcasecmp(t, "top")) return 0;
    if (!strcasecmp(t, "right") || !strcasecmp(t, "bottom")) return size;
    if (!strcasecmp(t, "center")) return size / 2;
    int used, n = number100(t, &used);
    if (!used) return fallback;
    if (t[used] == '%') return size * (float)n / 10000.0f;
    return (float)n / 100.0f;
}

/* Paint one gradient (linear-, radial-, repeating-) over a w x h box. */
static void paint_gradient(canvas *v, int x, int y, int w, int h, const char *g, int glen, const Radii *radii) {
    if (w <= 0 || h <= 0) return;
    int radial = glen > 6 && (!strncasecmp(g, "radial", 6) || !strncasecmp(g, "repeating-radial", 16));
    int repeating = !strncasecmp(g, "repeating", 9);
    int conic = strstr(g, "conic") && strstr(g, "conic") < g + 12;
    const char *open = memchr(g, '(', (size_t)glen);
    if (!open) return;
    const char *args[18];
    int lens[18];
    int count = split_args(open + 1, (int)(g + glen - open - 2), args, lens, 18);
    if (!count) return;
    float dx = 0, dy = 1, length = (float)h, cx = (float)w / 2, cy = (float)h / 2, rx = 1, ry = 1;
    int first = 0;
    char head[160];
    int hl = lens[0] < 159 ? lens[0] : 159;
    memcpy(head, args[0], (size_t)hl);
    head[hl] = 0;
    unsigned dummy;
    char word[96];
    const char *hp = head;
    value_part(&hp, word, sizeof(word));
    int head_is_color = parse_color(word, &dummy) != 0;
    if (!radial && !conic) {
        float angle = 180; /* degrees: to bottom */
        if (!head_is_color) {
            first = 1;
            if (!strncasecmp(head, "to ", 3)) {
                int up = strstr(head, "top") != NULL, down = strstr(head, "bottom") != NULL;
                int left = strstr(head, "left") != NULL, right = strstr(head, "right") != NULL;
                if ((up || down) && (left || right)) { /* a corner: square to the other diagonal */
                    float vx = right ? (float)h : -(float)h, vy = down ? (float)w : -(float)w, l = gsqrt(vx * vx + vy * vy);
                    dx = vx / l;
                    dy = vy / l;
                    angle = -1;
                } else angle = up ? 0 : right ? 90 : left ? 270 : 180;
            } else {
                int used, n = number100(head, &used);
                const char *u = head + used;
                angle = (float)n / 100.0f;
                if (!strncasecmp(u, "turn", 4)) angle *= 360;
                else if (!strncasecmp(u, "rad", 3)) angle *= 57.29578f;
                else if (!strncasecmp(u, "grad", 4)) angle *= 0.9f;
            }
            if (angle >= 0) {
                /* sin and cos of the angle, from a few terms (enough for drawing) */
                float a = angle;
                while (a < 0) a += 360;
                while (a >= 360) a -= 360;
                float r = a * 3.14159265f / 180.0f;
                float r2 = r > 3.14159265f ? r - 6.2831853f : r;
                float x2 = r2 * r2;
                float sn = r2 * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
                float cs = 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90))));
                dx = sn;
                dy = -cs;
            }
        }
        length = (dx < 0 ? -dx : dx) * (float)w + (dy < 0 ? -dy : dy) * (float)h;
        if (length < 1) length = 1;
    } else if (radial) {
        int circle = 0, closest = 0, farthest_side = 0;
        float explicit_r = -1;
        if (!head_is_color) {
            first = 1;
            circle = strstr(head, "circle") != NULL;
            closest = strstr(head, "closest-side") != NULL || strstr(head, "closest-corner") != NULL;
            farthest_side = strstr(head, "farthest-side") != NULL;
            const char *at = strstr(head, "at ");
            if (at) {
                const char *q = at + 3;
                char px[32] = "center", py[32] = "center";
                value_part(&q, px, sizeof(px));
                if (value_part(&q, py, sizeof(py)) <= 0) strcpy(py, "center");
                if (!strcasecmp(px, "top") || !strcasecmp(px, "bottom")) { /* (written y first) */
                    char t[32];
                    strcpy(t, px);
                    strcpy(px, py);
                    strcpy(py, t);
                }
                cx = at_value(px, (float)w, cx);
                cy = at_value(py, (float)h, cy);
            }
            const char *q = head;
            while (value_part(&q, word, sizeof(word)) > 0 && strcasecmp(word, "at")) {
                int used;
                number100(word, &used);
                if (used && explicit_r < 0) explicit_r = at_value(word, (float)(w > h ? w : h), -1);
            }
        }
        float fx = cx > (float)w - cx ? cx : (float)w - cx, fy = cy > (float)h - cy ? cy : (float)h - cy;
        float nx = cx < (float)w - cx ? cx : (float)w - cx, ny = cy < (float)h - cy ? cy : (float)h - cy;
        if (explicit_r > 0) rx = ry = explicit_r;
        else if (circle) {
            rx = ry = closest ? (nx < ny ? nx : ny) : farthest_side ? (fx > fy ? fx : fy) : gsqrt(fx * fx + fy * fy);
        } else if (closest) rx = nx, ry = ny;
        else if (farthest_side) rx = fx, ry = fy;
        else rx = fx * 1.41421356f, ry = fy * 1.41421356f;
        if (rx < 1) rx = 1;
        if (ry < 1) ry = 1;
        length = rx;
    } else { /* conic: shown as its first colour */
        first = head_is_color ? 0 : 1;
    }
    unsigned lut[256];
    if (!gradient_lut(args, lens, first, count, length, lut)) return;
    float top_zone = radii ? (radii->ry[0] > radii->ry[1] ? radii->ry[0] : radii->ry[1]) : 0;
    float bottom_zone = radii ? (radii->ry[2] > radii->ry[3] ? radii->ry[2] : radii->ry[3]) : 0;
    for (int py = 0; py < h; py++) {
        int vy = y + py;
        if (vy < 0 || vy >= v->height) continue;
        unsigned *row = v->pixels + (size_t)vy * (size_t)v->stride;
        int corner_row = radii && ((float)py < top_zone + 1 || (float)py >= (float)h - bottom_zone - 1);
        for (int px = 0; px < w; px++) {
            int vx = x + px;
            if (vx < 0 || vx >= v->width) continue;
            float t;
            if (conic) t = 0;
            else if (radial) {
                float ex = ((float)px + 0.5f - cx) / rx, ey = ((float)py + 0.5f - cy) / ry;
                t = gsqrt(ex * ex + ey * ey);
            } else t = (((float)px + 0.5f - (float)w / 2) * dx + ((float)py + 0.5f - (float)h / 2) * dy) / length + 0.5f;
            if (repeating) t -= (float)(int)t;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            unsigned c = lut[(int)(t * 255.0f + 0.5f)], a = color_alpha(c);
            if (corner_row) a = a * (unsigned)round_cover(radii, (float)w, (float)h, px, py) / 16;
            if (a) blend_px(&row[vx], c, a);
        }
    }
}

/* ---------------------------------------------------------------- background pictures */

/* One length of background-size / -position: px, or a percentage. */
static int bg_length(const char *t, int of, int *is_pct, int *pct) {
    int used, n = number100(t, &used);
    *is_pct = 0;
    if (!used) return 0;
    if (t[used] == '%') {
        *is_pct = 1;
        *pct = n / 100;
        return (int)((long long)of * n / 10000);
    }
    int ok;
    Len l = parse_len(t, 16, &ok);
    return ok && l.unit == 'p' ? l.value : 0;
}

static void paint_bg_picture(canvas *v, int x, int y, int w, int h, const Style *s, const image *img) {
    int iw = img->width, ih = img->height;
    if (iw <= 0 || ih <= 0 || w <= 0 || h <= 0) return;
    int dw = iw, dh = ih;
    if (s->bg_size) {
        char a[48] = "", b[48] = "";
        const char *p = s->bg_size;
        value_part(&p, a, sizeof(a));
        if (value_part(&p, b, sizeof(b)) <= 0 || !strcmp(b, ",")) strcpy(b, "auto");
        if (!strcasecmp(a, "cover") || !strcasecmp(a, "contain")) {
            int cover = a[1] == 'o' && a[2] == 'v';
            long long sw = (long long)w * ih, sh = (long long)h * iw; /* compare w/iw with h/ih */
            if ((sw > sh) == cover) {
                dw = w;
                dh = (int)((long long)ih * w / iw);
            } else {
                dh = h;
                dw = (int)((long long)iw * h / ih);
            }
        } else {
            int ip, pct;
            int aw = strcasecmp(a, "auto") ? bg_length(a, w, &ip, &pct) : -1;
            int ah = strcasecmp(b, "auto") ? bg_length(b, h, &ip, &pct) : -1;
            if (aw > 0 && ah > 0) dw = aw, dh = ah;
            else if (aw > 0) dw = aw, dh = (int)((long long)ih * aw / iw);
            else if (ah > 0) dh = ah, dw = (int)((long long)iw * ah / ih);
        }
    }
    if (dw <= 0 || dh <= 0) return;
    /* position: "x y", keywords, and "right 10px top 5px" */
    int ox = 0, oy = 0;
    if (s->bg_pos) {
        char t[4][48];
        int n = 0;
        const char *p = s->bg_pos;
        while (n < 4 && value_part(&p, t[n], sizeof(t[n])) > 0 && strcmp(t[n], ",")) n++;
        int xset = 0, yset = 0;
        for (int i = 0; i < n; i++) {
            int horizontal = !strcasecmp(t[i], "left") || !strcasecmp(t[i], "right");
            int vertical = !strcasecmp(t[i], "top") || !strcasecmp(t[i], "bottom");
            int ip, pct;
            if (horizontal || vertical || !strcasecmp(t[i], "center")) {
                int far = !strcasecmp(t[i], "right") || !strcasecmp(t[i], "bottom");
                int offset = 0;
                if (i + 1 < n && !position_word(t[i + 1])) offset = bg_length(t[++i], horizontal ? w - dw : h - dh, &ip, &pct);
                if (!strcasecmp(t[i - (i > 0 && !position_word(t[i]))], "center")) {
                    if (!xset && !(n == 2 && i == 1 && !yset && 0)) {
                        if (!xset) {
                            ox = (w - dw) / 2;
                            xset = 1;
                        } else {
                            oy = (h - dh) / 2;
                            yset = 1;
                        }
                    } else {
                        oy = (h - dh) / 2;
                        yset = 1;
                    }
                } else if (horizontal) {
                    ox = far ? w - dw - offset : offset;
                    xset = 1;
                } else {
                    oy = far ? h - dh - offset : offset;
                    yset = 1;
                }
            } else {
                if (!xset) {
                    ox = bg_length(t[i], w - dw, &ip, &pct);
                    xset = 1;
                } else {
                    oy = bg_length(t[i], h - dh, &ip, &pct);
                    yset = 1;
                }
            }
        }
        if (n == 1 && xset && !yset) oy = (h - dh) / 2; /* one value: the other is centred */
        if (n == 1 && yset && !xset) ox = (w - dw) / 2;
    }
    int rx = s->bg_repeat == 0 || s->bg_repeat == 2, ry = s->bg_repeat == 0 || s->bg_repeat == 3;
    int sx = ox, sy = oy;
    if (rx) while (sx > 0) sx -= dw;
    if (ry) while (sy > 0) sy -= dh;
    int tiles = 0;
    for (int ty = sy; ty < h && tiles < 4000; ty += dh) {
        for (int tx = sx; tx < w && tiles < 4000; tx += dw) {
            paint_image(v, img, x + tx, y + ty, dw, dh);
            tiles++;
            if (!rx) break;
        }
        if (!ry) break;
    }
}

/* An element's background pictures and gradients, the last layer first. */
static void paint_backgrounds(canvas *v, int x, int y, int w, int h, Node *n, const Radii *radii) {
    const char *layers = n->style.bg_image;
    if (!layers) return;
    const char *args[8];
    int lens[8];
    int count = split_args(layers, (int)strlen(layers), args, lens, 8);
    /* (clip to the box) */
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > v->width ? v->width : x + w, y1 = y + h > v->height ? v->height : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    canvas part = {v->pixels + (size_t)y0 * (size_t)v->stride + (size_t)x0, x1 - x0, y1 - y0, v->stride};
    int first_url = -1;
    for (int i = 0; i < count; i++)
        if (memmem_simple(args[i], lens[i], "url(")) {
            first_url = i;
            break;
        }
    canvas aside = {0};
    if (radii && first_url >= 0 && n->bg_state == 1) { /* round corners: drawn aside on a copy, then masked */
        aside = canvas_new(x1 - x0, y1 - y0);
        if (aside.pixels)
            for (int py = 0; py < aside.height; py++)
                memcpy(aside.pixels + (size_t)py * (size_t)aside.stride, part.pixels + (size_t)py * (size_t)part.stride, (size_t)aside.width * 4);
    }
    canvas *into = aside.pixels ? &aside : &part;
    for (int i = count - 1; i >= 0; i--) {
        if (memmem_simple(args[i], lens[i], "gradient(")) paint_gradient(into, x - x0, y - y0, w, h, args[i], lens[i], aside.pixels ? NULL : radii);
        else if (i == first_url && n->bg_state == 1 && n->bg_img) paint_bg_picture(into, x - x0, y - y0, w, h, &n->style, (image *)n->bg_img);
    }
    if (aside.pixels) {
        for (int py = 0; py < aside.height; py++) {
            unsigned *dst = part.pixels + (size_t)py * (size_t)part.stride, *src = aside.pixels + (size_t)py * (size_t)aside.stride;
            for (int px = 0; px < aside.width; px++) {
                int cover = round_cover(radii, (float)w, (float)h, px + x0 - x, py + y0 - y);
                if (cover == 16) dst[px] = src[px];
                else if (cover) blend_px(&dst[px], src[px], 255u * (unsigned)cover / 16);
            }
        }
        free(aside.pixels);
    }
}

/* ---------------------------------------------------------------- shadows */

typedef struct {
    int w, h, blur;
    Radii r;
    unsigned char *mask;
    int mw, mh;
} ShadowMask;
static ShadowMask shadow_masks[8];
static int shadow_next;

static void box_blur(unsigned char *m, int w, int h, int r, int vertical) {
    if (r < 1) return;
    int len = vertical ? h : w, lines = vertical ? w : h;
    unsigned char *line = malloc((size_t)len);
    if (!line) return;
    for (int l = 0; l < lines; l++) {
        for (int i = 0; i < len; i++) line[i] = vertical ? m[(size_t)i * w + l] : m[(size_t)l * w + i];
        int sum = 0, span = 2 * r + 1;
        for (int i = -r; i <= r; i++) sum += i >= 0 && i < len ? line[i] : 0;
        for (int i = 0; i < len; i++) {
            unsigned char out = (unsigned char)(sum / span);
            if (vertical) m[(size_t)i * w + l] = out;
            else m[(size_t)l * w + i] = out;
            int add = i + r + 1, drop = i - r;
            sum += (add < len ? line[add] : 0) - (drop >= 0 ? line[drop] : 0);
        }
    }
    free(line);
}

static void paint_shadow(canvas *v, int x, int y, int w, int h, const Style *s, const Radii *radii) {
    int blur = s->shadow[2], spread = s->shadow[3];
    int sw = w + 2 * spread, sh = h + 2 * spread;
    if (sw <= 0 || sh <= 0) return;
    int pad = blur;
    Radii r = {{0}, {0}};
    if (radii) {
        r = *radii;
        for (int i = 0; i < 4; i++)
            if (r.rx[i] > 0) {
                r.rx[i] += (float)spread;
                r.ry[i] += (float)spread;
                if (r.rx[i] < 0) r.rx[i] = r.ry[i] = 0;
            }
    }
    int mw = sw + 2 * pad, mh = sh + 2 * pad;
    if ((long long)mw * mh > 3000000) return;
    ShadowMask *m = NULL;
    for (int i = 0; i < 8; i++)
        if (shadow_masks[i].mask && shadow_masks[i].w == sw && shadow_masks[i].h == sh && shadow_masks[i].blur == blur &&
            !memcmp(&shadow_masks[i].r, &r, sizeof(r)))
            m = &shadow_masks[i];
    if (!m) {
        m = &shadow_masks[shadow_next];
        shadow_next = (shadow_next + 1) % 8;
        free(m->mask);
        m->mask = calloc_zero((size_t)mw * (size_t)mh);
        if (!m->mask) return;
        m->w = sw;
        m->h = sh;
        m->blur = blur;
        m->r = r;
        m->mw = mw;
        m->mh = mh;
        int any = radii != NULL;
        for (int py = 0; py < sh; py++)
            for (int px = 0; px < sw; px++)
                m->mask[(size_t)(py + pad) * mw + px + pad] = (unsigned char)(any ? round_cover(&r, (float)sw, (float)sh, px, py) * 255 / 16 : 255);
        int br = blur / 2; /* three box blurs come close to the soft edge CSS asks for */
        for (int k = 0; k < 3 && br > 0; k++) {
            box_blur(m->mask, mw, mh, br / 2 + (k == 0 ? br % 2 : 0), 0);
            box_blur(m->mask, mw, mh, br / 2 + (k == 0 ? br % 2 : 0), 1);
        }
    }
    unsigned a0 = color_alpha(s->shadow_color);
    int bx = x + s->shadow[0] - spread - pad, by = y + s->shadow[1] - spread - pad;
    for (int py = 0; py < m->mh; py++) {
        int vy = by + py;
        if (vy < 0 || vy >= v->height) continue;
        unsigned *row = v->pixels + (size_t)vy * (size_t)v->stride;
        const unsigned char *mrow = m->mask + (size_t)py * m->mw;
        for (int px = 0; px < m->mw; px++) {
            int vx = bx + px;
            if (vx < 0 || vx >= v->width || !mrow[px]) continue;
            /* (not under the box itself: its background covers that, or should show through) */
            if (vx >= x && vx < x + w && vy >= y && vy < y + h && (!radii || round_cover(radii, (float)w, (float)h, vx - x, vy - y) == 16)) continue;
            blend_px(&row[vx], s->shadow_color, a0 * mrow[px] / 255);
        }
    }
}

/* Draw the page, `scroll` pixels down, into `v` (the visible part). */
static void paint_page(canvas *v, int scroll, int left) {
    fill_rect(v, 0, 0, v->width, v->height, page_bg & 0xFFFFFF);
    canvas whole = *v;
    for (int i = 0; i < item_count; i++) {
        Item *it = &items[i];
        int x = it->x + left, y = it->y - scroll;
        if (y > whole.height || y + it->h < 0 || it->w <= 0) continue;
        v = &whole;
        canvas part;
        if (it->clipped) {
            /* Draw into the clip rectangle's part of the view. */
            int x0 = it->clip[0] + left, y0 = it->clip[1] - scroll, x1 = it->clip[2] + left, y1 = it->clip[3] - scroll;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > whole.width) x1 = whole.width;
            if (y1 > whole.height) y1 = whole.height;
            if (x1 <= x0 || y1 <= y0) continue;
            part.pixels = whole.pixels + (size_t)y0 * (size_t)whole.stride + (size_t)x0;
            part.width = x1 - x0;
            part.height = y1 - y0;
            part.stride = whole.stride;
            v = &part;
            x -= x0;
            y -= y0;
        }
        paint_opacity = it->node && it->node->type == NODE_ELEMENT ? it->node->style.alpha : 255;
        if (!paint_opacity) continue;
        Radii radii;
        int round = it->node && it->node->type == NODE_ELEMENT && (it->kind == IT_RECT || it->kind == IT_BORDER || it->kind == IT_IMAGE) &&
                    radii_for(&it->node->style, it->w, it->h, &radii);
        switch (it->kind) {
        case IT_RECT:
            if (it->node && it->node->type == NODE_ELEMENT && it->node->style.has_shadow && it->node->bg_item == i)
                paint_shadow(v, x, y, it->w, it->h, &it->node->style, round ? &radii : NULL);
            if (round) paint_round(v, x, y, it->w, it->h, &radii, 0, 0, 0, 0, 0, it->color);
            else paint_fill(v, x, y, it->w, it->h, it->color);
            if (it->node && it->node->type == NODE_ELEMENT && it->node->style.bg_image)
                paint_backgrounds(v, x, y, it->w, it->h, it->node, round ? &radii : NULL);
            break;
        case IT_BORDER:
            if (round) { /* one colour all round (the first side shown) */
                unsigned c = it->border_color[0];
                for (int k = 0; k < 4; k++)
                    if (it->border[k] && it->border_color[k] != BORDER_INVISIBLE) {
                        c = it->border_color[k];
                        break;
                    }
                paint_round(v, x, y, it->w, it->h, &radii, 1, it->border[0], it->border[1], it->border[2], it->border[3], c);
                break;
            }
            if (it->border[0] && it->border_color[0] != BORDER_INVISIBLE) paint_fill(v, x, y, it->w, it->border[0], it->border_color[0]);
            if (it->border[2] && it->border_color[2] != BORDER_INVISIBLE)
                paint_fill(v, x, y + it->h - it->border[2], it->w, it->border[2], it->border_color[2]);
            if (it->border[3] && it->border_color[3] != BORDER_INVISIBLE) paint_fill(v, x, y, it->border[3], it->h, it->border_color[3]);
            if (it->border[1] && it->border_color[1] != BORDER_INVISIBLE)
                paint_fill(v, x + it->w - it->border[1], y, it->border[1], it->h, it->border_color[1]);
            break;
        case IT_TEXT:
        case IT_MARKER: {
            unsigned color = (it->color & 0xFFFFFF) | (255 - color_alpha(it->color)) << 24;
            font_draw(v, x, y + it->base, pool + it->text, it->len, it->face, it->bold, it->italic, it->size, color);
            int thick = it->size >= 24 ? 2 : 1;
            if (it->underline) paint_fill(v, x, y + it->base + (it->size + 8) / 10, it->w, thick, it->color);
            if (it->strike) paint_fill(v, x, y + it->base - it->base * 3 / 10, it->w, thick, it->color);
            break;
        }
        case IT_IMAGE: {
            Node *n = it->node;
            if (n->img_state == 1 && n->img && round) { /* round corners (a round picture): drawn aside, then masked */
                canvas aside = canvas_new(it->w, it->h);
                if (aside.pixels) {
                    for (int py = 0; py < it->h; py++) /* (what is under it, so see-through pictures blend) */
                        for (int px = 0; px < it->w; px++) {
                            int vx = x + px, vy = y + py;
                            aside.pixels[(size_t)py * (size_t)aside.stride + (size_t)px] =
                                vx >= 0 && vy >= 0 && vx < v->width && vy < v->height ? v->pixels[(size_t)vy * (size_t)v->stride + (size_t)vx] : 0xFFFFFF;
                        }
                    unsigned keep = paint_opacity;
                    paint_opacity = 255;
                    paint_image(&aside, (image *)n->img, 0, 0, it->w, it->h);
                    paint_opacity = keep;
                    for (int py = 0; py < it->h; py++) {
                        int vy = y + py;
                        if (vy < 0 || vy >= v->height) continue;
                        for (int px = 0; px < it->w; px++) {
                            int vx = x + px;
                            if (vx < 0 || vx >= v->width) continue;
                            int cover = round_cover(&radii, (float)it->w, (float)it->h, px, py);
                            if (cover) blend_px(&v->pixels[(size_t)vy * (size_t)v->stride + (size_t)vx], aside.pixels[(size_t)py * (size_t)aside.stride + (size_t)px], paint_opacity * (unsigned)cover / 16);
                        }
                    }
                    free(aside.pixels);
                } else {
                    image_draw(v, (image *)n->img, x, y, it->w, it->h);
                }
            } else if (n->img_state == 1 && n->img) {
                paint_image(v, (image *)n->img, x, y, it->w, it->h);
            } else if (it->w > 4 && it->h > 4) { /* not (yet) shown: its description, if it has one */
                char alt[80];
                if (attr(n, "alt", alt, sizeof(alt)) && alt[0] && it->h >= 14) {
                    int len = (int)strlen(alt);
                    while (len > 0 && text_w(&n->style, alt, len) > it->w - 4) len--;
                    int a, d, g;
                    font_metrics(FONT_OF(&n->style), &a, &d, &g);
                    font_draw(v, x + 2, y + 1 + a, alt, len, FONT_OF(&n->style), 0x6A7287);
                }
            }
            break;
        }
        case IT_CONTROL:
            draw_control(v, it, x, y);
            break;
        }
    }
}

/* The deepest element whose box holds page point (px, py). */
static Node *node_at(int px, int py) {
    Node *found = NULL;
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT || n == document) continue;
        if (n->style.display == D_NONE) continue;
        if (n->w > 0 && n->h > 0 && px >= n->x && px < n->x + n->w && py >= n->y && py < n->y + n->h) found = n;
    }
    return found;
}

/* The item under page point (px, py): the topmost text, picture or
   control; -1 if there is none. */
static int item_at(int px, int py) {
    for (int i = item_count - 1; i >= 0; i--) {
        Item *it = &items[i];
        if (it->kind == IT_BORDER || it->kind == IT_MARKER) continue;
        if (it->clipped && (px < it->clip[0] || px >= it->clip[2] || py < it->clip[1] || py >= it->clip[3])) continue;
        int slack = it->kind == IT_TEXT ? it->size / 8 + 1 : 0; /* half a space: the gaps between words */
        if (px >= it->x - slack && px < it->x + it->w + slack && py >= it->y - slack && py < it->y + it->h + slack) {
            if (it->kind == IT_RECT && !it->node) continue;
            return i;
        }
    }
    return -1;
}

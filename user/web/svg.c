/* web/svg.c - drawing SVG pictures (logos, icons, simple drawings).
   Part of the browser.

   Handled: <svg> with viewBox (and preserveAspectRatio's default, "meet"),
   <g>, <path> (all commands, arcs included), <rect> (with rx), <circle>,
   <ellipse>, <line>, <polyline>, <polygon>, <use>, <symbol>, transforms,
   fill and stroke (colours, currentColor, opacity, nonzero / evenodd),
   gradients approximated by their first colour. Not drawn: text, filters,
   masks, clip paths, patterns. */

typedef struct {
    double a, b, c, d, e, f; /* x' = a x + c y + e;  y' = b x + d y + f */
} Mat;

static Mat mat_mul(Mat p, Mat q) { /* p then q's space: p * q */
    Mat r = {p.a * q.a + p.c * q.b, p.b * q.a + p.d * q.b, p.a * q.c + p.c * q.d, p.b * q.c + p.d * q.d,
             p.a * q.e + p.c * q.f + p.e, p.b * q.e + p.d * q.f + p.f};
    return r;
}

typedef struct {
    double x, y;
} Pt;

/* Outlines: points, and where each subpath starts (and if it is closed). */
typedef struct {
    Pt *p;
    int n, cap;
    int *start;
    unsigned char *closed;
    int nsub, subcap;
} Outline;

static void outline_point(Outline *o, double x, double y) {
    if (o->n == o->cap) {
        int cap = o->cap ? o->cap * 2 : 64;
        Pt *p = realloc(o->p, sizeof(Pt) * (size_t)cap);
        if (!p) return;
        o->p = p;
        o->cap = cap;
    }
    o->p[o->n].x = x;
    o->p[o->n].y = y;
    o->n++;
}

static void outline_begin(Outline *o) {
    if (o->nsub == o->subcap) {
        int cap = o->subcap ? o->subcap * 2 : 8;
        int *s = realloc(o->start, sizeof(int) * (size_t)cap);
        unsigned char *c = s ? realloc(o->closed, (size_t)cap) : NULL;
        if (s) o->start = s;
        if (!s || !c) return;
        o->closed = c;
        o->subcap = cap;
    }
    o->start[o->nsub] = o->n;
    o->closed[o->nsub] = 0;
    o->nsub++;
}

static void outline_free(Outline *o) {
    free(o->p);
    free(o->start);
    free(o->closed);
    memset(o, 0, sizeof(*o));
}

static int sub_end(const Outline *o, int s) { return s + 1 < o->nsub ? o->start[s + 1] : o->n; }

/* ---------------------------------------------------------------- rasterising */

typedef struct {
    unsigned *px;
    int w, h;
    float *cov; /* one row of coverage */
} Raster;

typedef struct {
    double x, dir;
} Cross;

static void blend(unsigned *d, unsigned color, double alpha) {
    if (alpha <= 0) return;
    if (alpha > 1) alpha = 1;
    double da = (*d >> 24) / 255.0;
    double oa = alpha + da * (1 - alpha);
    if (oa <= 0) return;
    unsigned out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        double s = (color >> shift) & 255, t = (*d >> shift) & 255;
        double v = (s * alpha + t * da * (1 - alpha)) / oa;
        out |= (unsigned)(v + 0.5) << shift;
    }
    *d = out | (unsigned)(oa * 255 + 0.5) << 24;
}

/* Fill the outline's (closed) subpaths. */
static void fill_outline(Raster *R, const Outline *o, unsigned color, double alpha, int evenodd) {
    if (!o->n || alpha <= 0) return;
    double ymin = 1e30, ymax = -1e30;
    for (int i = 0; i < o->n; i++) {
        if (o->p[i].y < ymin) ymin = o->p[i].y;
        if (o->p[i].y > ymax) ymax = o->p[i].y;
    }
    int y0 = (int)js_floor(ymin), y1 = (int)js_ceil(ymax);
    if (y0 < 0) y0 = 0;
    if (y1 > R->h) y1 = R->h;
    Cross *cross = malloc(sizeof(Cross) * (size_t)(o->n + 4));
    if (!cross) return;
    const int S = 4;
    for (int y = y0; y < y1; y++) {
        memset(R->cov, 0, sizeof(float) * (size_t)R->w);
        int any = 0;
        for (int s = 0; s < S; s++) {
            double sy = y + (s + 0.5) / S;
            int nc = 0;
            for (int sp = 0; sp < o->nsub; sp++) {
                int a = o->start[sp], b = sub_end(o, sp);
                for (int i = a; i < b; i++) {
                    Pt p = o->p[i], q = o->p[i + 1 < b ? i + 1 : a]; /* closed implicitly */
                    if (p.y == q.y) continue;
                    double lo = p.y < q.y ? p.y : q.y, hi = p.y < q.y ? q.y : p.y;
                    if (sy < lo || sy >= hi) continue;
                    cross[nc].x = p.x + (sy - p.y) * (q.x - p.x) / (q.y - p.y);
                    cross[nc].dir = q.y > p.y ? 1 : -1;
                    nc++;
                }
            }
            /* sort by x (insertion: few crossings per line) */
            for (int i = 1; i < nc; i++) {
                Cross c = cross[i];
                int j = i;
                while (j > 0 && cross[j - 1].x > c.x) {
                    cross[j] = cross[j - 1];
                    j--;
                }
                cross[j] = c;
            }
            int wind = 0;
            for (int i = 0; i + 1 < nc; i++) {
                wind += (int)cross[i].dir;
                int inside = evenodd ? (i + 1) % 2 : wind != 0;
                if (!inside) continue;
                double xa = cross[i].x, xb = cross[i + 1].x;
                if (xa < 0) xa = 0;
                if (xb > R->w) xb = R->w;
                if (xb <= xa) continue;
                int ia = (int)xa, ib = (int)xb;
                if (ia == ib) {
                    R->cov[ia] += (float)((xb - xa) / S);
                } else {
                    R->cov[ia] += (float)((ia + 1 - xa) / S);
                    for (int k = ia + 1; k < ib; k++) R->cov[k] += 1.0f / S;
                    if (ib < R->w) R->cov[ib] += (float)((xb - ib) / S);
                }
                any = 1;
            }
        }
        if (!any) continue;
        unsigned *row = R->px + (size_t)y * (size_t)R->w;
        for (int x = 0; x < R->w; x++) {
            if (R->cov[x] > 0.001f) blend(&row[x], color, R->cov[x] * alpha);
        }
    }
    free(cross);
}

/* Add a convex polygon, always wound the same way (so a nonzero fill of
   many of them is their union). */
static void add_convex(Outline *o, const Pt *pts, int n) {
    double area = 0;
    for (int i = 0; i < n; i++) {
        Pt p = pts[i], q = pts[(i + 1) % n];
        area += p.x * q.y - q.x * p.y;
    }
    if (area == 0) return;
    outline_begin(o);
    for (int i = 0; i < n; i++) {
        Pt p = area > 0 ? pts[i] : pts[n - 1 - i];
        outline_point(o, p.x, p.y);
    }
}

/* The outline of a stroke: a quad per segment and a disc at each joint. */
static void stroke_outline(const Outline *path, double width, Outline *out) {
    double hw = width / 2;
    if (hw <= 0) return;
    for (int sp = 0; sp < path->nsub; sp++) {
        int a = path->start[sp], b = sub_end(path, sp);
        int segs = path->closed[sp] ? b - a : b - a - 1;
        for (int k = 0; k < segs; k++) {
            Pt p = path->p[a + k], q = path->p[a + (k + 1) % (b - a)];
            double dx = q.x - p.x, dy = q.y - p.y, len = js_sqrt(dx * dx + dy * dy);
            if (len < 1e-9) continue;
            double nx = -dy / len * hw, ny = dx / len * hw;
            Pt quad[4] = {{p.x + nx, p.y + ny}, {q.x + nx, q.y + ny}, {q.x - nx, q.y - ny}, {p.x - nx, p.y - ny}};
            add_convex(out, quad, 4);
        }
        if (hw >= 0.75) { /* round joins (and caps) */
            for (int k = a; k < b; k++) {
                Pt disc[8];
                for (int i = 0; i < 8; i++) {
                    double t = i * PI / 4;
                    disc[i].x = path->p[k].x + js_cos(t) * hw;
                    disc[i].y = path->p[k].y + js_sin(t) * hw;
                }
                add_convex(out, disc, 8);
            }
        }
    }
}

/* ---------------------------------------------------------------- paths */

typedef struct {
    const char *s;
    int i;
} Scan;

static void scan_skip(Scan *S) {
    while (S->s[S->i] && (is_space(S->s[S->i]) || S->s[S->i] == ',')) S->i++;
}

static int scan_number(Scan *S, double *out) {
    scan_skip(S);
    const char *p = S->s + S->i;
    if (!(*p == '-' || *p == '+' || *p == '.' || (*p >= '0' && *p <= '9'))) return 0;
    /* a number ends at a second '.' too ("0.5.5" is 0.5 and .5) */
    int k = 0, dot = 0, exp = 0;
    if (p[k] == '-' || p[k] == '+') k++;
    while (p[k]) {
        if (p[k] >= '0' && p[k] <= '9') k++;
        else if (p[k] == '.' && !dot && !exp) dot = 1, k++;
        else if ((p[k] == 'e' || p[k] == 'E') && !exp && (p[k + 1] == '-' || p[k + 1] == '+' || (p[k + 1] >= '0' && p[k + 1] <= '9'))) {
            exp = 1;
            k++;
            if (p[k] == '-' || p[k] == '+') k++;
        } else break;
    }
    if (k == 0 || (k == 1 && (p[0] == '-' || p[0] == '+' || p[0] == '.'))) return 0;
    *out = parse_number(p, k, NULL, 1);
    S->i += k;
    return 1;
}

static int scan_flag(Scan *S, int *out) {
    scan_skip(S);
    char c = S->s[S->i];
    if (c != '0' && c != '1') return 0;
    *out = c == '1';
    S->i++;
    return 1;
}

static Pt mat_apply(Mat m, double x, double y) {
    Pt p = {m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f};
    return p;
}

static double mat_scale(Mat m) {
    double sx = js_sqrt(m.a * m.a + m.b * m.b), sy = js_sqrt(m.c * m.c + m.d * m.d);
    return (sx + sy) / 2;
}

static void cubic(Outline *o, Mat m, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3) {
    Pt p0 = mat_apply(m, x0, y0), p1 = mat_apply(m, x1, y1), p2 = mat_apply(m, x2, y2), p3 = mat_apply(m, x3, y3);
    double len = js_sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y)) +
                 js_sqrt((p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y)) +
                 js_sqrt((p3.x - p2.x) * (p3.x - p2.x) + (p3.y - p2.y) * (p3.y - p2.y));
    int n = (int)(len / 3);
    if (n < 4) n = 4;
    if (n > 64) n = 64;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double x = u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x;
        double y = u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y;
        outline_point(o, x, y);
    }
}

/* An elliptical arc from (x1,y1) to (x2,y2), SVG's endpoint form (SVG 1.1 F.6.5). */
static void arc(Outline *o, Mat m, double x1, double y1, double rx, double ry, double phi_deg, int large, int sweep, double x2, double y2) {
    if (x1 == x2 && y1 == y2) return;
    if (rx == 0 || ry == 0) {
        Pt p = mat_apply(m, x2, y2);
        outline_point(o, p.x, p.y);
        return;
    }
    rx = js_fabs(rx);
    ry = js_fabs(ry);
    double phi = phi_deg * PI / 180, cs = js_cos(phi), sn = js_sin(phi);
    double dx2 = (x1 - x2) / 2, dy2 = (y1 - y2) / 2;
    double x1p = cs * dx2 + sn * dy2, y1p = -sn * dx2 + cs * dy2;
    double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1) {
        double r = js_sqrt(lambda);
        rx *= r;
        ry *= r;
    }
    double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double coef = den > 0 && num > 0 ? js_sqrt(num / den) : 0;
    if (large == sweep) coef = -coef;
    double cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    double cx = cs * cxp - sn * cyp + (x1 + x2) / 2, cy = sn * cxp + cs * cyp + (y1 + y2) / 2;
    double t1 = js_atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double t2 = js_atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    double dt = t2 - t1;
    if (sweep && dt < 0) dt += 2 * PI;
    if (!sweep && dt > 0) dt -= 2 * PI;
    int n = (int)(js_fabs(dt) * (rx > ry ? rx : ry) * mat_scale(m) / 3);
    if (n < 4) n = 4;
    if (n > 96) n = 96;
    for (int i = 1; i <= n; i++) {
        double t = t1 + dt * i / n;
        double x = cx + rx * js_cos(t) * cs - ry * js_sin(t) * sn;
        double y = cy + rx * js_cos(t) * sn + ry * js_sin(t) * cs;
        Pt p = mat_apply(m, x, y);
        outline_point(o, p.x, p.y);
    }
}

static void path_outline(const char *d, Mat m, Outline *o) {
    Scan S = {d, 0};
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; /* current, subpath start, last control */
    char cmd = 0, last = 0;
    int open = 0;
    for (int guard = 0; guard < 200000; guard++) {
        scan_skip(&S);
        char c = S.s[S.i];
        if (!c) break;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            cmd = c;
            S.i++;
        } else if (!cmd) {
            break;
        }
        int rel = cmd >= 'a';
        char C = (char)(cmd & ~32);
        double v[7];
        int want = C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6 : C == 'S' || C == 'Q' ? 4 : C == 'A' ? 7 : 0;
        if (C == 'Z') {
            if (open && o->nsub) o->closed[o->nsub - 1] = 1;
            cx = sx;
            cy = sy;
            open = 0;
            last = 'Z';
            cmd = 0; /* a following number would be an error */
            continue;
        }
        int ok = 1;
        for (int k = 0; k < want && ok; k++) {
            if (C == 'A' && (k == 3 || k == 4)) {
                int f;
                ok = scan_flag(&S, &f);
                v[k] = f;
            } else {
                ok = scan_number(&S, &v[k]);
            }
        }
        if (!ok) break;
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (C) {
        case 'M': {
            cx = v[0] + ox;
            cy = v[1] + oy;
            sx = cx;
            sy = cy;
            outline_begin(o);
            Pt p = mat_apply(m, cx, cy);
            outline_point(o, p.x, p.y);
            open = 1;
            cmd = rel ? 'l' : 'L'; /* more pairs are lines */
            break;
        }
        case 'L': case 'H': case 'V': {
            if (!open) {
                outline_begin(o);
                Pt p = mat_apply(m, cx, cy);
                outline_point(o, p.x, p.y);
                open = 1;
            }
            if (C == 'L') cx = v[0] + ox, cy = v[1] + oy;
            else if (C == 'H') cx = v[0] + ox;
            else cy = v[0] + oy;
            Pt p = mat_apply(m, cx, cy);
            outline_point(o, p.x, p.y);
            break;
        }
        case 'C': case 'S': case 'Q': case 'T': {
            if (!open) {
                outline_begin(o);
                Pt p = mat_apply(m, cx, cy);
                outline_point(o, p.x, p.y);
                open = 1;
            }
            double x1, y1, x2, y2, x, y;
            char L = (char)(last & ~32);
            if (C == 'C') {
                x1 = v[0] + ox, y1 = v[1] + oy, x2 = v[2] + ox, y2 = v[3] + oy, x = v[4] + ox, y = v[5] + oy;
            } else if (C == 'S') {
                x1 = L == 'C' || L == 'S' ? 2 * cx - lcx : cx;
                y1 = L == 'C' || L == 'S' ? 2 * cy - lcy : cy;
                x2 = v[0] + ox, y2 = v[1] + oy, x = v[2] + ox, y = v[3] + oy;
            } else {
                double qx, qy;
                if (C == 'Q') qx = v[0] + ox, qy = v[1] + oy, x = v[2] + ox, y = v[3] + oy;
                else {
                    qx = L == 'Q' || L == 'T' ? 2 * cx - lcx : cx;
                    qy = L == 'Q' || L == 'T' ? 2 * cy - lcy : cy;
                    x = v[0] + ox, y = v[1] + oy;
                }
                /* a quadratic as a cubic */
                x1 = cx + 2.0 / 3 * (qx - cx), y1 = cy + 2.0 / 3 * (qy - cy);
                x2 = x + 2.0 / 3 * (qx - x), y2 = y + 2.0 / 3 * (qy - y);
                cubic(o, m, cx, cy, x1, y1, x2, y2, x, y);
                lcx = qx;
                lcy = qy;
                cx = x;
                cy = y;
                break;
            }
            cubic(o, m, cx, cy, x1, y1, x2, y2, x, y);
            lcx = x2;
            lcy = y2;
            cx = x;
            cy = y;
            break;
        }
        case 'A': {
            if (!open) {
                outline_begin(o);
                Pt p = mat_apply(m, cx, cy);
                outline_point(o, p.x, p.y);
                open = 1;
            }
            double x = v[5] + ox, y = v[6] + oy;
            arc(o, m, cx, cy, v[0], v[1], v[2], (int)v[3], (int)v[4], x, y);
            cx = x;
            cy = y;
            break;
        }
        default:
            guard = 200000;
        }
        last = C == 'M' ? 'M' : cmd;
        if (C == 'M') last = 'M';
        else last = C;
    }
}

static void ellipse_outline(Outline *o, Mat m, double cx, double cy, double rx, double ry) {
    if (rx <= 0 || ry <= 0) return;
    int n = (int)((rx > ry ? rx : ry) * mat_scale(m));
    if (n < 12) n = 12;
    if (n > 96) n = 96;
    outline_begin(o);
    for (int i = 0; i < n; i++) {
        double t = 2 * PI * i / n;
        Pt p = mat_apply(m, cx + rx * js_cos(t), cy + ry * js_sin(t));
        outline_point(o, p.x, p.y);
    }
    o->closed[o->nsub - 1] = 1;
}

/* ---------------------------------------------------------------- the document */

typedef struct {
    unsigned fill, stroke, current;
    int fill_none, stroke_none, evenodd;
    double stroke_width, opacity, fill_opacity, stroke_opacity;
} SvgStyle;

/* A presentation property: from style="..." first, then the attribute. */
static int svg_prop(Node *n, const char *name, char *out, int max) {
    char style[1024];
    if (attr(n, "style", style, sizeof(style)) && style_lookup(style, name, out, max)) return 1;
    return attr(n, name, out, max);
}

static double svg_length(const char *s, double fallback) {
    while (is_space(*s)) s++;
    int used;
    double v = parse_number(s, (int)strlen(s), &used, 1);
    if (!used || is_nan(v)) return fallback;
    return v;
}

static double svg_attr_num(Node *n, const char *name, double fallback) {
    char v[64];
    if (!attr(n, name, v, sizeof(v))) return fallback;
    return svg_length(v, fallback);
}

static Node *svg_find_id(Node *root, const char *id) {
    char v[128];
    for (Node *d = root; d; d = next_in_tree(d, root)) {
        if (d->type == NODE_ELEMENT && attr(d, "id", v, sizeof(v)) && !strcmp(v, id)) return d;
    }
    return NULL;
}

/* A paint: "none", a colour, currentColor, or url(#gradient). */
static void svg_paint(Node *root, const char *v, unsigned current, unsigned *color, int *none) {
    while (is_space(*v)) v++;
    if (!strncasecmp(v, "none", 4) || !strncasecmp(v, "transparent", 11)) {
        *none = 1;
        return;
    }
    if (!strncasecmp(v, "currentColor", 12)) {
        *none = 0;
        *color = current;
        return;
    }
    if (!strncasecmp(v, "url(", 4)) {
        char id[128];
        int k = 0;
        const char *p = strchr(v, '#');
        if (p) {
            p++;
            while (*p && *p != ')' && *p != '"' && *p != '\'' && k < 127) id[k++] = *p++;
        }
        id[k] = 0;
        Node *g = k ? svg_find_id(root, id) : NULL;
        /* a gradient: its first stop's colour (following href to another gradient) */
        for (int hops = 0; g && hops < 4; hops++) {
            for (Node *s = g; s; s = next_in_tree(s, g)) {
                char c[64];
                if (s->type == NODE_ELEMENT && is(s->tag, "stop") && svg_prop(s, "stop-color", c, sizeof(c))) {
                    unsigned col;
                    if (parse_color(c, &col) == 1) {
                        *color = col;
                        *none = 0;
                        return;
                    }
                }
            }
            char href[128];
            if (!attr(g, "href", href, sizeof(href)) && !attr(g, "xlink:href", href, sizeof(href))) break;
            g = href[0] == '#' ? svg_find_id(root, href + 1) : NULL;
        }
        *none = 0; /* unknown paint server: keep the colour we had */
        return;
    }
    unsigned c;
    int got = parse_color(v, &c);
    if (got == 1) {
        *color = c;
        *none = 0;
    } else if (got == 2) {
        *none = 1;
    }
}

static Mat parse_transform(const char *t) {
    Mat m = {1, 0, 0, 1, 0, 0};
    const char *p = t;
    for (int guard = 0; guard < 32 && *p; guard++) {
        while (*p && (is_space(*p) || *p == ',')) p++;
        if (!*p) break;
        char name[16];
        int k = 0;
        while (*p && is_alpha(*p) && k < 15) name[k++] = *p++;
        name[k] = 0;
        while (is_space(*p)) p++;
        if (*p != '(') break;
        p++;
        double v[6] = {0, 0, 0, 0, 0, 0};
        int n = 0;
        Scan S = {p, 0};
        while (n < 6 && scan_number(&S, &v[n])) n++;
        p += S.i;
        while (*p && *p != ')') p++;
        if (*p) p++;
        Mat t2 = {1, 0, 0, 1, 0, 0};
        if (!strcmp(name, "matrix") && n == 6) {
            Mat mm = {v[0], v[1], v[2], v[3], v[4], v[5]};
            t2 = mm;
        } else if (!strcmp(name, "translate")) {
            t2.e = v[0];
            t2.f = n > 1 ? v[1] : 0;
        } else if (!strcmp(name, "scale")) {
            t2.a = v[0];
            t2.d = n > 1 ? v[1] : v[0];
        } else if (!strcmp(name, "rotate")) {
            double r = v[0] * PI / 180, c = js_cos(r), s = js_sin(r);
            Mat rot = {c, s, -s, c, 0, 0};
            if (n >= 3) {
                Mat a = {1, 0, 0, 1, v[1], v[2]}, b = {1, 0, 0, 1, -v[1], -v[2]};
                t2 = mat_mul(mat_mul(a, rot), b);
            } else {
                t2 = rot;
            }
        } else if (!strcmp(name, "skewX")) {
            t2.c = js_tan(v[0] * PI / 180);
        } else if (!strcmp(name, "skewY")) {
            t2.b = js_tan(v[0] * PI / 180);
        }
        m = mat_mul(m, t2);
    }
    return m;
}

static void svg_element(Raster *R, Node *root, Node *n, Mat m, SvgStyle st, int depth);

static void svg_children(Raster *R, Node *root, Node *n, Mat m, SvgStyle st, int depth) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type == NODE_ELEMENT) svg_element(R, root, c, m, st, depth + 1);
    }
}

static void draw_shape(Raster *R, Outline *o, SvgStyle *st, Mat m) {
    if (!st->fill_none) fill_outline(R, o, st->fill, st->opacity * st->fill_opacity, st->evenodd);
    if (!st->stroke_none && st->stroke_width > 0) {
        Outline s;
        memset(&s, 0, sizeof(s));
        double w = st->stroke_width * mat_scale(m);
        if (w < 0.6) w = 0.6; /* hairlines stay visible */
        stroke_outline(o, w, &s);
        fill_outline(R, &s, st->stroke, st->opacity * st->stroke_opacity, 0);
        outline_free(&s);
    }
}

static void svg_element(Raster *R, Node *root, Node *n, Mat m, SvgStyle st, int depth) {
    if (depth > 40) return;
    const char *t = n->tag;
    if (is(t, "defs") || is(t, "symbol") || is(t, "clippath") || is(t, "mask") || is(t, "title") || is(t, "desc") ||
        is(t, "style") || is(t, "script") || is(t, "lineargradient") || is(t, "radialgradient") || is(t, "pattern") ||
        is(t, "filter") || is(t, "metadata") || is(t, "text") || is(t, "foreignobject"))
        return;
    char v[256];
    if (svg_prop(n, "display", v, sizeof(v)) && !strncasecmp(v, "none", 4)) return;
    if (svg_prop(n, "visibility", v, sizeof(v)) && !strncasecmp(v, "hidden", 6)) return;
    /* inherited properties */
    if (svg_prop(n, "color", v, sizeof(v))) {
        unsigned c;
        if (parse_color(v, &c) == 1) st.current = c;
    }
    if (svg_prop(n, "fill", v, sizeof(v))) svg_paint(root, v, st.current, &st.fill, &st.fill_none);
    if (svg_prop(n, "stroke", v, sizeof(v))) svg_paint(root, v, st.current, &st.stroke, &st.stroke_none);
    if (svg_prop(n, "stroke-width", v, sizeof(v))) st.stroke_width = svg_length(v, st.stroke_width);
    if (svg_prop(n, "fill-rule", v, sizeof(v))) st.evenodd = !strncasecmp(v, "evenodd", 7);
    if (svg_prop(n, "fill-opacity", v, sizeof(v))) st.fill_opacity = svg_length(v, 1);
    if (svg_prop(n, "stroke-opacity", v, sizeof(v))) st.stroke_opacity = svg_length(v, 1);
    if (svg_prop(n, "opacity", v, sizeof(v))) st.opacity *= svg_length(v, 1);
    if (attr(n, "transform", v, sizeof(v))) m = mat_mul(m, parse_transform(v));

    Outline o;
    memset(&o, 0, sizeof(o));
    if (is(t, "g") || is(t, "a") || is(t, "switch")) {
        svg_children(R, root, n, m, st, depth);
    } else if (is(t, "svg") && depth > 0) { /* a nested viewport: placed at x, y */
        Mat at = {1, 0, 0, 1, svg_attr_num(n, "x", 0), svg_attr_num(n, "y", 0)};
        svg_children(R, root, n, mat_mul(m, at), st, depth);
    } else if (is(t, "use")) {
        char href[128];
        if (attr(n, "href", href, sizeof(href)) || attr(n, "xlink:href", href, sizeof(href))) {
            Node *ref = href[0] == '#' ? svg_find_id(root, href + 1) : NULL;
            if (ref && ref != n) {
                Mat at = {1, 0, 0, 1, svg_attr_num(n, "x", 0), svg_attr_num(n, "y", 0)};
                Mat um = mat_mul(m, at);
                if (is(ref->tag, "symbol")) {
                    /* a symbol: its content, scaled from its viewBox to the use's size */
                    char vb[128];
                    double w = svg_attr_num(n, "width", 0), h = svg_attr_num(n, "height", 0);
                    if (attr(ref, "viewbox", vb, sizeof(vb)) && w > 0 && h > 0) {
                        Scan S = {vb, 0};
                        double box[4];
                        int k = 0;
                        while (k < 4 && scan_number(&S, &box[k])) k++;
                        if (k == 4 && box[2] > 0 && box[3] > 0) {
                            double s = w / box[2] < h / box[3] ? w / box[2] : h / box[3];
                            Mat vm = {s, 0, 0, s, -box[0] * s + (w - box[2] * s) / 2, -box[1] * s + (h - box[3] * s) / 2};
                            um = mat_mul(um, vm);
                        }
                    }
                    svg_children(R, root, ref, um, st, depth + 1);
                } else {
                    svg_element(R, root, ref, um, st, depth + 1);
                }
            }
        }
    } else if (is(t, "path")) {
        if (attr(n, "d", v, sizeof(v)) || 1) {
            /* path data can be long: read it whole */
            int need = n->attrs ? (int)strlen(n->attrs) + 1 : 1;
            char *d = malloc((size_t)need);
            if (d && attr(n, "d", d, need)) path_outline(d, m, &o);
            free(d);
        }
        draw_shape(R, &o, &st, m);
    } else if (is(t, "rect")) {
        double x = svg_attr_num(n, "x", 0), y = svg_attr_num(n, "y", 0), w = svg_attr_num(n, "width", 0), h = svg_attr_num(n, "height", 0);
        double rx = svg_attr_num(n, "rx", -1), ry = svg_attr_num(n, "ry", -1);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx < 0) rx = ry = 0;
        if (rx > w / 2) rx = w / 2;
        if (ry > h / 2) ry = h / 2;
        if (w > 0 && h > 0) {
            outline_begin(&o);
            if (rx <= 0 || ry <= 0) {
                Pt c[4] = {mat_apply(m, x, y), mat_apply(m, x + w, y), mat_apply(m, x + w, y + h), mat_apply(m, x, y + h)};
                for (int i = 0; i < 4; i++) outline_point(&o, c[i].x, c[i].y);
            } else { /* rounded corners: quarter ellipses */
                double cxs[4] = {x + w - rx, x + w - rx, x + rx, x + rx}, cys[4] = {y + ry, y + h - ry, y + h - ry, y + ry};
                for (int corner = 0; corner < 4; corner++) {
                    for (int i = 0; i <= 6; i++) {
                        double a = (-PI / 2) + corner * PI / 2 + i * (PI / 2) / 6;
                        Pt p = mat_apply(m, cxs[corner] + rx * js_cos(a), cys[corner] + ry * js_sin(a));
                        outline_point(&o, p.x, p.y);
                    }
                }
            }
            o.closed[o.nsub - 1] = 1;
        }
        draw_shape(R, &o, &st, m);
    } else if (is(t, "circle")) {
        double r = svg_attr_num(n, "r", 0);
        ellipse_outline(&o, m, svg_attr_num(n, "cx", 0), svg_attr_num(n, "cy", 0), r, r);
        draw_shape(R, &o, &st, m);
    } else if (is(t, "ellipse")) {
        ellipse_outline(&o, m, svg_attr_num(n, "cx", 0), svg_attr_num(n, "cy", 0), svg_attr_num(n, "rx", 0), svg_attr_num(n, "ry", 0));
        draw_shape(R, &o, &st, m);
    } else if (is(t, "line")) {
        outline_begin(&o);
        Pt a = mat_apply(m, svg_attr_num(n, "x1", 0), svg_attr_num(n, "y1", 0)), b = mat_apply(m, svg_attr_num(n, "x2", 0), svg_attr_num(n, "y2", 0));
        outline_point(&o, a.x, a.y);
        outline_point(&o, b.x, b.y);
        st.fill_none = 1; /* lines are only stroked */
        draw_shape(R, &o, &st, m);
    } else if (is(t, "polyline") || is(t, "polygon")) {
        int need = n->attrs ? (int)strlen(n->attrs) + 1 : 1;
        char *pts = malloc((size_t)need);
        if (pts && attr(n, "points", pts, need)) {
            Scan S = {pts, 0};
            double x, y;
            outline_begin(&o);
            while (scan_number(&S, &x) && scan_number(&S, &y)) {
                Pt p = mat_apply(m, x, y);
                outline_point(&o, p.x, p.y);
            }
            if (is(t, "polygon")) o.closed[o.nsub - 1] = 1;
        }
        free(pts);
        draw_shape(R, &o, &st, m);
    }
    outline_free(&o);
}

/* The size an <svg> asks for: width / height, else its viewBox, else 300 x 150. */
static void svg_natural_size(Node *svg, int *w, int *h) {
    char v[128];
    double vb[4] = {0, 0, 0, 0};
    int have_vb = 0;
    if (attr(svg, "viewbox", v, sizeof(v))) {
        Scan S = {v, 0};
        int k = 0;
        while (k < 4 && scan_number(&S, &vb[k])) k++;
        have_vb = k == 4 && vb[2] > 0 && vb[3] > 0;
    }
    double ww = -1, hh = -1;
    if (attr(svg, "width", v, sizeof(v)) && !strchr(v, '%')) ww = svg_length(v, -1);
    if (attr(svg, "height", v, sizeof(v)) && !strchr(v, '%')) hh = svg_length(v, -1);
    if (ww <= 0 && hh <= 0) {
        ww = have_vb ? vb[2] : 300;
        hh = have_vb ? vb[3] : 150;
    } else if (ww <= 0) {
        ww = have_vb ? hh * vb[2] / vb[3] : hh;
    } else if (hh <= 0) {
        hh = have_vb ? ww * vb[3] / vb[2] : ww;
    }
    *w = (int)(ww + 0.5);
    *h = (int)(hh + 0.5);
    if (*w > 4096) *w = 4096;
    if (*h > 4096) *h = 4096;
}

/* Draw an <svg> element into a new w x h picture. 0 on success. */
static int svg_render_node(Node *svg, int w, int h, unsigned color, image *out) {
    if (w <= 0 || h <= 0) svg_natural_size(svg, &w, &h);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
    Raster R;
    R.w = w;
    R.h = h;
    R.px = malloc(sizeof(unsigned) * (size_t)w * (size_t)h);
    R.cov = malloc(sizeof(float) * (size_t)w);
    if (!R.px || !R.cov) {
        free(R.px);
        free(R.cov);
        return -1;
    }
    memset(R.px, 0, sizeof(unsigned) * (size_t)w * (size_t)h);
    /* the viewBox, fitted ("meet") and centred */
    Mat m = {1, 0, 0, 1, 0, 0};
    char v[128];
    int nw, nh;
    svg_natural_size(svg, &nw, &nh);
    if (attr(svg, "viewbox", v, sizeof(v))) {
        Scan S = {v, 0};
        double vb[4];
        int k = 0;
        while (k < 4 && scan_number(&S, &vb[k])) k++;
        if (k == 4 && vb[2] > 0 && vb[3] > 0) {
            double s = (double)w / vb[2] < (double)h / vb[3] ? (double)w / vb[2] : (double)h / vb[3];
            char par[64] = "";
            attr(svg, "preserveaspectratio", par, sizeof(par));
            double sx = s, sy = s;
            if (!strncasecmp(par, "none", 4)) sx = (double)w / vb[2], sy = (double)h / vb[3];
            Mat vm = {sx, 0, 0, sy, -vb[0] * sx + (w - vb[2] * sx) / 2, -vb[1] * sy + (h - vb[3] * sy) / 2};
            m = vm;
        }
    } else if (nw > 0 && nh > 0) {
        Mat sm = {(double)w / nw, 0, 0, (double)h / nh, 0, 0};
        m = sm;
    }
    SvgStyle st;
    memset(&st, 0, sizeof(st));
    st.fill = 0x000000;
    st.stroke_none = 1;
    st.stroke_width = 1;
    st.opacity = st.fill_opacity = st.stroke_opacity = 1;
    st.current = color;
    /* the root's own presentation attributes, then its content */
    if (svg_prop(svg, "fill", v, sizeof(v))) svg_paint(svg, v, st.current, &st.fill, &st.fill_none);
    if (svg_prop(svg, "stroke", v, sizeof(v))) svg_paint(svg, v, st.current, &st.stroke, &st.stroke_none);
    if (svg_prop(svg, "stroke-width", v, sizeof(v))) st.stroke_width = svg_length(v, 1);
    if (svg_prop(svg, "fill-rule", v, sizeof(v))) st.evenodd = !strncasecmp(v, "evenodd", 7);
    svg_children(&R, svg, svg, m, st, 0);
    free(R.cov);
    out->width = w;
    out->height = h;
    out->pixels = R.px;
    return 0;
}

/* Is this file an SVG picture? */
static int looks_like_svg(const char *data, size_t len, const char *type) {
    if (type && strstr(type, "svg")) return 1;
    size_t n = len < 1024 ? len : 1024;
    for (size_t i = 0; i + 4 < n; i++) {
        if (data[i] == '<' && (data[i + 1] == 's' || data[i + 1] == 'S') && !strncasecmp(data + i, "<svg", 4)) return 1;
    }
    return 0;
}

/* Draw an SVG file's text at w x h (0: its own size). */
static int svg_render_text(const char *text, int len, int w, int h, unsigned color, image *out, int *natural_w, int *natural_h) {
    Node *frag = parse_fragment(text, len);
    if (!frag) return -1;
    Node *svg = NULL;
    for (Node *d = frag; d && !svg; d = next_in_tree(d, frag)) {
        if (d->type == NODE_ELEMENT && is(d->tag, "svg")) svg = d;
    }
    int r = -1;
    if (svg) {
        svg_natural_size(svg, natural_w, natural_h);
        r = svg_render_node(svg, w, h, color, out);
    }
    free_tree(frag);
    return r;
}

/* web/css.c - stylesheets: parsing CSS, matching selectors against the
   document tree, and working out each element's style (the cascade).
   Part of the browser (included by programs/browser.c). */

enum { D_INLINE, D_BLOCK, D_LIST_ITEM, D_NONE, D_INLINE_BLOCK, D_FLEX, D_INLINE_FLEX, D_GRID, D_TABLE, D_TABLE_ROW,
       D_TABLE_CELL, D_TABLE_GROUP, D_TABLE_CAPTION, D_INLINE_GRID };
enum { P_STATIC, P_RELATIVE, P_ABSOLUTE, P_FIXED };
enum { A_LEFT, A_CENTER, A_RIGHT };
enum { J_START, J_CENTER, J_END, J_BETWEEN, J_AROUND, J_EVENLY };
enum { AL_STRETCH, AL_START, AL_CENTER, AL_END, AL_AUTO = 255 };
#define GRID_SPAN 1000
enum { VA_BASELINE, VA_MIDDLE, VA_TOP, VA_BOTTOM, VA_TEXT_TOP, VA_TEXT_BOTTOM, VA_SUB, VA_SUPER, VA_LENGTH, VA_INHERIT };
#define AUTO (-30000)
static int layout_viewport_w, layout_viewport_h; /* a frame's size (0 in the window: the usual) */
#define VIEW_W (layout_viewport_w ? layout_viewport_w : 800) /* the page width, for vw units and @media */
#define VIEW_H (layout_viewport_h ? layout_viewport_h : 560)

static int is_block_level(int d) {
    return d == D_BLOCK || d == D_LIST_ITEM || d == D_FLEX || d == D_GRID || d == D_TABLE || d == D_TABLE_ROW ||
           d == D_TABLE_GROUP || d == D_TABLE_CAPTION;
}

/* ---------------------------------------------------------------- values */

static int number100(const char *s, int *used) {
    int i = 0, sign = 1, whole = 0, frac = 0, digits = 0;
    if (s[i] == '-' || s[i] == '+') {
        if (s[i] == '-') sign = -1;
        i++;
    }
    while (s[i] >= '0' && s[i] <= '9') {
        if (whole < 1000000) whole = whole * 10 + (s[i] - '0');
        i++;
        digits++;
    }
    if (s[i] == '.') {
        i++;
        int place = 10;
        while (s[i] >= '0' && s[i] <= '9') {
            frac += (s[i] - '0') * place;
            place /= 10;
            i++;
            digits++;
        }
    }
    if (digits && (s[i] == 'e' || s[i] == 'E') && (s[i + 1] == '-' || (s[i + 1] >= '0' && s[i + 1] <= '9'))) {
        /* 1e3 and the like: rare in CSS; skip the exponent. */
        i++;
        if (s[i] == '-' || s[i] == '+') i++;
        while (s[i] >= '0' && s[i] <= '9') i++;
    }
    *used = digits ? i : 0;
    return sign * (whole * 100 + frac);
}

/* calc(), min(), max() and clamp(): worked out as px + percent. */
typedef struct {
    double px, pct, num;
    char kind; /* 'l' a length, 'n' a plain number, 0 not understood */
} CalcV;

static Len parse_len(const char *v, int font, int *ok);
static CalcV calc_sum(const char **p, int font, int depth);

static CalcV calc_value(const char **p, int font, int depth) {
    CalcV r = {0, 0, 0, 0};
    while (is_space(**p)) (*p)++;
    const char *s = *p;
    if (depth > 16) return r;
    if (*s == '(' || !strncasecmp(s, "calc(", 5) || !strncasecmp(s, "-webkit-calc(", 13)) {
        *p = strchr(s, '(') + 1;
        r = calc_sum(p, font, depth + 1);
        while (is_space(**p)) (*p)++;
        if (**p == ')') (*p)++;
        return r;
    }
    int fn = !strncasecmp(s, "min(", 4) ? 1 : !strncasecmp(s, "max(", 4) ? 2 : !strncasecmp(s, "clamp(", 6) ? 3 : 0;
    if (fn) {
        *p = strchr(s, '(') + 1;
        CalcV args[8];
        int n = 0;
        for (;;) {
            CalcV a = calc_sum(p, font, depth + 1);
            if (!a.kind) return r;
            if (n < 8) args[n++] = a;
            while (is_space(**p)) (*p)++;
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == ')') (*p)++;
            break;
        }
        /* Compare them as they would be in a page as wide as the window
           (percentages can only be guessed at here). */
        double est[8];
        for (int i = 0; i < n; i++) est[i] = args[i].kind == 'n' ? args[i].num : args[i].px + args[i].pct * VIEW_W / 100;
        int pick = 0;
        if (fn == 3 && n == 3) { /* clamp(lo, want, hi) */
            pick = est[1] < est[0] ? 0 : est[1] > est[2] ? 2 : 1;
        } else {
            for (int i = 1; i < n; i++)
                if (fn == 1 ? est[i] < est[pick] : est[i] > est[pick]) pick = i;
        }
        return args[pick];
    }
    char tok[48];
    int k = 0;
    if (*s == '+' || *s == '-') tok[k++] = *s++;
    while (*s && (is_alpha(*s) || (*s >= '0' && *s <= '9') || *s == '.' || *s == '%') && k < 47) tok[k++] = *s++;
    tok[k] = 0;
    *p = s;
    if (!k) return r;
    int used, n100 = number100(tok, &used);
    if (used && !tok[used]) {
        r.kind = 'n';
        r.num = n100 / 100.0;
        return r;
    }
    int ok;
    Len l = parse_len(tok, font, &ok);
    if (!ok || (l.unit != 'p' && l.unit != '%')) return r;
    r.kind = 'l';
    if (l.unit == '%') r.pct = l.value / 100.0;
    else r.px = l.value;
    return r;
}

static CalcV calc_product(const char **p, int font, int depth) {
    CalcV v = calc_value(p, font, depth);
    for (;;) {
        while (is_space(**p)) (*p)++;
        char op = **p;
        if (op != '*' && op != '/') return v;
        (*p)++;
        CalcV w = calc_value(p, font, depth);
        if (!v.kind || !w.kind) return (CalcV){0, 0, 0, 0};
        if (op == '*') {
            if (v.kind == 'n' && w.kind == 'n') v.num *= w.num;
            else if (v.kind == 'n') {
                w.px *= v.num;
                w.pct *= v.num;
                v = w;
            } else if (w.kind == 'n') {
                v.px *= w.num;
                v.pct *= w.num;
            }
        } else if (w.kind == 'n' && w.num != 0) {
            if (v.kind == 'n') v.num /= w.num;
            else {
                v.px /= w.num;
                v.pct /= w.num;
            }
        }
    }
}

static CalcV calc_sum(const char **p, int font, int depth) {
    CalcV v = calc_product(p, font, depth);
    for (;;) {
        while (is_space(**p)) (*p)++;
        char op = **p;
        if (op != '+' && op != '-') return v;
        (*p)++;
        CalcV w = calc_product(p, font, depth);
        if (!v.kind || !w.kind) return (CalcV){0, 0, 0, 0};
        double sign = op == '-' ? -1 : 1;
        if (v.kind == 'n' && w.kind == 'n') v.num += sign * w.num;
        else {
            if (v.kind == 'n') v = (CalcV){v.num, 0, 0, 'l'}; /* (a bare 0 and the like) */
            if (w.kind == 'n') w = (CalcV){w.num, 0, 0, 'l'};
            v.px += sign * w.px;
            v.pct += sign * w.pct;
        }
    }
}

static int round_half(double d) { return (int)(d < 0 ? d - 0.5 : d + 0.5); }

/* A length: px, or percent, or auto. `font` is the font size for em. */
static Len parse_len(const char *v, int font, int *ok) {
    Len l = {0, 'n', 0};
    *ok = 1;
    while (is_space(*v)) v++;
    if (strncasecmp(v, "auto", 4) == 0) {
        l.unit = 'a';
        return l;
    }
    if (strncasecmp(v, "none", 4) == 0) return l;
    /* sizes from the content: max-content 'x', min-content 'm', fit-content 'f' */
    if (strncasecmp(v, "max-content", 11) == 0 || strncasecmp(v, "-webkit-max-content", 19) == 0) {
        l.unit = 'x';
        return l;
    }
    if (strncasecmp(v, "min-content", 11) == 0 || strncasecmp(v, "-webkit-min-content", 19) == 0) {
        l.unit = 'm';
        return l;
    }
    if (strncasecmp(v, "fit-content", 11) == 0 || strncasecmp(v, "-webkit-fit-content", 19) == 0 ||
        strncasecmp(v, "-moz-fit-content", 16) == 0) {
        l.unit = 'f';
        return l;
    }
    if (strncasecmp(v, "calc(", 5) == 0 || strncasecmp(v, "min(", 4) == 0 || strncasecmp(v, "max(", 4) == 0 ||
        strncasecmp(v, "clamp(", 6) == 0 || strncasecmp(v, "-webkit-calc(", 13) == 0) {
        const char *p = v;
        CalcV r = calc_value(&p, font, 0);
        if (!r.kind) {
            *ok = 0;
            return l;
        }
        if (r.kind == 'n') r.px = r.num;
        int px = round_half(r.px), pct = round_half(r.pct * 100);
        if (!pct) {
            l.unit = 'p';
            l.value = px;
        } else if (!px) {
            l.unit = '%';
            l.value = pct;
        } else {
            l.unit = 'c';
            l.value = pct;
            l.px = px;
        }
        return l;
    }
    int used;
    int n = number100(v, &used);
    if (!used) {
        *ok = 0;
        return l;
    }
    const char *u = v + used;
    l.unit = 'p';
    if (*u == '%') {
        l.unit = '%';
        l.value = n;
    } else if (!strncasecmp(u, "px", 2)) l.value = n / 100;
    else if (!strncasecmp(u, "rem", 3)) l.value = n * 16 / 100;
    else if (!strncasecmp(u, "em", 2) || !strncasecmp(u, "ch", 2)) l.value = n * font / 100;
    else if (!strncasecmp(u, "ex", 2)) l.value = n * font / 200;
    else if (!strncasecmp(u, "pt", 2)) l.value = n * 4 / 300;
    else if (!strncasecmp(u, "pc", 2)) l.value = n * 16 / 100;
    else if (!strncasecmp(u, "in", 2)) l.value = n * 96 / 100;
    else if (!strncasecmp(u, "cm", 2)) l.value = n * 3780 / 10000;
    else if (!strncasecmp(u, "mm", 2)) l.value = n * 378 / 10000;
    else if (!strncasecmp(u, "vw", 2)) l.value = n * VIEW_W / 10000;
    else if (!strncasecmp(u, "vh", 2) || !strncasecmp(u, "dvh", 3) || !strncasecmp(u, "svh", 3)) l.value = n * VIEW_H / 10000;
    else if (!strncasecmp(u, "vmin", 4)) l.value = n * VIEW_H / 10000;
    else if (!strncasecmp(u, "vmax", 4)) l.value = n * VIEW_W / 10000;
    else if (is_alpha(*u)) {
        *ok = 0;
        return l;
    } else {
        l.value = n / 100; /* a bare number */
    }
    /* under a pixel but not nothing (padding-top: 0.05px still stops margins
       collapsing through): px marks it, as px is only used by 'c' lengths */
    if (l.unit == 'p' && !l.value && n > 0) l.px = 1;
    return l;
}

static const struct {
    const char *name;
    unsigned rgb;
} NAMED_COLORS[] = {
    {"black", 0x000000},       {"white", 0xFFFFFF},        {"red", 0xFF0000},          {"green", 0x008000},
    {"blue", 0x0000FF},        {"yellow", 0xFFFF00},       {"gray", 0x808080},         {"grey", 0x808080},
    {"silver", 0xC0C0C0},      {"maroon", 0x800000},       {"navy", 0x000080},         {"purple", 0x800080},
    {"teal", 0x008080},        {"olive", 0x808000},        {"lime", 0x00FF00},         {"aqua", 0x00FFFF},
    {"cyan", 0x00FFFF},        {"fuchsia", 0xFF00FF},      {"magenta", 0xFF00FF},      {"orange", 0xFFA500},
    {"darkgray", 0xA9A9A9},    {"darkgrey", 0xA9A9A9},     {"lightgray", 0xD3D3D3},    {"lightgrey", 0xD3D3D3},
    {"dimgray", 0x696969},     {"dimgrey", 0x696969},      {"whitesmoke", 0xF5F5F5},   {"gainsboro", 0xDCDCDC},
    {"darkblue", 0x00008B},    {"darkred", 0x8B0000},      {"darkgreen", 0x006400},    {"brown", 0xA52A2A},
    {"pink", 0xFFC0CB},        {"gold", 0xFFD700},         {"beige", 0xF5F5DC},        {"ivory", 0xFFFFF0},
    {"lavender", 0xE6E6FA},    {"crimson", 0xDC143C},      {"coral", 0xFF7F50},        {"salmon", 0xFA8072},
    {"tomato", 0xFF6347},      {"indigo", 0x4B0082},       {"violet", 0xEE82EE},       {"skyblue", 0x87CEEB},
    {"steelblue", 0x4682B4},   {"slategray", 0x708090},    {"slategrey", 0x708090},    {"aliceblue", 0xF0F8FF},
    {"ghostwhite", 0xF8F8FF},  {"snow", 0xFFFAFA},         {"linen", 0xFAF0E6},        {"wheat", 0xF5DEB3},
    {"khaki", 0xF0E68C},       {"tan", 0xD2B48C},          {"chocolate", 0xD2691E},    {"sienna", 0xA0522D},
    {"firebrick", 0xB22222},   {"midnightblue", 0x191970}, {"royalblue", 0x4169E1},    {"dodgerblue", 0x1E90FF},
    {"deepskyblue", 0x00BFFF}, {"seagreen", 0x2E8B57},     {"forestgreen", 0x228B22},  {"limegreen", 0x32CD32},
    {"darkorange", 0xFF8C00},  {"orangered", 0xFF4500},    {"hotpink", 0xFF69B4},      {"deeppink", 0xFF1493},
    {"plum", 0xDDA0DD},        {"orchid", 0xDA70D6},       {"honeydew", 0xF0FFF0},     {"mintcream", 0xF5FFFA},
    {"azure", 0xF0FFFF},       {"lightblue", 0xADD8E6},    {"lightgreen", 0x90EE90},   {"lightyellow", 0xFFFFE0},
    {"lightcyan", 0xE0FFFF},   {"lightpink", 0xFFB6C1},    {"darkslategray", 0x2F4F4F}, {"cornflowerblue", 0x6495ED},
    {"cadetblue", 0x5F9EA0},   {"darkcyan", 0x008B8B},     {"darkmagenta", 0x8B008B},  {"darkviolet", 0x9400D3},
    {"mediumblue", 0x0000CD},  {"oldlace", 0xFDF5E6},      {"seashell", 0xFFF5EE},     {"papayawhip", 0xFFEFD5},
    {"cornsilk", 0xFFF8DC},    {"floralwhite", 0xFFFAF0},  {"lemonchiffon", 0xFFFACD}, {"lightslategray", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"darkslateblue", 0x483D8B}, {"slateblue", 0x6A5ACD}, {"rebeccapurple", 0x663399},
    {"darkolivegreen", 0x556B2F}, {"olivedrab", 0x6B8E23}, {"goldenrod", 0xDAA520},   {"darkgoldenrod", 0xB8860B},
    {"peru", 0xCD853F},        {"rosybrown", 0xBC8F8F},    {"indianred", 0xCD5C5C},    {"lightcoral", 0xF08080},
    {"darksalmon", 0xE9967A},  {"lightsalmon", 0xFFA07A},  {"palegreen", 0x98FB98},   {"aquamarine", 0x7FFFD4},
    {"turquoise", 0x40E0D0},   {"darkturquoise", 0x00CED1}, {"powderblue", 0xB0E0E6}, {"navajowhite", 0xFFDEAD},
};

/* 1: a colour (in *out); 2: transparent; 0: not understood. */
static int parse_color(const char *v, unsigned *out) {
    while (is_space(*v)) v++;
    if (*v == '#') {
        int d[8], n = 0;
        while (n < 8 && hex_digit(v[1 + n]) >= 0) {
            d[n] = hex_digit(v[1 + n]);
            n++;
        }
        if (n == 3 || n == 4) {
            if (n == 4 && d[3] == 0) return 2;
            *out = (unsigned)(d[0] * 17) << 16 | (unsigned)(d[1] * 17) << 8 | (unsigned)(d[2] * 17);
            if (n == 4) *out |= (unsigned)(255 - d[3] * 17) << 24;
            return 1;
        }
        if (n == 6 || n == 8) {
            if (n == 8 && d[6] == 0 && d[7] == 0) return 2;
            *out = (unsigned)(d[0] << 20 | d[1] << 16 | d[2] << 12 | d[3] << 8 | d[4] << 4 | d[5]);
            if (n == 8) *out |= (unsigned)(255 - (d[6] << 4 | d[7])) << 24;
            return 1;
        }
        return 0;
    }
    if (strncasecmp(v, "rgb", 3) == 0 || strncasecmp(v, "hsl", 3) == 0) {
        int hsl = tolower((unsigned char)v[0]) == 'h';
        const char *p = strchr(v, '(');
        if (!p) return 0;
        p++;
        int c[4] = {0, 0, 0, 100}, k = 0;
        while (k < 4 && *p && *p != ')') {
            while (is_space(*p) || *p == ',' || *p == '/') p++;
            int used, n = number100(p, &used);
            if (!used) break;
            p += used;
            int percent = *p == '%';
            if (percent) p++;
            while (is_alpha(*p)) p++; /* "deg" */
            if (k == 3) n = percent ? n / 100 : n;          /* alpha 0..100 */
            else if (hsl) n = n / 100;                      /* h in degrees, s and l in percent */
            else n = percent ? n * 255 / 10000 : n / 100;  /* 0..255 */
            c[k++] = n;
        }
        if (k < 3) return 0;
        if (k == 4 && c[3] == 0) return 2;
        if (hsl) {
            /* HSL to RGB, in whole numbers. */
            int h = ((c[0] % 360) + 360) % 360, s = c[1], l = c[2];
            int q = l < 50 ? l * (100 + s) / 100 : l + s - l * s / 100;
            int pp = 2 * l - q;
            int rgb[3];
            for (int i = 0; i < 3; i++) {
                int t = h + (i == 0 ? 120 : i == 2 ? -120 : 0);
                t = (t + 360) % 360;
                int v100;
                if (t < 60) v100 = pp + (q - pp) * t / 60;
                else if (t < 180) v100 = q;
                else if (t < 240) v100 = pp + (q - pp) * (240 - t) / 60;
                else v100 = pp;
                rgb[i] = v100 * 255 / 100;
            }
            c[0] = rgb[0];
            c[1] = rgb[1];
            c[2] = rgb[2];
        }
        for (int i = 0; i < 3; i++) c[i] = c[i] < 0 ? 0 : c[i] > 255 ? 255 : c[i];
        *out = (unsigned)(c[0] << 16 | c[1] << 8 | c[2]);
        int alpha = c[3] < 0 ? 0 : c[3] > 100 ? 100 : c[3]; /* (percent) */
        if (alpha < 100) *out |= (unsigned)(255 - (alpha * 255 + 50) / 100) << 24;
        return 1;
    }
    if (strncasecmp(v, "light-dark(", 11) == 0) return parse_color(v + 11, out);
    if (strncasecmp(v, "transparent", 11) == 0) return 2;
    for (size_t i = 0; i < sizeof(NAMED_COLORS) / sizeof(NAMED_COLORS[0]); i++) {
        size_t n = strlen(NAMED_COLORS[i].name);
        if (strncasecmp(v, NAMED_COLORS[i].name, n) == 0 && !is_alpha(v[n])) {
            *out = NAMED_COLORS[i].rgb;
            return 1;
        }
    }
    return 0;
}

/* Split a value into space-separated parts (not inside parentheses). */
static int split_value(const char *v, char parts[][64], int max) {
    int n = 0;
    while (*v && n < max) {
        while (is_space(*v) || *v == ',') v++;
        if (!*v) break;
        int k = 0, depth = 0;
        while (*v && (depth || (!is_space(*v) && *v != ','))) {
            if (*v == '(') depth++;
            if (*v == ')') depth--;
            if (k < 63) parts[n][k++] = *v;
            v++;
        }
        parts[n++][k] = 0;
    }
    return n;
}

static int font_size(const char *v, int parent) {
    static const struct {
        const char *name;
        int px;
    } sizes[] = {{"xxx-large", 48}, {"xx-large", 32}, {"x-large", 24}, {"xx-small", 9},
                 {"x-small", 10},   {"small", 13},    {"medium", 16},  {"large", 18}};
    while (is_space(*v)) v++;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (strncasecmp(v, sizes[i].name, strlen(sizes[i].name)) == 0) return sizes[i].px;
    }
    if (strncasecmp(v, "larger", 6) == 0) return parent * 6 / 5;
    if (strncasecmp(v, "smaller", 7) == 0) return parent * 5 / 6;
    int ok;
    Len l = parse_len(v, parent, &ok);
    if (!ok || l.unit == 'a' || l.unit == 'n') return parent;
    int px = l.unit == '%' ? l.value * parent / 10000 : l.value;
    return px > 0 ? px : parent;
}

/* Border parts collected while cascading, settled afterwards. */
typedef struct {
    signed char style[4]; /* -1 not set, 0 none, 1 visible */
    int width[4];         /* -1 not set */
    unsigned color[4];
    unsigned char color_set[4];
} Borders;

/* Colours are 0xTTRRGGBB: TT is how transparent (0 opaque, 255 not seen). */
#define BORDER_INVISIBLE 0xFF000000u /* a border colour: transparent */
#define COLOR_ALPHA(c) (255u - ((c) >> 24))

static void border_part(Borders *b, int side, const char *v, int font) {
    char parts[6][64];
    int n = split_value(v, parts, 6);
    for (int i = 0; i < n; i++) {
        const char *p = parts[i];
        unsigned c;
        int ok;
        if (!strcasecmp_word(p, "none") || !strcasecmp_word(p, "hidden")) b->style[side] = 0;
        else if (!strcasecmp_word(p, "solid") || !strcasecmp_word(p, "dashed") || !strcasecmp_word(p, "dotted") ||
                 !strcasecmp_word(p, "double") || !strcasecmp_word(p, "groove") || !strcasecmp_word(p, "ridge") ||
                 !strcasecmp_word(p, "inset") || !strcasecmp_word(p, "outset"))
            b->style[side] = 1;
        else if (!strcasecmp_word(p, "thin")) b->width[side] = 1;
        else if (!strcasecmp_word(p, "medium")) b->width[side] = 3;
        else if (!strcasecmp_word(p, "thick")) b->width[side] = 5;
        else if ((ok = parse_color(p, &c)) != 0) {
            /* transparent: the border takes its room but is not drawn */
            b->color[side] = ok == 2 ? BORDER_INVISIBLE : c;
            b->color_set[side] = 1;
        } else {
            Len l = parse_len(p, font, &ok);
            if (ok && l.unit == 'p') b->width[side] = l.value < 0 ? 0 : l.value;
        }
    }
}

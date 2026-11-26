/* web/cascade.c - applying CSS declarations, selectors, stylesheets and
   the cascade. Part of the browser (included by programs/browser.c). */

static void set_sides(Len out[4], const char *v, int font) {
    char p[4][64];
    int n = split_value(v, p, 4), ok;
    Len val[4];
    for (int i = 0; i < n; i++) {
        val[i] = parse_len(p[i], font, &ok);
        if (!ok) return;
    }
    if (n == 1) out[0] = out[1] = out[2] = out[3] = val[0];
    else if (n == 2) out[0] = out[2] = val[0], out[1] = out[3] = val[1];
    else if (n == 3) out[0] = val[0], out[1] = out[3] = val[1], out[2] = val[2];
    else if (n == 4) out[0] = val[0], out[1] = val[1], out[2] = val[2], out[3] = val[3];
}

static int side_index(const char *side) {
    if (!strcmp(side, "top") || !strcmp(side, "block-start")) return 0;
    if (!strcmp(side, "right") || !strcmp(side, "inline-end")) return 1;
    if (!strcmp(side, "bottom") || !strcmp(side, "block-end")) return 2;
    if (!strcmp(side, "left") || !strcmp(side, "inline-start")) return 3;
    return -1;
}

static int word_is(const char *v, const char *w) {
    while (is_space(*v)) v++;
    size_t n = strlen(w);
    return strncasecmp(v, w, n) == 0 && !is_alpha(v[n]) && v[n] != '-';
}

/* Apply one declaration to a style. */
/* A value kept for later (grid templates are read when laid out): the
   same text gives the same copy, which lives as long as the browser. */
static const char *css_keep(const char *v) {
    static char **kept;
    static int kept_count, kept_cap;
    while (is_space(*v)) v++;
    int len = (int)strlen(v);
    const char *bang = strstr(v, "!important");
    if (bang) len = (int)(bang - v);
    while (len > 0 && is_space(v[len - 1])) len--;
    if (!len || (len == 4 && !strncasecmp(v, "none", 4))) return NULL;
    for (int i = 0; i < kept_count; i++)
        if (!strncmp(kept[i], v, (size_t)len) && !kept[i][len]) return kept[i];
    if (kept_count >= 4096 || !grow((void **)&kept, &kept_cap, kept_count + 1, sizeof(char *))) return NULL;
    char *copy = malloc((size_t)len + 1);
    if (!copy) return NULL;
    memcpy(copy, v, (size_t)len);
    copy[len] = 0;
    kept[kept_count++] = copy;
    return copy;
}

/* A grid line: "3", "-1", "span 2", "auto" (0). */
static short grid_line(const char *v) {
    while (is_space(*v)) v++;
    int span = 0;
    if (!strncasecmp(v, "span", 4)) {
        span = 1;
        v += 4;
        while (is_space(*v)) v++;
    }
    int n = 0, neg = 0;
    if (*v == '-') {
        neg = 1;
        v++;
    }
    if (*v < '0' || *v > '9') return span ? GRID_SPAN + 1 : 0;
    while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
    if (n > 999) n = 999;
    if (span) return (short)(GRID_SPAN + (n > 0 ? n : 1));
    return (short)(neg ? -n : n);
}

/* grid-template (and grid): rows / columns, maybe with area strings. */
static void grid_template(Style *s, const char *v) {
    char rows[512], areas[512];
    const char *slash = NULL;
    int depth = 0, quote = 0;
    for (const char *p = v; *p; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') quote = *p;
        else if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (*p == '/' && !depth) {
            slash = p;
            break;
        }
    }
    if (strstr(v, "auto-flow")) { /* grid: auto-flow / columns, or rows / auto-flow */
        s->grid_flow_column = !slash || strstr(slash, "auto-flow") != NULL;
        if (slash && !strstr(slash, "auto-flow")) s->grid_cols = css_keep(slash + 1);
        return;
    }
    int rl = 0, al = 0;
    quote = 0;
    for (const char *p = v; *p && p != slash && rl < 500 && al < 500; p++) {
        if (quote) {
            if (*p == quote) {
                quote = 0;
                areas[al++] = '"';
            } else areas[al++] = *p;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
            areas[al++] = '"';
        } else rows[rl++] = *p;
    }
    rows[rl] = 0;
    areas[al] = 0;
    s->grid_rows = css_keep(rows);
    s->grid_areas = al ? css_keep(areas) : NULL;
    s->grid_cols = slash ? css_keep(slash + 1) : NULL;
}

/* ---------------------------------------------------------------- backgrounds, shadows, transforms */

/* The next part of a value: a word, or a function call with its brackets;
   commas and slashes come back as parts of their own. Returns its length. */
static int value_part(const char **p, char *out, int max) {
    const char *s = *p;
    while (is_space(*s)) s++;
    int n = 0, depth = 0;
    if (*s == ',' || *s == '/') {
        out[0] = *s;
        out[1] = 0;
        *p = s + 1;
        return 1;
    }
    while (*s && (depth || (!is_space(*s) && *s != ',' && *s != '/'))) {
        if (*s == '(') depth++;
        else if (*s == ')') depth--;
        if (n < max - 1) out[n++] = *s;
        s++;
        if (depth < 0) break;
    }
    out[n] = 0;
    *p = s;
    return n;
}

static unsigned char repeat_word(const char *v) {
    while (is_space(*v)) v++;
    if (!strncasecmp(v, "no-repeat", 9)) return 1;
    if (!strncasecmp(v, "repeat-x", 8)) return 2;
    if (!strncasecmp(v, "repeat-y", 8)) return 3;
    if (!strncasecmp(v, "repeat no-repeat", 16)) return 2;
    if (!strncasecmp(v, "no-repeat repeat", 16)) return 3;
    return 0;
}

static int position_word(const char *w) {
    return !strcasecmp(w, "left") || !strcasecmp(w, "right") || !strcasecmp(w, "top") || !strcasecmp(w, "bottom") ||
           !strcasecmp(w, "center");
}

/* background: [image] [position [/ size]] [repeat] [color], layers by commas. */
static void background_shorthand(Style *s, const char *v) {
    char images[1024] = "", pos[128] = "", size[128] = "", part[512];
    char layer_pos[128] = "", layer_size[128] = "";
    int layer = 0, after_slash = 0, have_color = 0, url_layer = -1, layer_has_url = 0;
    unsigned color = 0;
    s->bg_repeat = 0;
    for (const char *p = v; value_part(&p, part, sizeof(part)) > 0;) {
        if (!strcmp(part, ",")) { /* (the placing that counts is the picture's layer's, else the first's) */
            if ((layer_has_url && url_layer < 0) || (!layer && !layer_has_url)) {
                strcpy(pos, layer_pos);
                strcpy(size, layer_size);
                if (layer_has_url) url_layer = layer;
            }
            layer++;
            after_slash = layer_has_url = 0;
            layer_pos[0] = layer_size[0] = 0;
            continue;
        }
        if (!strcmp(part, "/")) {
            after_slash = 1;
            continue;
        }
        if (strstr(part, "url(") || strstr(part, "gradient(")) {
            if (strstr(part, "url(")) layer_has_url = 1;
            if (images[0]) strncat_safe(images, ", ", sizeof(images) - strlen(images) - 1);
            strncat_safe(images, part, sizeof(images) - strlen(images) - 1);
            continue;
        }
        if (!strncasecmp(part, "repeat", 6) || !strncasecmp(part, "no-repeat", 9) || !strncasecmp(part, "space", 5) ||
            !strncasecmp(part, "round", 5)) {
            if (!layer || (url_layer < 0 && !layer_has_url) || layer_has_url) s->bg_repeat = repeat_word(part);
            continue;
        }
        int got = parse_color(part, &color);
        if (got) {
            have_color = got;
            continue;
        }
        int used;
        number100(part, &used);
        if (after_slash && (used || !strcasecmp(part, "auto") || !strcasecmp(part, "cover") || !strcasecmp(part, "contain"))) {
            if (layer_size[0]) strncat_safe(layer_size, " ", sizeof(layer_size) - strlen(layer_size) - 1);
            strncat_safe(layer_size, part, sizeof(layer_size) - strlen(layer_size) - 1);
        } else if (used || position_word(part)) {
            if (layer_pos[0]) strncat_safe(layer_pos, " ", sizeof(layer_pos) - strlen(layer_pos) - 1);
            strncat_safe(layer_pos, part, sizeof(layer_pos) - strlen(layer_pos) - 1);
        }
    }
    if ((layer_has_url && url_layer < 0) || (!layer && !layer_has_url)) {
        strcpy(pos, layer_pos);
        strcpy(size, layer_size);
    }
    s->bg_image = images[0] ? css_keep(images) : NULL;
    s->bg_pos = pos[0] ? css_keep(pos) : NULL;
    s->bg_size = size[0] ? css_keep(size) : NULL;
    if (have_color == 1) {
        s->background = color;
        s->has_background = 1;
    } else {
        s->background = BORDER_INVISIBLE;
        s->has_background = s->bg_image || s->has_shadow;
    }
}

/* box-shadow: the first one not drawn inside (x y [blur [spread]] [color]). */
static void box_shadow(Style *s, const char *v) {
    char part[256];
    s->has_shadow = 0;
    if (word_is(v, "none")) return;
    int lens[4], count = 0, inset = 0, got_color = 0;
    unsigned color = s->color;
    for (const char *p = v;;) {
        int n = value_part(&p, part, sizeof(part));
        if (!n || !strcmp(part, ",")) {
            if (!inset && count >= 2) {
                for (int i = 0; i < 4; i++) s->shadow[i] = (short)(i < count ? lens[i] : 0);
                if (s->shadow[2] < 0) s->shadow[2] = 0;
                s->shadow_color = got_color ? color : (s->color & 0xFFFFFF) | 0x80000000u;
                s->has_shadow = 1;
                if (!s->has_background) s->background = BORDER_INVISIBLE;
                s->has_background = 1;
                return;
            }
            if (!n) return;
            count = inset = got_color = 0;
            continue;
        }
        if (!strcasecmp(part, "inset")) {
            inset = 1;
            continue;
        }
        unsigned c;
        if (parse_color(part, &c)) {
            color = c;
            got_color = 1;
            continue;
        }
        int ok;
        Len l = parse_len(part, s->size, &ok);
        if (ok && l.unit == 'p' && count < 4) lens[count++] = l.value;
    }
}

/* transform: the translation in it (translate, translateX/Y, translate3d,
   matrix); rotations and scaling are not drawn. */
static void translation(Style *s, const char *v, int font) {
    s->translated = 0;
    s->tx = s->ty = (Len){0, 'p', 0};
    char part[256];
    for (const char *p = v; value_part(&p, part, sizeof(part)) > 0;) {
        const char *open = strchr(part, '(');
        if (!open) continue;
        char args[4][64];
        int n = 0;
        const char *a = open + 1;
        while (n < 4 && *a && *a != ')') {
            while (is_space(*a) || *a == ',') a++;
            int k = 0, depth = 0;
            while (*a && (depth || (*a != ',' && *a != ')' && !(is_space(*a) && !depth)))) {
                if (*a == '(') depth++;
                if (*a == ')') depth--;
                if (k < 63) args[n][k++] = *a;
                a++;
            }
            args[n][k] = 0;
            if (k) n++;
            while (is_space(*a)) a++;
            if (*a == ',') a++;
        }
        int ok;
        if (!strncasecmp(part, "translateX(", 11) && n >= 1) s->tx = parse_len(args[0], font, &ok);
        else if (!strncasecmp(part, "translateY(", 11) && n >= 1) s->ty = parse_len(args[0], font, &ok);
        else if ((!strncasecmp(part, "translate(", 10) || !strncasecmp(part, "translate3d(", 12)) && n >= 1) {
            s->tx = parse_len(args[0], font, &ok);
            if (n >= 2) s->ty = parse_len(args[1], font, &ok);
        } else if (!strncasecmp(part, "matrix(", 7)) {
            if (n < 4) continue;
            /* matrix(a, b, c, d, e, f): e and f move it */
            const char *e = open + 1;
            int commas = 0;
            while (*e && commas < 4) commas += *e++ == ',';
            int used;
            int ex = number100(e, &used);
            while (*e && *e != ',') e++;
            int ey = *e ? number100(e + 1 + (e[1] == ' '), &used) : 0;
            s->tx = (Len){ex / 100, 'p', 0};
            s->ty = (Len){ey / 100, 'p', 0};
        } else continue;
        s->translated = 1;
    }
}

/* A corner's radius: px, or a percentage (stored negative). */
static short corner_len(const char *v, int font) {
    int ok;
    Len l = parse_len(v, font, &ok);
    if (!ok) return 0;
    if (l.unit == '%') return (short)(-(l.value / 100 > 100 ? 100 : l.value / 100 < 1 ? 1 : l.value / 100));
    if (l.unit == 'p') return (short)(l.value < 0 ? 0 : l.value > 30000 ? 30000 : l.value);
    return 0;
}

static int align_word(const char *v, int self) {
    while (is_space(*v)) v++;
    if (!strncasecmp(v, "safe ", 5) || !strncasecmp(v, "unsafe ", 7)) {
        while (*v && !is_space(*v)) v++;
        while (is_space(*v)) v++;
    }
    if (!strncasecmp(v, "center", 6)) return AL_CENTER;
    if (!strncasecmp(v, "flex-end", 8) || !strncasecmp(v, "end", 3) || !strncasecmp(v, "self-end", 8) || !strncasecmp(v, "right", 5))
        return AL_END;
    if (!strncasecmp(v, "stretch", 7)) return AL_STRETCH;
    if (!strncasecmp(v, "normal", 6) || !strncasecmp(v, "auto", 4)) return self ? AL_AUTO : AL_STRETCH;
    if (!strncasecmp(v, "flex-start", 10) || !strncasecmp(v, "start", 5) || !strncasecmp(v, "self-start", 10) ||
        !strncasecmp(v, "left", 4) || !strncasecmp(v, "baseline", 8) || !strncasecmp(v, "first", 5) || !strncasecmp(v, "last", 4))
        return AL_START;
    return self ? AL_AUTO : AL_STRETCH;
}

/* Which of our faces stands for a font-family list: the first family we
   know (or a generic one); a list of only unknown (web) fonts gets sans. */
static int face_of_family(const char *n) {
    if (strstr(n, "verdana") || !strcmp(n, "geneva") || strstr(n, "dejavu sans") || strstr(n, "bitstream vera sans"))
        return strstr(n, "mono") ? FACE_MONO : FACE_WIDE; /* (wider than Arial) */
    if (strstr(n, "mono") || strstr(n, "courier") || strstr(n, "consol") || strstr(n, "menlo") || strstr(n, "monaco") ||
        strstr(n, "code") || !strcmp(n, "fixed") || strstr(n, "typewriter"))
        return FACE_MONO;
    if (strstr(n, "sans") || strstr(n, "arial") || strstr(n, "helvetica") || strstr(n, "verdana") || strstr(n, "tahoma") ||
        strstr(n, "segoe") || strstr(n, "roboto") || strstr(n, "system-ui") || strstr(n, "apple-system") ||
        strstr(n, "blinkmacsystemfont") || strstr(n, "trebuchet") || strstr(n, "calibri") || strstr(n, "lucida grande"))
        return FACE_SANS;
    if (strstr(n, "serif") || strstr(n, "times") || strstr(n, "georgia") || strstr(n, "garamond") || strstr(n, "cambria") ||
        strstr(n, "palatino") || strstr(n, "baskerville") || strstr(n, "book antiqua") || strstr(n, "didot") ||
        strstr(n, "constantia") || strstr(n, "charter"))
        return FACE_SERIF;
    return -1;
}

static int face_for_family(const char *v, int current) {
    while (*v == ' ') v++;
    if (!strncasecmp(v, "inherit", 7) || !strncasecmp(v, "unset", 5)) return current;
    if (!strncasecmp(v, "initial", 7)) return FACE_SERIF;
    char name[64];
    int web = -1; /* the first family a page brings (@font-face) */
    while (*v) {
        while (*v == ' ' || *v == ',' || *v == '"' || *v == '\'') v++;
        int n = 0;
        while (*v && *v != ',') {
            if (*v != '"' && *v != '\'' && n < 63) name[n++] = (char)tolower((unsigned char)*v);
            v++;
        }
        while (n && name[n - 1] == ' ') n--;
        name[n] = 0;
        if (!n) continue;
        if (!strcmp(name, "!important")) break;
        if (web < 0) {
            int slot = web_font_family(name);
            if (slot >= 0) {
                web = slot;
                continue;
            }
        }
        int face = face_of_family(name);
        if (face >= 0) return web >= 0 ? FACE_WEB + web * 4 + face : face;
    }
    return web >= 0 ? FACE_WEB + web * 4 + FACE_SANS : FACE_SANS;
}

/* @font-face { font-family; src; font-weight; font-style; unicode-range }
   (a src may be a long data: address: worked on where it lies). */
static void font_face_rule(const char *decls, int len) {
    char family[64] = "", weight[32] = "", style[32] = "", range[256] = "";
    const char *src = NULL;
    int src_len = 0;
    for (int i = 0; i < len;) {
        while (i < len && (is_space(decls[i]) || decls[i] == ';')) i++;
        int colon = i;
        while (colon < len && decls[colon] != ':' && decls[colon] != ';') colon++;
        if (colon >= len || decls[colon] != ':') break;
        char name[32];
        int n = 0;
        for (int q = i; q < colon && n < 31; q++)
            if (!is_space(decls[q])) name[n++] = (char)tolower((unsigned char)decls[q]);
        name[n] = 0;
        int v = colon + 1, e = v, depth = 0;
        char quote = 0;
        while (e < len && (quote || depth || decls[e] != ';')) {
            char c = decls[e];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') quote = c;
            else if (c == '(') depth++;
            else if (c == ')') depth--;
            e++;
        }
        while (v < e && is_space(decls[v])) v++;
        int vl = e - v;
        char small[256];
        int sl = vl < 255 ? vl : 255;
        memcpy(small, decls + v, (size_t)sl);
        small[sl] = 0;
        if (!strcmp(name, "font-family")) {
            int k = 0;
            for (int q = 0; small[q] && k < 63; q++)
                if (small[q] != '"' && small[q] != '\'') family[k++] = (char)tolower((unsigned char)small[q]);
            while (k && family[k - 1] == ' ') k--;
            family[k] = 0;
        } else if (!strcmp(name, "src")) {
            src = decls + v;
            src_len = vl;
        } else if (!strcmp(name, "font-weight")) strncpy(weight, small, sizeof(weight) - 1);
        else if (!strcmp(name, "font-style")) strncpy(style, small, sizeof(style) - 1);
        else if (!strcmp(name, "unicode-range")) strncpy(range, small, sizeof(range) - 1);
        i = e + 1;
    }
    if (!family[0] || !src) return;
    if (range[0]) { /* split by alphabet: keep the part with the Latin letters */
        int latin = 0;
        for (char *r = range; *r && !latin;) {
            while (*r && (is_space(*r) || *r == ',')) r++;
            if (strncasecmp(r, "u+", 2)) break;
            r += 2;
            unsigned lo = 0, hi;
            int wild = 0;
            while (hex_digit(*r) >= 0 || *r == '?') {
                lo = lo * 16 + (unsigned)(*r == '?' ? 0 : hex_digit(*r));
                wild = wild * 16 + (*r == '?' ? 15 : 0);
                r++;
            }
            hi = lo + (unsigned)wild;
            if (*r == '-') {
                r++;
                hi = 0;
                while (hex_digit(*r) >= 0) hi = hi * 16 + (unsigned)hex_digit(*r++);
            }
            latin = lo <= 'A' && hi >= 'z';
        }
        if (!latin) return;
    }
    /* the best of the sources: WOFF2, WOFF or TrueType */
    const char *best = NULL;
    int best_len = 0, best_rank = 0;
    for (int i = 0; i + 4 <= src_len;) {
        if (strncasecmp(src + i, "url(", 4)) {
            i++;
            continue;
        }
        int u = i + 4;
        while (u < src_len && is_space(src[u])) u++;
        char quote = u < src_len && (src[u] == '"' || src[u] == '\'') ? src[u++] : 0;
        int start = u;
        while (u < src_len && (quote ? src[u] != quote : src[u] != ')')) u++;
        int url_len = u - start;
        const char *url = src + start;
        /* its format(): before the next url( */
        int next = u;
        while (next + 4 <= src_len && strncasecmp(src + next, "url(", 4)) next++;
        if (next + 4 > src_len) next = src_len; /* (no more sources: to the end) */
        char kind[32] = "";
        for (int f = u; f + 7 <= next; f++)
            if (!strncasecmp(src + f, "format(", 7)) {
                int q = 0;
                for (f += 7; f < next && src[f] != ')' && q < 31; f++)
                    if (src[f] != '"' && src[f] != '\'' && !is_space(src[f])) kind[q++] = (char)tolower((unsigned char)src[f]);
                kind[q] = 0;
                break;
            }
        if (!kind[0]) {
            if (url_len > 5 && !strncasecmp(url, "data:", 5)) {
                int woff2 = 0, woff = 0;
                for (int q = 5; q + 5 <= url_len && q < 60; q++) {
                    if (!strncasecmp(url + q, "woff2", 5)) woff2 = 1;
                    else if (!strncasecmp(url + q, "woff", 4)) woff = 1;
                }
                strcpy(kind, woff2 ? "woff2" : woff ? "woff" : "truetype");
            } else { /* (by the file's name) */
                int dot = -1, stop = url_len;
                for (int q = 0; q < url_len; q++) {
                    if (url[q] == '?' || url[q] == '#') {
                        stop = q;
                        break;
                    }
                    if (url[q] == '.') dot = q;
                }
                int q = 0;
                for (int x = dot + 1; dot >= 0 && x < stop && q < 31; x++) kind[q++] = (char)tolower((unsigned char)url[x]);
                kind[q] = 0;
            }
        }
        int rank = !strcmp(kind, "woff2") ? 4 : !strcmp(kind, "woff") ? 3 : !strcmp(kind, "truetype") || !strcmp(kind, "ttf") ? 2
                   : !strcmp(kind, "opentype") || !strcmp(kind, "otf") ? 1 : 0;
        if (rank > best_rank) {
            best_rank = rank;
            best = url;
            best_len = url_len;
        }
        i = u;
    }
    if (!best) return;
    char *url = malloc((size_t)best_len + 1);
    if (!url) return;
    memcpy(url, best, (size_t)best_len);
    url[best_len] = 0;
    int used, wn = number100(weight, &used);
    int bold = used ? wn >= 60000 : !strncasecmp(weight, "bold", 4);
    int italic = !strncasecmp(style, "italic", 6) || !strncasecmp(style, "oblique", 7);
    int slot = web_font_declare(family, bold, italic, url);
    if (getenv("BROWSER_TIMING")) printf("[font] declared %s%s%s as %d\n", family, bold ? " bold" : "", italic ? " italic" : "", slot);
    free(url);
}

static int font_size_word(const char *v) {
    static const char *const words[] = {"xx-small", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large",
                                        "smaller", "larger"};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (!strcasecmp_word(v, words[i])) return 1;
    return 0;
}

/* line-height: 0 for normal, px, or a factor (stored * -100), as the
   Style keeps it. */
static int parse_line_height(const char *v, int current, int size) {
    while (is_space(*v)) v++;
    if (!strncasecmp(v, "normal", 6) || !strncasecmp(v, "initial", 7)) return 0;
    if (!strncasecmp(v, "inherit", 7) || !strncasecmp(v, "unset", 5)) return current;
    int used, n = number100(v, &used);
    if (!used) return current;
    if (!v[used] || is_space(v[used]) || v[used] == '!') return n > 0 ? -n : current;
    int ok;
    Len l = parse_len(v, size, &ok);
    if (!ok) return current;
    if (l.unit == '%') return l.value > 0 ? (int)((long long)l.value * size / 10000) : current;
    if (l.unit == 'p') return l.value > 0 ? l.value : current;
    return current;
}

/* SVG paint properties met while styling an element inside <svg>: kept
   for the SVG drawing (which reads them before the attributes). */
static int svg_collecting;
static char svg_props[1024];
static int svg_props_len;

static void svg_note(const char *name, const char *v) {
    static const char *const names[] = {"fill", "stroke", "stroke-width", "fill-opacity", "stroke-opacity", "opacity",
                                        "fill-rule", "stroke-linecap", "stroke-linejoin", "stop-color", "stop-opacity",
                                        "visibility", "stroke-dasharray", NULL};
    int known = 0;
    for (int i = 0; names[i] && !known; i++) known = !strcmp(names[i], name);
    if (!known) return;
    /* drop an earlier value for the same name */
    for (int i = 0; i < svg_props_len;) {
        int nl = (int)strlen(svg_props + i) + 1, vl = (int)strlen(svg_props + i + nl) + 1;
        if (!strcmp(svg_props + i, name)) {
            memmove(svg_props + i, svg_props + i + nl + vl, (size_t)(svg_props_len - i - nl - vl));
            svg_props_len -= nl + vl;
        } else i += nl + vl;
    }
    int nl = (int)strlen(name) + 1, vl = (int)strlen(v) + 1;
    if (svg_props_len + nl + vl > (int)sizeof(svg_props)) return;
    memcpy(svg_props + svg_props_len, name, (size_t)nl);
    memcpy(svg_props + svg_props_len + nl, v, (size_t)vl);
    svg_props_len += nl + vl;
}

static int parent_size64; /* the parent's font size in 1/64 px, while one element is styled */

/* font-size in 1/64 px (keeps 13.33px and the like exact). */
static int font_size64(const char *v, int parent) {
    static const struct {
        const char *name;
        int px;
    } sizes[] = {{"xxx-large", 48}, {"xx-large", 32}, {"x-large", 24}, {"xx-small", 9},
                 {"x-small", 10},   {"small", 13},    {"medium", 16},  {"large", 18}};
    while (is_space(*v)) v++;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (strncasecmp(v, sizes[i].name, strlen(sizes[i].name)) == 0) return sizes[i].px * 64;
    if (strncasecmp(v, "larger", 6) == 0) return parent * 6 / 5;
    if (strncasecmp(v, "smaller", 7) == 0) return parent * 5 / 6;
    int used;
    long long n = number100(v, &used); /* (* 100) */
    if (used) {
        const char *u = v + used;
        long long r = -1;
        if (!strncasecmp(u, "px", 2)) r = n * 64 / 100;
        else if (*u == '%') r = n * parent / 10000;
        else if (!strncasecmp(u, "rem", 3)) r = n * 16 * 64 / 100;
        else if (!strncasecmp(u, "em", 2)) r = n * parent / 100;
        else if (!strncasecmp(u, "pt", 2)) r = n * 64 * 4 / 300;
        if (r > 0) return (int)(r > 1000 * 64 ? 1000 * 64 : r);
        if (r == 0) return parent;
    }
    return font_size(v, (parent + 32) / 64) * 64; /* (calc(), vw and the rest: in whole px) */
}

static int em_size; /* the element's final font size (worked out first), for em in other properties; 0: not yet */

static void apply(Style *s, Borders *b, const char *name, const char *v, int parent_size) {
    unsigned color;
    int ok;
    while (is_space(*v)) v++;
    if (svg_collecting) svg_note(name, v);
    int font = em_size ? em_size : s->size;
    switch (name[0]) {
    case 'a':
        if (!strcmp(name, "align-items")) {
            s->align_items = word_is(v, "center") ? AL_CENTER
                             : (word_is(v, "flex-start") || word_is(v, "start") || word_is(v, "baseline") || word_is(v, "self-start")) ? AL_START
                             : (word_is(v, "flex-end") || word_is(v, "end")) ? AL_END
                                                                              : AL_STRETCH;
        } else if (!strcmp(name, "align-self")) {
            s->align_self = word_is(v, "center") ? AL_CENTER
                            : (word_is(v, "flex-start") || word_is(v, "start")) ? AL_START
                            : (word_is(v, "flex-end") || word_is(v, "end")) ? AL_END
                            : word_is(v, "stretch") ? AL_STRETCH
                                                    : AL_AUTO;
        }
        return;
    case 'b':
        if (!strcmp(name, "background-color")) {
            int got = word_is(v, "currentcolor") ? (color = s->color, 1) : parse_color(v, &color);
            if (got == 1) {
                s->background = color;
                s->has_background = 1;
            } else if (got == 2 || word_is(v, "none")) {
                s->background = BORDER_INVISIBLE;
                s->has_background = s->bg_image || s->has_shadow;
            }
        } else if (!strcmp(name, "background")) {
            background_shorthand(s, v);
        } else if (!strcmp(name, "background-image")) {
            s->bg_image = strstr(v, "url(") || strstr(v, "gradient(") ? css_keep(v) : NULL;
            if (s->bg_image && !s->has_background) s->background = BORDER_INVISIBLE;
            s->has_background = s->has_background || s->bg_image || s->has_shadow;
        } else if (!strcmp(name, "background-size")) {
            s->bg_size = css_keep(v);
        } else if (!strcmp(name, "background-position")) {
            s->bg_pos = css_keep(v);
        } else if (!strcmp(name, "background-repeat")) {
            s->bg_repeat = repeat_word(v);
        } else if (!strcmp(name, "box-shadow")) {
            box_shadow(s, v);
        } else if (!strcmp(name, "border")) {
            for (int i = 0; i < 4; i++) border_part(b, i, v, font);
        } else if (!strncmp(name, "border-", 7)) {
            const char *rest = name + 7;
            if (!strcmp(rest, "width") || !strcmp(rest, "style") || !strcmp(rest, "color")) {
                char p[4][64];
                int n = split_value(v, p, 4);
                for (int i = 0; i < 4 && n; i++) border_part(b, i, p[n == 1 ? 0 : n == 2 ? i % 2 : n == 3 ? (i == 3 ? 1 : i) : i], font);
            } else if (!strcmp(rest, "radius")) {
                /* a b c d [/ e f g h] */
                const char *slash = strchr(v, '/');
                char across[128], down[128];
                int al = slash ? (int)(slash - v) : (int)strlen(v);
                if (al > 127) al = 127;
                memcpy(across, v, (size_t)al);
                across[al] = 0;
                strncpy(down, slash ? slash + 1 : across, 127);
                down[127] = 0;
                for (int axis = 0; axis < 2; axis++) {
                    char p[4][64];
                    int n = split_value(axis ? down : across, p, 4);
                    for (int i = 0; i < 4 && n; i++) {
                        const char *one = p[n == 1 ? 0 : n == 2 ? i % 2 : n == 3 ? (i == 3 ? 1 : i) : i];
                        s->radius[i + axis * 4] = corner_len(one, font);
                    }
                }
            } else if (strstr(rest, "-radius")) { /* border-top-left-radius: across [down] */
                int corner = !strncmp(rest, "top-left", 8) || !strncmp(rest, "start-start", 11) ? 0
                             : !strncmp(rest, "top-right", 9) || !strncmp(rest, "start-end", 9) ? 1
                             : !strncmp(rest, "bottom-right", 12) || !strncmp(rest, "end-end", 7) ? 2
                             : !strncmp(rest, "bottom-left", 11) || !strncmp(rest, "end-start", 9) ? 3 : -1;
                if (corner >= 0) {
                    char p[2][64];
                    int n = split_value(v, p, 2);
                    if (n) {
                        s->radius[corner] = corner_len(p[0], font);
                        s->radius[corner + 4] = corner_len(p[n > 1 ? 1 : 0], font);
                    }
                }
            } else if (!strncmp(rest, "collapse", 8) || !strcmp(rest, "spacing") || !strcmp(rest, "image")) {
                /* not drawn */
            } else {
                char side[16];
                int k = 0;
                while (rest[k] && rest[k] != '-' && k < 15) side[k] = rest[k], k++;
                side[k] = 0;
                int i = side_index(side);
                if (i < 0) {
                    if (!strcmp(side, "inline")) {
                        border_part(b, 1, v, font);
                        border_part(b, 3, v, font);
                    } else if (!strcmp(side, "block")) {
                        border_part(b, 0, v, font);
                        border_part(b, 2, v, font);
                    }
                    return;
                }
                border_part(b, i, v, font); /* border-top, border-top-width, -style, -color */
            }
        } else if (!strcmp(name, "box-sizing")) {
            s->box_sizing = word_is(v, "border-box");
        } else if (!strcmp(name, "bottom")) {
            s->offset[2] = parse_len(v, font, &ok);
        }
        return;
    case 'c':
        if (!strcmp(name, "content")) {
            s->content = css_keep(v);
            return;
        }
        if (!strcmp(name, "color")) {
            if (parse_color(v, &color) == 1) s->color = color;
        } else if (!strcmp(name, "clear")) {
            s->clear = word_is(v, "left") ? 1 : word_is(v, "right") ? 2 : word_is(v, "both") ? 3 : 0;
        } else if (!strcmp(name, "clip") || !strcmp(name, "clip-path")) {
            /* The "visually hidden" idiom: rect(0 0 0 0), rect(1px, 1px, 1px, 1px), inset(50%). */
            s->clip_rect = (strstr(v, "rect(") && !strstr(v, "auto")) || strstr(v, "inset(50%") != NULL;
        } else if (!strcmp(name, "caption-side")) {
            s->caption_bottom = word_is(v, "bottom");
        } else if (!strcmp(name, "cursor")) {
            s->cursor_pointer = word_is(v, "pointer");
        } else if (!strcmp(name, "column-gap")) {
            Len l = parse_len(v, font, &ok);
            if (ok && l.unit == 'p') s->gap_col = l.value;
        }
        return;
    case 'd':
        if (!strcmp(name, "display")) {
            if (word_is(v, "none")) s->display = D_NONE;
            else if (word_is(v, "list-item")) s->display = D_LIST_ITEM;
            else if (word_is(v, "inline-grid")) s->display = D_INLINE_GRID;
            else if (word_is(v, "inline-block") || word_is(v, "inline-table")) s->display = D_INLINE_BLOCK;
            else if (word_is(v, "inline-flex")) s->display = D_INLINE_FLEX;
            else if (word_is(v, "inline") || word_is(v, "contents")) s->display = D_INLINE;
            else if (word_is(v, "flex")) s->display = D_FLEX;
            else if (word_is(v, "grid")) s->display = D_GRID;
            else if (word_is(v, "table-row-group") || word_is(v, "table-header-group") || word_is(v, "table-footer-group")) s->display = D_TABLE_GROUP;
            else if (word_is(v, "table-row")) s->display = D_TABLE_ROW;
            else if (word_is(v, "table-cell")) s->display = D_TABLE_CELL;
            else if (word_is(v, "table-caption")) s->display = D_TABLE_CAPTION;
            else if (word_is(v, "table")) s->display = D_TABLE;
            else s->display = D_BLOCK; /* block, flow-root, ... */
        }
        return;
    case 'f':
        if (!strcmp(name, "font-weight")) {
            int used, n = number100(v, &used);
            s->bold = used ? n >= 60000 : (word_is(v, "bold") || word_is(v, "bolder"));
        } else if (!strcmp(name, "font-size")) {
            s->size64 = font_size64(v, s->size64 ? parent_size64 : parent_size * 64);
            s->size = (s->size64 + 32) / 64;
        } else if (!strcmp(name, "font-family")) {
            s->face = (unsigned char)face_for_family(v, s->face);
        } else if (!strcmp(name, "font-style")) {
            s->italic = word_is(v, "italic") || word_is(v, "oblique");
        } else if (!strcmp(name, "font")) {
            if (word_is(v, "inherit") || word_is(v, "unset")) return; /* (as inherited already) */
            if (word_is(v, "initial")) {
                s->size = 16;
                s->size64 = 16 * 64;
                s->bold = s->italic = 0;
                s->line_height = 0;
                s->face = FACE_SERIF;
                return;
            }
            char parts[8][64];
            int n = split_value(v, parts, 8);
            s->line_height = 0;
            /* [style] [weight] size[/line-height] families: after the size, the rest */
            for (int i = 0; i < n; i++) {
                int used;
                number100(parts[i], &used);
                if (!(used && parts[i][used]) && !font_size_word(parts[i])) continue;
                const char *at = strstr(v, parts[i]), *lh = strchr(parts[i], '/');
                if (!at) break;
                at += strlen(parts[i]);
                if (lh) lh++;
                while (*at == ' ') at++;
                if ((!lh || !*lh) && *at == '/') {
                    at++;
                    while (*at == ' ') at++;
                    lh = at;
                    while (*at && *at != ' ') at++;
                }
                if (lh && *lh) s->line_height = parse_line_height(lh, 0, font_size(parts[i], parent_size));
                while (*at == ' ') at++;
                if (*at) s->face = (unsigned char)face_for_family(at, s->face);
                break;
            }
            for (int i = 0; i < n; i++) {
                int used, num = number100(parts[i], &used);
                if (!strcasecmp_word(parts[i], "bold") || !strcasecmp_word(parts[i], "bolder")) s->bold = 1;
                else if (!strcasecmp_word(parts[i], "italic")) s->italic = 1;
                else if (used && !parts[i][used]) {
                    if (num >= 60000) s->bold = 1;
                } else if (used || !strncasecmp(parts[i], "small", 5) || !strncasecmp(parts[i], "large", 5) ||
                           !strncasecmp(parts[i], "medium", 6) || !strncasecmp(parts[i], "x-", 2) || !strncasecmp(parts[i], "xx-", 3)) {
                    char *slash = strchr(parts[i], '/');
                    if (slash) *slash = 0;
                    s->size64 = font_size64(parts[i], parent_size64 ? parent_size64 : parent_size * 64);
                    s->size = (s->size64 + 32) / 64;
                }
            }
        } else if (!strcmp(name, "float")) {
            s->floating = word_is(v, "left") ? 1 : word_is(v, "right") ? 2 : 0;
        } else if (!strcmp(name, "flex-direction")) {
            s->flex_dir = word_is(v, "column") || word_is(v, "column-reverse");
        } else if (!strcmp(name, "flex-wrap")) {
            s->flex_wrap = word_is(v, "wrap") || word_is(v, "wrap-reverse");
        } else if (!strcmp(name, "flex-flow")) {
            s->flex_dir = strstr(v, "column") != NULL;
            s->flex_wrap = strstr(v, "wrap") && !strstr(v, "nowrap");
        } else if (!strcmp(name, "flex-grow")) {
            int used;
            s->grow = number100(v, &used);
        } else if (!strcmp(name, "flex-shrink")) {
            int used;
            s->shrink = number100(v, &used);
        } else if (!strcmp(name, "flex-basis")) {
            s->basis = parse_len(v, font, &ok);
        } else if (!strcmp(name, "flex")) {
            if (word_is(v, "none")) {
                s->grow = s->shrink = 0;
                s->basis.unit = 'a';
            } else if (word_is(v, "auto")) {
                s->grow = s->shrink = 100;
                s->basis.unit = 'a';
            } else {
                char parts[3][64];
                int n = split_value(v, parts, 3), used;
                int g = number100(parts[0], &used);
                if (used && !parts[0][used]) {
                    s->grow = g;
                    s->shrink = 100;
                    s->basis = (Len){0, 'p', 0};
                    if (n > 1) {
                        int sh = number100(parts[1], &used);
                        if (used && !parts[1][used]) s->shrink = sh;
                        else s->basis = parse_len(parts[1], font, &ok);
                    }
                    if (n > 2) s->basis = parse_len(parts[2], font, &ok);
                } else {
                    s->grow = 100; /* flex: 200px */
                    s->basis = parse_len(parts[0], font, &ok);
                }
            }
        }
        return;
    case 'g':
        if (!strcmp(name, "gap") || !strcmp(name, "grid-gap")) {
            char parts[2][64];
            int n = split_value(v, parts, 2);
            Len r = parse_len(parts[0], font, &ok);
            if (ok && r.unit == 'p') s->gap_row = s->gap_col = r.value;
            if (n > 1) {
                Len c = parse_len(parts[1], font, &ok);
                if (ok && c.unit == 'p') s->gap_col = c.value;
            }
        } else if (!strcmp(name, "grid-template-columns")) {
            s->grid_cols = css_keep(v);
        } else if (!strcmp(name, "grid-template-rows")) {
            s->grid_rows = css_keep(v);
        } else if (!strcmp(name, "grid-template-areas")) {
            s->grid_areas = strchr(v, '"') || strchr(v, '\'') ? css_keep(v) : NULL;
        } else if (!strcmp(name, "grid-auto-columns")) {
            s->grid_auto_cols = css_keep(v);
        } else if (!strcmp(name, "grid-auto-rows")) {
            s->grid_auto_rows = css_keep(v);
        } else if (!strcmp(name, "grid-auto-flow")) {
            s->grid_flow_column = strstr(v, "column") != NULL;
        } else if (!strcmp(name, "grid-template") || !strcmp(name, "grid")) {
            grid_template(s, v);
        } else if (!strcmp(name, "grid-column") || !strcmp(name, "grid-row")) {
            short *line = name[5] == 'c' ? s->grid_col : s->grid_row;
            const char *slash = strchr(v, '/');
            line[0] = grid_line(v);
            line[1] = slash ? grid_line(slash + 1) : (line[0] >= GRID_SPAN ? 0 : 0);
        } else if (!strcmp(name, "grid-column-start") || !strcmp(name, "grid-row-start")) {
            (name[5] == 'c' ? s->grid_col : s->grid_row)[0] = grid_line(v);
        } else if (!strcmp(name, "grid-column-end") || !strcmp(name, "grid-row-end")) {
            (name[5] == 'c' ? s->grid_col : s->grid_row)[1] = grid_line(v);
        } else if (!strcmp(name, "grid-area")) {
            /* row-start / column-start / row-end / column-end, or an area's name */
            const char *p = v;
            while (is_space(*p)) p++;
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
                if (strncasecmp(p, "span", 4) && strncasecmp(p, "auto", 4)) {
                    s->grid_area = css_keep(p);
                    return;
                }
            }
            short got[4] = {0, 0, 0, 0};
            for (int i = 0; i < 4 && p; i++) {
                got[i] = grid_line(p);
                p = strchr(p, '/');
                if (p) p++;
            }
            s->grid_row[0] = got[0];
            s->grid_col[0] = got[1];
            s->grid_row[1] = got[2];
            s->grid_col[1] = got[3];
        } else if (!strcmp(name, "grid-row-gap") || !strcmp(name, "row-gap")) {
            Len l = parse_len(v, font, &ok);
            if (ok && l.unit == 'p') s->gap_row = l.value;
        } else if (!strcmp(name, "grid-column-gap")) {
            Len l = parse_len(v, font, &ok);
            if (ok && l.unit == 'p') s->gap_col = l.value;
        }
        return;
    case 'h':
        if (!strcmp(name, "height")) s->height = parse_len(v, font, &ok);
        return;
    case 'i':
        if (!strcmp(name, "inset")) { /* top right bottom left, written as margins are */
            char parts[4][64];
            int n = split_value(v, parts, 4);
            static const int pick[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
            for (int i = 0; i < 4 && n > 0; i++) s->offset[i] = parse_len(parts[pick[(n > 4 ? 4 : n) - 1][i]], font, &ok);
        }
        return;
    case 'j':
        if (!strcmp(name, "justify-items")) {
            s->justify_items = align_word(v, 0);
            return;
        }
        if (!strcmp(name, "justify-self")) {
            s->justify_self = align_word(v, 1);
            return;
        }
        if (!strcmp(name, "justify-content")) {
            s->justify = word_is(v, "center") ? J_CENTER
                         : (word_is(v, "flex-end") || word_is(v, "end") || word_is(v, "right")) ? J_END
                         : word_is(v, "space-between") ? J_BETWEEN
                         : word_is(v, "space-around") ? J_AROUND
                         : word_is(v, "space-evenly") ? J_EVENLY
                                                      : J_START;
        }
        return;
    case 'l':
        if (!strcmp(name, "line-height")) {
            s->line_height = parse_line_height(v, s->line_height, s->size);
            return;
        }
        if (!strcmp(name, "left")) s->offset[3] = parse_len(v, font, &ok);
        else if (!strcmp(name, "list-style") || !strcmp(name, "list-style-type")) s->list_none = strstr(v, "none") != NULL;
        return;
    case 'm':
        if (!strcmp(name, "mask-image") || !strcmp(name, "mask")) {
            /* a picture mask is drawn; a gradient one (a fade) can't be, so the background stays off */
            s->mask_image = strstr(v, "url(") ? css_keep(v) : NULL;
            s->masked = !s->mask_image && strstr(v, "gradient(") != NULL;
            if (!strcmp(name, "mask") && s->mask_image) { /* its position, size and repeat, written as for backgrounds */
                Style t = *s;
                background_shorthand(&t, v);
                s->mask_size = t.bg_size;
                s->mask_pos = t.bg_pos;
                s->mask_repeat = t.bg_repeat;
            }
            return;
        }
        if (!strcmp(name, "mask-size")) {
            s->mask_size = css_keep(v);
            return;
        }
        if (!strcmp(name, "mask-position")) {
            s->mask_pos = css_keep(v);
            return;
        }
        if (!strcmp(name, "mask-repeat")) {
            s->mask_repeat = repeat_word(v);
            return;
        }
        if (!strcmp(name, "margin")) set_sides(s->margin_len, v, font);
        else if (!strncmp(name, "margin-", 7)) {
            const char *side = name + 7;
            Len l = parse_len(v, font, &ok);
            if (!ok) return;
            int i = side_index(side);
            if (i >= 0) s->margin_len[i] = l;
            else if (!strcmp(side, "inline")) s->margin_len[1] = s->margin_len[3] = l;
            else if (!strcmp(side, "block")) s->margin_len[0] = s->margin_len[2] = l;
        } else if (!strcmp(name, "max-width")) s->max_width = parse_len(v, font, &ok);
        else if (!strcmp(name, "min-width")) s->min_width = parse_len(v, font, &ok);
        else if (!strcmp(name, "max-height")) s->max_height = parse_len(v, font, &ok);
        else if (!strcmp(name, "min-height")) s->min_height = parse_len(v, font, &ok);
        return;
    case 'o':
        if (!strcmp(name, "order")) {
            int used;
            s->order = number100(v, &used) / 100;
        } else if (!strcmp(name, "overflow") || !strcmp(name, "overflow-x") || !strcmp(name, "overflow-y")) {
            s->overflow_hidden = word_is(v, "hidden") || word_is(v, "clip") || word_is(v, "auto") || word_is(v, "scroll");
        } else if (!strcmp(name, "opacity")) {
            int used, n = number100(v, &used);
            if (used && v[used] == '%') n /= 100;
            if (used) s->opacity = (unsigned char)(n <= 0 ? 0 : n >= 100 ? 255 : n * 255 / 100);
            if (used && n <= 5) s->hidden = 1;
        }
        return;
    case 'p':
        if (!strcmp(name, "place-items") || !strcmp(name, "place-self")) {
            int self = name[6] == 's';
            const char *second = v;
            while (*second && !is_space(*second)) second++;
            while (is_space(*second)) second++;
            int a = align_word(v, self), j = *second && *second != '!' ? align_word(second, self) : a;
            if (self) {
                s->align_self = (unsigned char)a;
                s->justify_self = (unsigned char)j;
            } else {
                s->align_items = (unsigned char)a;
                s->justify_items = (unsigned char)j;
            }
            return;
        }
        if (!strcmp(name, "place-content")) {
            const char *second = v;
            while (*second && !is_space(*second)) second++;
            while (is_space(*second)) second++;
            const char *j = *second && *second != '!' ? second : v;
            s->justify = word_is(j, "center") ? J_CENTER
                         : (word_is(j, "flex-end") || word_is(j, "end") || word_is(j, "right")) ? J_END
                         : word_is(j, "space-between") ? J_BETWEEN
                         : word_is(j, "space-around") ? J_AROUND
                         : word_is(j, "space-evenly") ? J_EVENLY
                                                      : J_START;
            return;
        }
        if (!strcmp(name, "padding")) set_sides(s->padding, v, font);
        else if (!strncmp(name, "padding-", 8)) {
            Len l = parse_len(v, font, &ok);
            if (!ok) return;
            int i = side_index(name + 8);
            if (i >= 0) s->padding[i] = l;
            else if (!strcmp(name + 8, "inline")) s->padding[1] = s->padding[3] = l;
            else if (!strcmp(name + 8, "block")) s->padding[0] = s->padding[2] = l;
        } else if (!strcmp(name, "position")) {
            s->position = word_is(v, "absolute") ? P_ABSOLUTE
                          : word_is(v, "fixed") ? P_FIXED
                          : (word_is(v, "relative") || word_is(v, "sticky")) ? P_RELATIVE
                                                                             : P_STATIC;
        }
        return;
    case 'r':
        if (!strcmp(name, "right")) s->offset[1] = parse_len(v, font, &ok);
        return;
    case 't':
        if (!strcmp(name, "text-align")) {
            s->align = word_is(v, "center") || word_is(v, "-webkit-center") ? A_CENTER
                       : (word_is(v, "right") || word_is(v, "end")) ? A_RIGHT
                                                                     : A_LEFT;
        } else if (!strcmp(name, "text-decoration") || !strcmp(name, "text-decoration-line")) {
            if (strstr(v, "underline")) s->underline = 1;
            if (strstr(v, "line-through")) s->strike = 1;
            if (strstr(v, "none")) s->underline = s->strike = 0;
        } else if (!strcmp(name, "text-indent")) {
            Len l = parse_len(v, font, &ok);
            if (ok) s->indent = l.unit == 'p' ? l.value : l.unit == '%' ? l.value * VIEW_W / 10000 : 0;
        } else if (!strcmp(name, "transform")) {
            translation(s, v, font);
        } else if (!strcmp(name, "text-transform")) {
            s->upper = word_is(v, "uppercase");
        } else if (!strcmp(name, "top")) {
            s->offset[0] = parse_len(v, font, &ok);
        }
        return;
    case 'v':
        if (!strcmp(name, "vertical-align")) {
            s->valign = word_is(v, "middle") ? VA_MIDDLE : word_is(v, "top") ? VA_TOP : word_is(v, "bottom") ? VA_BOTTOM
                        : word_is(v, "text-top") ? VA_TEXT_TOP : word_is(v, "text-bottom") ? VA_TEXT_BOTTOM
                        : word_is(v, "sub") ? VA_SUB : word_is(v, "super") ? VA_SUPER : word_is(v, "inherit") ? VA_INHERIT : VA_BASELINE;
            if (s->valign == VA_BASELINE && !word_is(v, "baseline")) {
                Len l = parse_len(v, font, &ok);
                if (ok && (l.unit == 'p' || l.unit == '%') && l.value) {
                    s->valign = VA_LENGTH;
                    s->valign_len = l;
                }
            }
            return;
        }
        if (!strcmp(name, "visibility")) s->hidden = word_is(v, "hidden") || word_is(v, "collapse");
        return;
    case 'w':
        if (!strcmp(name, "width")) s->width = parse_len(v, font, &ok);
        else if (!strcmp(name, "white-space") || !strcmp(name, "white-space-collapse") || !strcmp(name, "text-wrap")) {
            if (word_is(v, "pre") || word_is(v, "pre-wrap") || word_is(v, "break-spaces") || word_is(v, "preserve")) {
                s->pre = 1;
                s->nowrap = 0;
            } else if (word_is(v, "nowrap")) {
                s->nowrap = 1;
                s->pre = 0;
            } else if (word_is(v, "normal") || word_is(v, "pre-line") || word_is(v, "wrap") || word_is(v, "collapse")) {
                s->pre = 0;
                s->nowrap = 0;
            }
        }
        return;
    case 'z':
        if (!strcmp(name, "z-index")) {
            int used;
            s->z_index = number100(v, &used) / 100;
        }
        return;
    }
}

/* Apply "a: b; c: d" declarations. Pass 0: the normal ones; pass 1: only
   those marked !important. */
/* ---- custom properties: --name: value, used as var(--name, fallback) ---- */

static Node *var_scope;   /* the element whose style is being worked out */
static char *vars_buf;    /* custom properties being collected for it */
static int vars_len, vars_cap;

static void vars_add(const char *name, const char *value) {
    int nl = (int)strlen(name) + 1, vl = (int)strlen(value) + 1;
    if (!grow((void **)&vars_buf, &vars_cap, vars_len + nl + vl, 1)) return;
    memcpy(vars_buf + vars_len, name, (size_t)nl);
    memcpy(vars_buf + vars_len + nl, value, (size_t)vl);
    vars_len += nl + vl;
}

/* The value of custom property `name` for element e: its own, or the
   nearest ancestor's (the last definition wins). */
static const char *lookup_var(Node *e, const char *name, int len) {
    for (Node *n = e; n; n = n->parent) {
        const char *found = NULL;
        for (int i = 0; i < n->css_vars_len;) {
            const char *k = n->css_vars + i;
            int kl = (int)strlen(k);
            const char *v = k + kl + 1;
            if (kl == len && !memcmp(k, name, (size_t)len)) found = v;
            i += kl + 1 + (int)strlen(v) + 1;
        }
        if (found) return found;
    }
    return NULL;
}

/* Copy `in` to `out` with every var(...) replaced. 0 if one cannot be
   resolved (then the declaration is ignored, as browsers do). */
static char var_path[16][96]; /* the variables being worked out, outermost first */
static int var_path_n;
static char var_cycle[96];    /* a loop was found through this one (unwinding to it) */

static int resolve_vars(const char *in, char *out, int max, int depth) {
    int k = 0;
    if (depth == 0) {
        var_path_n = 0;
        var_cycle[0] = 0;
    }
    if (depth > 8) return 0;
    for (const char *p = in; *p;) {
        if (strncmp(p, "var(", 4) != 0) {
            if (k < max - 1) out[k++] = *p;
            p++;
            continue;
        }
        p += 4;
        while (is_space(*p)) p++;
        const char *name = p;
        while (*p && *p != ',' && *p != ')' && !is_space(*p)) p++;
        char lname[96];
        int nl = (int)(p - name) < 95 ? (int)(p - name) : 95;
        for (int i = 0; i < nl; i++) lname[i] = (char)tolower((unsigned char)name[i]);
        lname[nl] = 0;
        while (is_space(*p)) p++;
        const char *fallback = NULL;
        int fl = 0;
        if (*p == ',') {
            p++;
            fallback = p;
            int d = 0;
            while (*p && (d || *p != ')')) {
                if (*p == '(') d++;
                if (*p == ')') d--;
                p++;
            }
            fl = (int)(p - fallback);
        }
        if (*p == ')') p++;
        const char *v = lookup_var(var_scope, lname, nl);
        char sub[512];
        /* A variable that refers to itself (directly or through others) is
           invalid, and so are the others in the loop: no fallback inside the
           loop counts, only that of the var() the loop was entered by. */
        int in_loop = 0;
        for (int i = 0; i < var_path_n; i++)
            if (!strcmp(var_path[i], lname)) in_loop = 1;
        if (in_loop) {
            if (!var_cycle[0]) memcpy(var_cycle, lname, (size_t)nl + 1);
            return 0;
        }
        int got = 0;
        if (v && var_path_n < 16) {
            memcpy(var_path[var_path_n++], lname, (size_t)nl + 1);
            got = resolve_vars(v, sub, sizeof(sub), depth + 1);
            var_path_n--;
        }
        if (var_cycle[0]) {
            if (strcmp(var_cycle, lname)) return 0; /* (still inside the loop) */
            var_cycle[0] = 0;                       /* the loop is behind us: this var() may fall back */
        }
        /* a variable that can't be worked out (missing, or in a loop) takes the fallback */
        if (!got && fallback) {
            char fb[512];
            int n = fl < 511 ? fl : 511;
            memcpy(fb, fallback, (size_t)n);
            fb[n] = 0;
            got = resolve_vars(fb, sub, sizeof(sub), depth + 1);
        }
        if (!got) return 0;
        for (const char *q = sub; *q && k < max - 1; q++) out[k++] = *q;
    }
    out[k] = 0;
    return 1;
}

/* pass 0: normal declarations; 1: !important ones; 2: collect custom properties. */
static void apply_all(Style *s, Borders *b, const char *decls, int pass, int parent_size) {
    const char *p = decls;
    while (*p) {
        while (is_space(*p) || *p == ';') p++;
        if (!*p) break;
        char name[96];
        int n = 0;
        while (*p && *p != ':' && *p != ';' && *p != '{' && *p != '}') {
            if (!is_space(*p) && n < 95) name[n++] = (char)tolower((unsigned char)*p);
            p++;
        }
        name[n] = 0;
        if (*p != ':') {
            /* Not a declaration (perhaps a nested rule): skip it. */
            int depth = 0;
            while (*p && (depth || *p != ';')) {
                if (*p == '{') depth++;
                if (*p == '}' && --depth <= 0) {
                    p++;
                    break;
                }
                p++;
            }
            continue;
        }
        p++;
        char value[512];
        int k = 0, depth = 0;
        char quote = 0;
        while (*p && (quote || depth || (*p != ';' && *p != '}'))) {
            if (quote) {
                if (*p == quote) quote = 0;
            } else if (*p == '"' || *p == '\'') {
                quote = *p;
            } else if (*p == '(') {
                depth++;
            } else if (*p == ')') {
                depth--;
            }
            if (k < 511) value[k++] = *p;
            p++;
        }
        while (k > 0 && is_space(value[k - 1])) k--;
        value[k] = 0;
        int important = 0;
        char *bang = strstr(value, "!important");
        if (!bang) bang = strstr(value, "! important");
        if (bang) {
            *bang = 0;
            important = 1;
        }
        if (pass == 2) {
            if (name[0] == '-' && name[1] == '-') {
                const char *v = value;
                while (is_space(*v)) v++;
                vars_add(name, v);
            }
            continue;
        }
        if (!strncmp(name, "-webkit-mask", 12)) memmove(name, name + 8, strlen(name + 8) + 1); /* -webkit-mask-image -> mask-image */
        if (pass >= 3) { /* passes 3 and 4: only the font size (normal, !important), so em in the rest uses it */
            if (strcmp(name, "font-size") && strcmp(name, "font")) continue;
            if (important != pass - 3) continue;
        } else if (important != pass || name[0] == '-') continue;
        if (strstr(value, "var(")) {
            char resolved[512];
            if (!resolve_vars(value, resolved, sizeof(resolved), 0)) continue;
            apply(s, b, name, resolved, parent_size);
        } else {
            apply(s, b, name, value, parent_size);
        }
    }
}

/* ================================================================ selectors */

#define MAX_PARTS 5
enum {
    PS_FIRST = 1, PS_LAST = 2, PS_ROOT = 4, PS_EMPTY = 8, PS_CHECKED = 16, PS_DISABLED = 32, PS_NTH = 64,
    PS_ONLY = 128, PS_FOCUS = 256, PS_NTH_LAST = 512, PS_FIRST_TYPE = 1024, PS_LAST_TYPE = 2048
};

typedef struct {
    char name[24];
    char op;           /* 0 present, '=' equal, '^' starts, '$' ends, '*' contains, '~' word, '|' prefix */
    char value[48];
} AttrTest;

typedef struct {
    char tag[16];
    char id[48];
    char classes[6][64];
    int class_count;
    unsigned tag_key, id_key, class_keys[6], not_key;
    AttrTest attrs[2];
    int attr_count;
    unsigned short pseudo;
    int nth_a, nth_b;
    char not_kind;     /* :not(): '.' class, '#' id, 't' tag, 0 none */
    char not_name[40];
    char combinator;   /* how this part relates to the one on its right: ' ' '>' '+' '~' */
} Compound;

typedef struct {
    int part_count;
    unsigned need[MAX_PARTS]; /* keys the subject's ancestors must have */
    int need_count;
    unsigned key;      /* fingerprint of the rightmost part, for quick rejection */
    int specificity, origin, order;
    unsigned char pseudo_el; /* for ::before (1) or ::after (2) */
    const char *decls;
    const char *sel; /* the selector text (for CSS_DEBUG) */
    int sel_len;
    Compound parts[MAX_PARTS]; /* (last: a stored rule keeps only part_count of them) */
} Rule;

/* The page's rules: each stored at its own size, in big chunks freed with the page. */
static Rule **rules;
static int rule_count, rule_cap, rule_order;

typedef struct RuleChunk {
    struct RuleChunk *next;
    size_t used;
    char data[256 * 1024 - 64];
} RuleChunk;
static RuleChunk *rule_chunks;

static Rule *rule_store(const Rule *r) {
    size_t size = (__builtin_offsetof(Rule, parts) + sizeof(Compound) * (size_t)r->part_count + 15) & ~(size_t)15;
    if (!rule_chunks || rule_chunks->used + size > sizeof(rule_chunks->data)) {
        RuleChunk *c = malloc(sizeof(RuleChunk));
        if (!c) return NULL;
        c->next = rule_chunks;
        c->used = 0;
        rule_chunks = c;
    }
    Rule *out = (Rule *)(rule_chunks->data + rule_chunks->used);
    rule_chunks->used += size;
    memcpy(out, r, size);
    return out;
}

static void free_rules(void) {
    while (rule_chunks) {
        RuleChunk *next = rule_chunks->next;
        free(rule_chunks);
        rule_chunks = next;
    }
    free(rules);
    rules = NULL;
    rule_cap = 0;
    rule_count = rule_order = 0;
}

static unsigned fingerprint(char kind, const char *name) {
    unsigned h = 2166136261u;
    h = (h ^ (unsigned char)kind) * 16777619u;
    for (; *name; name++) h = (h ^ (unsigned char)*name) * 16777619u;
    return h ? h : 1;
}

/* Parse "2n+1", "odd", "even", "3" into a, b. */
static void parse_nth(const char *s, int *a, int *b) {
    while (is_space(*s)) s++;
    if (!strncasecmp(s, "odd", 3)) {
        *a = 2;
        *b = 1;
        return;
    }
    if (!strncasecmp(s, "even", 4)) {
        *a = 2;
        *b = 0;
        return;
    }
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    int n = 0, digits = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0'), digits++;
    if (*s == 'n' || *s == 'N') {
        *a = sign * (digits ? n : 1);
        s++;
        while (is_space(*s)) s++;
        int bs = 1;
        if (*s == '-') bs = -1;
        if (*s == '-' || *s == '+') s++;
        while (is_space(*s)) s++;
        *b = bs * atoi(s);
    } else {
        *a = 0;
        *b = sign * n;
    }
}

/* Parse one compound selector. Returns 0 if it cannot match anything we
   lay out (pseudo-elements, :hover...). */
static int parse_compound(const char *s, int n, Compound *c, int *spec) {
    memset(c, 0, sizeof(*c));
    int i = 0;
    while (i < n) {
        char ch = s[i];
        if (ch == '*') {
            i++;
            continue;
        }
        if (ch == '[') {
            int end = i;
            while (end < n && s[end] != ']') end++;
            if (c->attr_count == 2) return 0;
            AttrTest *t = &c->attrs[c->attr_count++];
            int k = 0, j = i + 1;
            while (j < end && is_space(s[j])) j++;
            while (j < end && (is_alpha(s[j]) || s[j] == '-' || s[j] == '_' || (s[j] >= '0' && s[j] <= '9'))) {
                if (k < 23) t->name[k++] = (char)tolower((unsigned char)s[j]);
                j++;
            }
            t->name[k] = 0;
            while (j < end && is_space(s[j])) j++;
            if (j < end) {
                t->op = s[j] == '=' ? '=' : s[j];
                j += s[j] == '=' ? 1 : 2;
                while (j < end && is_space(s[j])) j++;
                char q = s[j] == '"' || s[j] == '\'' ? s[j++] : 0;
                k = 0;
                while (j < end && (q ? s[j] != q : !is_space(s[j]))) {
                    if (k < 47) t->value[k++] = s[j];
                    j++;
                }
                t->value[k] = 0;
            }
            *spec += 100;
            i = end + 1;
            continue;
        }
        if (ch == ':') {
            if (i + 1 < n && s[i + 1] == ':') return 0; /* ::before, ::after, ::placeholder... */
            int j = i + 1;
            char pseudo[24];
            int k = 0;
            while (j < n && k < 23 && (is_alpha(s[j]) || s[j] == '-')) pseudo[k++] = (char)tolower((unsigned char)s[j++]);
            pseudo[k] = 0;
            char arg[64] = "";
            if (j < n && s[j] == '(') {
                int depth = 0, a = 0;
                j++;
                while (j < n && (depth || s[j] != ')')) {
                    if (s[j] == '(') depth++;
                    if (s[j] == ')') depth--;
                    if (a < 63) arg[a++] = s[j];
                    j++;
                }
                arg[a] = 0;
                j++;
            }
            i = j;
            *spec += 100;
            if (!strcmp(pseudo, "link") || !strcmp(pseudo, "any-link")) {
                strcpy(c->tag, c->tag[0] ? c->tag : "a");
            } else if (!strcmp(pseudo, "root")) c->pseudo |= PS_ROOT;
            else if (!strcmp(pseudo, "first-child")) c->pseudo |= PS_FIRST;
            else if (!strcmp(pseudo, "last-child")) c->pseudo |= PS_LAST;
            else if (!strcmp(pseudo, "only-child")) c->pseudo |= PS_ONLY;
            else if (!strcmp(pseudo, "first-of-type")) c->pseudo |= PS_FIRST_TYPE;
            else if (!strcmp(pseudo, "last-of-type")) c->pseudo |= PS_LAST_TYPE;
            else if (!strcmp(pseudo, "empty")) c->pseudo |= PS_EMPTY;
            else if (!strcmp(pseudo, "checked")) c->pseudo |= PS_CHECKED;
            else if (!strcmp(pseudo, "disabled")) c->pseudo |= PS_DISABLED;
            else if (!strcmp(pseudo, "focus") || !strcmp(pseudo, "focus-within") || !strcmp(pseudo, "focus-visible")) c->pseudo |= PS_FOCUS;
            else if (!strcmp(pseudo, "enabled") || !strcmp(pseudo, "is") || !strcmp(pseudo, "where")) {
                if (!strcmp(pseudo, "is") || !strcmp(pseudo, "where")) {
                    if (!arg[0]) return 0; /* :is() with nothing in it matches nothing */
                }
                if (arg[0] && strchr(arg, ',')) return 0; /* :is(a, b): not handled */
                if (arg[0]) { /* :is(x) / :where(x) with one simple selector */
                    Compound inner;
                    int spec2 = 0;
                    if (!parse_compound(arg, (int)strlen(arg), &inner, &spec2)) return 0;
                    if (inner.tag[0]) strcpy(c->tag, inner.tag);
                    if (inner.class_count && c->class_count < 6) strcpy(c->classes[c->class_count++], inner.classes[0]);
                }
            } else if (!strcmp(pseudo, "nth-child") || !strcmp(pseudo, "nth-last-child")) {
                c->pseudo |= pseudo[4] == 'l' ? PS_NTH_LAST : PS_NTH;
                parse_nth(arg, &c->nth_a, &c->nth_b);
            } else if (!strcmp(pseudo, "not")) {
                const char *a = arg;
                while (is_space(*a)) a++;
                if (*a == '.' || *a == '#') {
                    c->not_kind = *a;
                    a++;
                } else if (is_alpha(*a)) {
                    c->not_kind = 't';
                } else {
                    continue; /* :not(:something): ignored */
                }
                int m = 0;
                while (*a && m < 39 && (is_alpha(*a) || *a == '-' || *a == '_' || (*a >= '0' && *a <= '9'))) c->not_name[m++] = *a++;
                c->not_name[m] = 0;
                if (*a && !is_space(*a)) c->not_kind = 0; /* more complex: ignore the :not */
            } else {
                return 0; /* :hover, :active, :visited, :placeholder-shown... */
            }
            continue;
        }
        char kind = 0;
        if (ch == '.' || ch == '#') {
            kind = ch;
            i++;
        }
        char name[64];
        int k = 0, too_long = 0;
        while (i < n && s[i] != '.' && s[i] != '#' && s[i] != ':' && s[i] != '[') {
            if (s[i] == '\\' && i + 1 < n) i++; /* escaped characters in class names */
            if (k < 63) name[k++] = kind ? s[i] : (char)tolower((unsigned char)s[i]);
            else too_long = 1;
            i++;
        }
        name[k] = 0;
        if (!k || too_long) return 0;
        if ((kind == '#' && k > 47) || (!kind && k > 15)) return 0; /* (longer than the fields hold) */
        if (kind == '#') {
            strcpy(c->id, name);
            *spec += 10000;
        } else if (kind == '.') {
            if (c->class_count == 6 || k > 63) return 0;
            strcpy(c->classes[c->class_count++], name);
            *spec += 100;
        } else {
            if (k > 15) return 0;
            strcpy(c->tag, name);
            *spec += 1;
        }
    }
    return 1;
}

/* Parse one selector (no commas) into r. 0 if it cannot match anything. */
static int build_rule(const char *selector, int n, Rule *out) {
    while (n > 0 && is_space(*selector)) selector++, n--;
    while (n > 0 && is_space(selector[n - 1])) n--;
    if (n <= 0) return 0;
    /* a rule for the box made before or after an element's content */
    int pseudo_el = 0;
    static const char *const ends[] = {"::before", "::after", ":before", ":after"};
    for (int k = 0; k < 4 && !pseudo_el; k++) {
        int l = (int)strlen(ends[k]);
        if (n >= l && !strncasecmp(selector + n - l, ends[k], (size_t)l) && (n == l || selector[n - l - 1] != ':' || k < 2)) {
            pseudo_el = strstr(ends[k], "before") ? 1 : 2;
            n -= l;
        }
    }
    if (pseudo_el && (n == 0 || is_space(selector[n - 1]) || selector[n - 1] == '>' || selector[n - 1] == '+' || selector[n - 1] == '~')) {
        static const char any[] = "*"; /* "::before" alone, or "a > ::before" */
        if (n == 0) {
            selector = any;
            n = 1;
        } else return 0;
    }
    Compound parts[16];
    int count = 0, spec = 0, i = 0;
    char next_combinator = ' ';
    while (i < n) {
        int saw_space = 0;
        while (i < n && is_space(selector[i])) i++, saw_space = 1;
        if (i < n && (selector[i] == '>' || selector[i] == '+' || selector[i] == '~')) {
            next_combinator = selector[i++];
            continue;
        }
        (void)saw_space;
        if (i >= n) break;
        int start = i, depth = 0;
        char quote = 0;
        while (i < n && (depth || quote || (!is_space(selector[i]) && selector[i] != '>' && selector[i] != '+' && selector[i] != '~'))) {
            if (quote) {
                if (selector[i] == quote) quote = 0;
            } else if (selector[i] == '"' || selector[i] == '\'') {
                quote = selector[i];
            } else if (selector[i] == '(' || selector[i] == '[') {
                depth++;
            } else if (selector[i] == ')' || selector[i] == ']') {
                depth--;
            }
            i++;
        }
        if (count == 16 || !parse_compound(selector + start, i - start, &parts[count], &spec)) return 0;
        Compound *c = &parts[count];
        c->tag_key = c->tag[0] ? fingerprint('t', c->tag) : 0;
        c->id_key = c->id[0] ? fingerprint('#', c->id) : 0;
        for (int k = 0; k < c->class_count; k++) c->class_keys[k] = fingerprint('.', c->classes[k]);
        c->not_key = c->not_kind ? fingerprint(c->not_kind, c->not_name) : 0;
        parts[count].combinator = count ? next_combinator : ' ';
        next_combinator = ' ';
        count++;
    }
    if (!count) return 0;
    Rule r;
    memset(&r, 0, sizeof(r));
    /* Keep the rightmost few: a long selector then matches a little more
       widely than it should, never less. */
    int first = count > MAX_PARTS ? count - MAX_PARTS : 0;
    for (int k = first; k < count; k++) r.parts[r.part_count++] = parts[k];
    if (first) r.parts[0].combinator = ' ';
    const Compound *last = &r.parts[r.part_count - 1];
    r.key = last->id[0] ? fingerprint('#', last->id)
            : last->class_count ? fingerprint('.', last->classes[0])
            : last->tag[0] ? fingerprint('t', last->tag)
                           : 0;
    /* Parts joined to the subject by descendant and child combinators only
       are its ancestors: their keys must be among the subject's ancestors'. */
    for (int k = r.part_count - 1; k > 0; k--) {
        char comb = r.parts[k].combinator;
        if (comb != ' ' && comb != '>') break;
        const Compound *a = &r.parts[k - 1];
        unsigned key = a->id[0] ? fingerprint('#', a->id) : a->class_count ? fingerprint('.', a->classes[0]) : a->tag[0] ? fingerprint('t', a->tag) : 0;
        if (key) r.need[r.need_count++] = key;
    }
    r.specificity = spec + (pseudo_el ? 1 : 0);
    r.pseudo_el = (unsigned char)pseudo_el;
    *out = r;
    return 1;
}

static void add_rule(const char *selector, int n, const char *decls, int origin) {
    Rule r;
    if (!build_rule(selector, n, &r)) return;
    r.sel = selector;
    r.sel_len = n;
    r.origin = origin;
    r.order = rule_order++;
    r.decls = decls;
    if (!grow((void **)&rules, &rule_cap, rule_count + 1, sizeof(Rule *))) return;
    Rule *stored = rule_store(&r);
    if (stored) rules[rule_count++] = stored;
}

static int has_class(const Node *n, const char *name) {
    static char classes[4096];
    if (!attr(n, "class", classes, sizeof(classes))) return 0;
    size_t len = strlen(name);
    for (const char *p = classes; *p;) {
        while (is_space(*p)) p++;
        const char *start = p;
        while (*p && !is_space(*p)) p++;
        if ((size_t)(p - start) == len && memcmp(start, name, len) == 0) return 1;
    }
    return 0;
}

static Node *prev_element(const Node *n) {
    for (Node *p = n->prev; p; p = p->prev) {
        if (p->type == NODE_ELEMENT) return p;
    }
    return NULL;
}

static Node *next_element(const Node *n) {
    for (Node *p = n->next; p; p = p->next) {
        if (p->type == NODE_ELEMENT) return p;
    }
    return NULL;
}

static Node *focused_node; /* the form control with the keyboard (for :focus) */

static int attr_test(const Node *n, const AttrTest *t) {
    char value[256];
    if (!attr(n, t->name, value, sizeof(value))) return 0;
    size_t vl = strlen(value), tl = strlen(t->value);
    switch (t->op) {
    case 0: return 1;
    case '=': return !strcasecmp_word(value, t->value) && vl == tl;
    case '^': return tl && strncmp(value, t->value, tl) == 0;
    case '$': return tl && vl >= tl && strcmp(value + vl - tl, t->value) == 0;
    case '*': return tl && strstr(value, t->value) != NULL;
    case '|': return !strncmp(value, t->value, tl) && (value[tl] == 0 || value[tl] == '-');
    case '~': {
        for (const char *p = value; *p;) {
            while (is_space(*p)) p++;
            const char *start = p;
            while (*p && !is_space(*p)) p++;
            if ((size_t)(p - start) == tl && memcmp(start, t->value, tl) == 0) return 1;
        }
        return 0;
    }
    }
    return 0;
}

static int node_has_key(const Node *n, unsigned key) {
    for (int i = 0; i < n->class_count; i++) {
        if (n->class_keys[i] == key) return 1;
    }
    return 0;
}

static int compound_matches(const Compound *c, const Node *n) {
    if (n->type != NODE_ELEMENT) return 0;
    if (c->tag_key && c->tag_key != n->tag_key) return 0;
    if (c->id_key && c->id_key != n->id_key) return 0;
    for (int i = 0; i < c->class_count; i++) {
        if (!node_has_key(n, c->class_keys[i]) && (n->class_count < 8 || !has_class(n, c->classes[i]))) return 0;
    }
    for (int i = 0; i < c->attr_count; i++) {
        if (!attr_test(n, &c->attrs[i])) return 0;
    }
    if (c->pseudo) {
        if ((c->pseudo & PS_ROOT) && n != html_element) return 0;
        if ((c->pseudo & (PS_FIRST | PS_ONLY)) && prev_element(n)) return 0;
        if ((c->pseudo & (PS_LAST | PS_ONLY)) && next_element(n)) return 0;
        if (c->pseudo & (PS_FIRST_TYPE | PS_LAST_TYPE)) {
            for (Node *p = (c->pseudo & PS_FIRST_TYPE) ? n->prev : n->next; p; p = (c->pseudo & PS_FIRST_TYPE) ? p->prev : p->next) {
                if (p->type == NODE_ELEMENT && is(p->tag, n->tag)) return 0;
            }
        }
        if ((c->pseudo & PS_EMPTY) && n->first) return 0;
        if ((c->pseudo & PS_CHECKED) && !n->checked) return 0;
        if ((c->pseudo & PS_DISABLED) && !has_attr(n, "disabled")) return 0;
        if ((c->pseudo & PS_FOCUS) && n != focused_node) return 0;
        if (c->pseudo & (PS_NTH | PS_NTH_LAST)) {
            int index = 1;
            for (Node *p = (c->pseudo & PS_NTH) ? prev_element(n) : next_element(n); p;
                 p = (c->pseudo & PS_NTH) ? prev_element(p) : next_element(p))
                index++;
            if (c->nth_a == 0) {
                if (index != c->nth_b) return 0;
            } else {
                int diff = index - c->nth_b;
                if (diff % c->nth_a != 0 || diff / c->nth_a < 0) return 0;
            }
        }
    }
    if (c->not_kind) {
        if (c->not_kind == 't' && is(n->tag, c->not_name)) return 0;
        if (c->not_kind == '.' && (node_has_key(n, c->not_key) || (n->class_count >= 8 && has_class(n, c->not_name)))) return 0;
        if (c->not_kind == '#' && n->id_key == c->not_key) return 0;
    }
    return 1;
}

/* Does the selector, from part `p` leftwards, match with part p at `n`? */
static long stat_compound, stat_applied, stat_keyless;

static void filter_add(unsigned long long f[4], unsigned key) {
    f[(key >> 6) & 3] |= 1ull << (key & 63);
    f[(key >> 14) & 3] |= 1ull << ((key >> 8) & 63);
}

static int filter_has(const unsigned long long f[4], unsigned key) {
    return (f[(key >> 6) & 3] >> (key & 63) & 1) && (f[(key >> 14) & 3] >> ((key >> 8) & 63) & 1);
}

/* Can rule r possibly match element e, as far as e's ancestors go? */
static int ancestors_possible(const Rule *r, const Node *e) {
    for (int i = 0; i < r->need_count; i++) {
        if (!filter_has(e->ancestors, r->need[i])) return 0;
    }
    return 1;
}

static int matches_from(const Rule *r, int p, const Node *n) {
    stat_compound++;
    if (!compound_matches(&r->parts[p], n)) return 0;
    if (p == 0) return 1;
    char comb = r->parts[p].combinator;
    if (comb == '>') return n->parent && matches_from(r, p - 1, n->parent);
    if (comb == '+') {
        Node *s = prev_element(n);
        return s && matches_from(r, p - 1, s);
    }
    if (comb == '~') {
        for (Node *s = prev_element(n); s; s = prev_element(s)) {
            if (matches_from(r, p - 1, s)) return 1;
        }
        return 0;
    }
    for (Node *a = n->parent; a && a != document; a = a->parent) {
        if (matches_from(r, p - 1, a)) return 1;
    }
    return 0;
}

/* ================================================================ stylesheets */

/* One media feature, "(min-width: 40em)", for a screen VIEW_W x VIEW_H
   with a mouse, light colours, motion allowed. */
static int media_feature(const char *f) {
    while (*f == '(' || is_space(*f)) f++;
    char name[48];
    int k = 0;
    while (*f && *f != ':' && *f != ')' && *f != '<' && *f != '>' && *f != '=' && !is_space(*f) && k < 47) name[k++] = *f++;
    name[k] = 0;
    while (is_space(*f)) f++;
    int ok;
    if (*f == ':') {
        f++;
        while (is_space(*f)) f++;
        char value[64];
        int v = 0;
        while (*f && *f != ')' && v < 63) value[v++] = *f++;
        while (v && is_space(value[v - 1])) v--;
        value[v] = 0;
        int is_w = !strcmp(name, "width") || !strcmp(name, "min-width") || !strcmp(name, "max-width") ||
                   !strcmp(name, "device-width") || !strcmp(name, "min-device-width") || !strcmp(name, "max-device-width");
        int is_h = !strcmp(name, "height") || !strcmp(name, "min-height") || !strcmp(name, "max-height");
        if (is_w || is_h) {
            Len l = parse_len(value, 16, &ok);
            if (!ok || l.unit != 'p') return 1;
            int have = is_w ? VIEW_W : VIEW_H;
            if (!strncmp(name, "min-", 4)) return have >= l.value;
            if (!strncmp(name, "max-", 4)) return have <= l.value;
            return have == l.value;
        }
        if (!strcmp(name, "prefers-color-scheme")) return !strcmp(value, "light");
        if (!strcmp(name, "prefers-reduced-motion")) return strcmp(value, "reduce") != 0;
        if (!strcmp(name, "prefers-contrast")) return !strcmp(value, "no-preference");
        if (!strcmp(name, "orientation")) return !strcmp(value, VIEW_W >= VIEW_H ? "landscape" : "portrait");
        if (!strcmp(name, "hover") || !strcmp(name, "any-hover")) return !strcmp(value, "hover");
        if (!strcmp(name, "pointer") || !strcmp(name, "any-pointer")) return !strcmp(value, "fine");
        if (!strcmp(name, "min-resolution") || !strcmp(name, "min-device-pixel-ratio") || !strcmp(name, "-webkit-min-device-pixel-ratio")) {
            int used, n = number100(value, &used);
            return used && (strstr(value, "dpi") ? n <= 9600 : n <= 100); /* (one pixel per pixel) */
        }
        if (!strcmp(name, "max-resolution") || !strcmp(name, "max-device-pixel-ratio") || !strcmp(name, "-webkit-max-device-pixel-ratio")) {
            int used, n = number100(value, &used);
            return used && (strstr(value, "dpi") ? n >= 9600 : n >= 100);
        }
        if (!strcmp(name, "display-mode")) return !strcmp(value, "browser");
        if (!strcmp(name, "scripting")) return !strcmp(value, "enabled");
        if (!strcmp(name, "update")) return !strcmp(value, "fast");
        if (!strcmp(name, "forced-colors") || !strcmp(name, "inverted-colors")) return !strcmp(value, "none");
        if (!strcmp(name, "min-aspect-ratio") || !strcmp(name, "max-aspect-ratio") || !strcmp(name, "aspect-ratio")) {
            int used, a = number100(value, &used), bb = 100;
            const char *slash = strchr(value, '/');
            if (slash) bb = number100(slash + 1, &used);
            if (!a || !bb) return 1;
            long long have = (long long)VIEW_W * bb, want = (long long)VIEW_H * a;
            return name[1] == 'i' ? have >= want : name[1] == 'a' ? have <= want : have == want;
        }
        return 0; /* (a feature not known: it does not hold, as in browsers) */
    }
    if (*f == ')' || !*f) { /* (color), (hover), (grid): only those a screen with a mouse has */
        static const char *const have[] = {"color", "hover", "any-hover", "pointer", "any-pointer", "grid", "width", "height",
                                           "orientation", "aspect-ratio", "resolution", "prefers-reduced-motion", "update",
                                           "display-mode", "scripting", NULL};
        for (int i = 0; have[i]; i++)
            if (!strcmp(name, have[i])) return strcmp(name, "prefers-reduced-motion") != 0;
        return 0;
    }
    /* ranges: (width < 600px), (width >= 1024px), (400px <= width <= 700px) */
    const char *q = f;
    char op[3] = "";
    int o = 0;
    while ((*q == '<' || *q == '>' || *q == '=') && o < 2) op[o++] = *q++;
    op[o] = 0;
    while (is_space(*q)) q++;
    Len l = parse_len(q, 16, &ok);
    int have = strstr(name, "height") ? VIEW_H : VIEW_W;
    if (!ok || l.unit != 'p') return 1;
    if (!strcmp(op, "<")) return have < l.value;
    if (!strcmp(op, "<=")) return have <= l.value;
    if (!strcmp(op, ">")) return have > l.value;
    if (!strcmp(op, ">=")) return have >= l.value;
    if (!strcmp(op, "=")) return have == l.value;
    return 1;
}

/* One media query: [not|only] [type] [and (feature)]... */
static int media_query(const char *q) {
    while (is_space(*q)) q++;
    int negate = 0;
    if (!strncmp(q, "not ", 4)) {
        negate = 1;
        q += 4;
    } else if (!strncmp(q, "only ", 5)) q += 5;
    while (is_space(*q)) q++;
    int result = 1;
    if (*q && *q != '(') { /* the media type */
        char type[24];
        int k = 0;
        while (*q && !is_space(*q) && *q != '(' && k < 23) type[k++] = *q++;
        type[k] = 0;
        result = !strcmp(type, "all") || !strcmp(type, "screen");
    }
    /* features joined by "and" ("or" between them: any) */
    int any_or = strstr(q, " or ") != NULL, or_result = 0;
    for (const char *f = q; (f = strchr(f, '(')) != NULL;) {
        if (f[1] == '(') { /* ((a) and (b)): take the inner ones as they come */
            f++;
            continue;
        }
        int r = media_feature(f);
        if (any_or) or_result |= r;
        else result = result && r;
        const char *close = strchr(f, ')');
        if (!close) break;
        f = close + 1;
    }
    if (any_or) result = result && or_result;
    return negate ? !result : result;
}

/* A media query list (a <link media> or @media): does any query hold? */
static int media_applies(const char *q, int n) {
    char text[512];
    int k = 0;
    for (int i = 0; i < n && k < 511; i++) text[k++] = (char)tolower((unsigned char)q[i]);
    text[k] = 0;
    int start = 0, depth = 0, any = 0, saw = 0;
    for (int i = 0; i <= k; i++) {
        char c = i < k ? text[i] : ',';
        if (c == '(') depth++;
        else if (c == ')') depth--;
        if (c == ',' && depth <= 0) {
            text[i] = 0;
            const char *one = text + start;
            while (is_space(*one)) one++;
            if (*one) {
                saw = 1;
                any |= media_query(one);
            }
            start = i + 1;
        }
    }
    return saw ? any : 1;
}

/* @supports conditions: (property: value), not, and, or, selector(). A
   property is supported if this browser draws it. */
static int supports_eval(const char **p, const char *end);

static int supports_one(const char **p, const char *end) {
    const char *s = *p;
    while (s < end && is_space(*s)) s++;
    if (end - s >= 4 && !strncasecmp(s, "not", 3) && (is_space(s[3]) || s[3] == '(')) {
        s += 3;
        *p = s;
        return !supports_one(p, end);
    }
    int fn = end - s >= 9 && !strncasecmp(s, "selector(", 9);
    if (fn) s += 8;
    if (s >= end || *s != '(') {
        *p = end;
        return 0;
    }
    /* the bracket's inside */
    const char *in = s + 1, *close = in;
    int depth = 1;
    while (close < end && depth) {
        if (*close == '(') depth++;
        else if (*close == ')') depth--;
        if (depth) close++;
    }
    *p = close < end ? close + 1 : end;
    if (fn) return 1;
    const char *q = in;
    while (q < close && is_space(*q)) q++;
    if (q < close && (*q == '(' || !strncasecmp(q, "not", 3))) return supports_eval(&q, close); /* ((a) or (b)) */
    char prop[48];
    int n = 0;
    while (q < close && *q != ':' && n < 47) {
        if (!is_space(*q)) prop[n++] = (char)tolower((unsigned char)*q);
        q++;
    }
    prop[n] = 0;
    if (q >= close) return 0;
    q++;
    char value[96];
    int k = 0;
    while (q < close && k < 95) value[k++] = *q++;
    value[k] = 0;
    const char *pr = prop;
    if (!strncmp(pr, "-webkit-", 8)) pr += 8;
    static const char *const known[] = {
        "display", "color", "background", "background-color", "background-image", "background-size", "background-position",
        "background-repeat", "border", "border-radius", "box-shadow", "opacity", "width", "height", "min-width", "max-width",
        "min-height", "max-height", "margin", "padding", "position", "top", "left", "right", "bottom", "inset", "z-index",
        "font-family", "font-size", "font-weight", "font-style", "line-height", "text-align", "vertical-align", "float",
        "clear", "overflow", "visibility", "flex", "flex-direction", "flex-wrap", "flex-grow", "flex-shrink", "flex-basis",
        "justify-content", "align-items", "align-self", "gap", "row-gap", "column-gap", "grid-template-columns",
        "grid-template-rows", "grid-template-areas", "grid-area", "grid-column", "grid-row", "place-items", "place-content",
        "box-sizing", "transform", "mask-image", "mask", "mask-size", "mask-position", "content", "white-space",
        "text-transform", "text-decoration", "list-style", "order", "aspect-ratio", NULL};
    for (int i = 0; known[i]; i++)
        if (!strcmp(pr, known[i])) {
            if (!strcmp(pr, "display")) /* only the kinds of box laid out */
                return !strstr(value, "subgrid") && !strstr(value, "ruby") && !strstr(value, "math");
            if (!strcmp(pr, "transform")) return strstr(value, "translate") != NULL || strstr(value, "none") != NULL;
            return 1;
        }
    return 0;
}

static int supports_eval(const char **p, const char *end) {
    int result = supports_one(p, end);
    for (;;) {
        const char *s = *p;
        while (s < end && is_space(*s)) s++;
        if (end - s >= 3 && !strncasecmp(s, "and", 3)) {
            *p = s + 3;
            int r = supports_one(p, end);
            result = result && r;
        } else if (end - s >= 2 && !strncasecmp(s, "or", 2)) {
            *p = s + 2;
            int r = supports_one(p, end);
            result = result || r;
        } else return result;
    }
}

static int supports_applies(const char *q, int n) {
    const char *p = q;
    return supports_eval(&p, q + n);
}

static int block_end(const char *css, int len, int open) {
    int depth = 0;
    char quote = 0;
    for (int i = open; i < len; i++) {
        char c = css[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\') i++;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '{') {
            depth++;
        } else if (c == '}' && --depth == 0) {
            return i;
        }
    }
    return len;
}

/* Parse a stylesheet. It is modified, and must stay allocated while the
   rules are in use (they point into it). */
static void parse_css(char *css, int len, int origin) {
    for (int i = 0; i + 1 < len; i++) { /* blank out comments */
        if (css[i] == '/' && css[i + 1] == '*') {
            int j = i + 2;
            while (j + 1 < len && !(css[j] == '*' && css[j + 1] == '/')) j++;
            for (int k = i; k < j + 2 && k < len; k++) css[k] = ' ';
            i = j + 1;
        }
    }
    int i = 0;
    while (i < len) {
        while (i < len && (is_space(css[i]) || css[i] == ';' || css[i] == '}')) i++;
        if (i >= len) break;
        if (css[i] == '@') {
            int j = i;
            while (j < len && css[j] != '{' && css[j] != ';') j++;
            if (j >= len || css[j] == ';') {
                i = j + 1;
                continue;
            }
            int end = block_end(css, len, j);
            if (!strncasecmp(css + i, "@font-face", 10)) {
                font_face_rule(css + j + 1, end - j - 1);
                i = end + 1;
                continue;
            }
            int media = !strncasecmp(css + i, "@media", 6);
            int supports = !strncasecmp(css + i, "@supports", 9);
            if ((media && media_applies(css + i + 6, j - i - 6)) || (supports && supports_applies(css + i + 9, j - i - 9)) ||
                !strncasecmp(css + i, "@layer", 6) || !strncasecmp(css + i, "@container", 10)) {
                css[end] = 0;
                parse_css(css + j + 1, end - j - 1, origin);
            }
            i = end + 1;
            continue;
        }
        int open = i;
        while (open < len && css[open] != '{') open++;
        if (open >= len) break;
        int end = block_end(css, len, open);
        if (end < len) css[end] = 0;
        const char *decls = css + open + 1;
        int start = i, depth = 0;
        for (int k = i; k <= open; k++) {
            if (css[k] == '(' || css[k] == '[') depth++;
            if (css[k] == ')' || css[k] == ']') depth--;
            if ((css[k] == ',' && !depth) || k == open) {
                add_rule(css + start, k - start, decls, origin);
                start = k + 1;
            }
        }
        i = end + 1;
    }
}

static const char DEFAULT_CSS[] =
    "html,body,div,p,h1,h2,h3,h4,h5,h6,ul,ol,dl,dt,dd,pre,blockquote,form,header,footer,section,article,nav,main,"
    "aside,figure,figcaption,address,center,hr,fieldset,legend,details,summary,menu,hgroup,search,optgroup,"
    "frameset,frame,dir{display:block}"
    "li{display:list-item}"
    "head,script,style,title,meta,link,template,base,area,map,param,source,track,datalist,dialog,col,colgroup,"
    "noembed,noframes,[hidden],input[type=hidden]{display:none}"
    "table{display:table;border-spacing:2px}tr{display:table-row}td,th{display:table-cell;padding:1px}"
    "thead,tbody,tfoot{display:table-row-group}caption{display:table-caption;text-align:center}"
    "img,input,button,select,textarea,video,canvas,iframe,svg,object,embed,progress,meter{display:inline-block}"
    "body{margin:8px}"
    "h1{font-size:2em;font-weight:bold;margin:0.67em 0}"
    "h2{font-size:1.5em;font-weight:bold;margin:0.83em 0}"
    "h3{font-size:1.17em;font-weight:bold;margin:1em 0}"
    "h4,h5,h6{font-weight:bold;margin:1.33em 0}"
    "p,dl,figure{margin:1em 0}"
    "ul,ol,menu{margin:1em 0;padding-left:40px}"
    "li ul,li ol{margin:0}"
    "dd{margin-left:40px}"
    "blockquote{margin:1em 40px}"
    "figure{margin:1em 40px}"
    "pre,xmp{white-space:pre;margin:1em 0}"
    "pre,code,tt,kbd,samp,xmp,textarea{font-family:monospace;font-size:0.8125em}"
    "pre code,pre kbd,pre samp,pre tt,code code{font-size:1em}"
    "b,strong,th,dt,legend,summary{font-weight:bold}"
    "i,em,cite,var,dfn{font-style:italic}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "a{color:#0000ee;text-decoration:underline;cursor:pointer}"
    "center{text-align:center}"
    "table{text-align:left}" /* centring around a table centres the table, not its text */
    "small,sub,sup{font-size:smaller}big{font-size:larger}sub{vertical-align:sub}sup{vertical-align:super}"
    "thead,tbody,tfoot,tr{vertical-align:middle}td,th{vertical-align:inherit}"
    "mark{background-color:#ffef8a}"
    "hr{border:1px solid #c5cad6;margin:8px 0}"
    "fieldset{border:2px solid #c0c0c0;margin:0 2px;padding:6px 10px}"
    "input,select,textarea{border:1px solid #767676;background-color:#ffffff;color:#000000;padding:2px 3px}"
    "input,select,button{font-family:sans-serif;font-size:13.33px;font-weight:normal;font-style:normal}"
    "textarea{font-size:13px}"
    "button,input[type=submit],input[type=button],input[type=reset]{border:1px solid #767676;"
    "background-color:#e9e9ed;color:#000000;padding:2px 8px;cursor:pointer}"
    "input[type=checkbox],input[type=radio]{border:0;padding:0;background-color:transparent}"
    "textarea{white-space:pre}"
    "noscript{display:none}"; /* scripts run here, so <noscript> content is not shown */

static char *default_css;

/* ================================================================ the cascade */

#define INHERIT_FIELDS(s, p)            \
    do {                               \
        (s)->color = (p)->color;       \
        (s)->size = (p)->size;         \
        (s)->size64 = (p)->size64;     \
        (s)->bold = (p)->bold;         \
        (s)->italic = (p)->italic;     \
        (s)->underline = (p)->underline; \
        (s)->strike = (p)->strike;     \
        (s)->pre = (p)->pre;           \
        (s)->nowrap = (p)->nowrap;     \
        (s)->align = (p)->align;       \
        (s)->list_none = (p)->list_none; \
        (s)->upper = (p)->upper;       \
        (s)->hidden = (p)->hidden;     \
        (s)->cursor_pointer = (p)->cursor_pointer; \
        (s)->indent = (p)->indent;     \
        (s)->face = (p)->face;         \
        (s)->alpha = (p)->alpha;       \
        (s)->line_height = (p)->line_height; \
    } while (0)

/* Old HTML attributes that mean styles. */
static void presentational(Node *e, Style *s, Borders *b, const Style *parent) {
    char v[64];
    unsigned color;
    int ok;
    if (attr(e, "bgcolor", v, sizeof(v)) && parse_color(v, &color) == 1) {
        s->background = color;
        s->has_background = 1;
    }
    if (is(e->tag, "body") && attr(e, "text", v, sizeof(v)) && parse_color(v, &color) == 1) s->color = color;
    if (is(e->tag, "font")) {
        if (attr(e, "color", v, sizeof(v)) && parse_color(v, &color) == 1) s->color = color;
        if (attr(e, "size", v, sizeof(v))) {
            static const int sizes[8] = {10, 10, 13, 16, 18, 24, 32, 48};
            int n = atoi(v[0] == '+' || v[0] == '-' ? v + 1 : v);
            if (v[0] == '+') n = 3 + n;
            if (v[0] == '-') n = 3 - n;
            s->size = sizes[n < 1 ? 1 : n > 7 ? 7 : n];
        }
    }
    if (attr(e, "align", v, sizeof(v))) {
        if (is(e->tag, "table") || is(e->tag, "img")) {
            if (!strcasecmp_word(v, "center")) s->margin_len[1].unit = s->margin_len[3].unit = 'a';
            else if (!strcasecmp_word(v, "left")) s->floating = 1;
            else if (!strcasecmp_word(v, "right")) s->floating = 2;
        } else if (!strcasecmp_word(v, "center") || !strcasecmp_word(v, "middle")) {
            s->align = A_CENTER;
        } else if (!strcasecmp_word(v, "right")) {
            s->align = A_RIGHT;
        }
    }
    if (is(e->tag, "table") || is(e->tag, "td") || is(e->tag, "th") || is(e->tag, "img") || is(e->tag, "col") ||
        is(e->tag, "hr") || is(e->tag, "iframe") || is(e->tag, "canvas") || is(e->tag, "video") || is(e->tag, "embed") ||
        is(e->tag, "object") || is(e->tag, "svg")) {
        if (attr(e, "width", v, sizeof(v))) {
            Len l = parse_len(v, s->size, &ok);
            if (ok && l.unit != 'n' && l.unit != 'a') s->width = l;
        }
        if (attr(e, "height", v, sizeof(v)) && !is(e->tag, "table") && !is(e->tag, "td") && !is(e->tag, "th")) {
            Len l = parse_len(v, s->size, &ok);
            if (ok && l.unit != 'n' && l.unit != 'a') s->height = l;
        }
    }
    if ((is(e->tag, "td") || is(e->tag, "th")) && has_attr(e, "nowrap")) s->nowrap = 1;
    if (is(e->tag, "table") && attr(e, "border", v, sizeof(v)) && atoi(v) > 0) {
        for (int i = 0; i < 4; i++) {
            b->style[i] = 1;
            b->width[i] = atoi(v);
            b->color[i] = 0x808080;
            b->color_set[i] = 1;
        }
    }
    /* cellpadding on the table: padding of its cells. */
    if (is(e->tag, "td") || is(e->tag, "th")) {
        for (Node *t = e->parent; t && t != document; t = t->parent) {
            if (!is(t->tag, "table")) continue;
            if (attr(t, "cellpadding", v, sizeof(v))) {
                for (int i = 0; i < 4; i++) s->padding[i] = (Len){atoi(v), 'p', 0};
            }
            if (attr(t, "border", v, sizeof(v)) && atoi(v) > 0) {
                for (int i = 0; i < 4; i++) {
                    b->style[i] = 1;
                    b->width[i] = 1;
                    b->color[i] = 0xA0A0A0;
                    b->color_set[i] = 1;
                }
            }
            break;
        }
    }
    if (is(e->tag, "img") && attr(e, "border", v, sizeof(v)) && atoi(v) > 0) {
        for (int i = 0; i < 4; i++) {
            b->style[i] = 1;
            b->width[i] = atoi(v);
        }
    }
    (void)parent;
}

/* Rules filed by key: chains through rule_next. */
#define BUCKETS 4096
static int bucket_head[BUCKETS], keyless_head = -1, *rule_next, rule_next_cap, indexed_count = -1;

static void index_rules(void) {
    if (indexed_count == rule_count) return;
    for (int b = 0; b < BUCKETS; b++) bucket_head[b] = -1;
    keyless_head = -1;
    if (!grow((void **)&rule_next, &rule_next_cap, rule_count + 1, sizeof(int))) return;
    for (int i = rule_count - 1; i >= 0; i--) {
        int *head = rules[i]->key ? &bucket_head[rules[i]->key & (BUCKETS - 1)] : &keyless_head;
        rule_next[i] = *head;
        *head = i;
    }
    indexed_count = rule_count;
}

static char css_debug_tag[16];

static void compute_style(Node *e) {
    Style *s = &e->style;
    const Style *p = e->parent && e->parent->styled ? &e->parent->style : NULL;
    Style root;
    if (!p) {
        memset(&root, 0, sizeof(root));
        root.color = 0x000000;
        root.size = 16;
        root.size64 = 16 * 64;
        root.face = FACE_SERIF; /* (as browsers: Times New Roman unless a page says otherwise) */
        root.alpha = 255;
        p = &root;
    }
    memset(s, 0, sizeof(*s));
    INHERIT_FIELDS(s, p);
    s->display = D_INLINE;
    s->shrink = 100;
    s->basis.unit = 'a';
    s->align_self = AL_AUTO;
    s->justify_self = AL_AUTO;
    s->opacity = 255;
    for (int i = 0; i < 4; i++) {
        s->margin_len[i] = (Len){0, 'p', 0};
        s->padding[i] = (Len){0, 'p', 0};
        s->offset[i].unit = 'a';
    }
    s->width.unit = s->height.unit = 'a';
    s->min_width.unit = s->max_width.unit = s->min_height.unit = s->max_height.unit = 'n';
    Borders b;
    memset(&b, 0, sizeof(b));
    for (int i = 0; i < 4; i++) {
        b.style[i] = -1;
        b.width[i] = -1;
    }
    if (e->type != NODE_ELEMENT) {
        e->styled = 1;
        return;
    }

    /* Matching rules, in cascade order: built-in before the page's, then by
       specificity, then by order of appearance. A generated box (::before,
       ::after) takes its element's rules for it. */
    static int matched[1024];
    int n = 0;
    unsigned keys[66];
    int key_count = 0;
    Node *subject = e;
    int want = 0;
    if (e->tag[0] == ':') {
        subject = e->parent;
        want = e->tag[2] == 'b' ? 1 : 2;
    } else e->pseudo_flags = 0;
    keys[key_count++] = subject->tag_key;
    if (subject->id_key) keys[key_count++] = subject->id_key;
    for (int i = 0; i < subject->class_count; i++) keys[key_count++] = subject->class_keys[i];
    if (subject->class_count == 8) { /* more than 8 classes: rules are filed under any of them */
        static char text[4096];
        attr(subject, "class", text, sizeof(text));
        int seen = 0;
        for (char *c = text; *c && key_count < 66;) {
            while (is_space(*c)) c++;
            char *start = c;
            while (*c && !is_space(*c)) c++;
            char save = *c;
            *c = 0;
            if (*start && ++seen > 8) keys[key_count++] = fingerprint('.', start);
            *c = save;
        }
    }
    /* Only the rules filed under one of this element's keys (or under no
       key) can match it. */
    for (int k = 0; k < key_count; k++) {
        int dup = 0;
        for (int q = 0; q < k && !dup; q++) dup = keys[q] == keys[k];
        if (dup) continue;
        for (int i = bucket_head[keys[k] & (BUCKETS - 1)]; i >= 0 && n < 1024; i = rule_next[i]) {
            if (rules[i]->key == keys[k] && ancestors_possible(rules[i], subject) && matches_from(rules[i], rules[i]->part_count - 1, subject)) {
                if (rules[i]->pseudo_el == want) matched[n++] = i;
                else if (!want) e->pseudo_flags |= rules[i]->pseudo_el;
            }
        }
    }
    for (int i = keyless_head; i >= 0 && n < 1024; i = rule_next[i]) {
        stat_keyless++;
        if (ancestors_possible(rules[i], subject) && matches_from(rules[i], rules[i]->part_count - 1, subject)) {
            if (rules[i]->pseudo_el == want) matched[n++] = i;
            else if (!want) e->pseudo_flags |= rules[i]->pseudo_el;
        }
    }
    for (int i = 1; i < n; i++) {
        int r = matched[i], j = i;
        while (j > 0) {
            const Rule *a = rules[matched[j - 1]], *c = rules[r];
            int later = a->origin != c->origin ? a->origin > c->origin
                        : a->specificity != c->specificity ? a->specificity > c->specificity
                                                           : a->order > c->order;
            if (!later) break;
            matched[j] = matched[j - 1];
            j--;
        }
        matched[j] = r;
    }
    stat_applied += n;
    if (css_debug_tag[0] && is(e->tag, css_debug_tag)) { /* CSS_DEBUG=tag: show the rules that match it */
        for (int i = 0; i < n; i++) {
            const Rule *r = rules[matched[i]];
            char sel[100], dec[100];
            int sl = r->sel_len < 99 ? r->sel_len : 99, dl = 0;
            memcpy(sel, r->sel, (size_t)sl);
            sel[sl] = 0;
            for (; dl < 99 && r->decls[dl]; dl++) dec[dl] = r->decls[dl];
            dec[dl] = 0;
            printf("[css] %s { %s }\n", sel, dec);
        }
    }
    int first_page = 0;
    while (first_page < n && rules[matched[first_page]]->origin == 0) first_page++;
    static char inline_style[2048];
    int has_inline = attr(e, "style", inline_style, sizeof(inline_style));
    /* custom properties first: they may be used by any declaration */
    var_scope = e;
    vars_len = 0;
    for (int i = 0; i < n; i++) {
        if (strstr(rules[matched[i]]->decls, "--")) apply_all(s, &b, rules[matched[i]]->decls, 2, p->size);
    }
    if (has_inline && strstr(inline_style, "--")) apply_all(s, &b, inline_style, 2, p->size);
    free(e->css_vars);
    e->css_vars = NULL;
    e->css_vars_len = 0;
    if (vars_len) {
        e->css_vars = malloc((size_t)vars_len);
        if (e->css_vars) {
            memcpy(e->css_vars, vars_buf, (size_t)vars_len);
            e->css_vars_len = vars_len;
        }
    }
    e->in_svg = is(e->tag, "svg") || (e->parent && e->parent->in_svg && !is(e->parent->tag, "foreignobject"));
    svg_collecting = e->in_svg;
    svg_props_len = 0;
    parent_size64 = p->size64 ? p->size64 : p->size * 64;
    /* The font size first: a width in em written before font-size in a
       rule (or in an earlier rule) is in the element's own font size. */
    em_size = 0;
    for (int k = 3; k <= 4; k++) {
        for (int i = 0; i < n; i++)
            if (strstr(rules[matched[i]]->decls, "font")) apply_all(s, &b, rules[matched[i]]->decls, k, p->size);
        if (has_inline && strstr(inline_style, "font")) apply_all(s, &b, inline_style, k, p->size);
    }
    em_size = s->size; /* (the rules below set font-size again on the way: em still means this) */
    for (int i = 0; i < first_page; i++) apply_all(s, &b, rules[matched[i]]->decls, 0, p->size);
    presentational(e, s, &b, p);
    for (int i = first_page; i < n; i++) apply_all(s, &b, rules[matched[i]]->decls, 0, p->size);
    if (has_inline) apply_all(s, &b, inline_style, 0, p->size);
    for (int i = first_page; i < n; i++) apply_all(s, &b, rules[matched[i]]->decls, 1, p->size);
    if (has_inline) apply_all(s, &b, inline_style, 1, p->size);
    em_size = 0;
    svg_collecting = 0;
    if (!s->size64 || (s->size64 + 32) / 64 != s->size) s->size64 = s->size * 64; /* (set some other way: whole px) */
    s->alpha = (unsigned char)(p->alpha * s->opacity / 255);
    if (s->display == D_INLINE && is(e->tag, "button")) s->display = D_INLINE_BLOCK;
    if (s->valign == VA_INHERIT) {
        s->valign = p->valign;
        s->valign_len = p->valign_len;
    }
    if (e->parent && e->parent->type == NODE_ELEMENT) { /* children of flex and grid boxes are blocks (CSS: "blockified") */
        int pd = e->parent->style.display;
        if (pd == D_FLEX || pd == D_INLINE_FLEX || pd == D_GRID || pd == D_INLINE_GRID) {
            if (s->display == D_INLINE || s->display == D_INLINE_BLOCK) s->display = D_BLOCK;
            else if (s->display == D_INLINE_FLEX) s->display = D_FLEX;
            else if (s->display == D_INLINE_GRID) s->display = D_GRID;
        }
    }
    free(e->svg_css);
    e->svg_css = NULL;
    e->svg_css_len = 0;
    if (svg_props_len && (e->svg_css = malloc((size_t)svg_props_len)) != NULL) {
        memcpy(e->svg_css, svg_props, (size_t)svg_props_len);
        e->svg_css_len = svg_props_len;
    }

    for (int i = 0; i < 4; i++) {
        s->border[i] = b.style[i] == 1 ? (b.width[i] < 0 ? 3 : b.width[i]) : 0;
        s->border_color[i] = b.color_set[i] ? b.color[i] : s->color;
    }
    if (s->position == P_ABSOLUTE || s->position == P_FIXED) s->floating = 0;
    if (s->display == D_NONE) { /* not laid out at all, whatever else it says */
        s->floating = 0;
        if (s->position == P_ABSOLUTE || s->position == P_FIXED) s->position = P_STATIC;
    }
    e->styled = 1;
}

/* Work out the style of every node in the tree. */
/* Hash an element's tag, id and classes for selector matching. */
static void node_keys(Node *n) {
    static char text[1024];
    n->tag_key = fingerprint('t', n->tag);
    n->id_key = attr(n, "id", text, sizeof(text)) && text[0] ? fingerprint('#', text) : 0;
    n->class_count = 0;
    if (attr(n, "class", text, sizeof(text))) {
        for (char *c = text; *c && n->class_count < 8;) {
            while (is_space(*c)) c++;
            char *start = c;
            while (*c && !is_space(*c)) c++;
            char save = *c;
            *c = 0;
            if (*start) n->class_keys[n->class_count++] = fingerprint('.', start);
            *c = save;
        }
        /* More than 8 classes: the rest are checked the slow way (and
           class_count stays 8 to say so). */
    }
}

/* ---------------------------------------------------------------- ::before and ::after */

/* The boxes made for ::before and ::after. They are put in the tree while
   styles are worked out and the page is laid out, then taken out again
   (scripts never see them); the display list keeps pointing at them until
   the next layout. */
static Node **generated;
static int generated_count, generated_cap;

static void generated_clear(void) {
    for (int i = 0; i < generated_count; i++) free_tree(generated[i]);
    generated_count = 0;
}

static void generated_detach(void) {
    for (int i = 0; i < generated_count; i++) {
        Node *g = generated[i], *p = g->parent;
        if (!p) continue;
        if (g->prev) g->prev->next = g->next;
        else if (p->first == g) p->first = g->next;
        if (g->next) g->next->prev = g->prev;
        else if (p->last == g) p->last = g->prev;
        g->prev = g->next = NULL; /* (its parent stays: for links and clicks) */
    }
}

/* An element as clicks see it: a generated box is its element's. */
static Node *real_node(Node *n) {
    while (n && n->parent && (n->type == NODE_TEXT ? n->parent->tag[0] == ':' : n->tag[0] == ':')) n = n->parent;
    if (n && n->type == NODE_ELEMENT && n->tag[0] == ':') n = n->parent;
    return n;
}

static int is_replaced_tag(const char *t) {
    static const char *const tags[] = {"img", "input", "select", "textarea", "video", "canvas", "iframe", "svg", "object", "embed",
                                       "progress", "meter", "br", "audio", NULL};
    for (int i = 0; tags[i]; i++)
        if (!strcmp(t, tags[i])) return 1;
    return 0;
}

static void make_generated(Node *e, int which) {
    if (is_replaced_tag(e->tag)) return;
    Node *g = new_node(NODE_ELEMENT, which == 1 ? "::before" : "::after");
    if (!g || !grow((void **)&generated, &generated_cap, generated_count + 1, sizeof(Node *))) {
        free(g);
        return;
    }
    generated[generated_count++] = g;
    g->parent = e;
    if (which == 1) {
        g->next = e->first;
        if (e->first) e->first->prev = g;
        else e->last = g;
        e->first = g;
    } else {
        g->prev = e->last;
        if (e->last) e->last->next = g;
        else e->first = g;
        e->last = g;
    }
}

/* The text of a content value: strings (with \escapes), attr(), quotes. */
static void generated_text(Node *g) {
    const char *v = g->style.content;
    if (!v || word_is(v, "none") || word_is(v, "normal")) {
        g->style.display = D_NONE;
        return;
    }
    char text[512];
    int n = 0;
    for (const char *p = v; *p && n < 500;) {
        while (is_space(*p)) p++;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q && n < 500) {
                if (*p == '\\' && p[1]) {
                    p++;
                    if (hex_digit(*p) >= 0) {
                        unsigned code = 0;
                        for (int k = 0; k < 6 && hex_digit(*p) >= 0; k++) code = code * 16 + (unsigned)hex_digit(*p++);
                        if (*p == ' ') p++;
                        if (code == 0xA) code = '\n';
                        n += utf8_for(code, text + n);
                        continue;
                    }
                    if (*p == '\n') {
                        p++;
                        continue;
                    }
                }
                text[n++] = *p++;
            }
            if (*p) p++;
        } else if (!strncasecmp(p, "attr(", 5)) {
            char name[64], value[256];
            int k = 0;
            p += 5;
            while (*p && *p != ')' && k < 63) {
                if (!is_space(*p)) name[k++] = (char)tolower((unsigned char)*p);
                p++;
            }
            name[k] = 0;
            if (*p) p++;
            if (attr(g->parent, name, value, sizeof(value)))
                for (int q = 0; value[q] && n < 500; q++) text[n++] = value[q];
        } else if (!strncasecmp(p, "open-quote", 10)) {
            n += utf8_for(0x201C, text + n);
            p += 10;
        } else if (!strncasecmp(p, "close-quote", 11)) {
            n += utf8_for(0x201D, text + n);
            p += 11;
        } else { /* counters, url() pictures, no-open-quote...: nothing */
            int depth = 0;
            while (*p && (depth || !is_space(*p))) {
                if (*p == '(') depth++;
                if (*p == ')') depth--;
                p++;
            }
        }
    }
    if (!n) return;
    Node *t = new_node(NODE_TEXT, NULL);
    if (!t) return;
    t->text = copy_of(text, n);
    t->text_len = t->text ? n : 0;
    t->parent = g;
    g->first = g->last = t;
}

static void compute_styles(void) {
    const char *dbg = getenv("CSS_DEBUG");
    css_debug_tag[0] = 0;
    if (dbg) {
        int k = 0;
        for (; dbg[k] && k < 15; k++) css_debug_tag[k] = dbg[k];
        css_debug_tag[k] = 0;
    }
    index_rules();
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT) continue;
        node_keys(n);
        Node *p = n->parent;
        memset(n->ancestors, 0, sizeof(n->ancestors));
        if (p && p->type == NODE_ELEMENT) {
            memcpy(n->ancestors, p->ancestors, sizeof(n->ancestors));
            filter_add(n->ancestors, p->tag_key);
            if (p->id_key) filter_add(n->ancestors, p->id_key);
            for (int i = 0; i < p->class_count; i++) filter_add(n->ancestors, p->class_keys[i]);
            if (p->class_count == 8) { /* perhaps more than 8: add them all */
                static char text[4096];
                attr(p, "class", text, sizeof(text));
                for (char *c = text; *c;) {
                    while (is_space(*c)) c++;
                    char *start = c;
                    while (*c && !is_space(*c)) c++;
                    char save = *c;
                    *c = 0;
                    if (*start) filter_add(n->ancestors, fingerprint('.', start));
                    *c = save;
                }
            }
        }
    }
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        n->styled = 0;
    }
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type == NODE_TEXT) {
            n->style = n->parent->style;
            n->style.display = D_INLINE;
            n->styled = 1;
        } else {
            compute_style(n);
            if (n->tag[0] == ':') generated_text(n); /* (a box just made: now its text) */
            else if (n->pseudo_flags && n->style.display != D_NONE) {
                if (n->pseudo_flags & 1) make_generated(n, 1);
                if (n->pseudo_flags & 2) make_generated(n, 2);
            }
        }
    }
}

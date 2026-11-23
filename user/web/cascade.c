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
static void apply(Style *s, Borders *b, const char *name, const char *v, int parent_size) {
    unsigned color;
    int ok;
    while (is_space(*v)) v++;
    int font = s->size;
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
        if (!strcmp(name, "background-color") || !strcmp(name, "background")) {
            char parts[8][64];
            int n = split_value(v, parts, 8);
            for (int i = 0; i < n; i++) {
                int got = parse_color(parts[i], &color);
                if (got == 1) {
                    s->background = color;
                    s->has_background = 1;
                    break;
                }
                if (got == 2 || !strcasecmp_word(parts[i], "none")) s->has_background = 0;
            }
        } else if (!strcmp(name, "border")) {
            for (int i = 0; i < 4; i++) border_part(b, i, v, font);
        } else if (!strncmp(name, "border-", 7)) {
            const char *rest = name + 7;
            if (!strcmp(rest, "width") || !strcmp(rest, "style") || !strcmp(rest, "color")) {
                char p[4][64];
                int n = split_value(v, p, 4);
                for (int i = 0; i < 4 && n; i++) border_part(b, i, p[n == 1 ? 0 : n == 2 ? i % 2 : n == 3 ? (i == 3 ? 1 : i) : i], font);
            } else if (!strcmp(rest, "radius") || !strncmp(rest, "collapse", 8) || !strcmp(rest, "spacing") || !strcmp(rest, "image")) {
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
            else if (word_is(v, "inline-block") || word_is(v, "inline-table") || word_is(v, "inline-grid")) s->display = D_INLINE_BLOCK;
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
            s->size = font_size(v, parent_size);
        } else if (!strcmp(name, "font-style")) {
            s->italic = word_is(v, "italic") || word_is(v, "oblique");
        } else if (!strcmp(name, "font")) {
            char parts[8][64];
            int n = split_value(v, parts, 8);
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
                    s->size = font_size(parts[i], parent_size);
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
                    s->basis = (Len){0, 'p'};
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
        } else if (!strcmp(name, "grid-template-columns") || !strcmp(name, "grid-template")) {
            const char *rep = strstr(v, "repeat(");
            if (rep) {
                rep += 7;
                while (is_space(*rep)) rep++;
                if (!strncasecmp(rep, "auto-fill", 9) || !strncasecmp(rep, "auto-fit", 8)) {
                    const char *mm = strstr(rep, "minmax(");
                    Len l = parse_len(mm ? mm + 7 : strchr(rep, ',') ? strchr(rep, ',') + 1 : rep, font, &ok);
                    s->grid_columns = 0;
                    s->grid_min = ok && l.unit == 'p' && l.value > 0 ? l.value : 200;
                } else {
                    s->grid_columns = atoi(rep);
                }
            } else {
                char parts[16][64];
                int n = split_value(v, parts, 16), count = 0;
                for (int i = 0; i < n; i++) {
                    if (parts[i][0] != '[' && parts[i][0] != '/') count++;
                    if (parts[i][0] == '/') break;
                }
                s->grid_columns = count;
            }
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
    case 'j':
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
        if (!strcmp(name, "left")) s->offset[3] = parse_len(v, font, &ok);
        else if (!strcmp(name, "list-style") || !strcmp(name, "list-style-type")) s->list_none = strstr(v, "none") != NULL;
        return;
    case 'm':
        if (!strcmp(name, "mask-image") || !strcmp(name, "mask")) {
            s->masked = strstr(v, "url(") != NULL || strstr(v, "gradient(") != NULL;
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
            int used;
            if (number100(v, &used) <= 5 && used) s->hidden = 1;
        }
        return;
    case 'p':
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
        } else if (!strcmp(name, "text-transform")) {
            s->upper = word_is(v, "uppercase");
        } else if (!strcmp(name, "top")) {
            s->offset[0] = parse_len(v, font, &ok);
        }
        return;
    case 'v':
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
static int resolve_vars(const char *in, char *out, int max, int depth) {
    int k = 0;
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
        if (v) {
            if (!resolve_vars(v, sub, sizeof(sub), depth + 1)) return 0;
        } else if (fallback) {
            char fb[512];
            int n = fl < 511 ? fl : 511;
            memcpy(fb, fallback, (size_t)n);
            fb[n] = 0;
            if (!resolve_vars(fb, sub, sizeof(sub), depth + 1)) return 0;
        } else {
            return 0;
        }
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
        if (important != pass || name[0] == '-') continue;
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
    char classes[4][40];
    int class_count;
    unsigned tag_key, id_key, class_keys[4], not_key;
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
                    if (inner.class_count && c->class_count < 4) strcpy(c->classes[c->class_count++], inner.classes[0]);
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
        char name[48];
        int k = 0;
        while (i < n && s[i] != '.' && s[i] != '#' && s[i] != ':' && s[i] != '[') {
            if (s[i] == '\\' && i + 1 < n) i++; /* escaped characters in class names */
            if (k < 47) name[k++] = kind ? s[i] : (char)tolower((unsigned char)s[i]);
            i++;
        }
        name[k] = 0;
        if (!k) return 0;
        if (kind == '#') {
            strcpy(c->id, name);
            *spec += 10000;
        } else if (kind == '.') {
            if (c->class_count == 4 || k > 39) return 0;
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
    r.specificity = spec;
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
    char classes[256];
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

static int media_applies(const char *q, int n) {
    char text[256];
    int k = 0;
    for (int i = 0; i < n && k < 255; i++) text[k++] = (char)tolower((unsigned char)q[i]);
    text[k] = 0;
    if (strstr(text, "print") && !strstr(text, "screen")) return 0;
    if (strstr(text, "speech")) return 0;
    if (strstr(text, "prefers-color-scheme") && strstr(text, "dark")) return 0;
    if (strstr(text, "prefers-reduced-motion") && strstr(text, "reduce")) return 0;
    if (strstr(text, "orientation") && strstr(text, "portrait")) return 0;
    if (strstr(text, "hover: none") || strstr(text, "pointer: coarse") || strstr(text, "hover:none") || strstr(text, "pointer:coarse"))
        return 0; /* (we have a mouse) */
    if (!strncmp(text, "not ", 4) || strstr(text, " not ")) return 0;
    /* Width conditions, each checked against our page width. */
    for (const char *w = text; (w = strstr(w, "width")) != NULL; w += 5) {
        const char *colon = w + 5;
        while (*colon == ' ') colon++;
        int is_max = w >= text + 4 && !strncmp(w - 4, "max-", 4);
        int is_min = w >= text + 4 && !strncmp(w - 4, "min-", 4);
        int ok;
        if (*colon == ':') {
            Len l = parse_len(colon + 1, 16, &ok);
            if (!ok || l.unit != 'p') continue;
            if (is_max && VIEW_W > l.value) return 0;
            if (is_min && VIEW_W < l.value) return 0;
        } else if (*colon == '<' || *colon == '>') { /* (width < 600px), (width >= 1024px) */
            int less = *colon == '<';
            colon++;
            if (*colon == '=') colon++;
            Len l = parse_len(colon, 16, &ok);
            if (!ok || l.unit != 'p') continue;
            if (less ? VIEW_W >= l.value : VIEW_W < l.value) return 0;
        }
    }
    return 1;
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
            int media = !strncasecmp(css + i, "@media", 6);
            if ((media && media_applies(css + i + 6, j - i - 6)) || !strncasecmp(css + i, "@supports", 9) ||
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
    "pre,code,tt,kbd,samp,xmp{color:#8a2c2c}"
    "b,strong,th,dt,legend,summary{font-weight:bold}"
    "i,em,cite,var,dfn{font-style:italic}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "a{color:#1a5fd6;text-decoration:underline;cursor:pointer}"
    "center{text-align:center}"
    "table{text-align:left}" /* centring around a table centres the table, not its text */
    "small,sub,sup{font-size:smaller}big{font-size:larger}"
    "mark{background-color:#ffef8a}"
    "hr{border:1px solid #c5cad6;margin:8px 0}"
    "fieldset{border:2px solid #c0c0c0;margin:0 2px;padding:6px 10px}"
    "input,select,textarea{border:1px solid #767676;background-color:#ffffff;color:#000000;padding:2px 3px}"
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
                for (int i = 0; i < 4; i++) s->padding[i] = (Len){atoi(v), 'p'};
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
        root.color = 0x1C2030;
        root.size = 16;
        p = &root;
    }
    memset(s, 0, sizeof(*s));
    INHERIT_FIELDS(s, p);
    s->display = D_INLINE;
    s->shrink = 100;
    s->basis.unit = 'a';
    s->align_self = AL_AUTO;
    for (int i = 0; i < 4; i++) {
        s->margin_len[i] = (Len){0, 'p'};
        s->padding[i] = (Len){0, 'p'};
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
       specificity, then by order of appearance. */
    static int matched[1024];
    int n = 0;
    unsigned keys[10];
    int key_count = 0;
    keys[key_count++] = e->tag_key;
    if (e->id_key) keys[key_count++] = e->id_key;
    for (int i = 0; i < e->class_count; i++) keys[key_count++] = e->class_keys[i];
    /* Only the rules filed under one of this element's keys (or under no
       key) can match it. */
    for (int k = 0; k < key_count; k++) {
        int dup = 0;
        for (int q = 0; q < k && !dup; q++) dup = keys[q] == keys[k];
        if (dup) continue;
        for (int i = bucket_head[keys[k] & (BUCKETS - 1)]; i >= 0 && n < 1024; i = rule_next[i]) {
            if (rules[i]->key == keys[k] && ancestors_possible(rules[i], e) && matches_from(rules[i], rules[i]->part_count - 1, e))
                matched[n++] = i;
        }
    }
    for (int i = keyless_head; i >= 0 && n < 1024; i = rule_next[i]) {
        stat_keyless++;
        if (ancestors_possible(rules[i], e) && matches_from(rules[i], rules[i]->part_count - 1, e)) matched[n++] = i;
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
    for (int i = 0; i < first_page; i++) apply_all(s, &b, rules[matched[i]]->decls, 0, p->size);
    presentational(e, s, &b, p);
    for (int i = first_page; i < n; i++) apply_all(s, &b, rules[matched[i]]->decls, 0, p->size);
    if (has_inline) apply_all(s, &b, inline_style, 0, p->size);
    for (int i = first_page; i < n; i++) apply_all(s, &b, rules[matched[i]]->decls, 1, p->size);
    if (has_inline) apply_all(s, &b, inline_style, 1, p->size);

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
                static char text[1024];
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
        }
    }
}

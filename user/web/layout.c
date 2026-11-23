/* web/layout.c - turning the styled document tree into boxes and lines,
   recorded as a display list for painting. Part of the browser.

   A simplified CSS layout: block flow with margins, padding and borders;
   line boxes for text and inline boxes, flowing around floats; inline
   blocks and images; flexbox (rows, columns, wrapping, grow, shrink,
   justify-content, align-items); grid columns; tables with automatic
   column widths; absolute, fixed and relative positioning. */

/* ---------------------------------------------------------------- the display list */

enum { IT_RECT, IT_BORDER, IT_TEXT, IT_IMAGE, IT_CONTROL, IT_MARKER };

typedef struct {
    unsigned char kind, face, bold, italic, underline, strike;
    short size, base;  /* text: font size, and the baseline's distance from y */
    int x, y, w, h;
    unsigned color;
    Node *node;        /* the element it belongs to */
    int text, len;     /* IT_TEXT / IT_MARKER: offset in `pool` */
    int link;          /* -1, or the link it is part of */
    int border[4];
    unsigned border_color[4];
    unsigned char clipped;
    int clip[4];       /* x0 y0 x1 y1, when clipped */
} Item;

static Item *items;
static int item_count, item_cap;
static char *pool;
static int pool_len, pool_cap;
static char **links;
static int link_count, link_cap;
static int page_height, page_width;
static unsigned page_bg = 0xFFFFFF;
static int layout_pass; /* stamps intrinsic-size results */

static int new_item(int kind) {
    if (!grow((void **)&items, &item_cap, item_count + 1, sizeof(Item))) return -1;
    Item *it = &items[item_count];
    memset(it, 0, sizeof(*it));
    it->kind = (unsigned char)kind;
    it->link = -1;
    return item_count++;
}

static int pool_add(const char *text, int len, int upper) {
    if (!grow((void **)&pool, &pool_cap, pool_len + len + 1, 1)) return -1;
    int at = pool_len;
    for (int i = 0; i < len; i++) pool[at + i] = upper ? (char)toupper((unsigned char)text[i]) : text[i];
    pool[at + len] = 0;
    pool_len += len + 1;
    return at;
}

/* ---------------------------------------------------------------- text measures */

#define FONT_OF(s) (s)->face, (s)->bold, (s)->italic, (s)->size

/* The width of text in an element's font (upper-cased if it says so). */
static int text_w(const Style *s, const char *t, int len) {
    if (!s->upper) return font_width(FONT_OF(s), t, len);
    long long w = 0;
    char buf[128];
    for (int i = 0; i < len; i += 128) {
        int n = len - i < 128 ? len - i : 128;
        for (int k = 0; k < n; k++) buf[k] = (char)toupper((unsigned char)t[i + k]);
        w += font_width64(FONT_OF(s), buf, n);
    }
    return (int)((w + 32) / 64);
}

static int space_w(const Style *s) { return font_width(FONT_OF(s), " ", 1); }

/* A style's line height, and (in *above) where the baseline sits below the
   line's top: the font's ascent plus half the leading, as in CSS. */
static int line_metrics(const Style *s, int *above) {
    int a, d, g;
    font_metrics(FONT_OF(s), &a, &d, &g);
    int content = a + d, lh;
    if (s->line_height > 0) lh = s->line_height;
    else if (s->line_height < 0) lh = (int)(((long long)-s->line_height * s->size + 50) / 100);
    else lh = content + g;
    if (above) *above = a + (lh - content) / 2;
    return lh;
}

static int resolve(Len l, int base, int fallback) {
    if (l.unit == 'p') return l.value;
    if (l.unit == '%') return (int)((long long)l.value * base / 10000);
    if (l.unit == 'c') return (int)((long long)l.value * base / 10000) + l.px;
    return fallback;
}

static int pad(Node *n, int i, int cw) {
    int v = resolve(n->style.padding[i], cw, 0);
    return v < 0 ? 0 : v;
}

static int frame_h(Node *n, int cw) { return pad(n, 1, cw) + pad(n, 3, cw) + n->style.border[1] + n->style.border[3]; }
static int frame_v(Node *n, int cw) { return pad(n, 0, cw) + pad(n, 2, cw) + n->style.border[0] + n->style.border[2]; }

static int margin(Node *n, int i, int cw) {
    Len l = n->style.margin_len[i];
    if (l.unit == 'a') return 0;
    return resolve(l, cw, 0);
}

static int is_replaced(Node *n) {
    return n->type == NODE_ELEMENT && (is(n->tag, "img") || is(n->tag, "input") || is(n->tag, "select") ||
                                       is(n->tag, "textarea") || is(n->tag, "video") || is(n->tag, "canvas") ||
                                       is(n->tag, "iframe") || is(n->tag, "svg") || is(n->tag, "object") ||
                                       is(n->tag, "embed") || is(n->tag, "progress") || is(n->tag, "meter"));
}

static int out_of_flow(Node *n) { return n->style.position == P_ABSOLUTE || n->style.position == P_FIXED; }

/* Is this node laid out inside lines (rather than as a block)? */
static int is_inline_level(Node *n) {
    if (n->type == NODE_TEXT) return 1;
    int d = n->style.display;
    if (d == D_NONE) return 0;
    if (n->style.floating || out_of_flow(n)) return 0;
    return d == D_INLINE || d == D_INLINE_BLOCK || d == D_INLINE_FLEX || d == D_INLINE_GRID || d == D_TABLE_CELL;
}

/* ---------------------------------------------------------------- natural sizes of replaced elements */

static void control_label(Node *n, char *out, int max) {
    char type[16] = "text";
    attr(n, "type", type, sizeof(type));
    out[0] = 0;
    if (is(n->tag, "input")) {
        if (!strcasecmp_word(type, "submit") || !strcasecmp_word(type, "button") || !strcasecmp_word(type, "reset")) {
            if (!attr(n, "value", out, max)) strcpy(out, !strcasecmp_word(type, "reset") ? "Reset" : "Submit");
        }
    } else if (is(n->tag, "select")) {
        int index = 0;
        for (Node *o = n; o; o = next_in_tree(o, n)) {
            if (o->type == NODE_ELEMENT && is(o->tag, "option")) {
                if (index == n->selected_index) {
                    text_content(o, out, max);
                    return;
                }
                index++;
            }
        }
    }
}

/* The content size of a replaced element (before padding and border). */
static void svg_natural_size(Node *svg, int *w, int *h);

static void natural_size(Node *n, int *w, int *h) {
    char type[16] = "text", v[32];
    *w = 0;
    *h = 0;
    if (is(n->tag, "img")) {
        if (n->img_state == 1) {
            *w = n->img_w;
            *h = n->img_h;
        } else if (n->img_state == 2 && attr(n, "alt", v, sizeof(v)) && v[0]) {
            *w = text_w(&n->style, v, (int)strlen(v)) + 4; /* a picture that failed: its description instead */
            *h = line_metrics(&n->style, NULL);
        }
        return;
    }
    if (is(n->tag, "input")) {
        attr(n, "type", type, sizeof(type));
        if (!strcasecmp_word(type, "checkbox") || !strcasecmp_word(type, "radio")) {
            *w = *h = 13;
            return;
        }
        if (!strcasecmp_word(type, "submit") || !strcasecmp_word(type, "button") || !strcasecmp_word(type, "reset")) {
            char label[64];
            control_label(n, label, sizeof(label));
            *w = text_w(&n->style, label, (int)strlen(label));
            *h = line_metrics(&n->style, NULL);
            return;
        }
        if (!strcasecmp_word(type, "image")) {
            *w = *h = 20;
            return;
        }
        int size = attr(n, "size", v, sizeof(v)) ? atoi(v) : 20;
        /* (as browsers: size averages of the font's characters) */
        *w = (size > 0 ? size : 20) * text_w(&n->style, "x", 1) + text_w(&n->style, "W", 1) / 2;
        *h = line_metrics(&n->style, NULL);
        return;
    }
    if (is(n->tag, "select")) {
        int widest = 4;
        for (Node *o = n; o; o = next_in_tree(o, n)) {
            if (o->type == NODE_ELEMENT && is(o->tag, "option")) {
                char text[80];
                int len = text_content(o, text, sizeof(text));
                int tw = text_w(&n->style, text, len);
                if (tw > widest) widest = tw;
            }
        }
        *w = widest + 20;
        *h = line_metrics(&n->style, NULL);
        return;
    }
    if (is(n->tag, "textarea")) {
        int cols = attr(n, "cols", v, sizeof(v)) ? atoi(v) : 30, rows = attr(n, "rows", v, sizeof(v)) ? atoi(v) : 3;
        *w = (cols > 0 ? cols : 30) * text_w(&n->style, "0", 1);
        *h = (rows > 0 ? rows : 3) * line_metrics(&n->style, NULL);
        return;
    }
    if (is(n->tag, "progress") || is(n->tag, "meter")) {
        *w = 160;
        *h = 16;
        return;
    }
    if (is(n->tag, "svg")) {
        if (n->img_state == 1) {
            *w = n->img_w;
            *h = n->img_h;
        } else {
            svg_natural_size(n, w, h);
        }
        return;
    }
    *w = 300; /* video, canvas, iframe: the default object size */
    *h = 150;
}

/* Heights known before layout: a fixed one, a percentage of one, the
   viewport's for the root, or one an absolutely placed box was given. */
static Node *forced_node; /* (its border-box height is forced_h) */
static int forced_h, abs_base_h = -1;

static int definite_height(Node *n);

/* What a percentage height of n is a percentage of; -1 if that is not known. */
static int height_base(Node *n) {
    if (out_of_flow(n)) return abs_base_h;
    Node *p = n->parent;
    if (!p || p == document || p->type != NODE_ELEMENT) return VIEW_H;
    return definite_height(p);
}

/* n's content-box height if it is set before layout, else -1. */
static int definite_height(Node *n) {
    Style *s = &n->style;
    int h = -1;
    if (n == forced_node) return forced_h - frame_v(n, 0) > 0 ? forced_h - frame_v(n, 0) : 0;
    if (s->height.unit == 'p') h = s->height.value;
    else if (s->height.unit == '%' || s->height.unit == 'c') {
        int base = height_base(n);
        if (base >= 0) h = resolve(s->height, base, -1);
    }
    if (h < 0) return -1;
    if (s->box_sizing) h -= frame_v(n, 0);
    return h < 0 ? 0 : h;
}

/* The border-box size a replaced element gets in a container `cw` wide. */
static void replaced_size(Node *n, int cw, int *bw, int *bh) {
    int nw, nh;
    natural_size(n, &nw, &nh);
    Style *s = &n->style;
    int fw = frame_h(n, cw), fv = frame_v(n, cw);
    int w = resolve(s->width, cw, -1), h = resolve(s->height, 0, -1);
    if (s->height.unit == '%' || s->height.unit == 'c') {
        int base = height_base(n);
        h = base >= 0 ? resolve(s->height, base, -1) : -1;
    }
    if (n == forced_node) h = forced_h - (s->box_sizing ? 0 : fv);
    if (w >= 0 && s->box_sizing) w -= fw;
    if (h >= 0 && s->box_sizing) h -= fv;
    if (w < 0 && h < 0) {
        w = nw;
        h = nh;
    } else if (w < 0) {
        w = nh ? (int)((long long)nw * h / nh) : nw;
    } else if (h < 0) {
        h = nw ? (int)((long long)nh * w / nw) : nh;
    }
    int maxw = resolve(s->max_width, cw, -1);
    if (maxw >= 0 && s->box_sizing) maxw -= fw;
    if (maxw >= 0 && w > maxw) {
        if (w) h = (int)((long long)h * maxw / w);
        w = maxw;
    }
    int maxh = resolve(s->max_height, 0, -1);
    if (s->max_height.unit == 'p' && maxh >= 0 && h > maxh) {
        if (h) w = (int)((long long)w * maxh / h);
        h = maxh;
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    *bw = w + fw;
    *bh = h + fv;
}

/* ---------------------------------------------------------------- intrinsic widths */

static void intrinsic(Node *n, int *mn, int *mx);

static void grid_intrinsic(Node *n, int *mn, int *mx);

static void text_widths(Node *t, int *mn, int *mx) {
    const Style *s = &t->style;
    const char *p = t->text;
    int len = t->text_len;
    if (s->pre) { /* lines as they are */
        int widest = 0, start = 0;
        for (int i = 0; i <= len; i++) {
            if (i < len && p[i] != '\n') continue;
            int w = text_w(s, p + start, i - start);
            if (w > widest) widest = w;
            start = i + 1;
        }
        *mn = *mx = widest;
        return;
    }
    int space = space_w(s), longest = 0, line = 0, words = 0;
    for (int i = 0; i < len;) {
        if (is_space(p[i])) {
            i++;
            continue;
        }
        int start = i;
        while (i < len && !is_space(p[i])) i++;
        int w = text_w(s, p + start, i - start);
        if (w > longest) longest = w;
        line += (words++ ? space : 0) + w;
    }
    *mx = line;
    *mn = s->nowrap ? line : longest;
}

static void intrinsic(Node *n, int *mn, int *mx) {
    if (n->intrinsic_done == layout_pass) {
        *mn = n->min_content;
        *mx = n->max_content;
        return;
    }
    *mn = *mx = 0;
    if (n->type == NODE_TEXT) {
        text_widths(n, mn, mx);
    } else if (n->style.display == D_NONE) {
        /* nothing */
    } else if (is_replaced(n)) {
        int w, h;
        replaced_size(n, 0, &w, &h);
        *mn = *mx = w + margin(n, 1, 0) + margin(n, 3, 0);
    } else if (is(n->tag, "br")) {
        /* nothing */
    } else {
        Style *s = &n->style;
        int fixed = s->width.unit == 'p' ? s->width.value + (s->box_sizing ? 0 : frame_h(n, 0)) : -1;
        int run_mx = 0, run_mn = 0;
        int row = n->style.display == D_FLEX || n->style.display == D_INLINE_FLEX ? !s->flex_dir : 0;
        int column = (n->style.display == D_FLEX || n->style.display == D_INLINE_FLEX) && s->flex_dir;
        int table_row = n->style.display == D_TABLE_ROW;
        int gaps = 0, items = 0;
        if (s->display == D_GRID || s->display == D_INLINE_GRID) grid_intrinsic(n, mn, mx);
        else for (Node *c = n->first; c; c = c->next) {
            if (c->type == NODE_ELEMENT && (c->style.display == D_NONE || out_of_flow(c))) continue;
            if (c->type == NODE_ELEMENT && c->style.display == D_TABLE_CAPTION &&
                (n->style.display == D_TABLE || n->style.display == D_INLINE_BLOCK)) {
                int cmn, cmx;
                intrinsic(c, &cmn, &cmx); /* a caption wraps to the table's width */
                if (cmn > *mn) *mn = cmn;
                continue;
            }
            if (c->type == NODE_ELEMENT && is(c->tag, "br")) {
                if (run_mx > *mx) *mx = run_mx;
                run_mx = 0;
                continue;
            }
            int cmn, cmx;
            intrinsic(c, &cmn, &cmx);
            if (row || table_row) {
                *mx += cmx;
                *mn += row && s->flex_wrap ? 0 : cmn;
                if (row && s->flex_wrap && cmn > *mn) *mn = cmn;
                items++;
            } else if (is_inline_level(c) && !column) {
                run_mx += cmx + (c->type == NODE_TEXT && run_mx && c->text_len && is_space(c->text[0]) ? space_w(&c->style) : 0);
                if (cmn > run_mn) run_mn = cmn;
            } else {
                if (run_mx > *mx) *mx = run_mx;
                run_mx = 0;
                int extra = c->type == NODE_ELEMENT ? margin(c, 1, 0) + margin(c, 3, 0) : 0;
                if (cmx + extra > *mx) *mx = cmx + extra;
                if (cmn + extra > *mn) *mn = cmn + extra;
            }
        }
        if (row && items > 1) gaps = s->gap_col * (items - 1);
        if (table_row && items > 1) gaps = 2 * (items - 1);
        *mx += gaps;
        if (row && !s->flex_wrap) *mn += gaps;
        if (run_mx > *mx) *mx = run_mx;
        if (run_mn > *mn) *mn = run_mn;
        if (n->style.display == D_TABLE || n->style.display == D_TABLE_GROUP) {
            /* Rows: the widest row. (Already so: rows are blocks here.) */
        }
        *mn += frame_h(n, 0);
        *mx += frame_h(n, 0);
        if (fixed >= 0) {
            *mx = fixed;
            if (*mn < fixed || s->nowrap) *mn = fixed;
        }
        int maxw = s->max_width.unit == 'p' ? s->max_width.value : -1;
        if (maxw >= 0 && *mx > maxw) *mx = maxw > *mn ? maxw : *mn;
    }
    if (*mn > *mx) *mx = *mn;
    n->min_content = *mn;
    n->max_content = *mx;
    n->intrinsic_done = layout_pass;
}

/* ---------------------------------------------------------------- moving laid-out boxes */

static void shift_subtree(Node *n, int dx, int dy) {
    if (!dx && !dy) return;
    for (int i = n->items_from; i < n->items_to; i++) {
        items[i].x += dx;
        items[i].y += dy;
        if (items[i].clipped) {
            items[i].clip[0] += dx;
            items[i].clip[2] += dx;
            items[i].clip[1] += dy;
            items[i].clip[3] += dy;
        }
    }
    for (Node *d = n; d; d = next_in_tree(d, n)) {
        d->x += dx;
        d->y += dy;
    }
}

/* Make a laid-out box taller (stretching it in a flex line or table row):
   its background and border grow with it. */
static void stretch_to(Node *n, int h) {
    if (h <= n->h) return;
    n->h = h;
    if (n->bg_item >= 0) {
        items[n->bg_item].h = h;
        if (n->bg_item + 1 < item_count && items[n->bg_item + 1].kind == IT_BORDER && items[n->bg_item + 1].node == n)
            items[n->bg_item + 1].h = h;
    }
}

/* ---------------------------------------------------------------- boxes */

typedef struct {
    int x, y, w, h;
    int side; /* 1 left, 2 right */
} Float;

static void layout_box(Node *n, int x, int y, int width);
static int used_width(Node *n, int cw, int shrink);

/* Absolutely positioned boxes wait until the page is laid out, so that
   their containing blocks have their final sizes. */
typedef struct {
    Node *node;
    int static_x, static_y;
} Pending;
static Pending *pending;
static int pending_count, pending_cap;

/* ---------------------------------------------------------------- lines */

enum { AT_WORD, AT_BOX, AT_BREAK, AT_SPACE };

typedef struct {
    unsigned char kind, space_before;
    Node *node;          /* word: its text node; box: the element */
    int start, len;      /* word: part of the text */
    int w, h;
    int asc;             /* the baseline's distance from its top */
} Atom;

static Atom *atoms;
static int atom_count, atom_cap;

static void add_atom(Atom a) {
    if (!grow((void **)&atoms, &atom_cap, atom_count + 1, sizeof(Atom))) return;
    atoms[atom_count++] = a;
}

static int link_of(Node *n) {
    for (Node *a = n; a && a != document; a = a->parent) {
        if (a->type == NODE_ELEMENT && a->link >= 0) return a->link;
    }
    return -1;
}

static int pending_space; /* a space before the next word */

/* The baseline of the last line inside a box (page y), or -1. */
static int box_baseline(Node *n, int depth) {
    if (n->base_pass == layout_pass) return n->base_y;
    if (depth > 40) return -1;
    for (Node *c = n->last; c; c = c->prev) {
        if (c->type != NODE_ELEMENT || c->style.display == D_NONE || out_of_flow(c) || c->style.floating) continue;
        int b = box_baseline(c, depth + 1);
        if (b >= 0) return b;
    }
    return -1;
}

static int inline_baseline(Node *n) {
    if (is_replaced(n)) {
        char type[16] = "text";
        attr(n, "type", type, sizeof(type));
        int text = is(n->tag, "select") || is(n->tag, "textarea") ||
                   (is(n->tag, "input") && strcasecmp_word(type, "checkbox") && strcasecmp_word(type, "radio") &&
                    strcasecmp_word(type, "image"));
        if (!text) return -1;
        int a, d, g, inner = n->h - frame_v(n, 0);
        font_metrics(FONT_OF(&n->style), &a, &d, &g);
        if (is(n->tag, "textarea")) inner = a + d;
        return n->y + n->style.border[0] + pad(n, 0, 0) + (inner - a - d) / 2 + a;
    }
    if (n->style.overflow_hidden) return -1;
    return box_baseline(n, 0);
}

/* Break inline content into atoms: words, boxes, line breaks. */
static void gather(Node *n, int cw) {
    if (n->type == NODE_TEXT) {
        const char *t = n->text;
        int len = n->text_len;
        int asc, h = line_metrics(&n->style, &asc);
        if (n->style.pre) {
            int start = 0;
            for (int i = 0; i <= len; i++) {
                if (i < len && t[i] != '\n') continue;
                if (i > start) add_atom((Atom){AT_WORD, 0, n, start, i - start, text_w(&n->style, t + start, i - start), h, asc});
                if (i < len) add_atom((Atom){AT_BREAK, 0, n, 0, 0, 0, h, asc});
                start = i + 1;
            }
            return;
        }
        int i = 0;
        while (i < len) {
            if (is_space(t[i])) {
                pending_space = 1;
                i++;
                continue;
            }
            int start = i;
            while (i < len && !is_space(t[i])) i++;
            add_atom((Atom){AT_WORD, (unsigned char)pending_space, n, start, i - start, text_w(&n->style, t + start, i - start), h, asc});
            pending_space = 0;
        }
        return;
    }
    Style *s = &n->style;
    if (s->display == D_NONE) return;
    if (is(n->tag, "br")) {
        int asc, h = line_metrics(s, &asc);
        add_atom((Atom){AT_BREAK, 0, n, 0, 0, 0, h, asc});
        pending_space = 0;
        return;
    }
    if (s->display == D_INLINE && !is_replaced(n)) {
        int left = pad(n, 3, cw) + s->border[3] + margin(n, 3, cw);
        int right = pad(n, 1, cw) + s->border[1] + margin(n, 1, cw);
        if (left) add_atom((Atom){AT_SPACE, (unsigned char)pending_space, n, 1, 0, left, 0, 0}); /* (start 1: its left side) */
        if (left) pending_space = 0;
        for (Node *c = n->first; c; c = c->next) {
            if (c->type == NODE_ELEMENT && (c->style.floating || out_of_flow(c))) {
                add_atom((Atom){AT_BOX, 0, c, 0, 0, 0, 0, 0}); /* placed specially */
                continue;
            }
            gather(c, cw);
        }
        if (right) add_atom((Atom){AT_SPACE, 0, n, 2, 0, right, 0, 0});
        return;
    }
    /* An inline box: laid out now (at 0,0), moved into its line later. */
    int w = used_width(n, cw, 1);
    layout_box(n, 0, 0, w);
    int ml = margin(n, 3, cw), mr = margin(n, 1, cw), mt = margin(n, 0, cw), mb = margin(n, 2, cw);
    n->m[0] = mt;
    n->m[1] = mr;
    n->m[2] = mb;
    n->m[3] = ml;
    /* It sits on the baseline: with its last line's baseline (or, for
       pictures and the like, its bottom). */
    int base = inline_baseline(n), h = n->h + mt + mb;
    add_atom((Atom){AT_BOX, (unsigned char)pending_space, n, 0, 0, n->w + ml + mr, h, base >= 0 ? mt + base - n->y : h});
    pending_space = 0;
}

static Float *floats_now;
static int floats_count_now;

/* The left and right edges available at height y..y+h, given floats. */
static void line_edges(int left, int right, int y, int h, int *l, int *r) {
    *l = left;
    *r = right;
    for (int i = 0; i < floats_count_now; i++) {
        Float *f = &floats_now[i];
        if (y + h <= f->y || y >= f->y + f->h) continue;
        if (f->side == 1 && f->x + f->w > *l) *l = f->x + f->w;
        if (f->side == 2 && f->x < *r) *r = f->x;
    }
}

static int line_item_start; /* the first display item of the line being placed */

/* An inline element's background on this line, from x0 to x1: one box
   for all of it (its words, the spaces between, its padding). */
static void inline_bg(Node *e, int x0, int x1, int baseline) {
    int fa, fd, fg;
    font_metrics(FONT_OF(&e->style), &fa, &fd, &fg);
    int y = baseline - fa - pad(e, 0, 0), h = fa + fd + pad(e, 0, 0) + pad(e, 2, 0);
    for (int i = item_count - 1; i >= line_item_start && i >= 0; i--)
        if (items[i].kind == IT_RECT && items[i].node == e && items[i].y == y) {
            if (x1 > items[i].x + items[i].w) items[i].w = x1 - items[i].x;
            return;
        }
    int it = new_item(IT_RECT);
    if (it < 0) return;
    items[it].x = x0;
    items[it].y = y;
    items[it].w = x1 - x0;
    items[it].h = h;
    items[it].color = e->style.background;
    items[it].node = e;
}

/* Paint the words of one line: produce items. */
static void emit_word(Atom *a, int x, int baseline) {
    Node *t = a->node;
    Style *s = &t->style;
    int fa, fd, fg;
    font_metrics(FONT_OF(s), &fa, &fd, &fg);
    int top = baseline - fa, content = fa + fd;
    /* Backgrounds of inline elements (a highlighted word, a tag-like link). */
    for (Node *e = t->parent; e && e->type == NODE_ELEMENT && e->style.display == D_INLINE && !is_replaced(e); e = e->parent) {
        if (e->style.has_background) {
            inline_bg(e, x, x + a->w, baseline);
            break;
        }
    }
    /* The bounding boxes of the inline elements it is inside (for clicks). */
    for (Node *e = t->parent; e && e->type == NODE_ELEMENT && e->style.display == D_INLINE; e = e->parent) {
        if (e->w == 0 && e->h == 0) {
            e->x = x;
            e->y = top;
            e->w = a->w;
            e->h = content;
        } else {
            int x1 = e->x + e->w > x + a->w ? e->x + e->w : x + a->w;
            int y1 = e->y + e->h > top + content ? e->y + e->h : top + content;
            if (x < e->x) e->x = x;
            if (top < e->y) e->y = top;
            e->w = x1 - e->x;
            e->h = y1 - e->y;
        }
    }
    if (s->hidden) return;
    int it = new_item(IT_TEXT);
    if (it < 0) return;
    Item *item = &items[it];
    item->x = x;
    item->y = top;
    item->w = a->w;
    item->h = content;
    item->base = (short)fa;
    item->size = (short)s->size;
    item->face = s->face;
    item->color = s->color;
    item->bold = s->bold;
    item->italic = s->italic;
    item->underline = s->underline;
    item->strike = s->strike;
    item->node = t->parent;
    item->link = link_of(t);
    item->text = pool_add(t->text + a->start, a->len, s->upper);
    item->len = a->len;
}

/* Lay out inline content (the children run from..to of `parent`, or the
   whole parent when from is NULL) starting at y, between left and right.
   Returns the y below the last line. */
static int layout_lines(Node *parent, Node *from, Node *to, int left, int right, int y) {
    int saved_count = atom_count;
    int saved_space = pending_space; /* we may be inside another line's gathering */
    pending_space = 0;
    int cw = right - left;
    for (Node *c = from ? from : parent->first; c && c != to; c = c->next) {
        if (c->type == NODE_ELEMENT && (c->style.floating || out_of_flow(c))) {
            add_atom((Atom){AT_BOX, 0, c, 0, 0, 0, 0, 0});
            continue;
        }
        gather(c, cw);
    }
    Style *ps = &parent->style;
    int align = ps->align;
    int nowrap = ps->nowrap || ps->pre;
    int i = saved_count;
    int first_line = 1;
    while (i < atom_count) {
        /* Skip spaces at the start of a line. */
        int line_start = i;
        int above, lh = line_metrics(ps, &above), below = lh - above; /* (the strut) */
        int l, r;
        line_edges(left, right, y, lh, &l, &r);
        int width = 0, j = i, in_flow = 0;
        for (; j < atom_count; j++) {
            Atom *a = &atoms[j];
            if (a->kind == AT_BREAK) {
                if (a->asc > above) above = a->asc;
                if (a->h - a->asc > below) below = a->h - a->asc;
                in_flow = 1;
                j++;
                break;
            }
            if (a->kind == AT_BOX && a->node->type == NODE_ELEMENT && (a->node->style.floating || out_of_flow(a->node))) continue;
            in_flow = 1;
            int space = a->space_before && j > line_start && a->kind != AT_SPACE ? space_w(&a->node->style) : 0;
            int fits = width + space + a->w <= r - l;
            int may_wrap = !nowrap && !(a->kind == AT_WORD && a->node->style.nowrap);
            if (!fits && j > line_start && may_wrap) break;
            width += space + a->w;
            if (a->kind != AT_SPACE) {
                if (a->asc > above) above = a->asc;
                if (a->h - a->asc > below) below = a->h - a->asc;
            }
        }
        lh = above + below;
        if (j == line_start) j++; /* always make progress */
        /* Floats met in this line: placed at the line's top. */
        for (int k = line_start; k < j; k++) {
            Atom *a = &atoms[k];
            if (a->kind != AT_BOX || a->node->type != NODE_ELEMENT) continue;
            Node *f = a->node;
            if (out_of_flow(f)) {
                if (grow((void **)&pending, &pending_cap, pending_count + 1, sizeof(Pending)))
                    pending[pending_count++] = (Pending){f, l, y};
                continue;
            }
            if (!f->style.floating) continue;
            int fw = used_width(f, cw, 1);
            int fl, fr;
            line_edges(left, right, y, 1, &fl, &fr);
            int fx = f->style.floating == 1 ? fl + margin(f, 3, cw) : fr - fw - margin(f, 1, cw);
            layout_box(f, fx, y + margin(f, 0, cw), fw);
            if (floats_count_now < 64) {
                floats_now[floats_count_now++] =
                    (Float){fx - margin(f, 3, cw), y, fw + margin(f, 1, cw) + margin(f, 3, cw), f->h + margin(f, 0, cw) + margin(f, 2, cw), f->style.floating};
            }
            line_edges(left, right, y, lh, &l, &r);
        }
        /* Place the line. */
        int x = l;
        if (first_line && from == parent->first) {
            x += ps->indent;
            width += ps->indent > 0 ? ps->indent : 0;
        }
        int spare = r - l - width;
        if (spare > 0 && align == A_CENTER) x += spare / 2;
        else if (spare > 0 && align == A_RIGHT) x += spare;
        int baseline = y + above, line_items = item_count;
        line_item_start = item_count;
        if (in_flow) {
            parent->base_y = baseline;
            parent->base_pass = layout_pass;
        }
        for (int k = line_start; k < j; k++) {
            Atom *a = &atoms[k];
            if (a->kind == AT_BREAK) continue;
            if (a->kind == AT_BOX && a->node->type == NODE_ELEMENT && (a->node->style.floating || out_of_flow(a->node))) continue;
            int space = a->space_before && k > line_start && a->kind != AT_SPACE ? space_w(&a->node->style) : 0;
            /* A link spanning words: stretch its underline across the space. */
            if (space && a->kind == AT_WORD && item_count > line_items && items[item_count - 1].kind == IT_TEXT &&
                items[item_count - 1].link >= 0 && items[item_count - 1].link == link_of(a->node))
                items[item_count - 1].w += space;
            x += space;
            if (a->kind == AT_WORD) {
                emit_word(a, x, baseline);
            } else if (a->kind == AT_SPACE && a->node->type == NODE_ELEMENT && a->node->style.has_background && !a->node->style.hidden) {
                int m = a->start == 1 ? margin(a->node, 3, 0) : margin(a->node, 1, 0); /* (its padding, not its margin) */
                if (a->start == 1) inline_bg(a->node, x + m, x + a->w, baseline);
                else inline_bg(a->node, x, x + a->w - m, baseline);
            } else if (a->kind == AT_BOX) {
                Node *b = a->node;
                shift_subtree(b, x + b->m[3] - b->x, baseline - a->asc + b->m[0] - b->y);
            }
            x += a->w;
        }
        if (in_flow) y += lh; /* a "line" of only floats takes no room */
        i = j;
        first_line = 0;
    }
    atom_count = saved_count;
    pending_space = saved_space;
    return y;
}

/* ---------------------------------------------------------------- block flow */

/* The border-box width a block-level box gets in a container `cw` wide.
   `shrink`: size to the content (floats, inline blocks, flex items...). */
static int used_width(Node *n, int cw, int shrink) {
    Style *s = &n->style;
    if (is_replaced(n)) {
        int w, h;
        replaced_size(n, cw, &w, &h);
        return w;
    }
    int fw = frame_h(n, cw);
    int avail = cw - margin(n, 1, cw) - margin(n, 3, cw);
    int w = resolve(s->width, cw, -1);
    if (w >= 0) {
        if (!s->box_sizing) w += fw;
    } else if (s->width.unit == 'x' || s->width.unit == 'm' || s->width.unit == 'f') {
        int mn, mx;
        intrinsic(n, &mn, &mx);
        w = s->width.unit == 'x' ? mx : s->width.unit == 'm' ? mn : mx < avail ? mx : avail;
        if (w < mn) w = mn;
    } else if (shrink || s->display == D_TABLE) {
        int mn, mx;
        intrinsic(n, &mn, &mx);
        w = mx < avail ? mx : avail;
        if (w < mn) w = mn;
        if (s->display == D_TABLE && !shrink && w < mn) w = mn;
    } else {
        w = avail;
    }
    int maxw = resolve(s->max_width, cw, -1);
    if (maxw >= 0) {
        if (!s->box_sizing) maxw += fw;
        if (w > maxw) w = maxw;
    }
    int minw = resolve(s->min_width, cw, -1);
    if (minw >= 0) {
        if (!s->box_sizing) minw += fw;
        if (w < minw) w = minw;
    }
    if (w < fw) w = fw;
    return w;
}

static void list_marker(Node *li, int x, int y) {
    if (li->style.list_none || li->style.hidden) return;
    char marker[16];
    Node *list = li->parent;
    if (list && is(list->tag, "ol")) {
        char v[16];
        int number = attr(list, "start", v, sizeof(v)) ? atoi(v) : 1;
        for (Node *p = list->first; p && p != li; p = p->next) {
            if (p->type == NODE_ELEMENT && p->style.display == D_LIST_ITEM) number++;
        }
        int k = 0;
        char digits[12];
        do digits[k++] = (char)('0' + number % 10);
        while (number /= 10);
        int m = 0;
        while (k) marker[m++] = digits[--k];
        marker[m++] = '.';
        marker[m] = 0;
    } else {
        int nested = 0;
        for (Node *p = li->parent; p && p != document; p = p->parent) nested += is(p->tag, "ul") || is(p->tag, "ol");
        strcpy(marker, nested > 1 ? "-" : "*");
    }
    int len = (int)strlen(marker);
    int it = new_item(IT_MARKER);
    if (it < 0) return;
    int above, fa, fd, fg, w = text_w(&li->style, marker, len);
    line_metrics(&li->style, &above);
    font_metrics(FONT_OF(&li->style), &fa, &fd, &fg);
    items[it].x = x - w - space_w(&li->style);
    items[it].y = y + above - fa;
    items[it].w = w;
    items[it].h = fa + fd;
    items[it].base = (short)fa;
    items[it].size = (short)li->style.size;
    items[it].face = li->style.face;
    items[it].color = li->style.color;
    items[it].text = pool_add(marker, len, 0);
    items[it].len = len;
    items[it].node = li;
}

/* Lay out the children of a block container: blocks one under another,
   inline content in lines, floats to the sides. Returns the height used. */
/* ---------------------------------------------------------------- collapsing margins */

/* Two margins that meet become one: the larger, or for negative ones as
   CSS says (the largest positive plus the most negative). */
static int combine_margins(int a, int b) {
    if (a >= 0 && b >= 0) return a > b ? a : b;
    if (a < 0 && b < 0) return a < b ? a : b;
    return a + b;
}

/* Do a block's margins meet its children's at the top (bottom)? Not
   through a border, padding, or a box with its own formatting context. */
static int margins_meet(Node *n, int bottom) {
    if (n->type != NODE_ELEMENT || n == html_element || is_replaced(n)) return 0;
    Style *s = &n->style;
    if (s->display != D_BLOCK && s->display != D_LIST_ITEM) return 0;
    if (s->overflow_hidden || s->floating || out_of_flow(n)) return 0;
    if (n->parent && n->parent->type == NODE_ELEMENT) {
        int pd = n->parent->style.display;
        if (pd == D_FLEX || pd == D_INLINE_FLEX || pd == D_GRID || pd == D_INLINE_GRID) return 0;
    }
    if (bottom) return !s->border[2] && !pad(n, 2, 0) && (s->height.unit == 'a' || s->height.unit == 'n') && s->min_height.unit != 'p';
    return !s->border[0] && !pad(n, 0, 0);
}

/* The first child that is a block in the flow (NULL if text or an inline
   box comes first). */
static Node *first_flow_block(Node *n) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type == NODE_TEXT) {
            for (int i = 0; i < c->text_len; i++)
                if (!is_space(c->text[i]) || c->style.pre) return NULL;
            continue;
        }
        if (c->style.display == D_NONE || out_of_flow(c) || c->style.floating) continue;
        return is_inline_level(c) ? NULL : c;
    }
    return NULL;
}

/* A block's top margin with those of the first children it meets. */
static int top_margin(Node *c, int cw, int depth) {
    int m = margin(c, 0, cw);
    Node *f;
    if (depth < 32 && margins_meet(c, 0) && (f = first_flow_block(c)) != NULL) m = combine_margins(m, top_margin(f, cw, depth + 1));
    return m;
}

static int layout_flow(Node *n, int left, int top, int width) {
    int y = top;
    int prev_margin = 0;
    int at_start = 1, hoisted_top = margins_meet(n, 0); /* (the first child's top margin went up into ours) */
    Float *saved_floats = floats_now;
    int saved_floats_count = floats_count_now;
    static Float float_store[64][64];
    static int float_depth;
    if (float_depth < 64) {
        floats_now = float_store[float_depth];
        floats_count_now = 0;
    }
    float_depth++;
    int right = left + width;
    Node *run_start = NULL;
    for (Node *c = n->first;; c = c->next) {
        int inline_child = c && is_inline_level(c);
        if (c && !inline_child && c->type == NODE_ELEMENT && (c->style.floating || out_of_flow(c)) && run_start) inline_child = 1;
        if (c && inline_child) {
            if (!run_start) run_start = c;
            continue;
        }
        if (run_start) {
            /* Only whitespace? Then it makes no line. */
            int content = 0;
            for (Node *k = run_start; k != c && !content; k = k->next) {
                if (k->type == NODE_ELEMENT) content = 1;
                else
                    for (int i = 0; i < k->text_len && !content; i++) content = !is_space(k->text[i]) || k->style.pre;
            }
            if (content) {
                y += prev_margin;
                prev_margin = 0;
                at_start = 0;
                y = layout_lines(n, run_start, c, left, right, y);
            }
            run_start = NULL;
        }
        if (!c) break;
        if (c->type != NODE_ELEMENT || c->style.display == D_NONE) continue;
        if (out_of_flow(c)) {
            if (grow((void **)&pending, &pending_cap, pending_count + 1, sizeof(Pending)))
                pending[pending_count++] = (Pending){c, left, y + prev_margin};
            continue;
        }
        if (c->style.floating) {
            int fw = used_width(c, width, 1);
            int fl, fr;
            line_edges(left, right, y + prev_margin, 1, &fl, &fr);
            int fy = y + prev_margin;
            int fx = c->style.floating == 1 ? fl + margin(c, 3, width) : fr - fw - margin(c, 1, width);
            layout_box(c, fx, fy + margin(c, 0, width), fw);
            if (floats_count_now < 64) {
                floats_now[floats_count_now++] = (Float){fx - margin(c, 3, width), fy, fw + margin(c, 1, width) + margin(c, 3, width),
                                                         c->h + margin(c, 0, width) + margin(c, 2, width), c->style.floating};
            }
            continue;
        }
        /* A block. */
        if (c->style.clear) {
            for (int i = 0; i < floats_count_now; i++) {
                Float *f = &floats_now[i];
                if ((c->style.clear & f->side) && f->y + f->h > y) y = f->y + f->h;
            }
        }
        int mt = at_start && hoisted_top ? 0 : top_margin(c, width, 0);
        int gap = combine_margins(prev_margin, mt);
        int by = y + gap;
        /* Floats beside it narrow it (as if it started a new formatting context). */
        int bl, br;
        line_edges(left, right, by, 1, &bl, &br);
        int avail = br - bl;
        int w = used_width(c, avail, 0);
        int x = bl + margin(c, 3, avail);
        /* Auto side margins centre a narrower box. */
        int spare = avail - w - margin(c, 3, avail) - margin(c, 1, avail);
        if (spare > 0) {
            int la = c->style.margin_len[3].unit == 'a', ra = c->style.margin_len[1].unit == 'a';
            if (la && ra) x += spare / 2;
            else if (la) x += spare;
        }
        if (c->style.display == D_LIST_ITEM) list_marker(c, x + pad(c, 3, avail) + c->style.border[3], by + pad(c, 0, avail) + c->style.border[0]);
        layout_box(c, x, by, w);
        /* its bottom margin, with its last child's if they meet */
        int mb = margin(c, 2, width);
        if (margins_meet(c, 1)) mb = combine_margins(mb, c->trail_margin);
        at_start = 0;
        if (c->h == 0 && margins_meet(c, 0) && margins_meet(c, 1)) {
            prev_margin = combine_margins(gap, mb); /* an empty block: its margins collapse through it */
            continue;
        }
        y = by + c->h;
        prev_margin = mb;
    }
    n->trail_margin = 0;
    if (margins_meet(n, 1)) n->trail_margin = prev_margin; /* (it becomes part of ours) */
    else y += prev_margin > 0 ? prev_margin : 0;
    /* Contain the floats. */
    for (int i = 0; i < floats_count_now; i++) {
        if (floats_now[i].y + floats_now[i].h > y) y = floats_now[i].y + floats_now[i].h;
    }
    float_depth--;
    floats_now = saved_floats;
    floats_count_now = saved_floats_count;
    return y - top;
}

/* ---------------------------------------------------------------- flexbox */

typedef struct {
    Node *node;
    int basis, size, min, max_c, grow, shrink;
} FlexItem;

static int flex_order_cmp(const FlexItem *a, const FlexItem *b) { return a->node->style.order - b->node->style.order; }

static int layout_flex(Node *n, int left, int top, int width) {
    Style *s = &n->style;
    FlexItem list[256];
    int count = 0;
    for (Node *c = n->first; c && count < 256; c = c->next) {
        if (c->type == NODE_TEXT) {
            int blank = 1;
            for (int i = 0; i < c->text_len && blank; i++) blank = is_space(c->text[i]);
            if (blank) continue;
        } else if (c->style.display == D_NONE) {
            continue;
        } else if (out_of_flow(c)) {
            if (grow((void **)&pending, &pending_cap, pending_count + 1, sizeof(Pending))) pending[pending_count++] = (Pending){c, left, top};
            continue;
        }
        FlexItem it = {c, 0, 0, 0, 0, 0, 0};
        list[count++] = it;
    }
    /* order: a stable sort */
    for (int i = 1; i < count; i++) {
        FlexItem t = list[i];
        int j = i;
        while (j > 0 && flex_order_cmp(&list[j - 1], &t) > 0) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = t;
    }
    if (s->flex_dir) { /* column: one under another */
        int y = top;
        for (int i = 0; i < count; i++) {
            Node *c = list[i].node;
            if (c->type == NODE_TEXT) {
                y = layout_lines(n, c, c->next, left, left + width, y);
                continue;
            }
            int align = c->style.align_self != AL_AUTO ? c->style.align_self : s->align_items;
            int shrink = align != AL_STRETCH || is_replaced(c);
            int w = used_width(c, width, shrink);
            int x = left + margin(c, 3, width);
            int spare = width - w - margin(c, 3, width) - margin(c, 1, width);
            if (spare > 0) {
                if (c->style.margin_len[3].unit == 'a' && c->style.margin_len[1].unit == 'a') x += spare / 2;
                else if (c->style.margin_len[3].unit == 'a') x += spare;
                else if (align == AL_CENTER) x += spare / 2;
                else if (align == AL_END) x += spare;
            }
            y += margin(c, 0, width);
            layout_box(c, x, y, w);
            y += c->h + margin(c, 2, width) + (i < count - 1 ? s->gap_row : 0);
        }
        int known = definite_height(n);
        if (known < 0 && s->min_height.unit == 'p') known = s->min_height.value - (s->box_sizing ? frame_v(n, 0) : 0);
        int spare = known - (y - top);
        if (spare > 0) { /* room left: items that grow take it, else justify-content places them */
            int grow_total = 0;
            for (int i = 0; i < count; i++)
                if (list[i].node->type == NODE_ELEMENT) grow_total += list[i].node->style.grow;
            int moved = 0;
            if (grow_total > 0) {
                for (int i = 0; i < count; i++) {
                    Node *c = list[i].node;
                    if (c->type != NODE_ELEMENT) continue;
                    if (moved) shift_subtree(c, 0, moved);
                    if (c->style.grow > 0) {
                        int add = (int)((long long)spare * c->style.grow / grow_total);
                        stretch_to(c, c->h + add);
                        moved += add;
                    }
                }
                y += moved;
            } else if (s->justify == J_CENTER || s->justify == J_END) {
                int dy = s->justify == J_CENTER ? spare / 2 : spare;
                for (int i = 0; i < count; i++)
                    if (list[i].node->type == NODE_ELEMENT) shift_subtree(list[i].node, 0, dy);
            } else if ((s->justify == J_BETWEEN || s->justify == J_AROUND || s->justify == J_EVENLY) && count > 0) {
                int slots = s->justify == J_BETWEEN ? count - 1 : s->justify == J_AROUND ? count : count + 1;
                int each = slots > 0 ? spare / slots : 0, first = s->justify == J_AROUND ? each / 2 : s->justify == J_EVENLY ? each : 0;
                if (s->justify == J_BETWEEN && count == 1) first = 0;
                for (int i = 0; i < count; i++)
                    if (list[i].node->type == NODE_ELEMENT) shift_subtree(list[i].node, 0, first + each * i);
            }
        }
        return y - top;
    }
    /* row: the main event */
    for (int i = 0; i < count; i++) {
        FlexItem *f = &list[i];
        Node *c = f->node;
        int mn, mx;
        intrinsic(c, &mn, &mx);
        if (c->type == NODE_TEXT) {
            f->basis = mx;
            f->min = mn;
            f->grow = 0;
            f->shrink = 100;
            continue;
        }
        Style *cs = &c->style;
        int margins = margin(c, 1, width) + margin(c, 3, width);
        int basis = resolve(cs->basis, width, -1);
        if (basis >= 0 && !cs->box_sizing && cs->basis.unit != 'a') basis += frame_h(c, width);
        if (basis < 0) basis = cs->width.unit != 'a' && cs->width.unit != 'n' ? used_width(c, width, 0) : (mx < width ? mx : width);
        if (is_replaced(c)) basis = used_width(c, width, 1);
        int maxw = resolve(cs->max_width, width, -1);
        if (maxw >= 0 && basis > maxw + (cs->box_sizing ? 0 : frame_h(c, width))) basis = maxw + (cs->box_sizing ? 0 : frame_h(c, width));
        int minw = resolve(cs->min_width, width, -1);
        f->min = cs->min_width.unit == 'p' || cs->min_width.unit == '%' ? minw + (cs->box_sizing ? 0 : frame_h(c, width)) : mn;
        if (cs->min_width.unit == 'n' && cs->overflow_hidden) f->min = 0;
        if (f->min > basis && cs->basis.unit != 'a') f->min = basis < mn ? basis : f->min;
        f->basis = basis + margins;
        f->min += margins;
        f->grow = cs->grow;
        f->shrink = cs->shrink;
    }
    int y = top;
    int i = 0;
    while (i < count) {
        /* One line of items. */
        int j = i, sum = 0;
        while (j < count) {
            int add = list[j].basis + (j > i ? s->gap_col : 0);
            if (s->flex_wrap && j > i && sum + add > width) break;
            sum += add;
            j++;
        }
        int free = width - sum;
        int grow_total = 0;
        long long shrink_total = 0;
        for (int k = i; k < j; k++) {
            grow_total += list[k].grow;
            shrink_total += (long long)list[k].shrink * list[k].basis;
        }
        for (int k = i; k < j; k++) {
            FlexItem *f = &list[k];
            f->size = f->basis;
            if (free > 0 && grow_total > 0) f->size += (int)((long long)free * f->grow / grow_total);
            else if (free < 0 && shrink_total > 0) f->size += (int)((long long)free * f->shrink * f->basis / shrink_total);
            if (f->size < f->min) f->size = f->min;
        }
        int used = (j - i - 1) * s->gap_col;
        for (int k = i; k < j; k++) used += list[k].size;
        int left_over = width - used;
        /* Auto margins soak up free space first. */
        int auto_margins = 0;
        for (int k = i; k < j; k++) {
            Node *c = list[k].node;
            if (c->type == NODE_ELEMENT) auto_margins += (c->style.margin_len[3].unit == 'a') + (c->style.margin_len[1].unit == 'a');
        }
        int x = left, gap = s->gap_col;
        if (left_over > 0 && !auto_margins) {
            int items_here = j - i;
            switch (s->justify) {
            case J_CENTER: x += left_over / 2; break;
            case J_END: x += left_over; break;
            case J_BETWEEN: if (items_here > 1) gap += left_over / (items_here - 1); break;
            case J_AROUND: gap += left_over / items_here; x += left_over / items_here / 2; break;
            case J_EVENLY: gap += left_over / (items_here + 1); x += left_over / (items_here + 1); break;
            }
        }
        int line_h = 0;
        for (int k = i; k < j; k++) {
            FlexItem *f = &list[k];
            Node *c = f->node;
            if (c->type == NODE_TEXT) {
                int start_items = item_count;
                int h = layout_lines(n, c, c->next, x, x + f->size, y) - y;
                (void)start_items;
                if (h > line_h) line_h = h;
                x += f->size + gap;
                continue;
            }
            int ml = margin(c, 3, width), mr = margin(c, 1, width);
            if (left_over > 0 && auto_margins) {
                if (c->style.margin_len[3].unit == 'a') ml += left_over / auto_margins;
                if (c->style.margin_len[1].unit == 'a') mr += left_over / auto_margins;
            }
            int w = f->size - margin(c, 3, width) - margin(c, 1, width);
            if (w < 0) w = 0;
            layout_box(c, x + ml, y + margin(c, 0, width), w);
            int outer = c->h + margin(c, 0, width) + margin(c, 2, width);
            if (outer > line_h) line_h = outer;
            x += ml + w + mr + gap;
        }
        if (!s->flex_wrap) { /* one line: as tall as the container, if that is known */
            int known = definite_height(n);
            if (known > line_h) line_h = known;
        }
        /* Cross-axis alignment within the line. */
        for (int k = i; k < j; k++) {
            Node *c = list[k].node;
            if (c->type == NODE_TEXT) continue;
            int align = c->style.align_self != AL_AUTO ? c->style.align_self : s->align_items;
            int outer = c->h + margin(c, 0, width) + margin(c, 2, width);
            if (align == AL_STRETCH && (c->style.height.unit == 'a' || c->style.height.unit == 'n') && !is_replaced(c))
                stretch_to(c, line_h - margin(c, 0, width) - margin(c, 2, width));
            else if (align == AL_CENTER) shift_subtree(c, 0, (line_h - outer) / 2);
            else if (align == AL_END) shift_subtree(c, 0, line_h - outer);
        }
        y += line_h;
        i = j;
        if (i < count) y += s->gap_row;
    }
    return y - top;
}

/* ---------------------------------------------------------------- grid */

/* A track's size: 'p' px, '%' percent * 100, 'f' fr * 100, 'a' auto,
   'n' min-content, 'x' max-content, 'c' fit-content(px). */
typedef struct {
    char kind;
    int v;
} GSize;

typedef struct {
    GSize min, max;
    unsigned char autofit; /* from repeat(auto-fit, ...): dropped when empty */
} GTrack;

typedef struct {
    Node *node;          /* an element, or a text node (an anonymous item) */
    int r0, r1, c0, c1;  /* the lines it spans (from 0) */
    int mn, mx;          /* its min- and max-content widths, margins included */
    int h;               /* its height once laid out, margins included */
} GItem;

#define GRID_COLS 64
#define GRID_ROWS 2048

typedef struct {
    GTrack cols[GRID_COLS], *rows;
    int ncols, nrows, explicit_cols, explicit_rows, rows_cap;
    GTrack auto_col, auto_row;
    int col_size[GRID_COLS];
    GItem *items;
    int nitems;
    unsigned long long *used; /* per row: the columns taken */
    int used_rows;
} Grid;

static GSize grid_size(const char *t, int len, int font, int base) {
    char buf[64];
    if (len > 63) len = 63;
    memcpy(buf, t, (size_t)len);
    buf[len] = 0;
    if (!strncasecmp(buf, "auto", 4)) return (GSize){'a', 0};
    if (!strncasecmp(buf, "min-content", 11)) return (GSize){'n', 0};
    if (!strncasecmp(buf, "max-content", 11)) return (GSize){'x', 0};
    int used, n = number100(buf, &used);
    if (used && !strncasecmp(buf + used, "fr", 2)) return (GSize){'f', n};
    int ok;
    Len l = parse_len(buf, font, &ok);
    if (!ok) return (GSize){'a', 0};
    if (l.unit == '%') {
        if (base >= 0) return (GSize){'p', (int)((long long)l.value * base / 10000)};
        return (GSize){'a', 0};
    }
    if (l.unit == 'p') return (GSize){'p', l.value};
    return (GSize){'a', 0};
}

/* The next token of a track list: a size, minmax(...), repeat(...), or
   [names] (skipped). Returns its length; 0 at the end. */
static int grid_token(const char **p) {
    const char *s = *p;
    for (;;) {
        while (is_space(*s) || *s == ',') s++;
        if (*s != '[') break;
        while (*s && *s != ']') s++;
        if (*s) s++;
    }
    *p = s;
    int depth = 0, n = 0;
    while (s[n] && (depth || (!is_space(s[n]) && s[n] != ',' && s[n] != '['))) {
        if (s[n] == '(') depth++;
        else if (s[n] == ')') depth--;
        n++;
        if (depth < 0) break;
    }
    return n;
}

static GTrack grid_track(const char *t, int len, int font, int base) {
    GTrack k = {{'a', 0}, {'a', 0}, 0};
    if (len > 7 && !strncasecmp(t, "minmax(", 7)) {
        const char *in = t + 7, *comma = in;
        int depth = 0;
        while (comma < t + len && (depth || *comma != ',')) {
            if (*comma == '(') depth++;
            if (*comma == ')') depth--;
            comma++;
        }
        const char *a = in, *b = comma + 1;
        while (is_space(*a)) a++;
        while (b < t + len && is_space(*b)) b++;
        int al = (int)(comma - a), bl = (int)(t + len - 1 - b);
        while (al > 0 && is_space(a[al - 1])) al--;
        while (bl > 0 && is_space(b[bl - 1])) bl--;
        k.min = grid_size(a, al, font, base);
        k.max = grid_size(b, bl, font, base);
        if (k.min.kind == 'f') k.min = (GSize){'a', 0};
        return k;
    }
    if (len > 12 && !strncasecmp(t, "fit-content(", 12)) {
        GSize s = grid_size(t + 12, len - 13, font, base);
        k.max = (GSize){'c', s.kind == 'p' ? s.v : 0};
        return k;
    }
    GSize s = grid_size(t, len, font, base);
    if (s.kind == 'f') k.max = s;
    else k.min = k.max = s;
    return k;
}

/* A track's fixed size if it has one, else -1. */
static int grid_fixed(const GTrack *k) {
    if (k->min.kind == 'p' && (k->max.kind == 'p' || k->max.kind == 'f')) return k->min.v;
    if (k->max.kind == 'p') return k->max.v;
    if (k->min.kind == 'p') return k->min.v;
    return -1;
}

/* Read a track list into out (at most max). `avail` is the length the
   tracks share (for auto-fill), or -1 if unknown. */
static int grid_tracks(const char *v, int font, int avail, int gap, GTrack *out, int max) {
    int n = 0;
    if (!v) return 0;
    const char *p = v;
    for (int len; n < max && (len = grid_token(&p)) > 0; p += len) {
        if (len > 7 && !strncasecmp(p, "repeat(", 7)) {
            const char *in = p + 7, *comma = strchr(in, ',');
            if (!comma || comma > p + len) continue;
            while (is_space(*in)) in++;
            int fill = !strncasecmp(in, "auto-fill", 9), fit = !strncasecmp(in, "auto-fit", 8);
            /* the repeated tracks */
            GTrack one[16];
            int count = 0;
            char inner[256];
            int il = (int)(p + len - 1 - (comma + 1));
            if (il > 255) il = 255;
            if (il < 0) il = 0;
            memcpy(inner, comma + 1, (size_t)il);
            inner[il] = 0;
            const char *q = inner;
            for (int ql; count < 16 && (ql = grid_token(&q)) > 0; q += ql) one[count++] = grid_track(q, ql, font, avail);
            if (!count) continue;
            int times;
            if (fill || fit) {
                int each = 0;
                for (int i = 0; i < count; i++) {
                    int f = grid_fixed(&one[i]);
                    if (f < 0 && one[i].min.kind == 'p') f = one[i].min.v;
                    each += f > 0 ? f : 1;
                }
                each += gap * count;
                times = avail > 0 && each > 0 ? (avail + gap) / each : 1;
                if (times < 1) times = 1;
            } else {
                times = atoi(in);
                if (times < 1) times = 1;
            }
            for (int t = 0; t < times && n < max; t++)
                for (int i = 0; i < count && n < max; i++) {
                    out[n] = one[i];
                    out[n++].autofit = (unsigned char)fit;
                }
            continue;
        }
        out[n++] = grid_track(p, len, font, avail);
    }
    return n;
}

/* Named areas: "a a b" "c c b" -> each name's rectangle. */
typedef struct {
    char name[24];
    int r0, r1, c0, c1;
} GArea;

static int grid_areas(const char *v, GArea *areas, int max, int *rows, int *cols) {
    int count = 0, row = 0;
    *rows = *cols = 0;
    if (!v) return 0;
    for (const char *p = v; *p;) {
        if (*p != '"' && *p != '\'') {
            p++;
            continue;
        }
        char q = *p++;
        int col = 0;
        while (*p && *p != q) {
            while (is_space(*p)) p++;
            if (!*p || *p == q) break;
            char name[24];
            int n = 0;
            while (*p && *p != q && !is_space(*p)) {
                if (n < 23) name[n++] = *p;
                p++;
            }
            name[n] = 0;
            if (strcmp(name, ".") && name[0] != '.') {
                int k = 0;
                while (k < count && strcmp(areas[k].name, name)) k++;
                if (k == count && count < max) {
                    strcpy(areas[count].name, name);
                    areas[count++] = (GArea){"", row, row + 1, col, col + 1};
                    strcpy(areas[count - 1].name, name);
                } else if (k < count) {
                    if (row + 1 > areas[k].r1) areas[k].r1 = row + 1;
                    if (col + 1 > areas[k].c1) areas[k].c1 = col + 1;
                    if (row < areas[k].r0) areas[k].r0 = row;
                    if (col < areas[k].c0) areas[k].c0 = col;
                }
            }
            col++;
        }
        if (*p) p++;
        if (col > *cols) *cols = col;
        row++;
    }
    *rows = row;
    return count;
}

static int grid_free(Grid *g, int r, int c0, int c1) {
    if (r >= g->used_rows) return 1;
    unsigned long long mask = (c1 - c0 >= 64 ? ~0ULL : ((1ULL << (c1 - c0)) - 1)) << c0;
    return !(g->used[r] & mask);
}

static int grid_area_free(Grid *g, int r0, int r1, int c0, int c1) {
    for (int r = r0; r < r1; r++)
        if (!grid_free(g, r, c0, c1)) return 0;
    return 1;
}

static void grid_take(Grid *g, int r0, int r1, int c0, int c1) {
    if (r1 > GRID_ROWS) r1 = GRID_ROWS;
    if (r1 > g->used_rows) {
        if (!grow((void **)&g->used, &g->rows_cap, r1, sizeof(unsigned long long))) return;
        memset(g->used + g->used_rows, 0, sizeof(unsigned long long) * (size_t)(r1 - g->used_rows));
        g->used_rows = r1;
    }
    unsigned long long mask = (c1 - c0 >= 64 ? ~0ULL : ((1ULL << (c1 - c0)) - 1)) << c0;
    for (int r = r0; r < r1; r++) g->used[r] |= mask;
}

/* A line number (1-based, negative from the end of the explicit grid) as
   an index from 0. */
static int grid_index(int line, int explicit_count) {
    if (line > 0) return line - 1;
    if (line < 0) {
        int i = explicit_count + 1 + line;
        return i < 0 ? 0 : i;
    }
    return -1;
}

/* Resolve one axis of an item: start index and span (start -1: auto). */
static void grid_axis(const short line[2], int explicit_count, int *start, int *span) {
    int s = line[0], e = line[1];
    *start = -1;
    *span = 1;
    if (s >= GRID_SPAN) {
        *span = s - GRID_SPAN;
        if (e && e < GRID_SPAN) { /* span n / end */
            int ei = grid_index(e, explicit_count);
            *start = ei - *span;
            if (*start < 0) *start = 0;
        }
        return;
    }
    if (s) *start = grid_index(s, explicit_count);
    if (e >= GRID_SPAN) *span = e - GRID_SPAN;
    else if (e) {
        int ei = grid_index(e, explicit_count);
        if (*start < 0) { /* auto / end */
            *start = ei - 1;
            if (*start < 0) *start = 0;
        } else if (ei > *start) *span = ei - *start;
        else if (ei < *start) {
            *span = *start - ei;
            *start = ei;
        }
    }
    if (*span < 1) *span = 1;
}

static int grid_is_item(Node *c) {
    if (c->type == NODE_TEXT) {
        for (int i = 0; i < c->text_len; i++)
            if (!is_space(c->text[i])) return 1;
        return 0;
    }
    return c->style.display != D_NONE && !out_of_flow(c);
}

static int grid_order(Node *c) { return c->type == NODE_ELEMENT ? c->style.order : 0; }

static void grid_add_row(Grid *g, int upto) {
    if (upto > GRID_ROWS) upto = GRID_ROWS;
    while (g->nrows < upto) {
        GTrack *rows = realloc(g->rows, sizeof(GTrack) * (size_t)(g->nrows + 16));
        if (!rows) return;
        g->rows = rows;
        g->rows[g->nrows++] = g->auto_row;
    }
}

/* Set up a grid container: its tracks and where each item goes. `width`
   is its content width, or -1 when measuring. */
static int grid_setup(Node *n, int width, Grid *g) {
    Style *s = &n->style;
    memset(g, 0, sizeof(*g));
    int font = s->size;
    GArea areas[32];
    int area_rows, area_cols, area_count = grid_areas(s->grid_areas, areas, 32, &area_rows, &area_cols);
    g->ncols = grid_tracks(s->grid_cols, font, width, s->gap_col, g->cols, GRID_COLS);
    while (g->ncols < area_cols && g->ncols < GRID_COLS) g->cols[g->ncols++] = (GTrack){{'a', 0}, {'a', 0}, 0};
    g->explicit_cols = g->ncols;
    GTrack tmp[64];
    int auto_count = grid_tracks(s->grid_auto_cols, font, width, s->gap_col, tmp, 1);
    g->auto_col = auto_count ? tmp[0] : (GTrack){{'a', 0}, {'a', 0}, 0};
    auto_count = grid_tracks(s->grid_auto_rows, font, -1, s->gap_row, tmp, 1);
    g->auto_row = auto_count ? tmp[0] : (GTrack){{'a', 0}, {'a', 0}, 0};
    int height = definite_height(n);
    int nr = grid_tracks(s->grid_rows, font, height, s->gap_row, tmp, 64);
    if (nr < area_rows) {
        for (int i = nr; i < area_rows && i < 64; i++) tmp[i] = g->auto_row;
        nr = area_rows < 64 ? area_rows : 64;
    }
    g->explicit_rows = nr;
    g->rows = malloc(sizeof(GTrack) * (size_t)(nr + 16));
    if (!g->rows) return 0;
    memcpy(g->rows, tmp, sizeof(GTrack) * (size_t)nr);
    g->nrows = nr;
    /* The items, in order. */
    int count = 0;
    for (Node *c = n->first; c; c = c->next) count += grid_is_item(c);
    g->items = malloc(sizeof(GItem) * (size_t)(count ? count : 1));
    if (!g->items) return 0;
    for (Node *c = n->first; c; c = c->next)
        if (grid_is_item(c)) {
            memset(&g->items[g->nitems], 0, sizeof(GItem));
            g->items[g->nitems++].node = c;
        }
    for (int i = 1; i < g->nitems; i++) { /* (a stable insertion sort by order) */
        GItem t = g->items[i];
        int k = i - 1, ot = grid_order(t.node);
        while (k >= 0 && grid_order(g->items[k].node) > ot) {
            g->items[k + 1] = g->items[k];
            k--;
        }
        g->items[k + 1] = t;
    }
    /* Where each goes: definite places first, then rows that are fixed,
       then the rest from a cursor running along the rows. */
    int cols = g->ncols ? g->ncols : 1;
    int *rs = malloc(sizeof(int) * (size_t)g->nitems * 4);
    if (!rs) return 0;
    for (int i = 0; i < g->nitems; i++) {
        GItem *it = &g->items[i];
        int *r = rs + i * 4; /* row start, row span, column start, column span */
        r[0] = r[2] = -1;
        r[1] = r[3] = 1;
        if (it->node->type != NODE_ELEMENT) continue;
        Style *cs = &it->node->style;
        if (cs->grid_area) {
            for (int k = 0; k < area_count; k++)
                if (!strcmp(areas[k].name, cs->grid_area)) {
                    r[0] = areas[k].r0;
                    r[1] = areas[k].r1 - areas[k].r0;
                    r[2] = areas[k].c0;
                    r[3] = areas[k].c1 - areas[k].c0;
                }
            continue;
        }
        grid_axis(cs->grid_row, g->explicit_rows, &r[0], &r[1]);
        grid_axis(cs->grid_col, g->explicit_cols, &r[2], &r[3]);
        if (r[2] >= 0 && r[2] + r[3] > cols) cols = r[2] + r[3];
        if (r[2] < 0 && r[3] > cols) cols = r[3];
    }
    if (cols > GRID_COLS) cols = GRID_COLS;
    while (g->ncols < cols) g->cols[g->ncols++] = g->auto_col;
    for (int pass = 0; pass < 3; pass++) {
        int cur_r = 0, cur_c = 0;
        for (int i = 0; i < g->nitems; i++) {
            int *r = rs + i * 4;
            if (r[3] > g->ncols) r[3] = g->ncols;
            if (r[2] >= 0 && r[2] + r[3] > g->ncols) r[2] = g->ncols - r[3];
            int definite = r[0] >= 0 && r[2] >= 0, row_only = r[0] >= 0 && r[2] < 0;
            if (pass == 0 && !definite) continue;
            if (pass == 1 && !row_only) continue;
            if (pass == 2 && r[0] >= 0) continue;
            if (pass == 1) { /* the first columns free in that row */
                int c = 0;
                while (c + r[3] <= g->ncols && !grid_area_free(g, r[0], r[0] + r[1], c, c + r[3])) c++;
                r[2] = c + r[3] <= g->ncols ? c : 0;
            } else if (pass == 2) {
                if (r[2] >= 0) { /* column fixed: the next row where it fits */
                    if (r[2] < cur_c) cur_r++;
                    while (cur_r < GRID_ROWS && !grid_area_free(g, cur_r, cur_r + r[1], r[2], r[2] + r[3])) cur_r++;
                    r[0] = cur_r;
                    cur_c = r[2] + r[3];
                } else {
                    for (;;) {
                        if (cur_c + r[3] > g->ncols) {
                            cur_c = 0;
                            cur_r++;
                        }
                        if (cur_r >= GRID_ROWS || grid_area_free(g, cur_r, cur_r + r[1], cur_c, cur_c + r[3])) break;
                        cur_c++;
                    }
                    r[0] = cur_r;
                    r[2] = cur_c;
                    cur_c += r[3];
                }
            }
            if (r[0] >= GRID_ROWS) r[0] = GRID_ROWS - 1;
            grid_take(g, r[0], r[0] + r[1], r[2], r[2] + r[3]);
            GItem *it = &g->items[i];
            it->r0 = r[0];
            it->r1 = r[0] + r[1];
            it->c0 = r[2];
            it->c1 = r[2] + r[3];
            if (it->r1 > GRID_ROWS) it->r1 = GRID_ROWS;
            grid_add_row(g, it->r1);
        }
    }
    free(rs);
    /* auto-fit: tracks with nothing in them go */
    int last_used = -1;
    for (int i = 0; i < g->nitems; i++)
        if (g->items[i].c1 - 1 > last_used) last_used = g->items[i].c1 - 1;
    while (g->ncols > 1 && g->ncols - 1 > last_used && g->cols[g->ncols - 1].autofit) g->ncols--;
    /* Item widths (for content-sized tracks). */
    for (int i = 0; i < g->nitems; i++) {
        GItem *it = &g->items[i];
        intrinsic(it->node, &it->mn, &it->mx);
        if (it->node->type == NODE_ELEMENT) {
            int m = margin(it->node, 1, 0) + margin(it->node, 3, 0);
            it->mn += m;
            it->mx += m;
        }
    }
    return 1;
}

static void grid_free_all(Grid *g) {
    free(g->rows);
    free(g->items);
    free(g->used);
}

/* Size the columns for a content width (or -1: as wide as they want, for
   measuring); fills col_size. Returns the total, gaps included. */
static int grid_columns(Grid *g, int width, int gap, int *min_total, int stretch) {
    int base[GRID_COLS], limit[GRID_COLS], fr[GRID_COLS], n = g->ncols;
    long long fr_total = 0;
    for (int i = 0; i < n; i++) {
        GTrack *k = &g->cols[i];
        base[i] = k->min.kind == 'p' ? k->min.v : 0;
        fr[i] = k->max.kind == 'f' ? k->max.v : 0;
        fr_total += fr[i];
        limit[i] = k->max.kind == 'p' ? k->max.v : k->max.kind == 'f' ? 0 : -1; /* -1: content */
    }
    /* Content: items in one track first, then spanning ones. */
    for (int pass = 0; pass < 2; pass++)
        for (int t = 0; t < g->nitems; t++) {
            GItem *it = &g->items[t];
            int span = it->c1 - it->c0;
            if ((span == 1) != (pass == 0) || it->c0 >= n) continue;
            int c1 = it->c1 > n ? n : it->c1;
            if (span == 1) {
                GTrack *k = &g->cols[it->c0];
                int want_min = k->min.kind == 'x' ? it->mx : it->mn;
                if (k->min.kind == 'a' || k->min.kind == 'n' || k->min.kind == 'x') {
                    if (want_min > base[it->c0]) base[it->c0] = want_min;
                }
                if (limit[it->c0] != 0 && k->max.kind != 'p') {
                    int want = k->max.kind == 'n' ? it->mn : it->mx;
                    if (k->max.kind == 'c' && want > k->max.v) want = k->max.v > it->mn ? k->max.v : it->mn;
                    if (limit[it->c0] < 0 || want > limit[it->c0]) limit[it->c0] = want;
                }
            } else {
                int have = gap * (span - 1), flexible = 0;
                for (int c = it->c0; c < c1; c++) {
                    have += base[c];
                    flexible += g->cols[c].min.kind != 'p';
                }
                if (it->mn > have && flexible) {
                    int extra = (it->mn - have + flexible - 1) / flexible;
                    for (int c = it->c0; c < c1; c++)
                        if (g->cols[c].min.kind != 'p') base[c] += extra;
                }
            }
        }
    for (int i = 0; i < n; i++) {
        if (limit[i] < 0) limit[i] = base[i];
        if (limit[i] < base[i] && !fr[i]) limit[i] = base[i];
    }
    int gaps = n > 1 ? gap * (n - 1) : 0, total = gaps;
    for (int i = 0; i < n; i++) total += base[i];
    if (min_total) *min_total = total;
    if (width < 0) { /* measuring: everything at its limit; fr tracks at their content */
        int sum = gaps;
        for (int i = 0; i < n; i++) {
            int w = fr[i] ? 0 : limit[i];
            if (fr[i])
                for (int t = 0; t < g->nitems; t++)
                    if (g->items[t].c0 == i && g->items[t].c1 == i + 1 && g->items[t].mx > w) w = g->items[t].mx;
            if (w < base[i]) w = base[i];
            g->col_size[i] = w;
            sum += w;
        }
        return sum;
    }
    for (int i = 0; i < n; i++) g->col_size[i] = base[i];
    /* Grow the content-sized tracks towards their limits. */
    int free_space = width - total;
    for (int round = 0; round < 8 && free_space > 0; round++) {
        int growing = 0;
        for (int i = 0; i < n; i++) growing += !fr[i] && g->col_size[i] < limit[i];
        if (!growing) break;
        int share = free_space / growing;
        if (share < 1) share = 1;
        for (int i = 0; i < n && free_space > 0; i++) {
            if (fr[i] || g->col_size[i] >= limit[i]) continue;
            int add = limit[i] - g->col_size[i];
            if (add > share) add = share;
            if (add > free_space) add = free_space;
            g->col_size[i] += add;
            free_space -= add;
        }
    }
    /* fr tracks share what is left (none smaller than its content). */
    if (fr_total > 0) {
        int fixed = gaps;
        unsigned char frozen[GRID_COLS] = {0};
        for (int i = 0; i < n; i++)
            if (!fr[i]) fixed += g->col_size[i];
        for (int round = 0; round < 8; round++) {
            long long flex = 0;
            int left = width - fixed;
            for (int i = 0; i < n; i++)
                if (fr[i] && !frozen[i]) flex += fr[i];
                else if (fr[i]) left -= g->col_size[i];
            if (flex <= 0) break;
            /* the size of 1fr (fractions adding up to less than 1 share only part) */
            long long per = (long long)(left > 0 ? left : 0) * 100 / (flex < 100 ? 100 : flex);
            int again = 0;
            for (int i = 0; i < n; i++) {
                if (!fr[i] || frozen[i]) continue;
                int w = (int)(per * fr[i] / 100);
                if (w < base[i]) {
                    g->col_size[i] = base[i];
                    frozen[i] = 1;
                    again = 1;
                }
            }
            if (again) continue;
            int given = 0, last = -1;
            for (int i = 0; i < n; i++)
                if (fr[i] && !frozen[i]) {
                    g->col_size[i] = (int)(per * fr[i] / 100);
                    given += g->col_size[i];
                    last = i;
                }
            if (last >= 0 && flex >= 100 && left - given > 0 && left - given < n) g->col_size[last] += left - given;
            break;
        }
    } else if (free_space > 0 && stretch) {
        /* No fr: auto tracks stretch to fill (justify-content: normal). */
        int autos = 0;
        for (int i = 0; i < n; i++) autos += g->cols[i].max.kind == 'a';
        if (autos) {
            int share = free_space / autos, rest = free_space - share * autos;
            for (int i = 0; i < n; i++)
                if (g->cols[i].max.kind == 'a') {
                    g->col_size[i] += share + (rest > 0 ? 1 : 0);
                    if (rest > 0) rest--;
                }
        }
    }
    int sum = gaps;
    for (int i = 0; i < n; i++) sum += g->col_size[i];
    return sum;
}

static void grid_intrinsic(Node *n, int *mn, int *mx) {
    Grid g;
    *mn = *mx = 0;
    if (grid_setup(n, -1, &g)) {
        int min_total = 0;
        *mx = grid_columns(&g, -1, n->style.gap_col, &min_total, 0);
        *mn = min_total;
    }
    grid_free_all(&g);
}

static int layout_grid(Node *n, int left, int top, int width) {
    Style *s = &n->style;
    for (Node *c = n->first; c; c = c->next) /* absolutely placed children wait */
        if (c->type == NODE_ELEMENT && c->style.display != D_NONE && out_of_flow(c) &&
            grow((void **)&pending, &pending_cap, pending_count + 1, sizeof(Pending)))
            pending[pending_count++] = (Pending){c, left, top};
    Grid g;
    if (!grid_setup(n, width, &g) || !g.nitems) {
        grid_free_all(&g);
        return 0;
    }
    if (!g.ncols) {
        g.cols[0] = g.auto_col;
        g.ncols = 1;
    }
    int used_w = grid_columns(&g, width, s->gap_col, NULL, s->justify == J_START);
    int x0 = left;
    if (used_w < width) { /* justify-content */
        int spare = width - used_w;
        if (s->justify == J_CENTER) x0 += spare / 2;
        else if (s->justify == J_END) x0 += spare;
    }
    int col_x[GRID_COLS + 1];
    col_x[0] = x0;
    for (int i = 0; i < g.ncols; i++) col_x[i + 1] = col_x[i] + g.col_size[i] + s->gap_col;
    /* Lay each item out in its columns (at the top for now) to learn its height. */
    for (int i = 0; i < g.nitems; i++) {
        GItem *it = &g.items[i];
        Node *c = it->node;
        int c1 = it->c1 > g.ncols ? g.ncols : it->c1, c0 = it->c0 < c1 ? it->c0 : c1 - 1;
        int area_w = col_x[c1] - col_x[c0] - s->gap_col;
        if (c->type == NODE_TEXT) {
            int saved = item_count;
            it->h = layout_lines(n, c, c->next, col_x[c0], col_x[c0] + area_w, top) - top;
            item_count = saved;
            continue;
        }
        int js = c->style.justify_self != AL_AUTO ? c->style.justify_self : s->justify_items;
        int stretch = js == AL_STRETCH && c->style.width.unit == 'a' && !is_replaced(c);
        int w = used_width(c, area_w, !stretch);
        int ml = margin(c, 3, area_w), mr = margin(c, 1, area_w), x = col_x[c0] + ml;
        int spare = area_w - w - ml - mr;
        if (spare > 0 && js == AL_CENTER) x += spare / 2;
        else if (spare > 0 && js == AL_END) x += spare;
        layout_box(c, x, top + margin(c, 0, area_w), w);
        it->h = c->h + margin(c, 0, area_w) + margin(c, 2, area_w);
    }
    /* Row heights. */
    int nrows = g.nrows, *row_h = malloc(sizeof(int) * (size_t)(nrows + 1));
    if (!row_h) {
        grid_free_all(&g);
        return 0;
    }
    int height = definite_height(n);
    long long fr_rows = 0;
    for (int r = 0; r < nrows; r++) {
        GTrack *k = &g.rows[r];
        row_h[r] = k->min.kind == 'p' ? k->min.v : 0;
        if (k->max.kind == 'f') fr_rows += k->max.v;
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g.nitems; i++) {
            GItem *it = &g.items[i];
            int span = it->r1 - it->r0;
            if ((span == 1) != (pass == 0) || it->r0 >= nrows) continue;
            int r1 = it->r1 > nrows ? nrows : it->r1;
            if (span == 1) {
                GTrack *k = &g.rows[it->r0];
                if (k->max.kind != 'p' || k->min.kind != 'p') {
                    int want = it->h;
                    if (k->max.kind == 'p' && want > k->max.v) want = k->max.v > row_h[it->r0] ? k->max.v : row_h[it->r0];
                    if (want > row_h[it->r0]) row_h[it->r0] = want;
                }
            } else {
                int have = s->gap_row * (span - 1), last = -1;
                for (int r = it->r0; r < r1; r++) {
                    have += row_h[r];
                    if (g.rows[r].max.kind != 'p') last = r;
                }
                if (it->h > have && last >= 0) row_h[last] += it->h - have;
            }
        }
    int rows_total = nrows > 1 ? s->gap_row * (nrows - 1) : 0;
    for (int r = 0; r < nrows; r++) rows_total += row_h[r];
    if (height >= 0 && rows_total < height) { /* a set height: fr rows share the rest, else auto rows grow */
        int spare = height - rows_total;
        if (fr_rows > 0) {
            for (int r = 0; r < nrows; r++)
                if (g.rows[r].max.kind == 'f') row_h[r] += (int)((long long)spare * g.rows[r].max.v / fr_rows);
        } else {
            int autos = 0;
            for (int r = 0; r < nrows; r++) autos += g.rows[r].max.kind == 'a';
            for (int r = 0; r < nrows && autos; r++)
                if (g.rows[r].max.kind == 'a') row_h[r] += spare / autos;
        }
    }
    int *row_y = malloc(sizeof(int) * (size_t)(nrows + 1));
    if (!row_y) {
        free(row_h);
        grid_free_all(&g);
        return 0;
    }
    row_y[0] = top;
    for (int r = 0; r < nrows; r++) row_y[r + 1] = row_y[r] + row_h[r] + s->gap_row;
    /* Move each item to its row, aligned in its area. */
    for (int i = 0; i < g.nitems; i++) {
        GItem *it = &g.items[i];
        Node *c = it->node;
        int r1 = it->r1 > nrows ? nrows : it->r1, r0 = it->r0 < r1 ? it->r0 : r1 - 1;
        if (r0 < 0) r0 = 0;
        int area_h = row_y[r1] - row_y[r0] - s->gap_row;
        if (c->type == NODE_TEXT) { /* (an anonymous item: aligned as the container says) */
            int c1 = it->c1 > g.ncols ? g.ncols : it->c1, c0 = it->c0 < c1 ? it->c0 : c1 - 1;
            int x0 = col_x[c0], x1 = col_x[c1] - s->gap_col, y0 = row_y[r0], spare = area_h - it->h;
            int tw = it->mx < x1 - x0 ? it->mx : x1 - x0;
            if (s->justify_items == AL_CENTER) x0 += (x1 - x0 - tw) / 2;
            else if (s->justify_items == AL_END) x0 = x1 - tw;
            if (s->justify_items == AL_CENTER || s->justify_items == AL_END) x1 = x0 + tw;
            if (spare > 0 && s->align_items == AL_CENTER) y0 += spare / 2;
            else if (spare > 0 && s->align_items == AL_END) y0 += spare;
            layout_lines(n, c, c->next, x0, x1, y0);
            continue;
        }
        int as = c->style.align_self != AL_AUTO ? c->style.align_self : s->align_items;
        int mt = margin(c, 0, 0), mb = margin(c, 2, 0);
        int dy = row_y[r0] - top, spare = area_h - it->h;
        if (spare > 0 && as == AL_CENTER) dy += spare / 2;
        else if (spare > 0 && as == AL_END) dy += spare;
        if (dy) shift_subtree(c, 0, dy);
        if (as == AL_STRETCH && c->style.height.unit == 'a' && !is_replaced(c)) stretch_to(c, area_h - mt - mb);
    }
    int total = row_y[nrows] - top - (nrows ? s->gap_row : 0);
    free(row_h);
    free(row_y);
    grid_free_all(&g);
    return total > 0 ? total : 0;
}

/* ---------------------------------------------------------------- tables */

#define MAX_COLS 64

static int table_rows(Node *table, Node **rows, int max) {
    int n = 0;
    for (Node *c = table->first; c && n < max; c = c->next) {
        if (c->type != NODE_ELEMENT || c->style.display == D_NONE) continue;
        if (c->style.display == D_TABLE_ROW) rows[n++] = c;
        else if (c->style.display == D_TABLE_GROUP || is(c->tag, "tbody") || is(c->tag, "thead") || is(c->tag, "tfoot")) {
            for (Node *r = c->first; r && n < max; r = r->next) {
                if (r->type == NODE_ELEMENT && r->style.display != D_NONE && (r->style.display == D_TABLE_ROW || is(r->tag, "tr"))) rows[n++] = r;
            }
        }
    }
    return n;
}

static int colspan(Node *cell) {
    char v[8];
    int span = attr(cell, "colspan", v, sizeof(v)) ? atoi(v) : 1;
    return span < 1 ? 1 : span > MAX_COLS ? MAX_COLS : span;
}

static int is_cell(Node *c) {
    return c->type == NODE_ELEMENT && c->style.display != D_NONE && (c->style.display == D_TABLE_CELL || is(c->tag, "td") || is(c->tag, "th"));
}

static int layout_table(Node *table, int left, int top, int width) {
    /* Not static: tables inside table cells are laid out while this one is. */
    Node **rows = malloc(2048 * sizeof(Node *));
    if (!rows) return 0;
    char v[16];
    int spacing = attr(table, "cellspacing", v, sizeof(v)) ? atoi(v) : 2;
    int y = top;
    int nrows = table_rows(table, rows, 2048);
    int cols = 0;
    for (int r = 0; r < nrows; r++) {
        int count = 0;
        for (Node *c = rows[r]->first; c; c = c->next) {
            if (is_cell(c)) count += colspan(c);
        }
        if (count > cols) cols = count;
    }
    if (cols == 0) {
        /* No rows: its content is one anonymous cell (a figure styled as a
           table, with a picture and its caption, say): laid out as a block. */
        free(rows);
        return layout_flow(table, left, top, width);
    }
    /* Captions above (or, with caption-side: bottom, below). */
    for (Node *c = table->first; c; c = c->next) {
        if (c->type == NODE_ELEMENT && c->style.display == D_TABLE_CAPTION && !c->style.caption_bottom) {
            layout_box(c, left, y, width);
            y += c->h;
        }
    }
    if (cols > MAX_COLS) cols = MAX_COLS;
    int mins[MAX_COLS], maxs[MAX_COLS], fixed[MAX_COLS];
    for (int i = 0; i < cols; i++) mins[i] = maxs[i] = 0, fixed[i] = -1;
    int inner = width - spacing * (cols + 1);
    for (int r = 0; r < nrows; r++) {
        int col = 0;
        for (Node *c = rows[r]->first; c && col < cols; c = c->next) {
            if (!is_cell(c)) continue;
            int span = colspan(c);
            if (span == 1) {
                int mn, mx;
                intrinsic(c, &mn, &mx);
                if (mn > mins[col]) mins[col] = mn;
                if (mx > maxs[col]) maxs[col] = mx;
                int w = resolve(c->style.width, inner, -1);
                if (w >= 0) {
                    if (!c->style.box_sizing && c->style.width.unit == 'p') w += frame_h(c, inner);
                    if (w > fixed[col]) fixed[col] = w;
                }
            }
            col += span;
        }
    }
    /* Spanning cells: make sure their columns are together wide enough. */
    for (int r = 0; r < nrows; r++) {
        int col = 0;
        for (Node *c = rows[r]->first; c && col < cols; c = c->next) {
            if (!is_cell(c)) continue;
            int span = colspan(c);
            if (span > 1) {
                int mn, mx, have = 0;
                intrinsic(c, &mn, &mx);
                for (int k = col; k < col + span && k < cols; k++) have += mins[k];
                if (mn > have) {
                    int extra = (mn - have) / span;
                    for (int k = col; k < col + span && k < cols; k++) mins[k] += extra, maxs[k] = maxs[k] > mins[k] ? maxs[k] : mins[k];
                }
            }
            col += span;
        }
    }
    int widths[MAX_COLS];
    int sum_min = 0, sum_max = 0;
    for (int i = 0; i < cols; i++) {
        if (fixed[i] >= 0) {
            if (fixed[i] < mins[i]) fixed[i] = mins[i];
            maxs[i] = fixed[i];
        }
        if (maxs[i] < mins[i]) maxs[i] = mins[i];
        sum_min += mins[i];
        sum_max += maxs[i];
    }
    int table_fixed = table->style.width.unit == 'p' || table->style.width.unit == '%';
    if (sum_max <= inner) {
        int extra = table_fixed ? inner - sum_max : 0;
        int flexible = 0;
        for (int i = 0; i < cols; i++) flexible += fixed[i] < 0 ? maxs[i] : 0;
        for (int i = 0; i < cols; i++) {
            widths[i] = maxs[i];
            if (extra > 0) {
                if (flexible > 0) {
                    if (fixed[i] < 0) widths[i] += (int)((long long)extra * maxs[i] / flexible);
                } else {
                    widths[i] += extra / cols;
                }
            }
        }
    } else if (sum_min >= inner) {
        for (int i = 0; i < cols; i++) widths[i] = mins[i];
    } else {
        int room = inner - sum_min, spread = sum_max - sum_min;
        for (int i = 0; i < cols; i++) widths[i] = mins[i] + (spread ? (int)((long long)room * (maxs[i] - mins[i]) / spread) : 0);
    }
    /* Lay out the rows. */
    y += spacing;
    for (int r = 0; r < nrows; r++) {
        Node *row = rows[r];
        row->items_from = item_count;
        row->bg_item = -1;
        if (row->style.has_background) {
            int it = new_item(IT_RECT);
            if (it >= 0) {
                items[it].color = row->style.background;
                items[it].node = row;
                row->bg_item = it;
            }
        }
        int x = left + spacing, col = 0, row_h = 0;
        Node *cells[MAX_COLS];
        int cell_count = 0;
        for (Node *c = row->first; c && col < cols; c = c->next) {
            if (!is_cell(c)) continue;
            int span = colspan(c);
            int w = 0;
            for (int k = col; k < col + span && k < cols; k++) w += widths[k];
            w += spacing * (span - 1);
            layout_box(c, x, y, w);
            if (c->h > row_h) row_h = c->h;
            if (cell_count < MAX_COLS) cells[cell_count++] = c;
            x += w + spacing;
            col += span;
        }
        int min_row = resolve(row->style.height, 0, 0);
        if (min_row > row_h) row_h = min_row;
        for (int k = 0; k < cell_count; k++) stretch_to(cells[k], row_h);
        row->x = left;
        row->y = y;
        row->w = width;
        row->h = row_h;
        if (row->bg_item >= 0) {
            items[row->bg_item].x = left + spacing;
            items[row->bg_item].y = y;
            items[row->bg_item].w = width - 2 * spacing;
            items[row->bg_item].h = row_h;
        }
        row->items_to = item_count;
        y += row_h + spacing;
    }
    for (Node *c = table->first; c; c = c->next) {
        if (c->type == NODE_ELEMENT && c->style.display == D_TABLE_CAPTION && c->style.caption_bottom) {
            layout_box(c, left, y, width);
            y += c->h;
        }
    }
    free(rows);
    return y - top;
}

/* ---------------------------------------------------------------- one box */

/* Lay out `n` with its border box's top left at (x, y), `width` wide. */
static void layout_box(Node *n, int x, int y, int width) {
    Style *s = &n->style;
    n->x = x;
    n->y = y;
    n->w = width;
    n->items_from = item_count;
    n->bg_item = -1;
    /* Background and border, sized once the height is known. */
    if (s->has_background && !s->hidden && !s->masked) {
        int it = new_item(IT_RECT);
        if (it >= 0) {
            items[it].color = s->background;
            items[it].node = n;
            n->bg_item = it;
        }
    }
    int border_item = -1;
    if ((s->border[0] || s->border[1] || s->border[2] || s->border[3]) && !s->hidden) {
        border_item = new_item(IT_BORDER);
        if (border_item >= 0) {
            items[border_item].node = n;
            for (int i = 0; i < 4; i++) {
                items[border_item].border[i] = s->border[i];
                items[border_item].border_color[i] = s->border_color[i];
            }
        }
        if (n->bg_item < 0) n->bg_item = border_item;
    }
    int cw = width; /* percentages of padding are of the containing block; this is close */
    int content_x = x + s->border[3] + pad(n, 3, cw);
    int content_y = y + s->border[0] + pad(n, 0, cw);
    int content_w = width - frame_h(n, cw);
    if (content_w < 0) content_w = 0;
    int content_h;
    if (is_replaced(n)) {
        int bw, bh;
        replaced_size(n, cw, &bw, &bh);
        content_h = bh - frame_v(n, cw);
        int it = new_item(is(n->tag, "img") ? IT_IMAGE : IT_CONTROL);
        if (it >= 0) {
            items[it].x = content_x;
            items[it].y = content_y;
            items[it].w = content_w;
            items[it].h = content_h;
            items[it].node = n;
            items[it].color = s->color;
            items[it].link = link_of(n);
            if (s->hidden) items[it].w = 0;
        }
    } else if (s->display == D_FLEX || s->display == D_INLINE_FLEX) {
        content_h = layout_flex(n, content_x, content_y, content_w);
    } else if (s->display == D_GRID || s->display == D_INLINE_GRID) {
        content_h = layout_grid(n, content_x, content_y, content_w);
    } else if (s->display == D_TABLE) {
        content_h = layout_table(n, content_x, content_y, content_w);
    } else {
        content_h = layout_flow(n, content_x, content_y, content_w);
    }
    int h = definite_height(n);
    if (h < 0) h = content_h;
    int minh = s->min_height.unit == 'p' ? s->min_height.value : -1;
    if (minh >= 0 && s->box_sizing) minh -= frame_v(n, cw);
    if (minh > h) h = minh;
    int maxh = s->max_height.unit == 'p' ? s->max_height.value : -1;
    if (maxh >= 0 && s->box_sizing) maxh -= frame_v(n, cw);
    if (maxh >= 0 && h > maxh && s->overflow_hidden) h = maxh;
    n->h = h + frame_v(n, cw);
    if (n->bg_item >= 0) {
        items[n->bg_item].x = x;
        items[n->bg_item].y = y;
        items[n->bg_item].w = width;
        items[n->bg_item].h = n->h;
    }
    if (border_item >= 0 && border_item != n->bg_item) {
        items[border_item].x = x;
        items[border_item].y = y;
        items[border_item].w = width;
        items[border_item].h = n->h;
    }
    n->items_to = item_count;
    /* overflow: hidden cuts its content off at its padding box; the
       "visually hidden" clip cuts all of it. */
    if (s->overflow_hidden || (s->clip_rect && out_of_flow(n))) {
        int c[4] = {x + s->border[3], y + s->border[0], x + width - s->border[1], y + n->h - s->border[2]};
        if (s->clip_rect) c[2] = c[0], c[3] = c[1];
        for (int i = n->items_from; i < item_count; i++) {
            Item *it = &items[i];
            if (i == n->bg_item || (i == n->bg_item + 1 && it->kind == IT_BORDER && it->node == n)) {
                if (!s->clip_rect) continue;
            }
            if (!it->clipped) {
                memcpy(it->clip, c, sizeof(c));
                it->clipped = 1;
            } else {
                if (c[0] > it->clip[0]) it->clip[0] = c[0];
                if (c[1] > it->clip[1]) it->clip[1] = c[1];
                if (c[2] < it->clip[2]) it->clip[2] = c[2];
                if (c[3] < it->clip[3]) it->clip[3] = c[3];
            }
        }
    }
    /* transform: translate(): drawn moved, by lengths or parts of its own size */
    if (s->translated) {
        int dx = resolve(s->tx, n->w, 0), dy = resolve(s->ty, n->h, 0);
        if (dx || dy) shift_subtree(n, dx, dy);
    }
    /* Relative positioning: the box moves, the space it took stays. */
    if (s->position == P_RELATIVE) {
        int dx = resolve(s->offset[3], cw, AUTO), dy = resolve(s->offset[0], 0, AUTO);
        if (dx == AUTO) dx = resolve(s->offset[1], cw, AUTO) == AUTO ? 0 : -resolve(s->offset[1], cw, 0);
        if (dy == AUTO) dy = resolve(s->offset[2], 0, AUTO) == AUTO ? 0 : -resolve(s->offset[2], 0, 0);
        if (s->offset[0].unit == '%') dy = 0;
        shift_subtree(n, dx, dy);
    }
}

/* ---------------------------------------------------------------- the page */

static void place_pending(void) {
    for (int i = 0; i < pending_count; i++) {
        Node *n = pending[i].node;
        Style *s = &n->style;
        /* The containing block: the nearest positioned ancestor, or the page. */
        int cbx = 0, cby = 0, cbw = page_width, cbh = VIEW_H;
        if (s->position == P_ABSOLUTE) {
            for (Node *a = n->parent; a && a != document; a = a->parent) {
                if (a->type == NODE_ELEMENT && a->style.position != P_STATIC && (a->w || a->h)) {
                    cbx = a->x + a->style.border[3];
                    cby = a->y + a->style.border[0];
                    cbw = a->w - a->style.border[1] - a->style.border[3];
                    cbh = a->h - a->style.border[0] - a->style.border[2];
                    break;
                }
            }
        }
        int l = resolve(s->offset[3], cbw, AUTO), r = resolve(s->offset[1], cbw, AUTO);
        int t = resolve(s->offset[0], cbh, AUTO), b = resolve(s->offset[2], cbh, AUTO);
        int w;
        if (l != AUTO && r != AUTO && (s->width.unit == 'a' || s->width.unit == 'n')) {
            w = cbw - l - r - margin(n, 1, cbw) - margin(n, 3, cbw);
            if (w < 0) w = 0;
        } else {
            w = used_width(n, cbw, 1);
        }
        int x = l != AUTO ? cbx + l + margin(n, 3, cbw) : r != AUTO ? cbx + cbw - r - w - margin(n, 1, cbw) : pending[i].static_x;
        int y = t != AUTO ? cby + t + margin(n, 0, cbw) : pending[i].static_y;
        abs_base_h = cbh;
        if (t != AUTO && b != AUTO && (s->height.unit == 'a' || s->height.unit == 'n')) { /* stretched between top and bottom */
            forced_node = n;
            forced_h = cbh - t - b - margin(n, 0, cbw) - margin(n, 2, cbw);
            if (forced_h < 0) forced_h = 0;
        }
        layout_box(n, x, y, w);
        forced_node = NULL;
        abs_base_h = -1;
        if (t == AUTO && b != AUTO) shift_subtree(n, 0, cby + cbh - b - n->h - y);
        if (n->y + n->h > page_height) page_height = n->y + n->h;
    }
    pending_count = 0;
}

/* Number the links, in document order. */
static void collect_links(void) {
    for (int i = 0; i < link_count; i++) free(links[i]);
    link_count = 0;
    char href[1024];
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        n->link = -1;
        if (n->type == NODE_ELEMENT && (is(n->tag, "a") || is(n->tag, "area")) && attr(n, "href", href, sizeof(href)) && href[0]) {
            if (!grow((void **)&links, &link_cap, link_count + 1, sizeof(char *))) continue;
            links[link_count] = copy_of(href, (int)strlen(href));
            n->link = link_count++;
        }
    }
}

/* Lay out the whole document `width` pixels wide. */
static void layout_page(int width) {
    layout_pass++;
    item_count = 0;
    pool_len = 0;
    pending_count = 0;
    page_width = width;
    floats_count_now = 0;
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        n->x = n->y = n->w = n->h = 0;
        n->bg_item = -1;
    }
    /* The page background is the root's, or else the body's (which then
       colours the whole page, not just the body's box). */
    int html_bg = html_element->style.has_background, body_bg = body_element->style.has_background;
    page_bg = html_bg ? html_element->style.background : body_bg ? body_element->style.background : 0xFFFFFF;
    if (page_bg >> 24) { /* see-through: over white */
        unsigned a = COLOR_ALPHA(page_bg), r = page_bg >> 16 & 255, g = page_bg >> 8 & 255, b = page_bg & 255;
        page_bg = (r * a + 255 * (255 - a)) / 255 << 16 | (g * a + 255 * (255 - a)) / 255 << 8 | (b * a + 255 * (255 - a)) / 255;
    }
    html_element->style.has_background = 0;
    if (!html_bg) body_element->style.has_background = 0;
    html_element->style.display = D_BLOCK;
    layout_box(html_element, 0, 0, width);
    html_element->style.has_background = (unsigned char)html_bg;
    body_element->style.has_background = (unsigned char)body_bg;
    page_height = html_element->h;
    place_pending();
    for (int i = 0; i < item_count; i++) {
        if (items[i].y + items[i].h > page_height) page_height = items[i].y + items[i].h;
    }
    page_height += 8;
}

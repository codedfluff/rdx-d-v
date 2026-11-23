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
    unsigned char kind, cell, bold, italic, underline, strike;
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

/* Character cell width for a font size: the 8x16 font drawn at 1, 1.5 or
   2 times its size. */
static int cell_for(int size) { return size >= 30 ? 16 : size >= 22 ? 12 : 8; }
static int line_height_for(int size) { return cell_for(size) * 2 + 4; }

static int resolve(Len l, int base, int fallback) {
    if (l.unit == 'p') return l.value;
    if (l.unit == '%') return (int)((long long)l.value * base / 10000);
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
    return d == D_INLINE || d == D_INLINE_BLOCK || d == D_INLINE_FLEX || d == D_TABLE_CELL;
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
            *w = (int)strlen(v) * 8 + 4; /* a picture that failed: its description instead */
            *h = 18;
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
            *w = (int)strlen(label) * 8;
            *h = 16;
            return;
        }
        if (!strcasecmp_word(type, "image")) {
            *w = *h = 20;
            return;
        }
        int size = attr(n, "size", v, sizeof(v)) ? atoi(v) : 20;
        *w = (size > 0 ? size : 20) * 8;
        *h = 16;
        return;
    }
    if (is(n->tag, "select")) {
        int widest = 4;
        for (Node *o = n; o; o = next_in_tree(o, n)) {
            if (o->type == NODE_ELEMENT && is(o->tag, "option")) {
                char text[80];
                int len = text_content(o, text, sizeof(text));
                if (len > widest) widest = len;
            }
        }
        *w = widest * 8 + 20;
        *h = 16;
        return;
    }
    if (is(n->tag, "textarea")) {
        int cols = attr(n, "cols", v, sizeof(v)) ? atoi(v) : 30, rows = attr(n, "rows", v, sizeof(v)) ? atoi(v) : 3;
        *w = (cols > 0 ? cols : 30) * 8;
        *h = (rows > 0 ? rows : 3) * 18;
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

/* The border-box size a replaced element gets in a container `cw` wide. */
static void replaced_size(Node *n, int cw, int *bw, int *bh) {
    int nw, nh;
    natural_size(n, &nw, &nh);
    Style *s = &n->style;
    int fw = frame_h(n, cw), fv = frame_v(n, cw);
    int w = resolve(s->width, cw, -1), h = resolve(s->height, 0, -1);
    if (s->height.unit == '%') h = -1;
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

static void text_widths(Node *t, int *mn, int *mx) {
    int cw = cell_for(t->style.size);
    int longest = 0, total = 0, word = 0, line = 0;
    for (int i = 0; i <= t->text_len; i++) {
        char c = i < t->text_len ? t->text[i] : ' ';
        if (t->style.pre && c == '\n') {
            if (line > total) total = line;
            line = 0;
            if (word > longest) longest = word;
            word = 0;
            continue;
        }
        if (is_space(c) && !t->style.pre) {
            if (word > longest) longest = word;
            if (word) line += word + 1;
            word = 0;
        } else {
            word++;
            if (t->style.pre && i == t->text_len - 1) line += word;
        }
    }
    if (line > total) total = line;
    if (total > 0 && !t->style.pre) total--;
    *mx = total * cw;
    *mn = (t->style.nowrap || t->style.pre ? total : longest) * cw;
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
        int table_row = n->style.display == D_TABLE_ROW;
        int gaps = 0, items = 0;
        for (Node *c = n->first; c; c = c->next) {
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
            } else if (is_inline_level(c)) {
                run_mx += cmx + (c->type == NODE_TEXT && run_mx ? cell_for(c->style.size) : 0);
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

/* Break inline content into atoms: words, boxes, line breaks. */
static void gather(Node *n, int cw) {
    if (n->type == NODE_TEXT) {
        const char *t = n->text;
        int len = n->text_len;
        int cell = cell_for(n->style.size);
        int h = line_height_for(n->style.size);
        if (n->style.pre) {
            int start = 0;
            for (int i = 0; i <= len; i++) {
                if (i < len && t[i] != '\n') continue;
                if (i > start) add_atom((Atom){AT_WORD, 0, n, start, i - start, (i - start) * cell, h});
                if (i < len) add_atom((Atom){AT_BREAK, 0, n, 0, 0, 0, h});
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
            add_atom((Atom){AT_WORD, (unsigned char)pending_space, n, start, i - start, (i - start) * cell, h});
            pending_space = 0;
        }
        return;
    }
    Style *s = &n->style;
    if (s->display == D_NONE) return;
    if (is(n->tag, "br")) {
        add_atom((Atom){AT_BREAK, 0, n, 0, 0, 0, line_height_for(s->size)});
        pending_space = 0;
        return;
    }
    if (s->display == D_INLINE && !is_replaced(n)) {
        int left = pad(n, 3, cw) + s->border[3] + margin(n, 3, cw);
        int right = pad(n, 1, cw) + s->border[1] + margin(n, 1, cw);
        if (left) add_atom((Atom){AT_SPACE, (unsigned char)pending_space, n, 0, 0, left, 0});
        if (left) pending_space = 0;
        for (Node *c = n->first; c; c = c->next) {
            if (c->type == NODE_ELEMENT && (c->style.floating || out_of_flow(c))) {
                add_atom((Atom){AT_BOX, 0, c, 0, 0, 0, 0}); /* placed specially */
                continue;
            }
            gather(c, cw);
        }
        if (right) add_atom((Atom){AT_SPACE, 0, n, 0, 0, right, 0});
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
    add_atom((Atom){AT_BOX, (unsigned char)pending_space, n, 0, 0, n->w + ml + mr, n->h + mt + mb});
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

/* Paint the words of one line: produce items. */
static void emit_word(Atom *a, int x, int y, int line_h) {
    Node *t = a->node;
    Style *s = &t->style;
    int cell = cell_for(s->size);
    int top = y + line_h - a->h + 2;
    /* Backgrounds of inline elements (a highlighted word, a tag-like link). */
    for (Node *e = t->parent; e && e->type == NODE_ELEMENT && e->style.display == D_INLINE && !is_replaced(e); e = e->parent) {
        if (e->style.has_background) {
            int it = new_item(IT_RECT);
            if (it >= 0) {
                items[it].x = x - (a->space_before ? 0 : 0);
                items[it].y = top - 2 - pad(e, 0, 0);
                items[it].w = a->w;
                items[it].h = cell * 2 + 4 + pad(e, 0, 0) + pad(e, 2, 0);
                items[it].color = e->style.background;
                items[it].node = e;
            }
            break;
        }
    }
    /* The bounding boxes of the inline elements it is inside (for clicks). */
    for (Node *e = t->parent; e && e->type == NODE_ELEMENT && e->style.display == D_INLINE; e = e->parent) {
        if (e->w == 0 && e->h == 0) {
            e->x = x;
            e->y = top;
            e->w = a->w;
            e->h = cell * 2;
        } else {
            int x1 = e->x + e->w > x + a->w ? e->x + e->w : x + a->w;
            int y1 = e->y + e->h > top + cell * 2 ? e->y + e->h : top + cell * 2;
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
    item->h = cell * 2;
    item->cell = (unsigned char)cell;
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
            add_atom((Atom){AT_BOX, 0, c, 0, 0, 0, 0});
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
        int lh = line_height_for(ps->size);
        if (atoms[i].kind == AT_BREAK && !first_line) {
            /* nothing */
        }
        int l, r;
        line_edges(left, right, y, lh, &l, &r);
        int width = 0, j = i, in_flow = 0;
        for (; j < atom_count; j++) {
            Atom *a = &atoms[j];
            if (a->kind == AT_BREAK) {
                if (a->h > lh) lh = a->h;
                in_flow = 1;
                j++;
                break;
            }
            if (a->kind == AT_BOX && a->node->type == NODE_ELEMENT && (a->node->style.floating || out_of_flow(a->node))) continue;
            in_flow = 1;
            int space = a->space_before && j > line_start && a->kind != AT_SPACE ? cell_for(a->node->style.size) : 0;
            int fits = width + space + a->w <= r - l;
            int may_wrap = !nowrap && !(a->kind == AT_WORD && a->node->style.nowrap);
            if (!fits && j > line_start && may_wrap) break;
            width += space + a->w;
            if (a->h > lh) lh = a->h;
        }
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
        for (int k = line_start; k < j; k++) {
            Atom *a = &atoms[k];
            if (a->kind == AT_BREAK) continue;
            if (a->kind == AT_BOX && a->node->type == NODE_ELEMENT && (a->node->style.floating || out_of_flow(a->node))) continue;
            int space = a->space_before && k > line_start && a->kind != AT_SPACE ? cell_for(a->node->style.size) : 0;
            /* A link spanning words: stretch its underline across the space. */
            if (space && a->kind == AT_WORD && item_count && items[item_count - 1].kind == IT_TEXT &&
                items[item_count - 1].link >= 0 && items[item_count - 1].link == link_of(a->node) && items[item_count - 1].y == y + lh - a->h + 2)
                items[item_count - 1].w += space;
            x += space;
            if (a->kind == AT_WORD) {
                emit_word(a, x, y, lh);
            } else if (a->kind == AT_BOX) {
                Node *b = a->node;
                shift_subtree(b, x + b->m[3] - b->x, y + lh - a->h + b->m[0] - b->y);
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
    int cell = cell_for(li->style.size);
    int len = (int)strlen(marker);
    int it = new_item(IT_MARKER);
    if (it < 0) return;
    items[it].x = x - (len + 1) * cell;
    items[it].y = y + 2;
    items[it].w = len * cell;
    items[it].h = cell * 2;
    items[it].cell = (unsigned char)cell;
    items[it].color = li->style.color;
    items[it].text = pool_add(marker, len, 0);
    items[it].len = len;
    items[it].node = li;
}

/* Lay out the children of a block container: blocks one under another,
   inline content in lines, floats to the sides. Returns the height used. */
static int layout_flow(Node *n, int left, int top, int width) {
    int y = top;
    int prev_margin = 0;
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
        int mt = margin(c, 0, width), mb = margin(c, 2, width);
        int gap = mt > prev_margin ? mt : prev_margin;
        if (mt < 0 || prev_margin < 0) gap = mt + prev_margin;
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
        y = by + c->h;
        prev_margin = mb;
    }
    y += prev_margin > 0 ? prev_margin : 0;
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

static int layout_grid(Node *n, int left, int top, int width) {
    Style *s = &n->style;
    int columns = s->grid_columns;
    if (columns <= 0) columns = s->grid_min > 0 ? (width + s->gap_col) / (s->grid_min + s->gap_col) : 1;
    if (columns < 1) columns = 1;
    int col_w = (width - s->gap_col * (columns - 1)) / columns;
    int y = top, col = 0, row_h = 0, row_first = item_count;
    Node *row_nodes[64];
    int in_row = 0;
    (void)row_first;
    for (Node *c = n->first;; c = c->next) {
        int end = !c;
        if (!end) {
            if (c->type == NODE_TEXT || c->style.display == D_NONE) continue;
            if (out_of_flow(c)) {
                if (grow((void **)&pending, &pending_cap, pending_count + 1, sizeof(Pending))) pending[pending_count++] = (Pending){c, left, y};
                continue;
            }
            int x = left + col * (col_w + s->gap_col);
            int w = used_width(c, col_w, 0);
            layout_box(c, x + margin(c, 3, col_w), y + margin(c, 0, col_w), w);
            int outer = c->h + margin(c, 0, col_w) + margin(c, 2, col_w);
            if (outer > row_h) row_h = outer;
            if (in_row < 64) row_nodes[in_row++] = c;
            col++;
        }
        if (col == columns || (end && col)) {
            for (int k = 0; k < in_row; k++) {
                if (row_nodes[k]->style.height.unit == 'a') stretch_to(row_nodes[k], row_h - margin(row_nodes[k], 0, col_w) - margin(row_nodes[k], 2, col_w));
            }
            y += row_h + s->gap_row;
            row_h = 0;
            col = 0;
            in_row = 0;
        }
        if (end) break;
    }
    if (y > top) y -= s->gap_row;
    return y - top;
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
    } else if (s->display == D_GRID) {
        content_h = layout_grid(n, content_x, content_y, content_w);
    } else if (s->display == D_TABLE) {
        content_h = layout_table(n, content_x, content_y, content_w);
    } else {
        content_h = layout_flow(n, content_x, content_y, content_w);
    }
    int h = resolve(s->height, 0, -1);
    if (s->height.unit == '%') h = -1;
    if (h >= 0 && s->box_sizing) h -= frame_v(n, cw);
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
        layout_box(n, x, y, w);
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

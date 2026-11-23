/* web/dom.c - the document tree, and the HTML parser that builds it.
   Part of the browser (included by programs/browser.c).

   The parser is forgiving, as browsers must be: it fixes up the usual
   omissions (unclosed <p>, <li>, <td>, <tr>, a missing </head>) and
   ignores stray end tags. Text inside <script> and <style> is kept as is
   for the script engine and the stylesheet parser. */

/* ---------------------------------------------------------------- small helpers */

static int grow(void **array, int *cap, int need, size_t item) {
    if (need <= *cap) return 1;
    int fresh = *cap ? *cap * 2 : 256;
    while (fresh < need) fresh *= 2;
    void *p = realloc(*array, (size_t)fresh * item);
    if (!p) return 0;
    *array = p;
    *cap = fresh;
    return 1;
}

static char *copy_of(const char *s, int n) {
    char *c = malloc((size_t)n + 1);
    if (!c) return NULL;
    memcpy(c, s, (size_t)n);
    c[n] = 0;
    return c;
}

/* Append at most `most` bytes of `src` to `dst`. */
/* (the runtime has no strcat / strncpy / strrchr / memchr / strcspn) */
static char *web_strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}
static char *web_strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
static char *web_strrchr(const char *s, int c) {
    const char *last = NULL;
    for (; *s; s++) {
        if (*s == (char)c) last = s;
    }
    return (char *)last;
}
static void *web_memchr(const void *p, int c, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == (unsigned char)c) return (void *)(b + i);
    }
    return NULL;
}
static size_t web_strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}
#define strcat web_strcat
#define strncpy web_strncpy
#define strrchr web_strrchr
#define memchr web_memchr
#define strcspn web_strcspn

static int web_strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}
#define strcasecmp web_strcasecmp

static void strncat_safe(char *dst, const char *src, size_t most) {
    size_t at = strlen(dst);
    for (size_t i = 0; src[i] && i < most; i++) dst[at++] = src[i];
    dst[at] = 0;
}

enum { NODE_ELEMENT = 1, NODE_TEXT = 3 };

typedef struct Node Node;

/* Lengths in CSS can be absolute, relative to the containing block, or
   "auto"; they are resolved during layout. */
typedef struct {
    int value;      /* px, or percent * 100 */
    char unit;      /* 'p' px, '%' percent, 'c' percent + px (calc), 'a' auto, 'n' none / not set,
                       'x' max-content, 'm' min-content, 'f' fit-content */
    int px;         /* 'c': the px part */
} Len;

typedef struct {
    /* inherited */
    unsigned color;
    int size;                       /* font size in px (rounded) */
    int size64;                     /* font size in 1/64 px, for drawing and measuring text */
    unsigned char bold, italic, underline, strike, pre, nowrap, align, list_none, upper, hidden;
    unsigned char cursor_pointer;
    unsigned char face;             /* FACE_SANS, FACE_SERIF or FACE_MONO */
    unsigned char alpha;            /* opacity, this element's and its ancestors' together (255 opaque) */
    int indent;                     /* text-indent, px */
    int line_height;                /* 0 normal, > 0 px, < 0 a factor of the font size (* -100) */
    /* not inherited */
    unsigned char display, position, floating, clear, box_sizing, overflow_hidden, clip_rect;
    unsigned char masked; /* a mask-image we cannot apply: its background stays unpainted */
    unsigned char caption_bottom; /* caption-side: bottom */
    unsigned background;
    unsigned char has_background;
    int margin[4];                  /* px, AUTO for auto */
    Len margin_len[4];
    Len padding[4];
    int border[4];                  /* px */
    unsigned border_color[4];
    Len width, height, min_width, max_width, min_height, max_height;
    Len offset[4];                  /* top right bottom left, for positioned elements */
    /* flexbox and grid */
    unsigned char flex_dir, flex_wrap, justify, align_items, align_self;
    int grow, shrink;               /* * 100 */
    Len basis;
    int order;
    int gap_row, gap_col;
    /* grid containers: the templates as written (kept until the page goes) */
    const char *grid_cols, *grid_rows, *grid_areas, *grid_auto_cols, *grid_auto_rows;
    unsigned char grid_flow_column;
    /* grid items: lines (0 auto, n a line, -n from the end, GRID_SPAN + n a span) or an area's name */
    short grid_col[2], grid_row[2];
    const char *grid_area;
    unsigned char justify_items, justify_self;
    unsigned char opacity;          /* its own (255 opaque) */
    /* background pictures: the layers as written (url() and gradients), and how they are placed */
    const char *bg_image, *bg_size, *bg_pos;
    unsigned char bg_repeat;        /* 0 repeat, 1 no-repeat, 2 repeat-x, 3 repeat-y */
    /* mask-image: a picture whose see-through parts hide the background (icons drawn in a colour) */
    const char *mask_image, *mask_size, *mask_pos;
    unsigned char mask_repeat;
    /* box-shadow (the first outer one) */
    short shadow[4];                /* x, y, blur, spread */
    unsigned shadow_color;
    unsigned char has_shadow;
    /* transform: only its translation */
    Len tx, ty;
    unsigned char translated;
    const char *content;            /* ::before / ::after: the content property as written */
    unsigned char valign;           /* vertical-align: VA_... */
    Len valign_len;                 /* VA_LENGTH: px or % of the line height */
    short radius[8];                /* corners (top-left, top-right, bottom-right, bottom-left), across then down:
                                       px, or -percent */
    int z_index;
} Style;

struct Node {
    int type;
    char tag[16];
    char *attrs;             /* the tag's attribute text (elements) */
    char *text;              /* text nodes: the text, entities decoded */
    int text_len;
    Node *parent, *first, *last, *next, *prev;
    Style style;
    int styled;
    /* layout: the border box, in page coordinates */
    int x, y, w, h;
    int m[4];                /* used margins: top right bottom left */
    int items_from, items_to;/* its range in the display list */
    int bg_item;             /* its background item, or -1 */
    int min_content, max_content, intrinsic_done;
    int base_y, base_pass;   /* its last line's baseline (page y), and the layout pass that set it */
    int trail_margin;        /* its last child's bottom margin, when it collapses through this box's */
    unsigned char pseudo_flags; /* style sheets give it a ::before (1) / ::after (2) */
    /* <a href> */
    int link;
    /* <img> */
    void *img;               /* an image once loaded */
    int img_state;           /* 0 not tried, 1 loaded, 2 failed */
    int img_w, img_h;        /* natural size once loaded */
    void *bg_img;            /* its background picture (shared: the page's picture cache owns it) */
    signed char bg_state;    /* 0 not tried, 1 loaded, 2 failed */
    void *mask_img;          /* its mask-image (shared, like bg_img) */
    signed char mask_state;
    /* form controls */
    char *value;
    int checked, selected_index;
    /* the script engine's object for this node, and handlers */
    void *js;
    /* for matching selectors quickly: hashes of tag, id and classes */
    unsigned tag_key, id_key, class_keys[8];
    unsigned char class_count;
    unsigned long long ancestors[4]; /* a filter of the keys of all its ancestors */
    char *css_vars;          /* its CSS custom properties: "--name\0value\0" pairs */
    int css_vars_len;
    char *svg_css;           /* inside <svg>: paint properties from style sheets, "name\0value\0" pairs */
    int svg_css_len;
    unsigned char in_svg;
};

static Node *document;     /* the root (#document) */
static Node *html_element, *body_element, *head_element;
static int node_count;

static Node *new_node(int type, const char *tag) {
    Node *n = malloc(sizeof(Node));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->link = -1;
    if (tag) {
        size_t k = 0;
        for (; tag[k] && k < 15; k++) n->tag[k] = (char)tolower((unsigned char)tag[k]);
        n->tag[k] = 0;
    }
    node_count++;
    return n;
}

static void append_child(Node *parent, Node *child) {
    child->parent = parent;
    child->prev = parent->last;
    child->next = NULL;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

static void remove_child(Node *child) {
    Node *p = child->parent;
    if (!p) return;
    if (child->prev) child->prev->next = child->next;
    else p->first = child->next;
    if (child->next) child->next->prev = child->prev;
    else p->last = child->prev;
    child->parent = child->prev = child->next = NULL;
}

static void insert_before(Node *parent, Node *child, Node *before) {
    if (!before) {
        append_child(parent, child);
        return;
    }
    child->parent = parent;
    child->next = before;
    child->prev = before->prev;
    if (before->prev) before->prev->next = child;
    else parent->first = child;
    before->prev = child;
}

static void scaled_forget(const image *img); /* (paint.c) */

static void free_tree(Node *n) {
    if (!n) return;
    for (Node *c = n->first; c;) {
        Node *next = c->next;
        free_tree(c);
        c = next;
    }
    free(n->attrs);
    free(n->text);
    free(n->value);
    free(n->css_vars);
    free(n->svg_css);
    if (n->img) {
        scaled_forget((image *)n->img);
        image_free((image *)n->img);
        free(n->img);
    }
    free(n);
    node_count--;
}

/* Walk the tree in document order: the node after `n` (descending into
   children), stopping at `stop`'s subtree end. */
static Node *next_in_tree(Node *n, Node *stop) {
    if (n->first) return n->first;
    while (n && n != stop) {
        if (n->next) return n->next;
        n = n->parent;
    }
    return NULL;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is(const char *name, const char *want) { return strcmp(name, want) == 0; }

/* 0 if `a` is the word `b`, ignoring case (like strcasecmp). */
static int strcasecmp_word(const char *a, const char *b) {
    size_t n = strlen(b);
    return strncasecmp(a, b, n) != 0 || a[n] != 0;
}

static int in_list(const char *name, const char *const *list) {
    for (; *list; list++) {
        if (!strcmp(name, *list)) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- attributes */

/* The value of attribute `name` in attribute text, or 0 if absent. */
static int decode_text(char *text, int len);

/* Walks the attributes one by one (so a name inside another attribute's
   value never matches), and decodes &entities; in the value. */
static int attribute_in(const char *attrs, int len, const char *name, char *out, int max) {
    int n = (int)strlen(name);
    int i = 0;
    while (i < len) {
        while (i < len && (is_space(attrs[i]) || attrs[i] == '/')) i++;
        if (i >= len) break;
        int name_start = i;
        while (i < len && !is_space(attrs[i]) && attrs[i] != '=' && attrs[i] != '>' && (attrs[i] != '/' || i == name_start)) i++;
        int name_len = i - name_start;
        if (name_len == 0) {
            i++;
            continue;
        }
        while (i < len && is_space(attrs[i])) i++;
        int value_start = -1, value_len = 0;
        if (i < len && attrs[i] == '=') {
            i++;
            while (i < len && is_space(attrs[i])) i++;
            char quote = i < len && (attrs[i] == '"' || attrs[i] == '\'') ? attrs[i++] : 0;
            value_start = i;
            while (i < len && (quote ? attrs[i] != quote : !is_space(attrs[i]) && attrs[i] != '>')) i++;
            value_len = i - value_start;
            if (quote && i < len) i++;
        }
        if (name_len != n || strncasecmp(attrs + name_start, name, (size_t)n) != 0) continue;
        if (max <= 0) return 1;
        int k = value_start < 0 ? 0 : value_len < max - 1 ? value_len : max - 1;
        if (k) memcpy(out, attrs + value_start, (size_t)k);
        out[k] = 0;
        if (k && strchr(out, '&')) out[decode_text(out, k)] = 0;
        return 1;
    }
    return 0;
}

static int attr(const Node *n, const char *name, char *out, int max) {
    if (!n->attrs) return 0;
    return attribute_in(n->attrs, (int)strlen(n->attrs), name, out, max);
}

static int has_attr(const Node *n, const char *name) {
    char dummy[2];
    return attr(n, name, dummy, sizeof(dummy));
}

/* Replace (or add) an attribute in a node's attribute text. */
static void set_attr(Node *n, const char *name, const char *value) {
    size_t old_len = n->attrs ? strlen(n->attrs) : 0;
    char *fresh = malloc(old_len + strlen(name) + strlen(value) * 6 + 8);
    if (!fresh) return;
    size_t k = 0;
    /* Copy the old text without any existing `name=...`. */
    const char *a = n->attrs ? n->attrs : "";
    size_t nl = strlen(name);
    for (size_t i = 0; i < old_len;) {
        if ((i == 0 || is_space(a[i - 1])) && strncasecmp(a + i, name, nl) == 0 &&
            (a[i + nl] == '=' || is_space(a[i + nl]) || !a[i + nl])) {
            size_t j = i + nl;
            while (is_space(a[j])) j++;
            if (a[j] == '=') {
                j++;
                while (is_space(a[j])) j++;
                char q = (a[j] == '"' || a[j] == '\'') ? a[j++] : 0;
                while (a[j] && (q ? a[j] != q : !is_space(a[j]))) j++;
                if (q && a[j]) j++;
            }
            i = j;
            continue;
        }
        fresh[k++] = a[i++];
    }
    if (k && !is_space(fresh[k - 1])) fresh[k++] = ' ';
    memcpy(fresh + k, name, nl);
    k += nl;
    fresh[k++] = '=';
    fresh[k++] = '"';
    for (const char *v = value; *v; v++) {
        if (*v == '"') {
            memcpy(fresh + k, "&quot;", 6);
            k += 6;
        } else if (*v == '&') { /* read back through entity decoding, so escape it */
            memcpy(fresh + k, "&amp;", 5);
            k += 5;
        } else {
            fresh[k++] = *v;
        }
    }
    fresh[k++] = '"';
    fresh[k] = 0;
    free(n->attrs);
    n->attrs = fresh;
}

/* ---------------------------------------------------------------- characters */

static int hex_digit(char c) {
    c = (char)tolower((unsigned char)c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Turn a Unicode character into something the 8x16 font can show. */
static int ascii_for(unsigned code, char *out) {
    if (code < 128) {
        out[0] = (char)code;
        return 1;
    }
    const char *s = NULL;
    switch (code) {
    case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: s = " "; break;
    case 0x2018: case 0x2019: case 0x201A: case 0xB4: case 0x2032: s = "'"; break;
    case 0x201C: case 0x201D: case 0x201E: case 0xAB: case 0xBB: case 0x2033: s = "\""; break;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: s = "-"; break;
    case 0x2026: s = "..."; break;
    case 0xA9: s = "(c)"; break;
    case 0xAE: s = "(R)"; break;
    case 0x2122: s = "(TM)"; break;
    case 0x2022: case 0xB7: case 0x25CF: case 0x2219: case 0x22C5: s = "*"; break;
    case 0xD7: case 0x2715: case 0x2716: s = "x"; break;
    case 0xDF: s = "ss"; break;
    case 0xB0: s = " deg"; break;
    case 0x20AC: s = "EUR"; break;
    case 0xA3: s = "GBP"; break;
    case 0xA5: s = "JPY"; break;
    case 0x2192: case 0x203A: case 0x25B6: case 0x25B8: s = ">"; break;
    case 0x2190: case 0x2039: case 0x25C0: s = "<"; break;
    case 0x2191: case 0x25B2: s = "^"; break;
    case 0x2193: case 0x25BC: s = "v"; break;
    case 0x2713: case 0x2714: s = "v"; break;
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: s = ""; break;
    }
    if (!s) {
        static const char *latin = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        if (code >= 0xC0 && code <= 0xFF) {
            out[0] = latin[code - 0xC0];
            return 1;
        }
        s = "?";
    }
    int n = (int)strlen(s);
    memcpy(out, s, (size_t)n);
    return n;
}

/* A character as UTF-8 (characters that show nothing: none). */
static int utf8_for(unsigned code, char *out) {
    if (code == 0x200B || code == 0x200C || code == 0x200D || code == 0xFEFF || code == 0xAD) return 0;
    if (!code || (code >= 0xD800 && code < 0xE000) || code > 0x10FFFF) code = 0xFFFD;
    if (code < 0x80) {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800) {
        out[0] = (char)(0xC0 | code >> 6);
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code < 0x10000) {
        out[0] = (char)(0xE0 | code >> 12);
        out[1] = (char)(0x80 | (code >> 6 & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | code >> 18);
    out[1] = (char)(0x80 | (code >> 12 & 0x3F));
    out[2] = (char)(0x80 | (code >> 6 & 0x3F));
    out[3] = (char)(0x80 | (code & 0x3F));
    return 4;
}

/* UTF-8 text as plain ASCII (for the text console): into out (max bytes,
   NUL-terminated); returns the length. */
static int ascii_text(const char *s, int len, char *out, int max) {
    int k = 0;
    for (int i = 0; i < len && k < max - 1;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out[k++] = (char)c;
            i++;
            continue;
        }
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        unsigned code = c & (0x3F >> extra);
        int j = 1;
        for (; j <= extra && i + j < len && ((unsigned char)s[i + j] & 0xC0) == 0x80; j++) code = code << 6 | ((unsigned char)s[i + j] & 0x3F);
        i += j;
        char buf[8];
        int m = ascii_for(extra ? code : 0xFFFD, buf);
        for (int q = 0; q < m && k < max - 1; q++) out[k++] = buf[q];
    }
    out[k] = 0;
    return k;
}

/* Decode &entities; in place, keeping text as UTF-8 (the result is never
   longer; bytes that are not UTF-8 are read as Latin-1 and shown as their
   nearest ASCII). */
static int decode_text(char *text, int len) {
    int out = 0;
    for (int i = 0; i < len;) {
        unsigned char c = (unsigned char)text[i];
        char buf[8];
        int n;
        if (c == '&') {
            int end = i + 1;
            while (end < len && end - i < 12 && text[end] != ';' && !is_space(text[end]) && text[end] != '&' && text[end] != '<') end++;
            unsigned code = 0;
            int known = 0;
            if (end < len && text[end] == ';') {
                const char *e = text + i + 1;
                int el = end - i - 1;
                if (el > 1 && e[0] == '#') {
                    known = 1;
                    if (e[1] == 'x' || e[1] == 'X') {
                        for (int k = 2; k < el; k++) code = code * 16 + (unsigned)(hex_digit(e[k]) < 0 ? 0 : hex_digit(e[k]));
                    } else {
                        for (int k = 1; k < el; k++) code = code * 10 + (unsigned)(e[k] - '0');
                    }
                } else {
                    static const struct {
                        const char *name;
                        unsigned code;
                    } names[] = {{"amp", '&'},      {"lt", '<'},       {"gt", '>'},        {"quot", '"'},
                                 {"apos", '\''},    {"nbsp", 0xA0},    {"copy", 0xA9},     {"reg", 0xAE},
                                 {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"rsquo", 0x2019},
                                 {"lsquo", 0x2018}, {"rdquo", 0x201D}, {"ldquo", 0x201C},  {"bull", 0x2022},
                                 {"middot", 0xB7},  {"times", 0xD7},   {"trade", 0x2122},  {"laquo", 0xAB},
                                 {"raquo", 0xBB},   {"euro", 0x20AC},  {"pound", 0xA3},    {"deg", 0xB0},
                                 {"rarr", 0x2192},  {"larr", 0x2190},  {"uarr", 0x2191},   {"darr", 0x2193},
                                 {"eacute", 0xE9},  {"egrave", 0xE8},  {"aacute", 0xE1},   {"agrave", 0xE0},
                                 {"uuml", 0xFC},    {"ouml", 0xF6},    {"auml", 0xE4},     {"szlig", 0xDF},
                                 {"shy", 0xAD},     {"zwnj", 0x200C},  {"zwj", 0x200D},    {"thinsp", 0x2009},
                                 {"ensp", 0x2002},  {"emsp", 0x2003},  {"hyphen", 0x2010}, {"yen", 0xA5},
                                 {"cent", 0xA2},    {"sect", 0xA7},    {"para", 0xB6},     {"check", 0x2713}};
                    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
                        if ((int)strlen(names[k].name) == el && memcmp(names[k].name, e, (size_t)el) == 0) {
                            code = names[k].code;
                            known = 1;
                        }
                    }
                }
            }
            if (known && code) {
                n = utf8_for(code, buf);
                if (out + n > end + 1) n = ascii_for(code, buf);
                i = end + 1;
            } else {
                buf[0] = '&';
                n = 1;
                i++;
            }
        } else if (c >= 0x80) {
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            unsigned code = c & (0x3F >> extra);
            int k = 1;
            for (; k <= extra && i + k < len && ((unsigned char)text[i + k] & 0xC0) == 0x80; k++)
                code = code << 6 | ((unsigned char)text[i + k] & 0x3F);
            if (extra && k == extra + 1) n = utf8_for(code, buf); /* (k bytes in, never more out) */
            else {
                n = ascii_for(c, buf); /* not UTF-8: a Latin-1 byte */
                k = 1;
            }
            i += k;
        } else {
            buf[0] = (char)c;
            n = 1;
            i++;
        }
        if (out + n > i) n = i - out;
        memcpy(text + out, buf, (size_t)n);
        out += n;
    }
    return out;
}

/* ---------------------------------------------------------------- the parser */

static const char *const VOID_TAGS[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                        "param", "source", "track", "wbr", "keygen", NULL};
static const char *const CLOSES_P[] = {"address", "article", "aside", "blockquote", "div", "dl", "fieldset", "figure",
                                       "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "main",
                                       "menu", "nav", "ol", "p", "pre", "section", "table", "ul", "details", NULL};
static const char *const HEAD_TAGS[] = {"title", "meta", "link", "style", "script", "base", "noscript", "template", NULL};
static const char *const RAW_TAGS[] = {"script", "style", "textarea", "title", "xmp", NULL};

/* The innermost open element called `name`, below the nearest of `boundary`. */
static Node *open_element(Node *current, const char *name, const char *const *boundary) {
    for (Node *n = current; n && n != document; n = n->parent) {
        if (n->type == NODE_ELEMENT && is(n->tag, name)) return n;
        if (boundary && in_list(n->tag, boundary)) return NULL;
    }
    return NULL;
}

static Node *make_text(const char *t, int len, int decode) {
    Node *n = new_node(NODE_TEXT, NULL);
    if (!n) return NULL;
    n->text = malloc((size_t)len + 1);
    if (!n->text) return n;
    memcpy(n->text, t, (size_t)len);
    n->text_len = decode ? decode_text(n->text, len) : len;
    n->text[n->text_len] = 0;
    return n;
}

/* Parse a whole document. */
static void parse_html(const char *html, int len) {
    document = new_node(NODE_ELEMENT, "#document");
    html_element = new_node(NODE_ELEMENT, "html");
    head_element = new_node(NODE_ELEMENT, "head");
    body_element = new_node(NODE_ELEMENT, "body");
    append_child(document, html_element);
    append_child(html_element, head_element);
    append_child(html_element, body_element);
    Node *current = head_element;
    int in_body = 0;

    int i = 0;
    while (i < len) {
        if (html[i] != '<') {
            int start = i;
            while (i < len && html[i] != '<') i++;
            int all_space = 1;
            for (int k = start; k < i && all_space; k++) all_space = is_space(html[k]);
            if (!in_body && !all_space) { /* text means the body has begun */
                current = body_element;
                in_body = 1;
            }
            if (in_body || !all_space) {
                Node *t = make_text(html + start, i - start, 1);
                if (t) append_child(current, t);
            }
            continue;
        }
        if (i + 3 < len && memcmp(html + i, "<!--", 4) == 0) {
            const char *end = strstr(html + i + 4, "-->");
            i = end ? (int)(end - html) + 3 : len;
            continue;
        }
        int j = i + 1;
        int closing = j < len && html[j] == '/';
        if (closing) j++;
        char name[16];
        int n = 0;
        while (j < len && (is_alpha(html[j]) || (html[j] >= '0' && html[j] <= '9') || html[j] == '-' || html[j] == ':')) {
            if (n < 15) name[n++] = (char)tolower((unsigned char)html[j]);
            j++;
        }
        name[n] = 0;
        if (n == 0) {
            if (html[i + 1] == '!' || html[i + 1] == '?') { /* <!DOCTYPE>, <?xml?> */
                while (i < len && html[i] != '>') i++;
                i++;
                continue;
            }
            Node *t = make_text("<", 1, 0); /* a lone "<" */
            if (t) append_child(current, t);
            i++;
            continue;
        }
        int attrs_start = j;
        char quote = 0;
        while (j < len && (quote || html[j] != '>')) {
            if (quote) {
                if (html[j] == quote) quote = 0;
            } else if ((html[j] == '"' || html[j] == '\'') && j > attrs_start && (html[j - 1] == '=' || is_space(html[j - 1]))) {
                quote = html[j];
            }
            j++;
        }
        int attrs_len = j - attrs_start;
        int self_closing = j > attrs_start && html[j - 1] == '/'; /* <path ... /> */
        while (attrs_len > 0 && (is_space(html[attrs_start + attrs_len - 1]) || html[attrs_start + attrs_len - 1] == '/')) attrs_len--;
        i = j < len ? j + 1 : len;

        if (closing) {
            if (is(name, "head")) {
                current = body_element;
                continue;
            }
            if (is(name, "body") || is(name, "html")) continue; /* everything ends in the body anyway */
            if (is(name, "br")) {
                append_child(current, new_node(NODE_ELEMENT, "br"));
                continue;
            }
            if (is(name, "p") && !open_element(current, "p", (const char *const[]){"div", "td", "th", "li", "body", NULL})) {
                append_child(current, new_node(NODE_ELEMENT, "p")); /* </p> with no <p> makes an empty one */
                continue;
            }
            Node *open = open_element(current, name, NULL);
            if (open && open != body_element && open != head_element) current = open->parent;
            continue;
        }

        /* Where the element goes. */
        if (is(name, "html") || is(name, "body") || is(name, "head")) {
            /* Merge their attributes into the elements already made. */
            Node *target = is(name, "html") ? html_element : is(name, "body") ? body_element : head_element;
            if (attrs_len > 0 && !target->attrs) target->attrs = copy_of(html + attrs_start, attrs_len);
            if (is(name, "body")) {
                current = body_element;
                in_body = 1;
            }
            continue;
        }
        if (!in_body && !in_list(name, HEAD_TAGS)) {
            current = body_element;
            in_body = 1;
        }
        if (in_list(name, CLOSES_P)) {
            Node *p = open_element(current, "p", (const char *const[]){"div", "td", "th", "li", "blockquote", "body", "button", NULL});
            if (p) current = p->parent;
        }
        if (is(name, "li")) {
            Node *li = open_element(current, "li", (const char *const[]){"ul", "ol", "menu", NULL});
            if (li) current = li->parent;
        }
        if (is(name, "dt") || is(name, "dd")) {
            Node *d = open_element(current, "dd", (const char *const[]){"dl", NULL});
            if (!d) d = open_element(current, "dt", (const char *const[]){"dl", NULL});
            if (d) current = d->parent;
        }
        if (is(name, "tr")) {
            Node *tr = open_element(current, "tr", (const char *const[]){"table", NULL});
            if (tr) current = tr->parent;
        }
        if (is(name, "td") || is(name, "th")) {
            Node *cell = open_element(current, "td", (const char *const[]){"tr", "table", NULL});
            if (!cell) cell = open_element(current, "th", (const char *const[]){"tr", "table", NULL});
            if (cell) current = cell->parent;
        }
        if (is(name, "option")) {
            Node *o = open_element(current, "option", (const char *const[]){"select", NULL});
            if (o) current = o->parent;
        }
        if (is(name, "a")) { /* links do not nest */
            Node *a = open_element(current, "a", NULL);
            if (a) current = a->parent;
        }

        Node *e = new_node(NODE_ELEMENT, name);
        if (!e) break;
        if (attrs_len > 0) e->attrs = copy_of(html + attrs_start, attrs_len);
        append_child(current, e);
        if (in_list(name, RAW_TAGS)) {
            /* Content up to the end tag, as text. */
            char end_tag[24] = "</";
            strcpy(end_tag + 2, name);
            int k = i;
            size_t el = strlen(end_tag);
            while (k < len && strncasecmp(html + k, end_tag, el) != 0) k++;
            if (k > i) {
                /* Script and style text is code: no entity decoding. */
                Node *t = make_text(html + i, k - i, is(name, "title") || is(name, "textarea"));
                if (t) append_child(e, t);
            }
            while (k < len && html[k] != '>') k++;
            i = k < len ? k + 1 : len;
            continue;
        }
        /* In SVG, <x/> is complete; in HTML the slash means nothing. */
        if (!in_list(name, VOID_TAGS) && !(self_closing && open_element(e, "svg", NULL))) current = e;
    }
}

/* All the text inside a node (as textContent does). */
static int text_content(const Node *n, char *out, int max) {
    int k = 0;
    for (const Node *c = n->first; c; c = c->next) {
        if (c->type == NODE_TEXT) {
            for (int i = 0; i < c->text_len && k < max - 1; i++) out[k++] = c->text[i];
        } else {
            k += text_content(c, out + k, max - k);
        }
    }
    if (max > 0) out[k < max ? k : max - 1] = 0;
    return k;
}

static Node *find_element(Node *root, const char *tag) {
    for (Node *n = root; n; n = next_in_tree(n, root)) {
        if (n->type == NODE_ELEMENT && is(n->tag, tag)) return n;
    }
    return NULL;
}

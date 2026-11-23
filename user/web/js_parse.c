/* web/js_parse.c - reading JavaScript: the lexer and a recursive-descent
   parser that builds a syntax tree. Part of the browser. */

enum {
    /* expressions */
    N_NUM = 1, N_STR, N_TEMPLATE, N_REGEX, N_IDENT, N_THIS, N_SUPER, N_ARRAY, N_OBJECT, N_PROP, N_FUNC, N_CLASS,
    N_CALL, N_NEW, N_MEMBER, N_INDEX, N_UNARY, N_UPDATE, N_BINARY, N_LOGICAL, N_ASSIGN, N_COND, N_SEQ, N_SPREAD,
    N_HOLE, N_NEWTARGET, N_TAGGED,
    /* statements */
    N_VAR, N_DECL, N_EXPR, N_BLOCK, N_IF, N_FOR, N_FORIN, N_WHILE, N_DOWHILE, N_RETURN, N_BREAK, N_CONTINUE,
    N_THROW, N_TRY, N_SWITCH, N_CASE, N_FUNCDECL, N_CLASSDECL, N_EMPTY, N_LABEL,
    /* a function body not parsed yet: num = where its { is in the source,
       op = just past its }, line = its line, cache = the source */
    N_LAZY
};

/* flags */
enum {
    F_ARROW = 1, F_EXPR_BODY = 2, F_ASYNC = 4, F_METHOD = 8, F_GETTER = 16, F_SETTER = 32, F_STATIC = 64,
    F_COMPUTED = 128, F_SHORTHAND = 256, F_SPREAD = 512, F_OPTIONAL = 1024, F_PREFIX = 2048, F_FOR_OF = 4096,
    F_FIELD = 8192, F_USES_ARGS = 16384, F_LEXICAL = 32768, F_CTOR = 65536, F_GENERATOR = 131072, F_CHAIN = 262144,
    /* a lazy body (N_LAZY) starts "use strict" / mentions `arguments` */
    F_LAZY_STRICT = 524288, F_LAZY_ARGS = 1048576
};

struct Ast { /* (88 bytes: big scripts make hundreds of thousands) */
    unsigned type : 8;
    unsigned flags : 24;
    int op;      /* operator (punctuator code), or 'v' / 'l' / 'c' for var / let / const */
    int line;
    int cache_len;
    Ast *a, *b, *c, *d;
    Ast *list;   /* first child of a list */
    Ast *next;   /* next in the list this node is in */
    double num;
    JsStr *str;
    void *cache;   /* function: hoisted names; regex: compiled program */
};

/* Punctuators are packed as up to four characters. */
#define P1(a) ((int)(a))
#define P2(a, b) ((int)(a) | (int)(b) << 8)
#define P3(a, b, c) ((int)(a) | (int)(b) << 8 | (int)(c) << 16)
#define P4(a, b, c, d) ((int)(a) | (int)(b) << 8 | (int)(c) << 16 | (int)(d) << 24)

static int memchr_safe(const char *s, int n, char c);

enum { T_EOF, T_NUM, T_STR, T_IDENT, T_PUNCT, T_REGEX, T_TPL_FULL, T_TPL_HEAD, T_TPL_MID, T_TPL_TAIL, T_PRIVATE, T_BIG };

static JsStr *bigint_literal(const char *text, int len); /* (js_bigint.c) */

typedef struct {
    int type;
    int punct;
    double num;
    JsStr *str;      /* identifier name, string value, regex pattern, template chunk */
    JsStr *flags;    /* regex flags */
    int line;
    int newline_before;
    int start;       /* position in the source */
} Token;

#define MAX_BRACES 64

typedef struct {
    const char *src;
    int len, pos, line;
    Token tok;
    int regex_ok;              /* a '/' here would start a regular expression */
    unsigned char braces[MAX_BRACES]; /* 1: a template's ${ ... } */
    int depth;
} Lexer;

static Lexer L;
static int parse_failed;
static char parse_error[300];
static void syntax_error(const char *what);

/* Syntax trees live in an arena that is freed with the page. */
typedef struct Arena {
    struct Arena *next;
    int used;
    char data[65536 - 32];
} Arena;
static Arena *ast_arena;
static int arena_chunks; /* (64 KiB each) */

static void *arena_alloc(int size) {
    size = (size + 7) & ~7;
    if (!ast_arena || ast_arena->used + size > (int)sizeof(ast_arena->data)) {
        Arena *a = malloc(sizeof(Arena));
        if (!a) {
            js_oom = 1;
            static Ast dummy;
            memset(&dummy, 0, sizeof(dummy));
            return &dummy;
        }
        a->next = ast_arena;
        a->used = 0;
        ast_arena = a;
        arena_chunks++;
    }
    void *p = ast_arena->data + ast_arena->used;
    ast_arena->used += size;
    memset(p, 0, (size_t)size);
    return p;
}

/* Scripts' source text, kept while their syntax trees live: function
   bodies are parsed only when first called (most of a big page's code
   never is, and a parsed tree takes about 20 times the source's size). */
typedef struct KeptSource {
    struct KeptSource *next;
    char text[];
} KeptSource;
static KeptSource *kept_sources;
static size_t kept_bytes;

static void arena_free_all(void) {
    kept_bytes = 0;
    while (kept_sources) {
        KeptSource *next = kept_sources->next;
        free(kept_sources);
        kept_sources = next;
    }
    while (ast_arena) {
        Arena *next = ast_arena->next;
        free(ast_arena);
        arena_chunks--;
        ast_arena = next;
    }
}

static Ast *node(int type) {
    Ast *n = arena_alloc(sizeof(Ast));
    n->type = (unsigned char)type;
    n->line = L.tok.line;
    return n;
}

static char *parse_stack_base;
static char *js_stack_base; /* (js_eval.c: where scripts began on the stack) */

/* Too deep to go on safely? (deeply nested code would exhaust the stack) */
static int parse_too_deep(void) {
    char here;
    if (parse_stack_base && parse_stack_base - &here > 6500000) { /* (of the 8 MiB the main stack may grow to) */
        syntax_error("the code is nested too deeply");
        return 1;
    }
    return 0;
}

static void syntax_error(const char *what) {
    if (parse_failed) return;
    parse_failed = 1;
    char line[16];
    int v = L.tok.line, k = 0;
    char d[12];
    do d[k++] = (char)('0' + v % 10);
    while (v /= 10);
    int m = 0;
    while (k) line[m++] = d[--k];
    line[m] = 0;
    strcpy(parse_error, "SyntaxError: ");
    strncat_safe(parse_error, what, 120);
    strncat_safe(parse_error, " (line ", 8);
    strncat_safe(parse_error, line, 12);
    strncat_safe(parse_error, ", near: ", 9);
    /* the code around the problem, on one line */
    char near[64];
    int from = L.tok.start - 30 > 0 ? L.tok.start - 30 : 0, k2 = 0;
    for (int i = from; i < L.len && i < from + 60 && k2 < 63; i++) near[k2++] = L.src[i] == '\n' || L.src[i] == '\r' || L.src[i] == '\t' ? ' ' : L.src[i];
    near[k2] = 0;
    strncat_safe(parse_error, near, 64);
}

/* ---------------------------------------------------------------- the lexer */

static int ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 128; }
static int ident_part(int c) { return ident_start(c) || (c >= '0' && c <= '9'); }

static void put_utf8(Buf *b, unsigned code) {
    char t[4];
    if (code < 0x80) {
        t[0] = (char)code;
        buf_add(b, t, 1);
    } else if (code < 0x800) {
        t[0] = (char)(0xC0 | code >> 6);
        t[1] = (char)(0x80 | (code & 63));
        buf_add(b, t, 2);
    } else if (code < 0x10000) {
        t[0] = (char)(0xE0 | code >> 12);
        t[1] = (char)(0x80 | ((code >> 6) & 63));
        t[2] = (char)(0x80 | (code & 63));
        buf_add(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | code >> 18);
        t[1] = (char)(0x80 | ((code >> 12) & 63));
        t[2] = (char)(0x80 | ((code >> 6) & 63));
        t[3] = (char)(0x80 | (code & 63));
        buf_add(b, t, 4);
    }
}

/* An escape after a backslash at L.pos (which points past the '\'). */
static void lex_escape(Buf *b) {
    if (L.pos >= L.len) return;
    char c = L.src[L.pos++];
    switch (c) {
    case 'n': buf_ch(b, '\n'); return;
    case 't': buf_ch(b, '\t'); return;
    case 'r': buf_ch(b, '\r'); return;
    case 'b': buf_ch(b, '\b'); return;
    case 'f': buf_ch(b, '\f'); return;
    case 'v': buf_ch(b, '\v'); return;
    case '0':
        if (L.pos >= L.len || L.src[L.pos] < '0' || L.src[L.pos] > '9') {
            buf_add(b, "", 1);
            return;
        }
        break;
    case '\r':
        if (L.pos < L.len && L.src[L.pos] == '\n') L.pos++;
        L.line++;
        return;
    case '\n':
        L.line++;
        return;
    case 'x': {
        unsigned v = 0;
        for (int i = 0; i < 2 && L.pos < L.len && hex_digit(L.src[L.pos]) >= 0; i++) v = v * 16 + (unsigned)hex_digit(L.src[L.pos++]);
        put_utf8(b, v);
        return;
    }
    case 'u': {
        unsigned v = 0;
        if (L.pos < L.len && L.src[L.pos] == '{') {
            L.pos++;
            while (L.pos < L.len && L.src[L.pos] != '}') {
                if (hex_digit(L.src[L.pos]) >= 0) v = v * 16 + (unsigned)hex_digit(L.src[L.pos]);
                L.pos++;
            }
            L.pos++;
        } else {
            for (int i = 0; i < 4 && L.pos < L.len && hex_digit(L.src[L.pos]) >= 0; i++) v = v * 16 + (unsigned)hex_digit(L.src[L.pos++]);
            /* a surrogate pair */
            if (v >= 0xD800 && v < 0xDC00 && L.pos + 5 < L.len && L.src[L.pos] == '\\' && L.src[L.pos + 1] == 'u') {
                unsigned lo = 0;
                int ok = 1;
                for (int i = 0; i < 4; i++) {
                    int d = hex_digit(L.src[L.pos + 2 + i]);
                    if (d < 0) ok = 0;
                    lo = lo * 16 + (unsigned)(d < 0 ? 0 : d);
                }
                if (ok && lo >= 0xDC00 && lo < 0xE000) {
                    L.pos += 6;
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                }
            }
        }
        put_utf8(b, v);
        return;
    }
    }
    buf_ch(b, c);
}

/* A chunk of a template literal, from L.pos up to ` or ${. */
static void lex_template_chunk(int head) {
    Buf b = {0};
    int tail = 0;
    while (L.pos < L.len) {
        char c = L.src[L.pos];
        if (c == '`') {
            L.pos++;
            tail = 1;
            break;
        }
        if (c == '$' && L.pos + 1 < L.len && L.src[L.pos + 1] == '{') {
            L.pos += 2;
            if (L.depth < MAX_BRACES) L.braces[L.depth++] = 1;
            break;
        }
        if (c == '\\') {
            L.pos++;
            lex_escape(&b);
            continue;
        }
        if (c == '\n') L.line++;
        if (c == '\r') { /* template text normalises CR LF to LF */
            L.pos++;
            if (L.pos < L.len && L.src[L.pos] == '\n') continue;
            buf_ch(&b, '\n');
            continue;
        }
        buf_ch(&b, c);
        L.pos++;
    }
    L.tok.type = head ? (tail ? T_TPL_FULL : T_TPL_HEAD) : (tail ? T_TPL_TAIL : T_TPL_MID);
    L.tok.str = interned(buf_done(&b));
    L.regex_ok = 0;
}

static int is_keyword_before_regex(const JsStr *s) {
    static const char *words[] = {"return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "throw",
                                  "case", "do", "else", "yield", "await", NULL};
    for (int i = 0; words[i]; i++) {
        if ((int)strlen(words[i]) == s->len && memcmp(words[i], s->s, (size_t)s->len) == 0) return 1;
    }
    return 0;
}

static void next_token(void) {
    Token *t = &L.tok;
    t->newline_before = 0;
    t->str = NULL;
    if (parse_failed) {
        t->type = T_EOF;
        return;
    }
    /* whitespace and comments */
    for (;;) {
        if (L.pos >= L.len) break;
        unsigned char c = (unsigned char)L.src[L.pos];
        if (c == '\n') {
            L.line++;
            t->newline_before = 1;
            L.pos++;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            L.pos++;
        } else if (c == 0xC2 && L.pos + 1 < L.len && (unsigned char)L.src[L.pos + 1] == 0xA0) {
            L.pos += 2; /* no-break space */
        } else if (c == 0xEF && L.pos + 2 < L.len && (unsigned char)L.src[L.pos + 1] == 0xBB && (unsigned char)L.src[L.pos + 2] == 0xBF) {
            L.pos += 3; /* byte order mark */
        } else if (c == 0xE2 && L.pos + 2 < L.len && (unsigned char)L.src[L.pos + 1] == 0x80 &&
                   ((unsigned char)L.src[L.pos + 2] == 0xA8 || (unsigned char)L.src[L.pos + 2] == 0xA9)) {
            L.pos += 3; /* line / paragraph separator */
            t->newline_before = 1;
        } else if (c == '/' && L.pos + 1 < L.len && L.src[L.pos + 1] == '/') {
            while (L.pos < L.len && L.src[L.pos] != '\n') L.pos++;
        } else if (c == '/' && L.pos + 1 < L.len && L.src[L.pos + 1] == '*') {
            L.pos += 2;
            while (L.pos + 1 < L.len && !(L.src[L.pos] == '*' && L.src[L.pos + 1] == '/')) {
                if (L.src[L.pos] == '\n') {
                    L.line++;
                    t->newline_before = 1;
                }
                L.pos++;
            }
            L.pos += 2;
        } else if (c == '<' && L.pos + 3 < L.len && memcmp(L.src + L.pos, "<!--", 4) == 0) {
            while (L.pos < L.len && L.src[L.pos] != '\n') L.pos++; /* old HTML comment hiding */
        } else if (c == '-' && L.pos + 2 < L.len && memcmp(L.src + L.pos, "-->", 3) == 0 && (t->newline_before || L.pos == 0)) {
            while (L.pos < L.len && L.src[L.pos] != '\n') L.pos++;
        } else {
            break;
        }
    }
    t->line = L.line;
    t->start = L.pos;
    if (L.pos >= L.len) {
        t->type = T_EOF;
        return;
    }
    char c = L.src[L.pos];
    /* identifiers and keywords */
    if (ident_start((unsigned char)c) || c == '\\' || (c == '#' && L.pos + 1 < L.len && ident_start((unsigned char)L.src[L.pos + 1]))) {
        int start = L.pos;
        L.pos++;
        while (L.pos < L.len && ident_part((unsigned char)L.src[L.pos])) L.pos++;
        t->type = c == '#' ? T_PRIVATE : T_IDENT;
        t->str = intern(L.src + start, L.pos - start);
        L.regex_ok = is_keyword_before_regex(t->str);
        return;
    }
    /* numbers */
    if ((c >= '0' && c <= '9') || (c == '.' && L.pos + 1 < L.len && L.src[L.pos + 1] >= '0' && L.src[L.pos + 1] <= '9')) {
        char text[128];
        int k = 0, start = L.pos;
        while (L.pos < L.len && (ident_part((unsigned char)L.src[L.pos]) || L.src[L.pos] == '.' ||
                                 ((L.src[L.pos] == '+' || L.src[L.pos] == '-') && (L.src[L.pos - 1] == 'e' || L.src[L.pos - 1] == 'E') &&
                                  !(L.src[start] == '0' && L.pos > start + 1 && (L.src[start + 1] == 'x' || L.src[start + 1] == 'X'))))) {
            if (L.src[L.pos] == '.' && k && memchr_safe(text, k, '.')) break;
            if (L.src[L.pos] != '_' && L.src[L.pos] != 'n' && k < 127) text[k++] = L.src[L.pos];
            L.pos++;
        }
        if (L.src[L.pos - 1] == 'n') { /* a BigInt: 123n, 0x1fn */
            t->str = bigint_literal(text, k);
            t->type = T_BIG;
            L.regex_ok = 0;
            return;
        }
        /* legacy octal 0777 */
        if (k > 1 && text[0] == '0' && text[1] >= '0' && text[1] <= '7') {
            double v = 0;
            for (int i = 1; i < k; i++) v = v * 8 + (text[i] - '0');
            t->num = v;
        } else {
            t->num = parse_number(text, k, NULL, 0);
        }
        t->type = T_NUM;
        L.regex_ok = 0;
        return;
    }
    /* strings */
    if (c == '"' || c == '\'') {
        L.pos++;
        Buf b = {0};
        while (L.pos < L.len && L.src[L.pos] != c) {
            if (L.src[L.pos] == '\\') {
                L.pos++;
                lex_escape(&b);
            } else {
                if (L.src[L.pos] == '\n') {
                    syntax_error("a string runs past the end of its line");
                    break;
                }
                buf_ch(&b, L.src[L.pos++]);
            }
        }
        L.pos++;
        t->type = T_STR;
        t->str = interned(buf_done(&b));
        L.regex_ok = 0;
        return;
    }
    if (c == '`') {
        L.pos++;
        lex_template_chunk(1);
        return;
    }
    /* regular expressions */
    if (c == '/' && L.regex_ok) {
        int start = ++L.pos, in_class = 0;
        while (L.pos < L.len && (in_class || L.src[L.pos] != '/')) {
            if (L.src[L.pos] == '\\') L.pos++;
            else if (L.src[L.pos] == '[') in_class = 1;
            else if (L.src[L.pos] == ']') in_class = 0;
            else if (L.src[L.pos] == '\n') {
                syntax_error("a regular expression runs past the end of its line");
                break;
            }
            L.pos++;
        }
        int end = L.pos++;
        int fstart = L.pos;
        while (L.pos < L.len && ident_part((unsigned char)L.src[L.pos])) L.pos++;
        t->type = T_REGEX;
        t->str = intern(L.src + start, end - start);
        t->flags = intern(L.src + fstart, L.pos - fstart);
        L.regex_ok = 0;
        return;
    }
    /* punctuators: longest match */
    static const char *puncts[] = {">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
                                   "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=",
                                   "/=", "%=", "&=", "|=", "^=", "<<", ">>", "**", NULL};
    for (int i = 0; puncts[i]; i++) {
        int n = (int)strlen(puncts[i]);
        if (L.pos + n <= L.len && memcmp(L.src + L.pos, puncts[i], (size_t)n) == 0) {
            /* "?." followed by a digit is "?" then ".5" */
            if (n == 2 && puncts[i][0] == '?' && puncts[i][1] == '.' && L.pos + 2 < L.len && L.src[L.pos + 2] >= '0' &&
                L.src[L.pos + 2] <= '9')
                continue;
            int code = 0;
            for (int k = 0; k < n; k++) code |= (unsigned char)puncts[i][k] << (8 * k);
            L.pos += n;
            t->type = T_PUNCT;
            t->punct = code;
            L.regex_ok = !(code == P2('+', '+') || code == P2('-', '-'));
            return;
        }
    }
    L.pos++;
    t->type = T_PUNCT;
    t->punct = (unsigned char)c;
    if (c == '{') {
        if (L.depth < MAX_BRACES) L.braces[L.depth++] = 0;
    } else if (c == '}') {
        if (L.depth > 0 && L.braces[L.depth - 1] == 1) { /* the end of a template's ${ } */
            L.depth--;
            lex_template_chunk(0);
            return;
        }
        if (L.depth > 0) L.depth--;
    }
    L.regex_ok = !(c == ')' || c == ']' || c == '}');
    if (!strchr("(){}[];,<>+-*/%&|^!~?:=.@", c)) syntax_error("an unexpected character");
}

static int memchr_safe(const char *s, int n, char c) {
    for (int i = 0; i < n; i++) {
        if (s[i] == c) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- parser helpers */

static int is_p(int code) { return L.tok.type == T_PUNCT && L.tok.punct == code; }

static int is_word(const char *w) {
    return L.tok.type == T_IDENT && L.tok.str->len == (int)strlen(w) && memcmp(L.tok.str->s, w, (size_t)L.tok.str->len) == 0;
}

static int eat(int code) {
    if (is_p(code)) {
        next_token();
        return 1;
    }
    return 0;
}

static int eat_word(const char *w) {
    if (is_word(w)) {
        next_token();
        return 1;
    }
    return 0;
}

static void expect(int code, const char *what) {
    if (!eat(code)) {
        char msg[64] = "expected ";
        strncat_safe(msg, what, 40);
        syntax_error(msg);
    }
}

/* Automatic semicolon insertion. */
static void end_statement(void) {
    if (eat(';')) return;
    if (is_p('}') || L.tok.type == T_EOF || L.tok.newline_before) return;
    syntax_error("expected ;");
}

static const char *RESERVED[] = {"break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete",
                                 "do", "else", "export", "extends", "finally", "for", "function", "if", "import", "in",
                                 "instanceof", "new", "return", "super", "switch", "this", "throw", "try", "typeof", "var",
                                 "void", "while", "with", "null", "true", "false", NULL};

static int reserved(const JsStr *s) {
    for (int i = 0; RESERVED[i]; i++) {
        if ((int)strlen(RESERVED[i]) == s->len && memcmp(RESERVED[i], s->s, (size_t)s->len) == 0) return 1;
    }
    return 0;
}

typedef struct {
    Lexer lexer;
} Saved;

static void save_state(Saved *s) { s->lexer = L; }
static void restore_state(Saved *s) { L = s->lexer; }

static Ast *parse_assign(int no_in);
static Ast *parse_expression(int no_in);
static Ast *parse_statement(void);
static Ast *parse_function(int is_expr, int flags);
static Ast *parse_class(int is_expr);
static Ast *parse_block_body(void);
static void parse_function_body(Ast *fn);

/* Append to a list kept as (first, last). */
static void list_add(Ast **first, Ast **last, Ast *n) {
    if (!n) return;
    if (*last) (*last)->next = n;
    else *first = n;
    *last = n;
}

/* ---------------------------------------------------------------- expressions */

static Ast *parse_template(void) {
    Ast *t = node(N_TEMPLATE), *first = NULL, *last = NULL;
    for (;;) {
        Ast *s = node(N_STR);
        s->str = L.tok.str;
        list_add(&first, &last, s);
        int type = L.tok.type;
        next_token();
        if (type == T_TPL_FULL || type == T_TPL_TAIL) break;
        Ast *e = parse_expression(0);
        list_add(&first, &last, e);
        if (parse_failed) break;
        if (L.tok.type != T_TPL_MID && L.tok.type != T_TPL_TAIL) {
            syntax_error("a template's ${ } is not closed");
            break;
        }
    }
    t->list = first;
    return t;
}

static Ast *parse_property_key(Ast *p) {
    if (L.tok.type == T_IDENT || L.tok.type == T_STR || L.tok.type == T_PRIVATE) {
        p->str = L.tok.str;
        next_token();
    } else if (L.tok.type == T_NUM) {
        char text[40];
        number_to_text(L.tok.num, text);
        p->str = intern_c(text);
        next_token();
    } else if (eat('[')) {
        p->flags |= F_COMPUTED;
        p->a = parse_assign(0);
        expect(']', "]");
    } else {
        syntax_error("expected a property name");
    }
    return p;
}

static Ast *parse_object_literal(void) {
    Ast *o = node(N_OBJECT), *first = NULL, *last = NULL;
    next_token(); /* { */
    while (!is_p('}') && L.tok.type != T_EOF) {
        Ast *p = node(N_PROP);
        if (eat(P3('.', '.', '.'))) {
            p->flags |= F_SPREAD;
            p->b = parse_assign(0);
        } else {
            int flags = 0;
            if ((is_word("get") || is_word("set") || is_word("async")) ) {
                Saved s;
                save_state(&s);
                const char *w = is_word("get") ? "get" : is_word("set") ? "set" : "async";
                next_token();
                if (is_p(',') || is_p(':') || is_p('(') || is_p('}') || is_p('=')) {
                    restore_state(&s); /* a property called get / set / async */
                } else {
                    flags = w[0] == 'g' ? F_GETTER : w[0] == 's' ? F_SETTER : F_ASYNC;
                }
            }
            if (is_p('*')) {
                next_token();
                flags |= F_GENERATOR;
            }
            int key_type = L.tok.type;
            JsStr *key_word = L.tok.str;
            parse_property_key(p);
            if (is_p('(')) { /* a method */
                p->flags |= F_METHOD | flags;
                p->b = parse_function(1, F_METHOD | (flags & F_ASYNC));
                if (p->b && p->str) p->b->str = p->str;
            } else if (flags & (F_GETTER | F_SETTER)) {
                syntax_error("expected ( after a getter or setter name");
            } else if (eat(':')) {
                p->b = parse_assign(0);
            } else if (key_type == T_IDENT && !(p->flags & F_COMPUTED)) { /* {a} or {a = 1} (a pattern) */
                p->flags |= F_SHORTHAND;
                Ast *id = node(N_IDENT);
                id->str = key_word;
                if (is_p('=')) {
                    next_token();
                    Ast *def = node(N_ASSIGN);
                    def->op = '=';
                    def->a = id;
                    def->b = parse_assign(0);
                    p->b = def;
                } else {
                    p->b = id;
                }
            } else {
                syntax_error("expected : in an object");
            }
        }
        list_add(&first, &last, p);
        if (!eat(',')) break;
    }
    expect('}', "}");
    o->list = first;
    return o;
}

static Ast *parse_array_literal(void) {
    Ast *a = node(N_ARRAY), *first = NULL, *last = NULL;
    next_token(); /* [ */
    while (!is_p(']') && L.tok.type != T_EOF) {
        if (is_p(',')) {
            next_token();
            list_add(&first, &last, node(N_HOLE));
            continue;
        }
        Ast *e;
        if (eat(P3('.', '.', '.'))) {
            e = node(N_SPREAD);
            e->a = parse_assign(0);
        } else {
            e = parse_assign(0);
        }
        list_add(&first, &last, e);
        if (!eat(',')) break;
    }
    expect(']', "]");
    a->list = first;
    return a;
}

/* Whether the ( at a source position starts an arrow function's
   parameters, remembered: finding out means reading ahead to its ), and
   without this, code nested in many parentheses (as bundles are) would be
   read again for each of them. */
static int *arrow_memo_pos;          /* position + 1; 0: empty */
static unsigned char *arrow_memo_val; /* 1 no, 2 yes */
static int arrow_memo_cap, arrow_memo_n;

static void arrow_memo_clear(void) {
    free(arrow_memo_pos);
    free(arrow_memo_val);
    arrow_memo_pos = NULL;
    arrow_memo_val = NULL;
    arrow_memo_cap = arrow_memo_n = 0;
}

static int arrow_memo_get(int pos) {
    if (!arrow_memo_cap) return 0;
    unsigned h = ((unsigned)pos * 2654435761u) & (unsigned)(arrow_memo_cap - 1);
    while (arrow_memo_pos[h]) {
        if (arrow_memo_pos[h] == pos + 1) return arrow_memo_val[h];
        h = (h + 1) & (unsigned)(arrow_memo_cap - 1);
    }
    return 0;
}

static void arrow_memo_put(int pos, int val) {
    if ((arrow_memo_n + 1) * 2 > arrow_memo_cap) {
        int cap = arrow_memo_cap ? arrow_memo_cap * 2 : 1024;
        int *np = malloc(sizeof(int) * (size_t)cap);
        unsigned char *nv = malloc((size_t)cap);
        if (np) memset(np, 0, sizeof(int) * (size_t)cap);
        if (nv) memset(nv, 0, (size_t)cap);
        if (!np || !nv) {
            free(np);
            free(nv);
            return; /* (then it is just found out again) */
        }
        for (int i = 0; i < arrow_memo_cap; i++) {
            if (!arrow_memo_pos[i]) continue;
            unsigned h = ((unsigned)(arrow_memo_pos[i] - 1) * 2654435761u) & (unsigned)(cap - 1);
            while (np[h]) h = (h + 1) & (unsigned)(cap - 1);
            np[h] = arrow_memo_pos[i];
            nv[h] = arrow_memo_val[i];
        }
        free(arrow_memo_pos);
        free(arrow_memo_val);
        arrow_memo_pos = np;
        arrow_memo_val = nv;
        arrow_memo_cap = cap;
    }
    unsigned h = ((unsigned)pos * 2654435761u) & (unsigned)(arrow_memo_cap - 1);
    while (arrow_memo_pos[h]) {
        if (arrow_memo_pos[h] == pos + 1) {
            arrow_memo_val[h] = (unsigned char)val;
            return;
        }
        h = (h + 1) & (unsigned)(arrow_memo_cap - 1);
    }
    arrow_memo_pos[h] = pos + 1;
    arrow_memo_val[h] = (unsigned char)val;
    arrow_memo_n++;
}

/* Is the ( here the start of an arrow function's parameters? */
static int arrow_ahead(void) {
    int first = L.tok.start;
    int known = arrow_memo_get(first);
    if (known) return known == 2;
    Saved s;
    save_state(&s);
    enum { DEPTH = 512 };
    int starts[DEPTH];
    unsigned char paren[DEPTH];
    int depth = 0, result = 0, closed = -1; /* closed: where the ( just closed began */
    for (;;) {
        if (closed >= 0) { /* the token after a (...) */
            int yes = is_p(P2('=', '>')) && !L.tok.newline_before;
            arrow_memo_put(closed, yes ? 2 : 1);
            if (closed == first) {
                result = yes;
                break;
            }
            closed = -1;
        }
        if (L.tok.type == T_EOF) break;
        if (is_p('(') || is_p('[') || is_p('{')) {
            if (depth < DEPTH) {
                starts[depth] = L.tok.start;
                paren[depth] = is_p('(');
            }
            depth++;
        } else if (is_p(')') || is_p(']') || is_p('}')) {
            depth--;
            if (depth < 0) break;
            if (depth < DEPTH && paren[depth]) closed = starts[depth];
            else if (depth == 0) break;
        }
        next_token();
    }
    int failed = parse_failed;
    restore_state(&s);
    parse_failed = failed && parse_failed;
    return result;
}

static Ast *parse_arguments(void) {
    Ast *first = NULL, *last = NULL;
    next_token(); /* ( */
    while (!is_p(')') && L.tok.type != T_EOF) {
        Ast *e;
        if (eat(P3('.', '.', '.'))) {
            e = node(N_SPREAD);
            e->a = parse_assign(0);
        } else {
            e = parse_assign(0);
        }
        list_add(&first, &last, e);
        if (!eat(',')) break;
    }
    expect(')', ")");
    return first;
}

/* Arrow function: parameters (already as a list) and a body. */
static Ast *parse_arrow_body(Ast *params, int flags) {
    Ast *f = node(N_FUNC);
    f->cache_len = L.tok.start + 1; /* where it starts (for messages) */
    f->flags = F_ARROW | flags;
    f->list = params;
    expect(P2('=', '>'), "=>");
    if (is_p('{')) {
        parse_function_body(f);
    } else {
        f->flags |= F_EXPR_BODY;
        f->a = parse_assign(0);
    }
    return f;
}

static Ast *parse_paren_params(void) {
    Ast *first = NULL, *last = NULL;
    next_token(); /* ( */
    while (!is_p(')') && L.tok.type != T_EOF) {
        Ast *e;
        if (eat(P3('.', '.', '.'))) {
            e = node(N_SPREAD);
            e->a = parse_assign(0);
        } else {
            e = parse_assign(0);
        }
        list_add(&first, &last, e);
        if (!eat(',')) break;
    }
    expect(')', ")");
    return first;
}

static Ast *parse_primary(void) {
    Token t = L.tok;
    if (t.type == T_NUM) {
        Ast *n = node(N_NUM);
        n->num = t.num;
        next_token();
        return n;
    }
    if (t.type == T_BIG) { /* an N_NUM holding a BigInt (op 'n') */
        Ast *n = node(N_NUM);
        n->op = 'n';
        n->str = t.str;
        next_token();
        return n;
    }
    if (t.type == T_STR) {
        Ast *n = node(N_STR);
        n->str = t.str;
        next_token();
        return n;
    }
    if (t.type == T_TPL_FULL || t.type == T_TPL_HEAD) return parse_template();
    if (t.type == T_REGEX) {
        Ast *n = node(N_REGEX);
        n->str = t.str;
        Ast *f = node(N_STR);
        f->str = t.flags;
        n->a = f;
        next_token();
        return n;
    }
    if (t.type == T_PUNCT) {
        if (t.punct == '(') {
            if (arrow_ahead()) return parse_arrow_body(parse_paren_params(), 0);
            next_token();
            Ast *e = parse_expression(0);
            expect(')', ")");
            return e;
        }
        if (t.punct == '[') return parse_array_literal();
        if (t.punct == '{') return parse_object_literal();
        syntax_error("unexpected punctuation");
        return node(N_EMPTY);
    }
    if (t.type == T_PRIVATE) { /* #x in obj */
        Ast *n = node(N_STR);
        n->str = t.str;
        next_token();
        return n;
    }
    if (t.type == T_IDENT) {
        JsStr *w = t.str;
        if (is_word("function")) return parse_function(1, 0);
        if (is_word("class")) return parse_class(1);
        if (is_word("async")) {
            Saved s;
            save_state(&s);
            next_token();
            if (is_word("function") && !L.tok.newline_before) return parse_function(1, F_ASYNC);
            if (L.tok.type == T_IDENT && !L.tok.newline_before) { /* async x => ... */
                Ast *p = node(N_IDENT);
                p->str = L.tok.str;
                next_token();
                if (is_p(P2('=', '>'))) return parse_arrow_body(p, F_ASYNC);
            }
            if (is_p('(') && arrow_ahead()) return parse_arrow_body(parse_paren_params(), F_ASYNC);
            restore_state(&s);
        }
        if (is_word("this")) {
            next_token();
            return node(N_THIS);
        }
        if (is_word("super")) {
            next_token();
            return node(N_SUPER);
        }
        if (is_word("null") || is_word("true") || is_word("false")) {
            Ast *n = node(N_NUM);
            n->type = N_IDENT; /* evaluated as a constant by name */
            n->op = 'k';
            n->str = w;
            next_token();
            return n;
        }
        if (is_word("new")) {
            next_token();
            if (eat('.')) { /* new.target */
                next_token();
                return node(N_NEWTARGET);
            }
            Ast *n = node(N_NEW);
            /* the callee: a member expression without calls */
            Ast *callee;
            if (is_word("new")) callee = parse_primary();
            else callee = parse_primary();
            for (;;) {
                if (is_p('.')) {
                    next_token();
                    Ast *m = node(N_MEMBER);
                    m->a = callee;
                    m->str = L.tok.str;
                    if (L.tok.type != T_IDENT && L.tok.type != T_PRIVATE) syntax_error("expected a property name after .");
                    next_token();
                    callee = m;
                } else if (is_p('[')) {
                    next_token();
                    Ast *m = node(N_INDEX);
                    m->a = callee;
                    m->b = parse_expression(0);
                    expect(']', "]");
                    callee = m;
                } else {
                    break;
                }
            }
            n->a = callee;
            if (is_p('(')) n->list = parse_arguments();
            return n;
        }
        if (reserved(w) && !is_word("in") && !is_word("of")) {
            char msg[64] = "unexpected word ";
            strncat_safe(msg, w->s, 30);
            syntax_error(msg);
            next_token();
            return node(N_EMPTY);
        }
        next_token();
        Ast *id = node(N_IDENT);
        id->str = w;
        if (is_p(P2('=', '>')) && !L.tok.newline_before) return parse_arrow_body(id, 0);
        return id;
    }
    syntax_error("unexpected end of the script");
    return node(N_EMPTY);
}

static Ast *parse_call_member(void) {
    Ast *e = parse_primary();
    int in_optional = 0;
    for (;;) {
        if (parse_failed) return e;
        if (is_p('.')) {
            next_token();
            Ast *m = node(N_MEMBER);
            m->a = e;
            m->str = L.tok.str;
            if (L.tok.type != T_IDENT && L.tok.type != T_PRIVATE) syntax_error("expected a property name after .");
            next_token();
            if (in_optional) m->flags |= F_CHAIN; /* part of an optional chain */
            e = m;
        } else if (is_p(P2('?', '.'))) {
            next_token();
            in_optional = 1;
            if (is_p('(')) {
                Ast *c = node(N_CALL);
                c->a = e;
                c->flags |= F_OPTIONAL;
                c->list = parse_arguments();
                e = c;
            } else if (is_p('[')) {
                next_token();
                Ast *m = node(N_INDEX);
                m->a = e;
                m->flags |= F_OPTIONAL;
                m->b = parse_expression(0);
                expect(']', "]");
                e = m;
            } else {
                Ast *m = node(N_MEMBER);
                m->a = e;
                m->flags |= F_OPTIONAL;
                m->str = L.tok.str;
                next_token();
                e = m;
            }
        } else if (is_p('[')) {
            next_token();
            Ast *m = node(N_INDEX);
            m->a = e;
            if (in_optional) m->flags |= F_CHAIN;
            m->b = parse_expression(0);
            expect(']', "]");
            e = m;
        } else if (is_p('(')) {
            Ast *c = node(N_CALL);
            c->cache_len = L.tok.start + 1; /* where its ( is: for error messages */
            c->a = e;
            if (in_optional) c->flags |= F_CHAIN;
            c->list = parse_arguments();
            e = c;
        } else if (L.tok.type == T_TPL_FULL || L.tok.type == T_TPL_HEAD) {
            Ast *tg = node(N_TAGGED);
            tg->a = e;
            tg->b = parse_template();
            e = tg;
        } else {
            return e;
        }
    }
}

static Ast *parse_unary(void) {
    if (L.tok.type == T_PUNCT) {
        int p = L.tok.punct;
        if (p == '!' || p == '~' || p == '+' || p == '-') {
            next_token();
            Ast *u = node(N_UNARY);
            u->op = p;
            u->a = parse_unary();
            return u;
        }
        if (p == P2('+', '+') || p == P2('-', '-')) {
            next_token();
            Ast *u = node(N_UPDATE);
            u->op = p;
            u->flags |= F_PREFIX;
            u->a = parse_unary();
            return u;
        }
    }
    if (is_word("typeof") || is_word("void") || is_word("delete") || is_word("await")) {
        int op = L.tok.str->s[0] == 't' ? 't' : L.tok.str->s[0] == 'v' ? 'v' : L.tok.str->s[0] == 'd' ? 'd' : 'a';
        Saved s;
        save_state(&s);
        next_token();
        /* "await" used as a plain name */
        if (op == 'a' && (is_p(')') || is_p(';') || is_p(',') || is_p('=') || L.tok.type == T_EOF)) {
            restore_state(&s);
        } else {
            Ast *u = node(N_UNARY);
            u->op = op;
            u->a = parse_unary();
            return u;
        }
    }
    Ast *e = parse_call_member();
    if (L.tok.type == T_PUNCT && (L.tok.punct == P2('+', '+') || L.tok.punct == P2('-', '-')) && !L.tok.newline_before) {
        Ast *u = node(N_UPDATE);
        u->op = L.tok.punct;
        u->a = e;
        next_token();
        return u;
    }
    if (is_p(P2('*', '*'))) { /* right-associative, binds tighter than unary on its right */
        next_token();
        Ast *b = node(N_BINARY);
        b->op = P2('*', '*');
        b->a = e;
        b->b = parse_unary();
        return b;
    }
    return e;
}

static int binary_precedence(int no_in, int *logical) {
    *logical = 0;
    if (L.tok.type == T_IDENT) {
        if (is_word("instanceof")) return 8;
        if (is_word("in") && !no_in) return 8;
        return 0;
    }
    if (L.tok.type != T_PUNCT) return 0;
    switch (L.tok.punct) {
    case P2('?', '?'): *logical = 1; return 1;
    case P2('|', '|'): *logical = 1; return 2;
    case P2('&', '&'): *logical = 1; return 3;
    case '|': return 4;
    case '^': return 5;
    case '&': return 6;
    case P2('=', '='): case P2('!', '='): case P3('=', '=', '='): case P3('!', '=', '='): return 7;
    case '<': case '>': case P2('<', '='): case P2('>', '='): return 8;
    case P2('<', '<'): case P2('>', '>'): case P3('>', '>', '>'): return 9;
    case '+': case '-': return 10;
    case '*': case '/': case '%': return 11;
    }
    return 0;
}

static Ast *parse_binary(int min_prec, int no_in) {
    Ast *left = parse_unary();
    for (;;) {
        int logical, prec = binary_precedence(no_in, &logical);
        if (!prec || prec < min_prec || parse_failed) return left;
        int op = L.tok.type == T_IDENT ? (L.tok.str->s[1] == 'n' && L.tok.str->len == 2 ? P2('i', 'n') : P2('i', 'o')) : L.tok.punct;
        next_token();
        Ast *right = parse_binary(prec + 1, no_in);
        Ast *b = node(logical ? N_LOGICAL : N_BINARY);
        b->op = op;
        b->a = left;
        b->b = right;
        left = b;
    }
}

static Ast *parse_conditional(int no_in) {
    Ast *test = parse_binary(1, no_in);
    if (!is_p('?')) return test;
    next_token();
    Ast *c = node(N_COND);
    c->a = test;
    c->b = parse_assign(0);
    expect(':', ":");
    c->c = parse_assign(no_in);
    return c;
}

static int is_assign_op(int p) {
    switch (p) {
    case '=': case P2('+', '='): case P2('-', '='): case P2('*', '='): case P2('/', '='): case P2('%', '='):
    case P3('*', '*', '='): case P3('<', '<', '='): case P3('>', '>', '='): case P4('>', '>', '>', '='):
    case P2('&', '='): case P2('|', '='): case P2('^', '='): case P3('&', '&', '='): case P3('|', '|', '='):
    case P3('?', '?', '='):
        return 1;
    }
    return 0;
}

static Ast *parse_assign(int no_in) {
    if (parse_failed || parse_too_deep()) return node(N_EMPTY);
    if (is_word("yield")) {
        syntax_error("generators (yield) are not supported");
        return node(N_EMPTY);
    }
    Ast *left = parse_conditional(no_in);
    if (L.tok.type == T_PUNCT && is_assign_op(L.tok.punct)) {
        int op = L.tok.punct;
        if (left->type != N_IDENT && left->type != N_MEMBER && left->type != N_INDEX && left->type != N_OBJECT &&
            left->type != N_ARRAY)
            syntax_error("invalid assignment target");
        next_token();
        Ast *a = node(N_ASSIGN);
        a->op = op;
        a->a = left;
        a->b = parse_assign(no_in);
        return a;
    }
    return left;
}

static Ast *parse_expression(int no_in) {
    Ast *e = parse_assign(no_in);
    while (is_p(',') && !parse_failed) {
        next_token();
        Ast *s = node(N_SEQ);
        s->a = e;
        s->b = parse_assign(no_in);
        e = s;
    }
    return e;
}

/* ---------------------------------------------------------------- functions and classes */

static Ast *parse_block_body(void) {
    Ast *b = node(N_BLOCK), *first = NULL, *last = NULL;
    expect('{', "{");
    while (!is_p('}') && L.tok.type != T_EOF && !parse_failed) list_add(&first, &last, parse_statement());
    expect('}', "}");
    b->list = first;
    return b;
}

#define LAZY_MIN_BYTES 256
static int lazy_bodies;       /* parse_script set this: bodies may be left for later */
static int stat_lazy_made, stat_lazy_small, stat_lazy_parsed;
static JsStr *S_arguments_word;

/* A function's { body }: parsed now, or (if it is big) only skimmed to its
   end and parsed when the function is first called. Sets fn->a. */
static void parse_function_body(Ast *fn) {
    if (!lazy_bodies || !is_p('{') || parse_failed) {
        fn->a = parse_block_body();
        return;
    }
    if (!S_arguments_word) S_arguments_word = intern_c("arguments");
    Saved s;
    save_state(&s);
    int start = L.tok.start, line = L.tok.line, depth = 0, strict = 0, args = 0, first = 1;
    for (;;) {
        if (L.tok.type == T_EOF || parse_failed) break;
        if (is_p('{')) depth++;
        else if (is_p('}') && --depth == 0) break;
        next_token();
        if (first) { /* the body's first statement: a "use strict" directive? */
            strict = L.tok.type == T_STR && L.tok.str && L.tok.str->len == 10 && !memcmp(L.tok.str->s, "use strict", 10);
            first = 0;
        }
        if (L.tok.type == T_IDENT && L.tok.str == S_arguments_word) args = 1;
    }
    int end = L.tok.start + 1;
    if (L.tok.type == T_EOF || parse_failed || end - start < LAZY_MIN_BYTES) { /* small (or broken): parse it now */
        int failed = parse_failed;
        restore_state(&s);
        parse_failed = failed && parse_failed;
        stat_lazy_small++;
        fn->a = parse_block_body();
        return;
    }
    stat_lazy_made++;
    next_token(); /* } */
    Ast *lazy = node(N_LAZY);
    lazy->num = start;
    lazy->op = end;
    lazy->line = line;
    lazy->cache = (void *)L.src;
    fn->a = lazy;
    if (strict) fn->flags |= F_LAZY_STRICT;
    if (args) fn->flags |= F_LAZY_ARGS;
}

/* Parse a lazy body now (the function is being called). 0, or -1 with
   the reason in parse_error. */
static int parse_lazy_body(Ast *fn) {
    Ast *lazy = fn->a;
    stat_lazy_parsed++;
    Lexer saved_lexer = L;
    int saved_failed = parse_failed;
    char *saved_base = parse_stack_base;
    char base;
    parse_stack_base = js_stack_base && js_stack_base > &base ? js_stack_base : &base; /* (measured from where scripts began) */
    memset(&L, 0, sizeof(L));
    L.src = lazy->cache;
    L.len = lazy->op;
    L.pos = (int)lazy->num;
    L.line = lazy->line;
    L.regex_ok = 1;
    parse_failed = 0;
    parse_error[0] = 0;
    arrow_memo_clear();
    lazy_bodies = 1;
    next_token();
    Ast *body = parse_block_body();
    arrow_memo_clear();
    int ok = !parse_failed;
    if (ok) fn->a = body;
    L = saved_lexer;
    parse_failed = saved_failed;
    parse_stack_base = saved_base;
    return ok ? 0 : -1;
}

/* function [name] (params) { body }; with is_expr, the name is optional. */
static Ast *parse_function(int is_expr, int flags) {
    Ast *f = node(N_FUNC);
    f->cache_len = L.tok.start + 1; /* where it starts (for messages) */
    f->flags = flags;
    if (!(flags & F_METHOD)) {
        next_token(); /* function */
        if (eat('*')) {
            syntax_error("generators (function*) are not supported");
            return f;
        }
        if (L.tok.type == T_IDENT && !is_p('(')) {
            f->str = L.tok.str;
            next_token();
        } else if (!is_expr) {
            syntax_error("a function needs a name");
        }
    }
    if (!is_p('(')) {
        syntax_error("expected ( for a function's parameters");
        return f;
    }
    f->list = parse_paren_params();
    parse_function_body(f);
    return f;
}

static Ast *parse_class(int is_expr) {
    Ast *c = node(N_CLASS), *first = NULL, *last = NULL;
    next_token(); /* class */
    if (L.tok.type == T_IDENT && !is_word("extends")) {
        c->str = L.tok.str;
        next_token();
    } else if (!is_expr) {
        syntax_error("a class needs a name");
    }
    if (eat_word("extends")) c->a = parse_call_member();
    expect('{', "{");
    while (!is_p('}') && L.tok.type != T_EOF && !parse_failed) {
        if (eat(';')) continue;
        Ast *m = node(N_PROP);
        if (is_word("static")) {
            Saved s;
            save_state(&s);
            next_token();
            if (is_p('(') || is_p('=')) restore_state(&s);
            else m->flags |= F_STATIC;
        }
        if ((m->flags & F_STATIC) && is_p('{')) { /* a static block */
            Ast *body = parse_block_body();
            Ast *fn = node(N_FUNC);
            fn->flags = F_METHOD;
            fn->a = body;
            m->flags |= F_FIELD | F_COMPUTED;
            m->a = node(N_STR);
            m->a->str = intern_c("");
            m->b = node(N_CALL);
            m->b->a = fn;
            list_add(&first, &last, m);
            continue;
        }
        int flags = 0;
        if (is_word("get") || is_word("set") || is_word("async")) {
            Saved s;
            save_state(&s);
            const char *w = is_word("get") ? "g" : is_word("set") ? "s" : "a";
            next_token();
            if (is_p('(') || is_p('=') || is_p(';') || is_p('}')) restore_state(&s);
            else flags = w[0] == 'g' ? F_GETTER : w[0] == 's' ? F_SETTER : F_ASYNC;
        }
        if (eat('*')) flags |= F_GENERATOR;
        parse_property_key(m);
        if (is_p('(')) {
            m->flags |= F_METHOD | flags;
            m->b = parse_function(1, F_METHOD | (flags & F_ASYNC));
            if (m->b) m->b->str = m->str;
            if (m->str && !(m->flags & F_STATIC) && m->str->len == 11 && memcmp(m->str->s, "constructor", 11) == 0)
                m->flags |= F_CTOR;
        } else { /* a field */
            m->flags |= F_FIELD;
            if (eat('=')) m->b = parse_assign(0);
            end_statement();
        }
        list_add(&first, &last, m);
    }
    expect('}', "}");
    c->list = first;
    return c;
}

/* ---------------------------------------------------------------- statements */

static Ast *parse_var(int kind, int no_in) {
    Ast *v = node(N_VAR), *first = NULL, *last = NULL;
    v->op = kind;
    next_token(); /* var / let / const */
    for (;;) {
        Ast *d = node(N_DECL);
        if (is_p('{')) d->a = parse_object_literal();
        else if (is_p('[')) d->a = parse_array_literal();
        else if (L.tok.type == T_IDENT) {
            d->a = node(N_IDENT);
            d->a->str = L.tok.str;
            next_token();
        } else {
            syntax_error("expected a variable name");
            break;
        }
        if (eat('=')) d->b = parse_assign(no_in);
        list_add(&first, &last, d);
        if (!eat(',')) break;
    }
    v->list = first;
    return v;
}

static int lexical_start(void) {
    if (is_word("const")) return 1;
    if (!is_word("let")) return 0;
    Saved s;
    save_state(&s);
    next_token();
    int yes = L.tok.type == T_IDENT || is_p('[') || is_p('{');
    restore_state(&s);
    return yes;
}

static Ast *parse_statement(void) {
    if (parse_failed || parse_too_deep()) return NULL;
    if (L.tok.type == T_PUNCT) {
        if (is_p('{')) return parse_block_body();
        if (eat(';')) return node(N_EMPTY);
    }
    if (L.tok.type == T_IDENT) {
        if (is_word("var")) {
            Ast *v = parse_var('v', 0);
            end_statement();
            return v;
        }
        if (lexical_start()) {
            Ast *v = parse_var(is_word("const") ? 'c' : 'l', 0);
            end_statement();
            return v;
        }
        if (is_word("function")) {
            Ast *d = node(N_FUNCDECL);
            d->a = parse_function(0, 0);
            return d;
        }
        if (is_word("async")) {
            Saved s;
            save_state(&s);
            next_token();
            if (is_word("function") && !L.tok.newline_before) {
                Ast *d = node(N_FUNCDECL);
                d->a = parse_function(0, F_ASYNC);
                return d;
            }
            restore_state(&s);
        }
        if (is_word("class")) {
            Ast *d = node(N_CLASSDECL);
            d->a = parse_class(0);
            return d;
        }
        if (is_word("if")) {
            Ast *n = node(N_IF);
            next_token();
            expect('(', "(");
            n->a = parse_expression(0);
            expect(')', ")");
            n->b = parse_statement();
            if (eat_word("else")) n->c = parse_statement();
            return n;
        }
        if (is_word("for")) {
            next_token();
            if (is_word("await")) {
                syntax_error("for await is not supported");
                return NULL;
            }
            expect('(', "(");
            Ast *init = NULL;
            if (is_word("var")) init = parse_var('v', 1);
            else if (lexical_start()) init = parse_var(is_word("const") ? 'c' : 'l', 1);
            else if (!is_p(';')) init = parse_expression(1);
            if (is_word("of") || is_word("in")) {
                Ast *n = node(N_FORIN);
                if (is_word("of")) n->flags |= F_FOR_OF;
                next_token();
                n->a = init;
                n->b = (n->flags & F_FOR_OF) ? parse_assign(0) : parse_expression(0);
                expect(')', ")");
                n->d = parse_statement();
                return n;
            }
            Ast *n = node(N_FOR);
            n->a = init;
            expect(';', ";");
            if (!is_p(';')) n->b = parse_expression(0);
            expect(';', ";");
            if (!is_p(')')) n->c = parse_expression(0);
            expect(')', ")");
            n->d = parse_statement();
            return n;
        }
        if (is_word("while")) {
            Ast *n = node(N_WHILE);
            next_token();
            expect('(', "(");
            n->a = parse_expression(0);
            expect(')', ")");
            n->b = parse_statement();
            return n;
        }
        if (is_word("do")) {
            Ast *n = node(N_DOWHILE);
            next_token();
            n->a = parse_statement();
            if (!eat_word("while")) syntax_error("expected while");
            expect('(', "(");
            n->b = parse_expression(0);
            expect(')', ")");
            eat(';');
            return n;
        }
        if (is_word("return")) {
            Ast *n = node(N_RETURN);
            next_token();
            if (!is_p(';') && !is_p('}') && L.tok.type != T_EOF && !L.tok.newline_before) n->a = parse_expression(0);
            end_statement();
            return n;
        }
        if (is_word("break") || is_word("continue")) {
            Ast *n = node(is_word("break") ? N_BREAK : N_CONTINUE);
            next_token();
            if (L.tok.type == T_IDENT && !L.tok.newline_before && !reserved(L.tok.str)) {
                n->str = L.tok.str;
                next_token();
            }
            end_statement();
            return n;
        }
        if (is_word("throw")) {
            Ast *n = node(N_THROW);
            next_token();
            n->a = parse_expression(0);
            end_statement();
            return n;
        }
        if (is_word("try")) {
            Ast *n = node(N_TRY);
            next_token();
            n->a = parse_block_body();
            if (eat_word("catch")) {
                if (eat('(')) {
                    if (is_p('{')) n->b = parse_object_literal();
                    else if (is_p('[')) n->b = parse_array_literal();
                    else {
                        n->b = node(N_IDENT);
                        n->b->str = L.tok.str;
                        next_token();
                    }
                    expect(')', ")");
                }
                n->c = parse_block_body();
            }
            if (eat_word("finally")) n->d = parse_block_body();
            if (!n->c && !n->d) syntax_error("try needs catch or finally");
            return n;
        }
        if (is_word("switch")) {
            Ast *n = node(N_SWITCH), *first = NULL, *last = NULL;
            next_token();
            expect('(', "(");
            n->a = parse_expression(0);
            expect(')', ")");
            expect('{', "{");
            while (!is_p('}') && L.tok.type != T_EOF && !parse_failed) {
                Ast *c = node(N_CASE), *sf = NULL, *sl = NULL;
                if (eat_word("default")) {
                    c->a = NULL;
                } else if (eat_word("case")) {
                    c->a = parse_expression(0);
                } else {
                    syntax_error("expected case or default");
                    break;
                }
                expect(':', ":");
                while (!is_word("case") && !is_word("default") && !is_p('}') && L.tok.type != T_EOF && !parse_failed)
                    list_add(&sf, &sl, parse_statement());
                c->list = sf;
                list_add(&first, &last, c);
            }
            expect('}', "}");
            n->list = first;
            return n;
        }
        if (is_word("debugger")) {
            next_token();
            end_statement();
            return node(N_EMPTY);
        }
        if (is_word("import") || is_word("export")) {
            /* import(...) is an expression; declarations are not supported */
            Saved s;
            save_state(&s);
            next_token();
            if (!is_p('(') && !is_p('.')) {
                syntax_error("modules (import / export) are not supported");
                return NULL;
            }
            restore_state(&s);
        }
        if (is_word("with")) {
            syntax_error("with is not supported");
            return NULL;
        }
        /* a label */
        if (!reserved(L.tok.str)) {
            Saved s;
            save_state(&s);
            JsStr *name = L.tok.str;
            next_token();
            if (is_p(':')) {
                next_token();
                Ast *n = node(N_LABEL);
                n->str = name;
                n->a = parse_statement();
                return n;
            }
            restore_state(&s);
        }
    }
    Ast *n = node(N_EXPR);
    n->a = parse_expression(0);
    end_statement();
    return n;
}

/* Parse a whole script. NULL (with parse_error set) if it has a syntax error. */
static Ast *parse_script(const char *src, int len) {
    char base;
    parse_stack_base = js_stack_base && js_stack_base > &base ? js_stack_base : &base; /* (eval runs inside scripts) */
    arrow_memo_clear();
    /* the source is kept (bodies are parsed later from it) */
    lazy_bodies = 0;
    KeptSource *kept = len >= 4096 ? malloc(sizeof(KeptSource) + (size_t)len + 1) : NULL;
    if (kept) {
        memcpy(kept->text, src, (size_t)len);
        kept->text[len] = 0;
        kept->next = kept_sources;
        kept_sources = kept;
        kept_bytes += (size_t)len;
        src = kept->text;
        lazy_bodies = 1;
    }
    memset(&L, 0, sizeof(L));
    L.src = src;
    L.len = len;
    L.line = 1;
    L.regex_ok = 1;
    parse_failed = 0;
    parse_error[0] = 0;
    next_token();
    Ast *program = node(N_BLOCK), *first = NULL, *last = NULL;
    while (L.tok.type != T_EOF && !parse_failed) list_add(&first, &last, parse_statement());
    program->list = first;
    arrow_memo_clear();
    return parse_failed ? NULL : program;
}
